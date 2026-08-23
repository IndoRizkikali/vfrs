/*
 * pvc_lmi_common.c - LMI Common Infrastructure
 * VFRS - Virtual Frame Relay Switch
 * Cisco LMI / ANSI T1.617 Annex D / ITU-T Q.933 Annex A
 */

#include "vfr.h"
#include <string.h>

/* ============================================================
 * LMI Common Functions
 * ============================================================ */

/* Initialize LMI state with default parameters */
void lmi_init_state(vfr_lmi_state_t *lmi, int type)
{
    if (!lmi) return;

    memset(lmi, 0, sizeof(vfr_lmi_state_t));
    lmi->type = type;

    /* Default DCE parameters (network side) */
    lmi->n392 = 3;
    lmi->n393 = 4;
    lmi->t392 = 15;

    /* Default DTE parameters (user side) */
    lmi->n391 = 6;
    lmi->n392_dte = 3;
    lmi->n393_dte = 4;
    lmi->t391 = 10;
    lmi->polls_since_full = lmi->n391;

    /* Initial state: non-operational per X.36 §11.5 */
    lmi->dce_link_down = 1;
    lmi->dte_link_down = 1;
    lmi->dte_enabled = 0;
    lmi->dte_last_request_was_full = 0;
    lmi->dce_history = 0xFFFF;
    lmi->dte_history = 0xFFFF;

    /* Asynchronous status subscription default */
    if (type == LMI_TYPE_Q933A || type == LMI_TYPE_ANSI) {
        lmi->async_enabled = 1;
    } else {
        lmi->async_enabled = 0; /* Disabled for Cisco LMI */
    }

    /* Start DCE recovery timer */
    timer_set(&lmi->t392_timer, lmi->t392 * 1000);
}

int lmi_port_dlci_len(vfr_port_t *port)
{
    if (port && port->dlcibit == 23) {
        return 4;
    }
    vfrs_ctx_t *ctx = g_vfrs;
    if (!ctx) return 2;
    
    int has_4octet = 0;
    mutex_lock(&ctx->pvc_mutex);
    for (int i = 0; i < PVC_HASH_SIZE; i++) {
        vfr_pvc_detail_t *pvc = ctx->pvc_table[i];
        while (pvc) {
            if (strcmp(pvc->port_in, port->name) == 0 && pvc->dlci_in > 1023) {
                has_4octet = 1;
                break;
            }
            pvc = pvc->next;
        }
        if (has_4octet) break;
    }
    mutex_unlock(&ctx->pvc_mutex);
    return has_4octet ? 4 : 2;
}

/* Build common LMI header (Address + UI Control + PD + CallRef) */
size_t lmi_build_header(vfr_port_t *port, u8 *buf, size_t max_len, int protocol_disc)
{
    size_t pos = 0;
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    int dlci_len = lmi_port_dlci_len(port);

    if (dlci_len == 4) {
        /* 4-byte Q.922 Address */
        if (pos + 4 > max_len) return 0;
        buf[pos++] = 0x00;  /* DLCI[22:17]=0, EA=0 */
        buf[pos++] = 0x00;  /* DLCI[16:13]=0, EA=0 */
        buf[pos++] = 0x00;  /* DLCI[12:6]=0,  EA=0 */
        buf[pos++] = 0x01;  /* DLCI[5:0]=0,   EA=1 */
    } else {
        /* 2-byte Q.922 Address */
        if (pos + 2 > max_len) return 0;
        if (lmi && lmi->type == LMI_TYPE_CISCO) {
            buf[pos++] = 0xFC;  /* DLCI 1023, EA=0 */
            buf[pos++] = 0xF1;  /* DLCI 1023, EA=1 */
        } else {
            buf[pos++] = 0x00;  /* DLCI 0, EA=0 */
            buf[pos++] = 0x01;  /* DLCI 0, EA=1 */
        }
    }

    /* Control Field: UI (0x03) */
    if (pos + 1 > max_len) return 0;
    buf[pos++] = 0x03;

    /* Protocol discriminator */
    if (pos + 1 > max_len) return 0;
    buf[pos++] = protocol_disc;  /* 0x08 */

    /* Call reference - dummy (all zeros): 1-octet dummy call reference (X.36 §11.2) */
    if (pos + 1 > max_len) return 0;
    buf[pos++] = 0x00;  /* Dummy call reference = 0x00 */

    return pos;
}

/* Increment sequence number (skip 0, per X.36 §11.4.1.2 / Q.933 §A.3.2) */
u8 lmi_inc_seq(u8 seq)
{
    seq++;
    if (seq == 0) seq = 1;  /* the while(seq==0) was dead code — if sets seq=1, never 0 again */
    return seq;
}

/* ============================================================
 * LMI State Management (shared between DCE/DTE)
 * ============================================================ */

/* Reset DCE error counters upon channel-down detection (X.36 §11.4.1.6.1).
 * NOTE: dce_seq_send/recv are intentionally NOT reset — the spec requires the DCE
 * to continue link integrity verification procedures during link-down to detect
 * service restoration (X.36 §11.4.1.6.1 NOTE). Resetting sequence numbers would
 * cause a spurious sequence mismatch on the first post-recovery STATUS ENQUIRY. */
void lmi_reset_dce_state(vfr_lmi_state_t *lmi)
{
    if (!lmi) return;

    lmi->dce_errors = 0;
    lmi->dce_events = 0;
    lmi->polls_since_full = 0;
    lmi->dce_history = 0xFFFF;
    lmi->segment_active = 0;
    lmi->pvc_send_index = 0;
    lmi->dce_last_was_full = 0;

    /* Restart T392 */
    timer_set(&lmi->t392_timer, lmi->t392 * 1000);

    LOG_DEBUG("LMI DCE state reset");
}

/* Enable DTE-side polling on a port that has a working LMI context. */
int lmi_dte_enable(vfr_port_t *port, u8 n391, u16 t391, u8 n392_dte, u8 n393_dte)
{
    vfr_lmi_state_t *lmi;

    if (!port) return -1;
    lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    if (!lmi) {
        LOG_WARN("lmi_dte_enable: no LMI context on %s", port->name);
        return -1;
    }

    if (n391 > 0) lmi->n391 = n391;
    if (t391 > 0) lmi->t391 = t391;
    if (n392_dte > 0) lmi->n392_dte = n392_dte;
    if (n393_dte > 0) lmi->n393_dte = n393_dte;

    lmi->dte_enabled = 1;
    lmi->polls_since_full = lmi->n391;
    timer_set(&lmi->t391_timer, lmi->t391 * 1000);

    LOG_INFO("DTE polling enabled on %s (T391=%us, N391=%u, N392_DTE=%u, N393_DTE=%u)",
             port->name, lmi->t391, lmi->n391, lmi->n392_dte, lmi->n393_dte);
    return 0;
}

/* ============================================================
 * LMI Polling Timer Handler
 * ============================================================ */

int lmi_poll_timer(vfr_port_t *port)
{
    vfr_lmi_state_t *lmi;
    u8 reply_buf[256];
    int reply_len = 0;
    int full_status = 0;
    int trigger_propagate = 0;

    if (!port || !port->lmi_ctx) return -1;

    mutex_lock(&port->mutex);
    lmi = (vfr_lmi_state_t *)port->lmi_ctx;

    /* Check DCE T392 timer (waiting for STATUS ENQUIRY) */
    if (timer_is_expired(&lmi->t392_timer)) {
        lmi->dce_errors++;
        lmi->dce_events++;
        LOG_DEBUG("T392 expired on %s (errors=%u, events=%u)",
                  port->name, lmi->dce_errors, lmi->dce_events);

        /* Update sliding window error history (Type 2 timeout event is always an error) */
        lmi->dce_history = ((lmi->dce_history << 1) | 1) & ((1 << lmi->n393) - 1);

        /* Count errors in the last n393 events */
        int error_count = 0;
        for (int bits = 0; bits < lmi->n393; bits++) {
            if ((lmi->dce_history >> bits) & 1) {
                error_count++;
            }
        }

        /* Check error threshold (X.36 §11.6.2) */
        if (error_count >= lmi->n392) {
            if (!lmi->dce_link_down) {
                lmi->dce_link_down = 1;
                LOG_WARN("LMI DCE channel down on %s (timeout error threshold reached: %d/%d)",
                          port->name, error_count, lmi->n393);
                trigger_propagate = 1;
            }
            /* Reset DCE state and start recovery (restarts T392) */
            lmi_reset_dce_state(lmi);
        } else {
            /* Keep polling T392 while inside the window */
            timer_set(&lmi->t392_timer, lmi->t392 * 1000);
        }
    }

    /* Check DTE T391 timer (sending STATUS ENQUIRY — may be active on NNI) */
    if (timer_is_expired(&lmi->t391_timer)) {
        /* Only poll if interval > 0 (0 = DTE side disabled) */
        if (lmi->t391 == 0) {
            mutex_unlock(&port->mutex);
            return 0;
        }

        /* =================================================================
         * D-1: DTE Error Monitoring (X.36 §11.4.1.6.2)
         *
         * An "event" is defined as the transmission of a STATUS ENQUIRY.
         * The event is an ERROR when no valid STATUS was received before T391
         * expires (dte_status_received == 0) OR a STATUS arrived with an
         * invalid recv_seq (D-5: handler leaves dte_status_received = 0).
         * N392/N393 sliding-window — same algorithm as DCE side.
         * Skip the check on the very first poll (dte_events == 0) because
         * there is no prior polling interval to evaluate.
         * ================================================================= */
        if (lmi->dte_events > 0) {
            int dte_error = !lmi->dte_status_received;

            lmi->dte_history = ((lmi->dte_history << 1) | (u16)dte_error) &
                               (u16)((1u << lmi->n393_dte) - 1u);

            int dte_err_count = 0;
            for (int b = 0; b < lmi->n393_dte; b++) {
                if ((lmi->dte_history >> b) & 1) dte_err_count++;
            }

            if (dte_err_count >= lmi->n392_dte) {
                if (!lmi->dte_link_down) {
                    lmi->dte_link_down = 1;
                    LOG_WARN("DTE service-affecting condition on %s (%d/%d events in error)",
                             port->name, dte_err_count, lmi->n393_dte);
                    trigger_propagate = 1;
                }
            } else {
                /* Recovery: N392 consecutive error-free events (X.36 §11.4.1.6.2) */
                u16 recovery_mask = (u16)((1u << lmi->n392_dte) - 1u);
                if ((lmi->dte_history & recovery_mask) == 0 && lmi->dte_link_down) {
                    lmi->dte_link_down = 0;
                    LOG_INFO("DTE service-affecting condition cleared on %s", port->name);
                    trigger_propagate = 1;
                }
            }
            if (dte_error) {
                lmi->dte_errors++;
                /* X.36 §11.4.1.6.2 NOTE 1: If the unanswered STATUS ENQUIRY requested full status,
                 * the user equipment shall again request full status.
                 * polls_since_full being 0 means the last poll was a Full Status request. */
                if (lmi->polls_since_full == 0) {
                    lmi->polls_since_full = lmi->n391;
                }
                lmi->last_report_type = 0;  /* Cancel continued segmented session on error */
            }
        }
        lmi->dte_status_received = 0;  /* reset flag for the upcoming interval */
        lmi->dte_events++;             /* count this transmission as an event */

        full_status = (lmi->polls_since_full >= lmi->n391);
        lmi->dte_last_request_was_full = full_status;
        if (full_status && g_vfrs) {
            mutex_lock(&g_vfrs->pvc_mutex);
            for (int hash = 0; hash < PVC_HASH_SIZE; hash++) {
                vfr_pvc_detail_t *pvc = g_vfrs->pvc_table[hash];
                while (pvc) {
                    if (strcmp(pvc->port_in, port->name) == 0) {
                        pvc->lmi_reported = 0;
                    }
                    pvc = pvc->next;
                }
            }
            mutex_unlock(&g_vfrs->pvc_mutex);
        }

        /* CB-2: Increment DTE send seq BEFORE building the enquiry.
         * X.36 §11.4.1.2 step 2: "it increments the send sequence counter
         * and places its value into the send sequence number field".
         * Previously the increment happened after the send (used stale value). */
        lmi->dte_seq_send = lmi_inc_seq(lmi->dte_seq_send);

        /* Build STATUS ENQUIRY — builders read lmi->dte_seq_send for the LIV IE.
         * Pass max_len-2 to reserve space for FCS (I-3). */
        if (lmi->type == LMI_TYPE_ANSI) {
            reply_len = lmi_ansi_build_enquiry(port, reply_buf, sizeof(reply_buf) - 2);
        } else if (lmi->type == LMI_TYPE_CISCO) {
            reply_len = lmi_cisco_build_enquiry(port, reply_buf, sizeof(reply_buf) - 2);
        } else if (lmi->type == LMI_TYPE_Q933A) {
            reply_len = lmi_q933a_build_enquiry(port, reply_buf, sizeof(reply_buf) - 2);
        }

        if (reply_len > 0 && reply_len + 2 <= (int)sizeof(reply_buf)) {
            /* Compute and append FCS-16 per X.36 §9.2.4 only for serial and named pipe transports */
            if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
                u16 fcs = crc16_fcs(reply_buf, (size_t)reply_len);
                reply_buf[reply_len++] = fcs & 0xFF;
                reply_buf[reply_len++] = (fcs >> 8) & 0xFF;
            }

            if (port->ops && port->ops->send) {
                int ret = port->ops->send(port, reply_buf, (size_t)reply_len);
                if (ret >= 0) {
                    port->stats.tx_frames++;
                    port->stats.tx_bytes += (u64)ret;
                    LOG_DEBUG("LMI DTE STATUS ENQUIRY sent on %s (full=%d, seq=%u, len=%d)",
                              port->name, full_status, lmi->dte_seq_send, reply_len);
                } else {
                    LOG_WARN("Failed to send DTE STATUS ENQUIRY on %s", port->name);
                }
            }
        }

        timer_set(&lmi->t391_timer, lmi->t391 * 1000);
        if (full_status) {
            lmi->polls_since_full = 0;
        } else {
            lmi->polls_since_full++;
        }
    }

    mutex_unlock(&port->mutex);

    if (trigger_propagate && g_vfrs) {
        vfrs_propagate_port_status_change(g_vfrs, port);
    }

    return 0;
}

/* Build and send asynchronous STATUS notification (DCE initiated, Report Type = 0x02) */
int lmi_send_async_status(vfr_port_t *port, u32 dlci, u8 status_flags)
{
    vfr_lmi_state_t *lmi;
    u8 buf[256];
    size_t pos = 0;

    if (!port || !port->lmi_ctx || !port->ops || !port->ops->send) return -1;

    mutex_lock(&port->mutex);
    lmi = (vfr_lmi_state_t *)port->lmi_ctx;

    /* Asynchronous status support constraints */
    if (!lmi->async_enabled || lmi->dce_link_down) {
        mutex_unlock(&port->mutex);
        return -1;
    }
    if (lmi->type == LMI_TYPE_CISCO) {
        mutex_unlock(&port->mutex);
        return -1; /* Cisco LMI does not support async STATUS */
    }

    /* Build header: Address + UI Control + PD + CallRef */
    int pd = (lmi->type == LMI_TYPE_CISCO) ? 0x09 : 0x08;
    pos += lmi_build_header(port, buf, sizeof(buf), pd);

    /* Message type: STATUS (0x7D) */
    if (pos + 1 > sizeof(buf)) {
        mutex_unlock(&port->mutex);
        return -1;
    }
    buf[pos++] = 0x7D;

    /* Locking Shift to Codeset 5 for ANSI */
    if (lmi->type == LMI_TYPE_ANSI) {
        if (pos + 1 > sizeof(buf)) {
            mutex_unlock(&port->mutex);
            return -1;
        }
        buf[pos++] = 0x95;
    }

    /* Report Type and PVC Status IEs (No Link Integrity Verification IE in async messages) */
    int dlci_len = lmi_port_dlci_len(port);
    if (lmi->type == LMI_TYPE_ANSI) {
        pos += lmi_ansi_build_report_type_ie(buf + pos, sizeof(buf) - pos, LMI_REPORT_ASYNC_STATUS);
        pos += lmi_ansi_build_pvc_status_ie(buf + pos, sizeof(buf) - pos, dlci, status_flags, dlci_len);
    } else if (lmi->type == LMI_TYPE_Q933A) {
        pos += lmi_q933a_build_report_type_ie(buf + pos, sizeof(buf) - pos, LMI_REPORT_ASYNC_STATUS);
        pos += lmi_q933a_build_pvc_status_ie(buf + pos, sizeof(buf) - pos, dlci, status_flags, dlci_len);
    } else if (lmi->type == LMI_TYPE_CISCO) {
        pos += lmi_gof_build_report_type_ie(buf + pos, sizeof(buf) - pos, LMI_REPORT_ASYNC_STATUS);
        pos += lmi_gof_build_pvc_status_ie(buf + pos, sizeof(buf) - pos, dlci, status_flags, dlci_len);
    }

    /* Compute and append FCS-16 per X.36 §9.2.4 only for serial and named pipe transports */
    if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
        if (pos + 2 > sizeof(buf)) {
            mutex_unlock(&port->mutex);
            return -1;
        }
        u16 fcs = crc16_fcs(buf, pos);
        buf[pos++] = fcs & 0xFF;
        buf[pos++] = (fcs >> 8) & 0xFF;
    }

    /* Send frame */
    int ret = port->ops->send(port, buf, pos);
    if (ret >= 0) {
        port->stats.tx_frames++;
        port->stats.tx_bytes += (u64)ret;
        LOG_DEBUG("LMI Asynchronous STATUS sent on %s for DLCI %u (status=0x%02X)",
                  port->name, dlci, status_flags);
        mutex_unlock(&port->mutex);
        return 0;
    } else {
        LOG_WARN("Failed to send LMI Asynchronous STATUS on %s", port->name);
        mutex_unlock(&port->mutex);
        return -1;
    }
}

void lmi_free(vfr_port_t *port)
{
    if (!port || !port->lmi_ctx) return;
    free(port->lmi_ctx);
    port->lmi_ctx = NULL;
}
