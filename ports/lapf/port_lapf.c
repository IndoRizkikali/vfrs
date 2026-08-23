/*
 * port_lapf.c - LAPF (Q.922) Link Access Procedure for Frame Relay
 * VFRS - Virtual Frame Relay Switch
 */

#include "vfr.h"
#include "port_lapf.h"

/* Static helper to determine sequence inclusion in modulo 128 */
static inline int lapf_seq_in_range(u8 seq, u8 min, u8 max) {
    if (min <= max) {
        return (seq >= min && seq <= max);
    } else {
        return (seq >= min || seq <= max);
    }
}

/* Helper to determine C/R bit according to Table 3/Q.922 & §3.3.2:
 * "When the frame to be sent is a command frame, the C/R bit shall be set to 0.
 *  When the frame to be sent is a response frame, the C/R bit shall be set to 1."
 * In Q.922 (unlike ISDN Q.921), C/R bit is symmetrical for all entities:
 * Command = 0, Response = 1.
 */
static inline int lapf_get_tx_cr(vfr_port_t *port, int is_response) {
    (void)port;
    return is_response ? 1 : 0;
}

/* MDL-ERROR indication — logs the error condition per Q.922 Table V-1.
 * All MDL-ERROR indications use LOG_WARN as per design decision. */
static void lapf_mdl_error(vfr_port_t *port, vfr_lapf_state_t *lapf, char code) {
    static const char *desc[128] = {
        ['A'] = "Unsolicited supervisory response F=1",
        ['B'] = "Unsolicited DM response F=1",
        ['C'] = "Unsolicited UA response F=1",
        ['D'] = "Unsolicited UA response F=0",
        ['E'] = "Unsolicited DM response F=0",
        ['F'] = "Peer-initiated re-establishment (SABME)",
        ['G'] = "N200 exhaustion (SABME)",
        ['H'] = "N200 exhaustion (DISC)",
        ['I'] = "N200 exhaustion (status enquiry)",
        ['J'] = "N(R) sequence error",
        ['K'] = "Receipt of FRMR response",
        ['L'] = "Receipt of non-implemented frame",
        ['N'] = "Receipt of frame with wrong size",
        ['O'] = "N201 error",
    };
    unsigned char uc = (unsigned char)code;
    const char *d = (uc < 128 && desc[uc]) ? desc[uc] : "Unknown";
    LOG_WARN("LAPF on %s (DLCI %u): MDL-ERROR(%c) — %s [state=%d]",
             port ? port->name : "NULL", lapf ? lapf->dlci : 0, code, d, lapf ? lapf->state : 0);
}

/* Transmit helper for supervisory/unnumbered frames */
static void lapf_send_control(vfr_port_t *port, vfr_lapf_state_t *lapf, u8 ctrl_type, u8 nr, int pf, int is_response) {
    u8 buf[128];
    size_t addr_len = (port->dlcibit == 23) ? 4 : 2;
    size_t ctrl_len = 0;
    int cr = lapf_get_tx_cr(port, is_response);

    fr_encode_dlci_with_flags(buf, lapf->dlci, 0, 0, 0, cr, (int)addr_len);

    if (ctrl_type == LAPF_CTRL_S_RR || ctrl_type == LAPF_CTRL_S_RNR || ctrl_type == LAPF_CTRL_S_REJ) {
        buf[addr_len] = ctrl_type;
        buf[addr_len + 1] = (nr << 1) | (pf ? 1 : 0);
        ctrl_len = 2;
        if (ctrl_type == LAPF_CTRL_S_RR) lapf->rr_tx++;
        else if (ctrl_type == LAPF_CTRL_S_RNR) lapf->rnr_tx++;
        else lapf->rej_tx++;
    } else {
        buf[addr_len] = ctrl_type | (pf ? 0x10 : 0);
        ctrl_len = 1;
        if (ctrl_type == LAPF_CTRL_U_SABME) lapf->sabme_tx++;
        else if (ctrl_type == LAPF_CTRL_U_DISC) lapf->disc_tx++;
        else if (ctrl_type == LAPF_CTRL_U_UA) lapf->ua_tx++;
        else if (ctrl_type == LAPF_CTRL_U_DM) lapf->dm_tx++;
        else if (ctrl_type == LAPF_CTRL_U_FRMR) lapf->frmr_tx++;
        else if (ctrl_type == LAPF_CTRL_U_UI) lapf->ui_tx++; /* just statistics count (fixed) */
    }

    size_t total_len = addr_len + ctrl_len;
    size_t send_len = total_len;

    if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
        u16 fcs = crc16_fcs(buf, total_len);
        buf[total_len] = fcs & 0xFF;
        buf[total_len + 1] = (fcs >> 8) & 0xFF;
        send_len += 2;
    }

    if (port->ops && port->ops->send) {
        port->ops->send(port, buf, send_len);
    }
}

/* Transmit FRMR response frame with full 5-byte information field per Q.921 §3.10.1 & Figure 6/Q.921.
 *
 * Information field layout (modulo 128 per Figure 6/Q.921):
 *   Octet 5: Control field of rejected frame (byte 1)
 *   Octet 6: Control field of rejected frame (byte 2, 0000 0000 if unnumbered)
 *   Octet 7: V(S) in bits 8-2, 0 in bit 1
 *   Octet 8: V(R) in bits 8-2, C/R of rejected frame in bit 1 (0=cmd, 1=resp)
 *   Octet 9: 0 0 0 0 | Z Y X W  (error condition bits per Notes 5-8)
 */
static void lapf_send_frmr(vfr_port_t *port, vfr_lapf_state_t *lapf,
                           u8 rej_ctrl1, u8 rej_ctrl2, int rej_was_response,
                           u8 error_bits) {
    u8 buf[128];
    size_t addr_len = (port->dlcibit == 23) ? 4 : 2;
    int cr = lapf_get_tx_cr(port, 1);  /* FRMR is always a response */

    fr_encode_dlci_with_flags(buf, lapf->dlci, 0, 0, 0, cr, (int)addr_len);

    /* Control field: FRMR with F=0 (unsolicited) */
    buf[addr_len] = LAPF_CTRL_U_FRMR;  /* 0x87, F=0 */

    /* FRMR information field (5 bytes for modulo 128) per Figure 6/Q.921 */
    buf[addr_len + 1] = rej_ctrl1;                          /* Octet 5: Rejected frame ctrl byte 1 */
    buf[addr_len + 2] = rej_ctrl2;                          /* Octet 6: Rejected frame ctrl byte 2 (0 if U-frame) */
    buf[addr_len + 3] = (u8)(lapf->v_s << 1);               /* Octet 7: V(S) in bits 8-2, bit 1 = 0 */
    buf[addr_len + 4] = (u8)((lapf->v_r << 1) | (rej_was_response ? 1 : 0));  /* Octet 8: V(R) in bits 8-2, bit 1 = C/R */
    buf[addr_len + 5] = error_bits & 0x0F;                  /* Octet 9: 0000 | Z Y X W */

    size_t total_len = addr_len + 6;  /* addr + ctrl(1) + info(5) */
    size_t send_len = total_len;

    if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
        u16 fcs = crc16_fcs(buf, total_len);
        buf[total_len] = fcs & 0xFF;
        buf[total_len + 1] = (fcs >> 8) & 0xFF;
        send_len += 2;
    }

    lapf->frmr_tx++;

    if (port->ops && port->ops->send) {
        port->ops->send(port, buf, send_len);
    }

    LOG_INFO("LAPF on %s (DLCI %u): Sent FRMR response (rej_ctrl=0x%02X%02X, V(S)=%u, V(R)=%u, WXYZ=0x%X)",
             port->name, lapf->dlci, rej_ctrl1, rej_ctrl2, lapf->v_s, lapf->v_r, error_bits);
}

/* Transmit helper for I-frames */
static void lapf_send_i_frame(vfr_port_t *port, vfr_lapf_state_t *lapf, u8 ns, u8 nr, int pf, int is_response, const u8 *data, size_t len) {
    u8 buf[2048];
    size_t addr_len = (port->dlcibit == 23) ? 4 : 2;
    int cr = lapf_get_tx_cr(port, is_response);

    fr_encode_dlci_with_flags(buf, lapf->dlci, 0, 0, 0, cr, (int)addr_len);

    buf[addr_len] = ns << 1;
    buf[addr_len + 1] = (nr << 1) | (pf ? 1 : 0);

    if (len > 0) {
        memcpy(buf + addr_len + 2, data, len);
    }

    size_t total_len = addr_len + 2 + len;
    size_t send_len = total_len;

    if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
        u16 fcs = crc16_fcs(buf, total_len);
        buf[total_len] = fcs & 0xFF;
        buf[total_len + 1] = (fcs >> 8) & 0xFF;
        send_len += 2;
    }

    lapf->i_tx++;

    if (port->ops && port->ops->send) {
        port->ops->send(port, buf, send_len);
    }
}

/* Dequeue from backlog queue and transmit if window is open */
static void lapf_process_backlog(vfr_port_t *port, vfr_lapf_state_t *lapf) {
    if (!lapf) return;

    while (lapf->backlog_head != lapf->backlog_tail) {
        u8 outstanding = (lapf->v_s >= lapf->v_a) ? (lapf->v_s - lapf->v_a) : (128 - lapf->v_a + lapf->v_s);
        if (outstanding >= lapf->k) {
            break; /* Window is full */
        }

        /* Dequeue frame */
        lapf_tx_frame_t *f = &lapf->tx_backlog[lapf->backlog_head];
        u8 ns = lapf->v_s;

        /* Store in outstanding transmission window */
        memcpy(lapf->tx_window[ns].data, f->data, f->len);
        lapf->tx_window[ns].len = f->len;

        lapf_send_i_frame(port, lapf, ns, lapf->v_r, 0, 0, f->data, f->len);
        LOG_DEBUG("LAPF on %s (DLCI %u): Sent I-frame N(S)=%u from backlog", port->name, lapf->dlci, ns);

        lapf->v_s = (lapf->v_s + 1) % 128;
        lapf->backlog_head = (lapf->backlog_head + 1) % 64;

        timer_set(&lapf->t200_timer, lapf->t200);
        timer_cancel(&lapf->t203_timer);
    }
}

/* Initialize LAPF context on a port */
int lapf_port_init(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203) {
    if (!port) return -1;

    mutex_lock(&port->mutex);
    if (dlci == 0 && !port->svc_ctx) {
        LOG_WARN("LAPF on %s: Denied DLCI 0 LAPF initialization because SVC service is not active on this port", port->name);
        mutex_unlock(&port->mutex);
        return -1;
    }
    if (dlci == 1015 && port->type != PORT_TYPE_NNI) {
        LOG_WARN("LAPF on %s: Denied DLCI 1015 LAPF initialization because port is not NNI", port->name);
        mutex_unlock(&port->mutex);
        return -1;
    }

    if (port->lapf_ctx_count >= 16) {
        LOG_ERROR("LAPF on %s: Max LAPF contexts (16) reached", port->name);
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Check if context already exists for this DLCI */
    for (int i = 0; i < port->lapf_ctx_count; i++) {
        if (port->lapf_dlcis[i] == dlci) {
            if (port->lapf_ctxs[i]) {
                free(port->lapf_ctxs[i]);
            }
            vfr_lapf_state_t *lapf = calloc(1, sizeof(vfr_lapf_state_t));
            if (!lapf) {
                mutex_unlock(&port->mutex);
                return -1;
            }
            lapf->dlci = dlci;
            lapf->state = LAPF_STATE_TEI_ASSIGNED;
            lapf->k = k ? k : lapf_default_k_for_rate(port->cgst_ctx ? cgst_get_access_rate(port) : 0);
            lapf->n200 = n200 ? n200 : 3;
            /* Q.922 §5.9.3 default is 260 octets; 1598 is used here as the practical default
             * for LAN interconnection per §5.9.3 recommendation ("at least 1598 octets").
             * LAPF is currently used for SVC signaling (DLCI 0) and will extend to DLCI 1015
             * for VFRS-specific signaling. Both use cases benefit from the larger default. */
            lapf->n201 = n201 ? n201 : 1598;
            lapf->t200 = t200 ? t200 : 1500;
            lapf->t203 = t203 ? t203 : 30000;
            port->lapf_ctxs[i] = lapf;
            LOG_INFO("LAPF re-initialized on %s for DLCI %u: k=%u, n200=%u, n201=%u, t200=%ums, t203=%ums",
                     port->name, dlci, lapf->k, lapf->n200, lapf->n201, lapf->t200, lapf->t203);
            mutex_unlock(&port->mutex);
            return 0;
        }
    }

    vfr_lapf_state_t *lapf = calloc(1, sizeof(vfr_lapf_state_t));
    if (!lapf) {
        LOG_ERROR("Failed to allocate LAPF context for %s", port->name);
        mutex_unlock(&port->mutex);
        return -1;
    }

    lapf->dlci = dlci;
    lapf->state = LAPF_STATE_TEI_ASSIGNED;
    lapf->k = k ? k : lapf_default_k_for_rate(port->cgst_ctx ? cgst_get_access_rate(port) : 0);
    lapf->n200 = n200 ? n200 : 3;
    /* Q.922 §5.9.3 default is 260 octets; 1598 is used here as the practical default
     * for LAN interconnection per §5.9.3 recommendation ("at least 1598 octets").
     * LAPF is currently used for SVC signaling (DLCI 0) and will extend to DLCI 1015
     * for VFRS-specific signaling. Both use cases benefit from the larger default. */
    lapf->n201 = n201 ? n201 : 1598;
    lapf->t200 = t200 ? t200 : 1500;
    lapf->t203 = t203 ? t203 : 30000;

    int idx = port->lapf_ctx_count++;
    port->lapf_ctxs[idx] = lapf;
    port->lapf_dlcis[idx] = dlci;

    LOG_INFO("LAPF initialized on %s for DLCI %u: k=%u, n200=%u, n201=%u, t200=%ums, t203=%ums",
             port->name, dlci, lapf->k, lapf->n200, lapf->n201, lapf->t200, lapf->t203);

    mutex_unlock(&port->mutex);
    return 0;
}

/* Free LAPF context */
void lapf_free(vfr_port_t *port) {
    if (!port) return;
    mutex_lock(&port->mutex);
    for (int i = 0; i < port->lapf_ctx_count; i++) {
        if (port->lapf_ctxs[i]) {
            free(port->lapf_ctxs[i]);
            port->lapf_ctxs[i] = NULL;
        }
    }
    port->lapf_ctx_count = 0;
    mutex_unlock(&port->mutex);
}

/* Establish LAPF link actively */
int lapf_establish_link(vfr_port_t *port, u32 dlci) {
    if (!port) return -1;

    mutex_lock(&port->mutex);
    vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)port_get_lapf_ctx(port, dlci);
    if (!lapf) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
    lapf->retransmission_count = 0;
    lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0); /* SABME P=1 Command */
    timer_set(&lapf->t200_timer, lapf->t200);
    timer_cancel(&lapf->t203_timer);

    LOG_INFO("LAPF on %s (DLCI %u): Actively establishing link, sent SABME (T200 started)", port->name, dlci);
    mutex_unlock(&port->mutex);
    return 0;
}

/* Release LAPF link actively */
int lapf_release_link(vfr_port_t *port, u32 dlci) {
    if (!port) return -1;

    mutex_lock(&port->mutex);
    vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)port_get_lapf_ctx(port, dlci);
    if (!lapf) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    lapf->state = LAPF_STATE_AWAITING_RELEASE;
    lapf->retransmission_count = 0;
    lapf_send_control(port, lapf, LAPF_CTRL_U_DISC, 0, 1, 0); /* DISC P=1 Command */
    timer_set(&lapf->t200_timer, lapf->t200);
    timer_cancel(&lapf->t203_timer);

    LOG_INFO("LAPF on %s (DLCI %u): Actively releasing link, sent DISC", port->name, dlci);
    mutex_unlock(&port->mutex);
    return 0;
}

/* Send Layer 3 payload over LAPF (encapsulates into I-frame) */
int lapf_send_l3(vfr_port_t *port, u32 dlci, const u8 *data, size_t len) {
    if (!port) return -1;

    mutex_lock(&port->mutex);
    vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)port_get_lapf_ctx(port, dlci);
    if (!lapf) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    if (lapf->state != LAPF_STATE_ESTABLISHED && lapf->state != LAPF_STATE_TIMER_RECOVERY) {
        LOG_WARN("LAPF on %s (DLCI %u): Drop L3 transmit, link not established (state=%d)", port->name, dlci, lapf->state);
        mutex_unlock(&port->mutex);
        return -1;
    }

    if (len > lapf->n201) {
        LOG_WARN("LAPF on %s (DLCI %u): Drop L3 transmit, size %zu exceeds N201 (%u)", port->name, dlci, len, lapf->n201);
        mutex_unlock(&port->mutex);
        return -1;
    }

    u8 outstanding = (lapf->v_s >= lapf->v_a) ? (lapf->v_s - lapf->v_a) : (128 - lapf->v_a + lapf->v_s);
    if (outstanding >= lapf->k || lapf->peer_receiver_busy) {
        /* Queue in backlog */
        int next_tail = (lapf->backlog_tail + 1) % 64;
        if (next_tail == lapf->backlog_head) {
            LOG_WARN("LAPF on %s (DLCI %u): Backlog queue full, dropping L3 packet", port->name, dlci);
            mutex_unlock(&port->mutex);
            return -1;
        }

        memcpy(lapf->tx_backlog[lapf->backlog_tail].data, data, len);
        lapf->tx_backlog[lapf->backlog_tail].len = len;
        lapf->backlog_tail = next_tail;

        LOG_DEBUG("LAPF on %s (DLCI %u): Transmit window full/busy, queued frame in backlog (len=%zu)", port->name, dlci, len);
        mutex_unlock(&port->mutex);
        return 0;
    }

    /* Send immediately */
    u8 ns = lapf->v_s;
    memcpy(lapf->tx_window[ns].data, data, len);
    lapf->tx_window[ns].len = len;

    lapf_send_i_frame(port, lapf, ns, lapf->v_r, 0, 0, data, len);
    LOG_DEBUG("LAPF on %s (DLCI %u): Sent I-frame N(S)=%u, N(R)=%u (len=%zu)", port->name, dlci, ns, lapf->v_r, len);

    lapf->v_s = (lapf->v_s + 1) % 128;
    timer_set(&lapf->t200_timer, lapf->t200);
    timer_cancel(&lapf->t203_timer);

    mutex_unlock(&port->mutex);
    return 0;
}

#include "svc_signalling/svc_sig_common.h"

/* Callback for Layer 3 packets received */
void lapf_l3_recv_cb(vfr_port_t *port, const u8 *data, size_t len) {
    LOG_INFO("LAPF on %s: Received Layer 3 Payload (size=%zu)", port->name, len);
    if (port && port->svc_ctx) {
        svc_handle_l3_message(port, data, len);
    } else {
        LOG_WARN("LAPF on %s: Received L3 payload but SVC is not enabled on this port", port ? port->name : "NULL");
    }
}

/* XID parameter negotiation command/response parser */
static void lapf_handle_xid(vfr_port_t *port, vfr_lapf_state_t *lapf, const u8 *payload, size_t len, int pf, int is_response) {
    lapf->xid_rx++;

    if (len < 4) return;
    u8 fi = payload[0];
    u8 gi = payload[1];
    if (fi != 0x82 || gi != 0x80) {
        LOG_DEBUG("LAPF on %s (DLCI %u): Unsupported XID format (FI=%u, GI=%u)", port->name, lapf->dlci, fi, gi);
        return;
    }

    u16 gl = (payload[2] << 8) | payload[3];
    if (len < (size_t)(4 + gl)) return;

    u16 peer_n201_tx = lapf->n201;
    u16 peer_n201_rx = lapf->n201;
    u8 peer_k = lapf->k;
    u32 peer_t200 = lapf->t200;
    (void)peer_n201_rx;

    size_t offset = 4;
    while (offset < (size_t)(4 + gl)) {
        if (offset + 2 > len) break;
        u8 pi = payload[offset];
        u8 pl = payload[offset + 1];
        if (offset + 2 + pl > len) break;

        u32 pv = 0;
        if (pl == 1) {
            pv = payload[offset + 2];
        } else if (pl == 2) {
            pv = (payload[offset + 2] << 8) | payload[offset + 3];
        }

        switch (pi) {
            case 5: peer_n201_tx = (u16)pv; break;
            case 6: peer_n201_rx = (u16)pv; break;
            case 7: peer_k = (u8)pv; break;
            case 9: peer_t200 = pv * 100; break;
            default: break;
        }
        offset += 2 + pl;
    }

    if (!is_response) {
        /* XID Command: Negotiate parameters and respond */
        u8 resp_k = (peer_k < lapf->k) ? peer_k : lapf->k;
        u16 resp_n201 = (peer_n201_tx < lapf->n201) ? peer_n201_tx : lapf->n201;
        u32 resp_t200 = peer_t200 ? peer_t200 : lapf->t200;

        /* Apply negotiated parameters locally */
        lapf->k = resp_k;
        lapf->n201 = resp_n201;
        lapf->t200 = resp_t200;

        LOG_INFO("LAPF on %s (DLCI %u): Negotiated parameters via XID Command: k=%u, n201=%u, t200=%ums",
                 port->name, lapf->dlci, lapf->k, lapf->n201, lapf->t200);

        /* Build XID response */
        u8 resp[32];
        size_t addr_len = (port->dlcibit == 23) ? 4 : 2;
        int cr = lapf_get_tx_cr(port, 1); /* XID Response */
        fr_encode_dlci_with_flags(resp, lapf->dlci, 0, 0, 0, cr, (int)addr_len);

        /* Dynamically compute group parameter length (PI=5:4B, PI=7:3B, PI=9:3B -> 10 bytes) */
        u16 gl_len = 4 + 3 + 3;

        resp[addr_len] = LAPF_CTRL_U_XID | (pf ? 0x10 : 0); /* XID response frame */
        resp[addr_len + 1] = 0x82; /* FI */
        resp[addr_len + 2] = 0x80; /* GI */
        resp[addr_len + 3] = (u8)(gl_len >> 8);   /* GL MSB */
        resp[addr_len + 4] = (u8)(gl_len & 0xFF); /* GL LSB */

        /* PI = 5: Frame size (transmit) */
        resp[addr_len + 5] = 5;
        resp[addr_len + 6] = 2;
        resp[addr_len + 7] = (u8)(resp_n201 >> 8);
        resp[addr_len + 8] = (u8)(resp_n201 & 0xFF);

        /* PI = 7: Window size */
        resp[addr_len + 9] = 7;
        resp[addr_len + 10] = 1;
        resp[addr_len + 11] = resp_k;

        /* PI = 9: Retransmission timer */
        resp[addr_len + 12] = 9;
        resp[addr_len + 13] = 1;
        resp[addr_len + 14] = (u8)(resp_t200 / 100);

        size_t total_len = addr_len + 15;
        size_t send_len = total_len;

        if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
            u16 fcs = crc16_fcs(resp, total_len);
            resp[total_len] = fcs & 0xFF;
            resp[total_len + 1] = (fcs >> 8) & 0xFF;
            send_len += 2;
        }

        lapf->xid_tx++;
        if (port->ops && port->ops->send) {
            port->ops->send(port, resp, send_len);
        }
    } else {
        /* XID Response: just apply peer parameters if within limits */
        lapf->k = (peer_k < lapf->k) ? peer_k : lapf->k;
        lapf->n201 = (peer_n201_tx < lapf->n201) ? peer_n201_tx : lapf->n201;
        if (peer_t200) lapf->t200 = peer_t200;

        LOG_INFO("LAPF on %s (DLCI %u): Applied XID Response parameters: k=%u, n201=%u, t200=%ums",
                 port->name, lapf->dlci, lapf->k, lapf->n201, lapf->t200);
    }
}

/* Handle incoming LAPF frame (excl. Q.922 address) */
int lapf_handle_frame(vfr_port_t *port, const u8 *frame, size_t len) {
    if (!port || len == 0) return -1;

    mutex_lock(&port->mutex);

    int sent_supervisory = 0;  /* Set when any S-frame is sent during this frame's processing */

    /* Decode C/R bit from the first byte of Q.922 Address of incoming frame */
    int is_response = (frame[0] & 0x02) >> 1; /* C/R is in bit 2 of octet 1 */
    
    /* Determine Address Length using bounded helper (F8.14) */
    size_t addr_len = fr_get_addr_len(frame, len);
    if (addr_len == 0 || len < addr_len) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Verify DLCI context exists for this port */
    u32 dlci = fr_decode_dlci(frame);
    vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)port_get_lapf_ctx(port, dlci);
    if (!lapf) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    const u8 *payload = frame + addr_len;
    size_t payload_len = len - addr_len;
    if (payload_len == 0) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    u8 ctrl1 = payload[0];
    u8 ctrl2 = (payload_len > 1) ? payload[1] : 0;
    int pf = 0;
    u8 ctrl_type = 0xFF;

    if ((ctrl1 & 0x01) == 0) {
        /* I-frame (modulo 128) */
        ctrl_type = LAPF_CTRL_I;
        if (payload_len < 2) {
            lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_N);
            mutex_unlock(&port->mutex);
            return -1;
        }
        pf = ctrl2 & 0x01;
    } else if ((ctrl1 & 0x03) == 0x01) {
        /* S-frame (modulo 128) */
        ctrl_type = ctrl1;
        if (payload_len < 2) {
            LOG_ERROR("LAPF on %s: S-frame payload length %zu < 2", port->name, payload_len);
            lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_N);
            if (lapf->state == LAPF_STATE_ESTABLISHED || lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                lapf_send_frmr(port, lapf, ctrl1, 0, is_response, LAPF_FRMR_X | LAPF_FRMR_W);
                lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                lapf->retransmission_count = 0;
                lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                timer_set(&lapf->t200_timer, lapf->t200);
                timer_cancel(&lapf->t203_timer);
            }
            mutex_unlock(&port->mutex);
            return -1;
        }
        pf = ctrl2 & 0x01;
    } else if ((ctrl1 & 0x03) == 0x03) {
        /* U-frame */
        ctrl_type = ctrl1 & ~0x10; /* Mask out P/F bit */
        pf = (ctrl1 & 0x10) >> 4;
        if (ctrl_type != LAPF_CTRL_U_UI && ctrl_type != LAPF_CTRL_U_XID && ctrl_type != LAPF_CTRL_U_FRMR) {
            if (payload_len != 1) {
                LOG_ERROR("LAPF on %s: U-frame type 0x%02X payload length %zu != 1",
                          port->name, ctrl_type, payload_len);
                lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_N);
                if (lapf->state == LAPF_STATE_ESTABLISHED || lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                    lapf_send_frmr(port, lapf, ctrl1, 0, is_response, LAPF_FRMR_X | LAPF_FRMR_W);
                    lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                    lapf->retransmission_count = 0;
                    lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                    timer_set(&lapf->t200_timer, lapf->t200);
                    timer_cancel(&lapf->t203_timer);
                }
                mutex_unlock(&port->mutex);
                return -1;
            }
        }
    }

    /* Track statistics */
    if (ctrl_type == LAPF_CTRL_I) lapf->i_rx++;
    else if (ctrl_type == LAPF_CTRL_S_RR) lapf->rr_rx++;
    else if (ctrl_type == LAPF_CTRL_S_RNR) lapf->rnr_rx++;
    else if (ctrl_type == LAPF_CTRL_S_REJ) lapf->rej_rx++;
    else if (ctrl_type == LAPF_CTRL_U_SABME) lapf->sabme_rx++;
    else if (ctrl_type == LAPF_CTRL_U_DISC) lapf->disc_rx++;
    else if (ctrl_type == LAPF_CTRL_U_UA) lapf->ua_rx++;
    else if (ctrl_type == LAPF_CTRL_U_DM) lapf->dm_rx++;
    else if (ctrl_type == LAPF_CTRL_U_FRMR) lapf->frmr_rx++;
    else if (ctrl_type == LAPF_CTRL_U_UI) lapf->ui_rx++;

    LOG_DEBUG("LAPF on %s: Recv frame type=0x%02X, state=%d, pf=%d, resp=%d",
              port->name, ctrl_type, lapf->state, pf, is_response);

    /* Check for undefined/non-implemented control field — Q.922 §5.8.5 / §3.6.1 */
    if (ctrl_type == 0xFF && (lapf->state == LAPF_STATE_ESTABLISHED || lapf->state == LAPF_STATE_TIMER_RECOVERY)) {
        lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_L);
        lapf_send_frmr(port, lapf, ctrl1, ctrl2, is_response, LAPF_FRMR_W);
        lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
        lapf->retransmission_count = 0;
        lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
        timer_set(&lapf->t200_timer, lapf->t200);
        timer_cancel(&lapf->t203_timer);
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Process U-frames (affects all states) */
    if (ctrl_type == LAPF_CTRL_U_SABME) {
        LOG_INFO("LAPF on %s: Received SABME command", port->name);

        /* Q.922 Table V-1 Error F: peer-initiated re-establishment in states 7/8 */
        if (lapf->state == LAPF_STATE_ESTABLISHED || lapf->state == LAPF_STATE_TIMER_RECOVERY) {
            lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_F);
        }

        lapf->v_s = 0;
        lapf->v_r = 0;
        lapf->v_a = 0;
        lapf->peer_receiver_busy = 0;
        lapf->own_receiver_busy = 0;
        lapf->acknowledgement_pending = 0;
        lapf->rej_exception = 0;
        lapf->backlog_head = 0;
        lapf->backlog_tail = 0;

        lapf->state = LAPF_STATE_ESTABLISHED;
        lapf_send_control(port, lapf, LAPF_CTRL_U_UA, 0, pf, 1); /* UA Response */
        timer_cancel(&lapf->t200_timer);
        timer_set(&lapf->t203_timer, lapf->t203);
        
        LOG_INFO("LAPF on %s (DLCI %u): Link established (state=ESTABLISHED, T203 started)", port->name, lapf->dlci);
        mutex_unlock(&port->mutex);
        return 0;
    }

    if (ctrl_type == LAPF_CTRL_U_DISC) {
        LOG_INFO("LAPF on %s (DLCI %u): Received DISC command", port->name, lapf->dlci);
        if (lapf->state == LAPF_STATE_TEI_ASSIGNED) {
            lapf_send_control(port, lapf, LAPF_CTRL_U_DM, 0, pf, 1); /* DM Response */
        } else {
            lapf->state = LAPF_STATE_TEI_ASSIGNED;
            lapf_send_control(port, lapf, LAPF_CTRL_U_UA, 0, pf, 1); /* UA Response */
        }
        timer_cancel(&lapf->t200_timer);
        timer_cancel(&lapf->t203_timer);
        
        LOG_INFO("LAPF on %s: Link disconnected (state=TEI_ASSIGNED)", port->name);
        mutex_unlock(&port->mutex);
        return 0;
    }

    /* State-specific processing */
    switch (lapf->state) {
        case LAPF_STATE_TEI_ASSIGNED:
            if (ctrl_type == LAPF_CTRL_U_UA) {
                /* Table 5/Q.922: UA in TEI_ASSIGNED */
                if (pf) {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_C);  /* UA F=1 → MDL-ERROR(C) */
                } else {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_D);  /* UA F=0 → MDL-ERROR(D) */
                }
            } else if (ctrl_type == LAPF_CTRL_U_DM) {
                if (pf) {
                    /* DM F=1 in TEI_ASSIGNED → Ignore (Table 5/Q.922) */
                    LOG_DEBUG("LAPF on %s (DLCI %u): DM F=1 in TEI_ASSIGNED — ignored", port->name, lapf->dlci);
                } else {
                    /* DM F=0 in TEI_ASSIGNED → Establish (Table 5/Q.922) — Fix F-06 */
                    LOG_INFO("LAPF on %s (DLCI %u): DM F=0 in TEI_ASSIGNED — initiating establishment", port->name, lapf->dlci);
                    lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                    lapf->retransmission_count = 0;
                    lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                    timer_set(&lapf->t200_timer, lapf->t200);
                    timer_cancel(&lapf->t203_timer);
                }
            } else {
                /* Send DM response for any command received when disconnected */
                if (!is_response) {
                    lapf_send_control(port, lapf, LAPF_CTRL_U_DM, 0, pf, 1);
                }
            }
            break;

        case LAPF_STATE_AWAITING_ESTABLISHMENT:
            if (ctrl_type == LAPF_CTRL_U_UA && pf) {
                LOG_INFO("LAPF on %s (DLCI %u): Received UA response, link established", port->name, lapf->dlci);
                lapf->state = LAPF_STATE_ESTABLISHED;
                lapf->v_s = 0;
                lapf->v_r = 0;
                lapf->v_a = 0;
                lapf->rej_exception = 0;
                timer_cancel(&lapf->t200_timer);
                timer_set(&lapf->t203_timer, lapf->t203);
                lapf_process_backlog(port, lapf);
            } else if (ctrl_type == LAPF_CTRL_U_UA && !pf) {
                /* Unsolicited UA F=0 in state 5 → MDL-ERROR(D) */
                lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_D);
            } else if (ctrl_type == LAPF_CTRL_U_DM && pf) {
                LOG_WARN("LAPF on %s (DLCI %u): Received DM response, link establishment refused", port->name, lapf->dlci);
                lapf->state = LAPF_STATE_TEI_ASSIGNED;
                timer_cancel(&lapf->t200_timer);
            }
            break;

        case LAPF_STATE_AWAITING_RELEASE:
            if (ctrl_type == LAPF_CTRL_U_UA && pf) {
                LOG_INFO("LAPF on %s: Received UA response, link released", port->name);
                lapf->state = LAPF_STATE_TEI_ASSIGNED;
                timer_cancel(&lapf->t200_timer);
            } else if (ctrl_type == LAPF_CTRL_U_UA && !pf) {
                /* Unsolicited UA F=0 in state 6 → MDL-ERROR(D) */
                lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_D);
            } else if (ctrl_type == LAPF_CTRL_U_DM && pf) {
                LOG_INFO("LAPF on %s: Received DM response, link released", port->name);
                lapf->state = LAPF_STATE_TEI_ASSIGNED;
                timer_cancel(&lapf->t200_timer);
            }
            break;

        case LAPF_STATE_ESTABLISHED:
        case LAPF_STATE_TIMER_RECOVERY:
            if (ctrl_type == LAPF_CTRL_U_UA) {
                /* Table 5/Q.922: Unsolicited UA in ESTABLISHED/TIMER_RECOVERY */
                if (pf) {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_C);  /* UA F=1 → MDL-ERROR(C) */
                } else {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_D);  /* UA F=0 → MDL-ERROR(D) */
                }
                /* Per Table 5: UA in ESTABLISHED/TIMER_RECOVERY is logged but does NOT
                 * cause state change or disconnection. */
                break;
            }

            if (ctrl_type == LAPF_CTRL_U_DM) {
                if (pf) {
                    /* DM F=1 in ESTABLISHED → MDL-ERROR(B) only, no re-establish
                     * DM F=1 in TIMER_RECOVERY → Re-establish + MDL-ERROR(B) */
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_B);
                    if (lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                        lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                        lapf->retransmission_count = 0;
                        lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                        timer_set(&lapf->t200_timer, lapf->t200);
                        timer_cancel(&lapf->t203_timer);
                    }
                } else {
                    /* DM F=0 in ESTABLISHED or TIMER_RECOVERY → Re-establish + MDL-ERROR(E) */
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_E);
                    lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                    lapf->retransmission_count = 0;
                    lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                    timer_set(&lapf->t200_timer, lapf->t200);
                    timer_cancel(&lapf->t203_timer);
                }
                break;
            }

            /* UI Frame detection */
            if (ctrl_type == LAPF_CTRL_U_UI) {
                if (payload_len > 1 && payload[1] == 0x08) {
                    lapf_l3_recv_cb(port, payload + 1, payload_len - 1);
                } else {
                    LOG_INFO("LAPF on %s (DLCI %u): Received UI frame (ignored)", port->name, lapf->dlci);
                }
                break;
            }
            
            /* XID Frame detection */
            if (ctrl_type == LAPF_CTRL_U_XID) {
                lapf_handle_xid(port, lapf, payload + 1, payload_len - 1, pf, is_response);
                break;
            }

            /* FRMR Frame detection */
            if (ctrl_type == LAPF_CTRL_U_FRMR) {
                LOG_WARN("LAPF on %s (DLCI %u): Received FRMR response, resetting link", port->name, lapf->dlci);
                lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_K);
                lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                lapf->retransmission_count = 0;
                lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                timer_set(&lapf->t200_timer, lapf->t200);
                timer_cancel(&lapf->t203_timer);
                break;
            }

            /* Guard: if this is a U-frame, it was already handled above; do not fall through (Issue #4.12) */
            if ((ctrl1 & 0x03) == 0x03) {
                break;
            }
 
            /* Supervisory and Information frame processing */
            u8 nr = ctrl2 >> 1;
            
            bool nr_ok = lapf_seq_in_range(nr, lapf->v_a, lapf->v_s);
            if (!nr_ok) {
                LOG_ERROR("LAPF on %s (DLCI %u): Received invalid N(R)=%u (V(A)=%u, V(S)=%u)", port->name, lapf->dlci, nr, lapf->v_a, lapf->v_s);
                lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_J);
                lapf_send_frmr(port, lapf, ctrl1, ctrl2, is_response, LAPF_FRMR_Z);
                /* Reset link */
                lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                lapf->retransmission_count = 0;
                lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                timer_set(&lapf->t200_timer, lapf->t200);
                timer_cancel(&lapf->t203_timer);
                break;
            }

            /* Acknowledge frames up to N(R) - 1 */
            if (nr != lapf->v_a) {
                LOG_DEBUG("LAPF on %s (DLCI %u): Received N(R)=%u, acknowledging from V(A)=%u", port->name, lapf->dlci, nr, lapf->v_a);
                lapf->v_a = nr;
                
                /* Process pending backlog items as window opened */
                lapf_process_backlog(port, lapf);

                if (lapf->v_a == lapf->v_s) {
                    timer_cancel(&lapf->t200_timer);
                    timer_set(&lapf->t203_timer, lapf->t203);
                } else {
                    timer_set(&lapf->t200_timer, lapf->t200); /* restart T200 */
                }
            }

            /* Process Supervisory frames */
            if (ctrl_type == LAPF_CTRL_S_RR) {
                lapf->peer_receiver_busy = 0;
                if (pf && is_response && lapf->state == LAPF_STATE_ESTABLISHED) {
                    /* Table 5/Q.922: Unsolicited supervisory F=1 in ESTABLISHED → MDL-ERROR(A) */
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_A);
                } else if (pf && is_response && lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                    lapf->state = LAPF_STATE_ESTABLISHED;
                    timer_cancel(&lapf->t200_timer);
                    timer_set(&lapf->t203_timer, lapf->t203);
                    LOG_DEBUG("LAPF on %s (DLCI %u): Received RR Response with F=1, exited Timer Recovery", port->name, lapf->dlci);
                } else if (pf && !is_response) {
                    /* Command: Send RR response with F=1 */
                    lapf_send_control(port, lapf, LAPF_CTRL_S_RR, lapf->v_r, 1, 1);
                    sent_supervisory = 1;
                }
            } else if (ctrl_type == LAPF_CTRL_S_RNR) {
                lapf->peer_receiver_busy = 1;
                if (pf && is_response && lapf->state == LAPF_STATE_ESTABLISHED) {
                    /* Table 5/Q.922: Unsolicited supervisory F=1 in ESTABLISHED → MDL-ERROR(A) */
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_A);
                } else if (pf && is_response && lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                    lapf->state = LAPF_STATE_ESTABLISHED;
                    timer_cancel(&lapf->t200_timer);
                    timer_set(&lapf->t203_timer, lapf->t203);
                    LOG_DEBUG("LAPF on %s (DLCI %u): Received RNR Response with F=1, exited Timer Recovery", port->name, lapf->dlci);
                } else if (pf && !is_response) {
                    /* Command: Send RR response with F=1 */
                    lapf_send_control(port, lapf, LAPF_CTRL_S_RR, lapf->v_r, 1, 1);
                    sent_supervisory = 1;
                }
            } else if (ctrl_type == LAPF_CTRL_S_REJ) {
                lapf->peer_receiver_busy = 0;
                if (pf && is_response && lapf->state == LAPF_STATE_ESTABLISHED) {
                    /* Table 5/Q.922: Unsolicited supervisory F=1 in ESTABLISHED → MDL-ERROR(A) */
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_A);
                } else if (pf && is_response && lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                    lapf->state = LAPF_STATE_ESTABLISHED;
                    timer_cancel(&lapf->t200_timer);
                    timer_set(&lapf->t203_timer, lapf->t203);
                    LOG_DEBUG("LAPF on %s (DLCI %u): Received REJ Response with F=1, exited Timer Recovery", port->name, lapf->dlci);
                }

                /* Retransmit outstanding frames starting from N(R) */
                LOG_INFO("LAPF on %s (DLCI %u): Received REJ frame, retransmitting from N(R)=%u to V(S)=%u", port->name, lapf->dlci, nr, lapf->v_s);
                u8 tx_seq = nr;
                while (tx_seq != lapf->v_s) {
                    lapf_send_i_frame(port, lapf, tx_seq, lapf->v_r, 0, 0, lapf->tx_window[tx_seq].data, lapf->tx_window[tx_seq].len);
                    tx_seq = (tx_seq + 1) % 128;
                }
                timer_set(&lapf->t200_timer, lapf->t200);
                timer_cancel(&lapf->t203_timer);
            }

            /* Process Information (I) frames */
            if (ctrl_type == LAPF_CTRL_I) {
                u8 ns = ctrl1 >> 1;
                size_t info_len = payload_len - 2;
                if (info_len > lapf->n201) {
                    LOG_ERROR("LAPF on %s (DLCI %u): Received I-frame info field size %zu exceeds N201 %u, resetting link",
                              port->name, lapf->dlci, info_len, lapf->n201);
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_O);
                    lapf_send_frmr(port, lapf, ctrl1, ctrl2, is_response, LAPF_FRMR_Y);
                    lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                    lapf->retransmission_count = 0;
                    lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                    timer_set(&lapf->t200_timer, lapf->t200);
                    timer_cancel(&lapf->t203_timer);
                    break;
                }
                if (ns == lapf->v_r) {
                    LOG_DEBUG("LAPF on %s (DLCI %u): Received expected I-frame N(S)=%u", port->name, lapf->dlci, ns);
                    lapf->v_r = (lapf->v_r + 1) % 128;
                    lapf->rej_exception = 0;
                    
                    /* Pass Layer 3 payload to callback if PD is 0x08 */
                    if (payload_len > 2) {
                        if (payload[2] == 0x08) {
                            lapf_l3_recv_cb(port, payload + 2, payload_len - 2);
                        } else {
                            LOG_WARN("LAPF on %s (DLCI %u): I-frame has invalid Protocol Discriminator 0x%02X (expected 0x08) — dropped",
                                     port->name, lapf->dlci, payload[2]);
                        }
                    }

                    if (pf && !is_response) {
                        /* Command with P=1: acknowledge immediately with F=1 */
                        lapf_send_control(port, lapf, LAPF_CTRL_S_RR, lapf->v_r, 1, 1);
                        lapf->acknowledgement_pending = 0;
                        sent_supervisory = 1;
                    } else if (!pf && !is_response) {
                        /* Command with P=0: pending ack (send delayed RR or piggyback) */
                        lapf->acknowledgement_pending = 1;
                    }

                    /* Q.922 §5.6.2.4: Handle F-bit on I-frame responses */
                    if (is_response && pf) {
                        if (lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                            /* F=1 response in Timer Recovery: exit recovery */
                            timer_cancel(&lapf->t200_timer);
                            timer_set(&lapf->t203_timer, lapf->t203);
                            lapf->v_s = nr;
                            lapf->peer_receiver_busy = 0;
                            lapf->state = LAPF_STATE_ESTABLISHED;
                            LOG_DEBUG("LAPF on %s (DLCI %u): I-frame response F=1 exited Timer Recovery, V(S)=%u", port->name, lapf->dlci, nr);

                            /* §5.6.2.4 item 2b: If I frame available and not own-receiver-busy,
                             * send I-frame response with F=0 instead of RR */
                            if (!lapf->own_receiver_busy && lapf->backlog_head != lapf->backlog_tail) {
                                lapf_tx_frame_t *f = &lapf->tx_backlog[lapf->backlog_head];
                                u8 bns = lapf->v_s;
                                memcpy(lapf->tx_window[bns].data, f->data, f->len);
                                lapf->tx_window[bns].len = f->len;
                                lapf_send_i_frame(port, lapf, bns, lapf->v_r, 0, 0, f->data, f->len);
                                lapf->v_s = (lapf->v_s + 1) % 128;
                                lapf->backlog_head = (lapf->backlog_head + 1) % 64;
                                timer_set(&lapf->t200_timer, lapf->t200);
                                timer_cancel(&lapf->t203_timer);
                                sent_supervisory = 1;  /* Suppress deferred ack */
                            }
                        } else if (lapf->state == LAPF_STATE_ESTABLISHED) {
                            /* Unsolicited F=1 on I-response in ESTABLISHED: MDL-ERROR */
                            lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_A);
                        }
                    }
                } else {
                    /* Out of sequence frame */
                    if (!lapf->rej_exception) {
                        LOG_WARN("LAPF on %s (DLCI %u): Out of sequence N(S)=%u received (expected %u), sending REJ", port->name, lapf->dlci, ns, lapf->v_r);
                        /* Send REJ matching the C/R context of the triggering frame:
                         * If received I-frame was a command → send REJ as response
                         * If received I-frame was a response → send REJ as response F=0
                         * Per Q.921 §5.8.1 / Q.922 §5.8.1 */
                        lapf_send_control(port, lapf, LAPF_CTRL_S_REJ, lapf->v_r,
                                          is_response ? 0 : pf, 1);
                        lapf->rej_exception = 1;
                        sent_supervisory = 1;
                    }
                    lapf->acknowledgement_pending = 0;
                }
            }
            break;
    }

    /* Reset T203 timer if T200 is not running and link is in ESTABLISHED state */
    if (lapf->state == LAPF_STATE_ESTABLISHED && lapf->t200_timer.expire == 0) {
        timer_set(&lapf->t203_timer, lapf->t203);
    }

    /* Send delayed acknowledgement if pending and no other supervisory frame was sent */
    if (lapf->acknowledgement_pending && !sent_supervisory &&
        (lapf->state == LAPF_STATE_ESTABLISHED || lapf->state == LAPF_STATE_TIMER_RECOVERY)) {
        lapf_send_control(port, lapf, LAPF_CTRL_S_RR, lapf->v_r, 0, 1); /* RR Response F=0 */
        lapf->acknowledgement_pending = 0;
    }

    mutex_unlock(&port->mutex);
    return 0;
}

/* Process LAPF timer ticks (called periodically from main loop) */
void lapf_poll_timer(vfr_port_t *port) {
    if (!port) return;

    mutex_lock(&port->mutex);
    for (int idx = 0; idx < port->lapf_ctx_count; idx++) {
        vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)port->lapf_ctxs[idx];
        if (!lapf) continue;

        /* Check T200 (Retransmission) Timer */
        if (timer_is_expired(&lapf->t200_timer)) {
            lapf->t200_expires++;
            lapf->retransmission_count++;

            LOG_WARN("LAPF on %s (DLCI %u): T200 expired (retransmission count: %u/%u) in state %d",
                     port->name, lapf->dlci, lapf->retransmission_count, lapf->n200, lapf->state);

            if (lapf->retransmission_count >= lapf->n200) {
                LOG_ERROR("LAPF on %s (DLCI %u): Retransmission limit reached, resetting link", port->name, lapf->dlci);
                if (lapf->state == LAPF_STATE_AWAITING_ESTABLISHMENT) {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_G);
                } else if (lapf->state == LAPF_STATE_AWAITING_RELEASE) {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_H);
                } else if (lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_I);
                }
                lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                lapf->retransmission_count = 0;
                lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0); /* SABME P=1 */
                timer_set(&lapf->t200_timer, lapf->t200);
                timer_cancel(&lapf->t203_timer);
            } else {
                /* Retransmit depending on state */
                if (lapf->state == LAPF_STATE_AWAITING_ESTABLISHMENT) {
                    lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0); /* SABME P=1 */
                    timer_set(&lapf->t200_timer, lapf->t200);
                } else if (lapf->state == LAPF_STATE_AWAITING_RELEASE) {
                    lapf_send_control(port, lapf, LAPF_CTRL_U_DISC, 0, 1, 0); /* DISC P=1 */
                    timer_set(&lapf->t200_timer, lapf->t200);
                } else {
                    /* Established / Timer Recovery */
                    if (lapf->state == LAPF_STATE_ESTABLISHED) {
                        lapf->state = LAPF_STATE_TIMER_RECOVERY;
                        lapf->retransmission_count = 0;
                    }
                    
                    /* Retransmit oldest unacknowledged I-frame (V(A)) with P=1?
                     * Standard Q.921 says send RR command with P=1 to poll status */
                    lapf_send_control(port, lapf, LAPF_CTRL_S_RR, lapf->v_r, 1, 0); /* RR P=1 Command */
                    timer_set(&lapf->t200_timer, lapf->t200);
                }
            }
        }

        /* Check T203 (Idle) Timer */
        if (timer_is_expired(&lapf->t203_timer)) {
            lapf->t203_expires++;
            LOG_INFO("LAPF on %s (DLCI %u): T203 expired (link idle), polling peer", port->name, lapf->dlci);
            
            /* Send RR command with P=1 to poll peer */
            lapf->state = LAPF_STATE_TIMER_RECOVERY;
            lapf->retransmission_count = 0;
            lapf_send_control(port, lapf, LAPF_CTRL_S_RR, lapf->v_r, 1, 0); /* RR P=1 Command */
            timer_set(&lapf->t200_timer, lapf->t200);
            timer_cancel(&lapf->t203_timer);
        }
    }

    mutex_unlock(&port->mutex);
}
