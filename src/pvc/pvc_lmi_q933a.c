/*
 * pvc_lmi_q933a.c - ITU-T Q.933 Annex A / X.36 LMI Stack
 * VFRS - Virtual Frame Relay Switch
 */

#include "vfr.h"
#include <string.h>

#define Q933A_PD         0x08
#define Q933A_STATUS_ENQ 0x75
#define Q933A_STATUS     0x7D

/* ============================================================
 * Q.933 Annex A IE Builders & Parsers
 * ============================================================ */

/* Build Report Type IE - Q.933A format (3 octets) */
size_t lmi_q933a_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type)
{
    if (max_len < 3) return 0;
    buf[0] = LMI_IE_REPORT_TYPE_Q933A;
    buf[1] = 0x01;  /* Length of report type contents */
    buf[2] = report_type;
    return 3;
}

/* Build Link Integrity Verification IE - Q.933A format */
size_t lmi_q933a_build_link_integrity_ie(u8 *buf, size_t max_len, u8 send_seq, u8 recv_seq)
{
    if (max_len < 4) return 0;
    buf[0] = LMI_IE_LINK_INTEGRITY_Q933A;
    buf[1] = 0x02;  /* Length */
    buf[2] = send_seq;
    buf[3] = recv_seq;
    return 4;
}

/* Build PVC Status IE - Q.933A format (supports 2-byte and 4-byte DLCI formats) */
size_t lmi_q933a_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len)
{
    if (dlci_len == 4 || dlci > 1023) {
        if (max_len < 7) return 0;
        buf[0] = LMI_IE_PVC_STATUS_Q933A;
        buf[1] = 0x05;  /* Length for 4-byte DLCI */
        /* Encode DLCI in 23-bit format (X.36 §11.4 / Q.933A §A.4.3) */
        buf[2] = (u8)((dlci >> 17) & 0x3F);          /* DLCI[22:17] */
        buf[3] = (u8)(((dlci >> 13) & 0x0F) << 3);   /* DLCI[16:13] */
        buf[4] = (u8)((dlci >> 6) & 0x7F);           /* DLCI[12:6] */
        buf[5] = (u8)(((dlci & 0x3F) << 1) | 0x80);  /* DLCI[5:0] + ext=1 */
        buf[6] = status | LMI_PVC_EXT;
        return 7;
    } else {
        if (max_len < 5) return 0;
        buf[0] = LMI_IE_PVC_STATUS_Q933A;
        buf[1] = 0x03;  /* Length for 2-byte DLCI */
        buf[2] = (u8)((dlci >> 4) & 0x3F);          /* DLCI[9:4] */
        buf[3] = (u8)(((dlci & 0x0F) << 3) | 0x80);  /* DLCI[3:0] + ext=1 */
        buf[4] = status | LMI_PVC_EXT;
        return 5;
    }
}

/* Parse PVC Status IE - Q.933A format */
int lmi_q933a_parse_pvc_status_ie(const u8 *buf, size_t len, u32 *dlci, u8 *status)
{
    if (!buf || len < 2) return -1;
    if (buf[0] != LMI_IE_PVC_STATUS_Q933A) return -1;
    
    u8 ie_len = buf[1];
    if (len < (size_t)(ie_len + 2)) return -1;

    if (ie_len == 3) {
        *dlci = (((u32)(buf[2] & 0x3F)) << 4) | ((buf[3] & 0x78) >> 3);
        *status = buf[4];
        return 0;
    } else if (ie_len == 5) {
        *dlci = (((u32)(buf[2] & 0x3F)) << 17) |
                (((u32)(buf[3] & 0x78) >> 3) << 13) |
                (((u32)(buf[4] & 0x7F)) << 6) |
                (((u32)(buf[5] & 0x7E) >> 1));
        *status = buf[6];
        return 0;
    }
    return -1;
}

/* ============================================================
 * Q.933 Annex A LMI Stack Functions
 * ============================================================ */

/* Build Q.933 Annex A STATUS ENQUIRY */
size_t lmi_q933a_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len)
{
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    size_t pos = 0;
    u8 report_type;

    pos += lmi_build_header(port, buf, max_len, Q933A_PD);
    if (pos + 1 > max_len) return 0;
    buf[pos++] = Q933A_STATUS_ENQ;

    if (lmi->last_report_type == 0x04) {
        report_type = 0x04;
    } else {
        report_type = (lmi->polls_since_full >= lmi->n391) ?
                      LMI_REPORT_FULL_STATUS : LMI_REPORT_LINK_INTEGRITY;
    }
    pos += lmi_q933a_build_report_type_ie(buf + pos, max_len - pos, report_type);

    if (max_len - pos >= 4) {
        pos += lmi_q933a_build_link_integrity_ie(buf + pos, max_len - pos,
                                                 lmi->dte_seq_send, lmi->dte_dce_seq_recv);
    }


    return pos;
}

typedef struct {
    u32 dlci;
    u8 status;
    u8 is_new;
    u8 *new_flag_ptr;
    u8 *new_txsn_ptr;
} lmi_status_item_t;

static int compare_status_items(const void *a, const void *b) {
    u32 dlci_a = ((const lmi_status_item_t *)a)->dlci;
    u32 dlci_b = ((const lmi_status_item_t *)b)->dlci;
    return (dlci_a > dlci_b) - (dlci_a < dlci_b);
}

/* Build Q.933 Annex A STATUS response. */
size_t lmi_q933a_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status)
{
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    vfrs_ctx_t *ctx = g_vfrs;
    size_t pos = 0;
    int i;

    if (lmi) {
        lmi->dce_last_was_full = full_status;
    }

    pos += lmi_build_header(port, buf, max_len, Q933A_PD);
    if (pos + 1 > max_len) return 0;
    buf[pos++] = Q933A_STATUS;

    size_t report_type_pos = pos;
    pos += lmi_q933a_build_report_type_ie(buf + pos, max_len - pos,
                                          full_status ? LMI_REPORT_FULL_STATUS :
                                                        LMI_REPORT_LINK_INTEGRITY);

    /* Link integrity IE — DCE increments its send seq before placing it (§11.4.1.2 step 3) */
    if (lmi && max_len - pos >= 4) {
        lmi->dce_seq_send = lmi_inc_seq(lmi->dce_seq_send);
        pos += lmi_q933a_build_link_integrity_ie(buf + pos, max_len - pos,
                                                 lmi->dce_seq_send, lmi->dte_seq_recv);
    }

    /* PVC Status IEs (X.36 Annex G / X.36 §11.4) */
    if (full_status && ctx) {
        lmi_status_item_t *sorted_items = (lmi_status_item_t *)calloc(MAX_PVCS, sizeof(lmi_status_item_t));
        int num_items = 0;
        int dlci_len = lmi_port_dlci_len(port);

        if (sorted_items) {
            /* Collect PVCs and Multicast Endpoints under locks in correct order (pvc_mutex -> mcast_mutex) */
            ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
            mutex_lock(&ctx->pvc_mutex);
            for (i = 0; i < PVC_HASH_SIZE; i++) {
                vfr_pvc_detail_t *pvc = ctx->pvc_table[i];
                while (pvc) {
                    if (strcmp(pvc->port_in, port->name) == 0) {
                        if (num_items < MAX_PVCS) {
                            vfr_dlci_entry_t *entry = port_lookup_dlci(port, pvc->dlci_in);
                            sorted_items[num_items].dlci = pvc->dlci_in;
                            sorted_items[num_items].status = (entry && vfrs_dlci_is_active_unlocked(ctx, port, entry)) ? LMI_PVC_ACTIVE : 0;
                            sorted_items[num_items].is_new = pvc->lmi_new;
                            sorted_items[num_items].new_flag_ptr = &pvc->lmi_new;
                            sorted_items[num_items].new_txsn_ptr = &pvc->lmi_new_txsn;
                            num_items++;
                        }
                    }
                    pvc = pvc->next;
                }
            }

            /* Collect Multicast Endpoints */
            ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
            mutex_lock(&ctx->mcast_mutex);
            vfr_mcast_group_t *g = ctx->mcast_groups;
            while (g) {
                if (strcmp(g->source_port, port->name) == 0) {
                    if (num_items < MAX_PVCS) {
                        sorted_items[num_items].dlci = g->source_dlci;
                        sorted_items[num_items].status = vfrs_mcast_endpoint_is_active_unlocked(ctx, port->name, g->source_dlci) ? LMI_PVC_ACTIVE : 0;
                        sorted_items[num_items].is_new = g->root_lmi_new;
                        sorted_items[num_items].new_flag_ptr = &g->root_lmi_new;
                        sorted_items[num_items].new_txsn_ptr = &g->root_lmi_new_txsn;
                        num_items++;
                    }
                }
                vfr_mcast_member_t *m = g->members;
                while (m) {
                    if (strcmp(m->port_name, port->name) == 0) {
                        if (!vfrs_mcast_member_has_pvc_unlocked(ctx, g, m)) {
                            if (num_items < MAX_PVCS) {
                                sorted_items[num_items].dlci = m->dlci;
                                sorted_items[num_items].status = vfrs_mcast_endpoint_is_active_unlocked(ctx, port->name, m->dlci) ? LMI_PVC_ACTIVE : 0;
                                sorted_items[num_items].is_new = m->lmi_new;
                                sorted_items[num_items].new_flag_ptr = &m->lmi_new;
                                sorted_items[num_items].new_txsn_ptr = &m->lmi_new_txsn;
                                num_items++;
                            }
                        }
                    }
                    m = m->next;
                }
                g = g->next;
            }
            mutex_unlock(&ctx->mcast_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
            mutex_unlock(&ctx->pvc_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);

            /* Sort items by DLCI ascending */
            qsort(sorted_items, num_items, sizeof(lmi_status_item_t), compare_status_items);

            /* D-4: Use max_len as the effective MTU, reserving 2 bytes for FCS. */
            size_t lmi_mtu = (max_len > 2) ? (max_len - 2) : max_len;

            int start_idx = lmi->segment_active ? lmi->pvc_send_index : 0;
            int sent_count = 0;
            int has_more = 0;

            for (i = start_idx; i < num_items; i++) {
                lmi_status_item_t *item = &sorted_items[i];

                /* Build status byte: Active (A) bit & New (N) bit */
                u8 status = item->status;
                if (item->is_new) {
                    status |= LMI_PVC_NEW;
                    /* Track sequence number */
                    if (item->new_txsn_ptr) {
                        *item->new_txsn_ptr = lmi->dce_seq_send;
                    }
                }

                /* 23-bit DLCI (>1023) requires 7 bytes; 10-bit DLCI requires 5 bytes */
                size_t ie_sz = (dlci_len == 4 || item->dlci > 1023) ? 7 : 5;

                /* Check if we would overflow the MTU limit */
                if (pos + ie_sz > lmi_mtu) {
                    has_more = 1;
                    lmi->pvc_send_index = i;
                    lmi->segment_active = 1;
                    break;
                }

                pos += lmi_q933a_build_pvc_status_ie(buf + pos, max_len - pos,
                                                     item->dlci, status, dlci_len);
                sent_count++;
            }

            /* Set report type to Full Status Continued (0x04) if there are remaining segments */
            if (has_more) {
                /* Report Type value is stored at report_type_pos + 2 (IE ID at report_type_pos, length at report_type_pos + 1) */
                if (report_type_pos + 2 < pos && buf[report_type_pos] == LMI_IE_REPORT_TYPE_Q933A) {
                    buf[report_type_pos + 2] = 0x04;  /* Full Status Continued */
                }
                LOG_DEBUG("LMI Segmented STATUS sent on %s: sent %d PVCs, resuming at %u",
                          port->name, sent_count, lmi->pvc_send_index);
            } else {
                lmi->segment_active = 0;
                lmi->pvc_send_index = 0;
            }

            free(sorted_items);
        }
    }

    return pos;
}

/* Handle incoming Q.933 Annex A LMI frame. */
int lmi_q933a_handle_frame(vfr_port_t *port, const u8 *frame, size_t len)
{
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    u8 msg_type;
    u8 send_seq = 0, recv_seq = 0;
    int liv_present = 0;
    const u8 *p = frame;
    size_t pos = 0;
    u32 msg_max_dlci = 0;

    if (!lmi || len < 4) return -1;

    if (p[pos++] != Q933A_PD) return -1;
    if (p[pos++] != 0x00) return -1;  /* Dummy Call reference */
    msg_type = p[pos++];

    /* Detect and skip Locking Shift to Codeset 5 byte (0x95) if present (optional fallback) */
    if (pos < len && p[pos] == 0x95) {
        pos++;
    }

    /* Parse IEs */
    while (pos < len - 1) {
        u8 ie_id = p[pos++];
        u8 ie_len = p[pos++];

        switch (ie_id) {
        case 0x51: // Report Type
            if (ie_len >= 1) {
                lmi->last_report_type = p[pos];
            }
            break;
        case 0x53: // Link Integrity Verification
            if (ie_len >= 2) {
                send_seq = p[pos];
                recv_seq = p[pos + 1];
                liv_present = 1;
            }
            break;
        case 0x57: // PVC Status
            {
                u32 dlci_val = 0;
                u8 status_val = 0;
                if (lmi_q933a_parse_pvc_status_ie(p + pos - 2, ie_len + 2, &dlci_val, &status_val) == 0) {
                    if (msg_max_dlci > 0 && dlci_val <= msg_max_dlci) {
                        LOG_WARN("Q.933A on %s: Received out-of-order DLCI %u <= %u in STATUS message (permissive accept per X.36 §11.3.4 / X.76 §11.3.4)",
                                 port->name, dlci_val, msg_max_dlci);
                    }
                    if (dlci_val > msg_max_dlci) {
                        msg_max_dlci = dlci_val;
                    }
                    vfr_dlci_entry_t *entry = port_lookup_dlci(port, dlci_val);
                    if (entry) {
                        entry->active = (status_val & LMI_PVC_ACTIVE) ? 1 : 0;
                        if (entry->vc_type == VC_TYPE_PVC && entry->detail_ptr) {
                            vfr_pvc_detail_t *pvc = (vfr_pvc_detail_t *)entry->detail_ptr;
                            pvc->active = entry->active;
                            pvc->lmi_reported = 1;
                        }
                    }
                }
            }
            break;
        }
        pos += ie_len;
    }

    if (msg_type == Q933A_STATUS_ENQ) {
        lmi->dte_seq_recv = send_seq;
        lmi->dce_seq_recv = recv_seq;
        return 1;
    } else if (msg_type == Q933A_STATUS) {
        /* Asynchronous PVC Status messages (X.36 §11.4.2) do not contain LIV info
         * and must not update sequence numbers or satisfy DTE polling timers. */
        if (lmi->last_report_type == LMI_REPORT_ASYNC_STATUS) {
            return 0;
        }

        if (!liv_present) {
            LOG_WARN("Q.933A DTE on %s: Mandatory Link Integrity Verification IE missing in STATUS - Ignoring", port->name);
            return 0;
        }

        /* X.36 §11.4.1.6.2 Note 2: Link integrity response to a Full Status request must be ignored */
        if (lmi->dte_last_request_was_full && lmi->last_report_type == LMI_REPORT_LINK_INTEGRITY) {
            LOG_WARN("Q.933A DTE on %s: Received Link Integrity response to a Full Status request - Ignoring STATUS", port->name);
            return 0;
        }

        /* D-5: per X.36 §11.4.1.6.2 NOTE 1 — invalid recv_seq means ignore entire STATUS.
         * recv_seq == 0 is only valid as an initialization sentinel when the link is down. */
        int seq_valid = (lmi->dte_link_down) ? (recv_seq == 0 || recv_seq == lmi->dte_seq_send)
                                             : (recv_seq == lmi->dte_seq_send);
        if (seq_valid) {
            /* ITU-T X.36 Appendix III Loopback Detection:
             * Suspect loopback if received send_seq matches our send sequence counter */
            if (send_seq != 0 && send_seq == lmi->dte_seq_send) {
                LOG_WARN("Q.933A LMI on %s: Suspected physical layer loopback condition detected per ITU-T X.36 Appendix III (received send_seq=%u matches tx send_seq)",
                         port->name, send_seq);
            }

            lmi->dte_dce_seq_recv = send_seq;
            lmi->dte_status_received = 1;


            /* Check for omitted PVCs in Full Status or Full Status Continued report */
            if ((lmi->last_report_type == LMI_REPORT_FULL_STATUS || lmi->last_report_type == 0x04) && g_vfrs) {
                mutex_lock(&g_vfrs->pvc_mutex);
                for (int i = 0; i < PVC_HASH_SIZE; i++) {
                    vfr_pvc_detail_t *pvc = g_vfrs->pvc_table[i];
                    while (pvc) {
                        if (strcmp(pvc->port_in, port->name) == 0) {
                            if (!pvc->lmi_reported) {
                                if (lmi->last_report_type == 0x04) {
                                    /* Annex G: omit check only up to msg_max_dlci */
                                    if (pvc->dlci_in <= msg_max_dlci) {
                                        if (pvc->active) {
                                            pvc->active = 0;
                                            vfr_dlci_entry_t *entry = port_lookup_dlci(port, pvc->dlci_in);
                                            if (entry) entry->active = 0;
                                            LOG_INFO("DTE on %s: PVC %u marked inactive (omitted in Q.933A Segmented Full Status)",
                                                     port->name, pvc->dlci_in);
                                        }
                                    }
                                } else {
                                    /* Final Full Status: check all DLCIs */
                                    if (pvc->active) {
                                        pvc->active = 0;
                                        vfr_dlci_entry_t *entry = port_lookup_dlci(port, pvc->dlci_in);
                                        if (entry) entry->active = 0;
                                        LOG_INFO("DTE on %s: PVC %u marked inactive (omitted in Q.933A Full Status)",
                                                 port->name, pvc->dlci_in);
                                    }
                                }
                            }
                        }
                        pvc = pvc->next;
                    }
                }
                mutex_unlock(&g_vfrs->pvc_mutex);
            }

            if (lmi->last_report_type == 0x04) {
                timer_set(&lmi->t391_timer, 1);  /* Annex G: immediate re-poll */
                return 2;  /* Full Status Continued */
            }
        } else {
            LOG_DEBUG("Q.933A DTE: ignoring STATUS with invalid recv_seq=%u (expected %u) "
                      "— error counted at next T391", recv_seq, lmi->dte_seq_send);
        }
        return 0;
    }

    return -1;
}

int lmi_q933a_init(vfr_port_t *port, int t392, int n392, int n393)
{
    vfr_lmi_state_t *lmi;

    if (!port) return -1;

    lmi = calloc(1, sizeof(vfr_lmi_state_t));
    if (!lmi) return -1;

    port->lmi_ctx = lmi;
    lmi_init_state(lmi, LMI_TYPE_Q933A);
    if (t392 > 0) lmi->t392 = t392;
    if (n392 > 0) lmi->n392 = n392;
    if (n393 > 0) lmi->n393 = n393;

    LOG_INFO("Q.933 Annex A LMI initialized on %s", port->name);
    return 0;
}
