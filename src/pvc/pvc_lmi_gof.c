/*
 * pvc_lmi_gof.c - Cisco / Gang of Four LMI Stack
 * VFRS - Virtual Frame Relay Switch
 */

#include "vfr.h"
#include <string.h>

/* Cisco uses different IE codes */
#define CISCO_IE_REPORT_TYPE   0x01
#define CISCO_IE_PVC_STATUS    0x07

/* ============================================================
 * Cisco IE Builders
 * ============================================================ */

/* Build Report Type IE - Cisco format (3 octets) */
size_t lmi_gof_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type)
{
    if (max_len < 3) return 0;
    buf[0] = CISCO_IE_REPORT_TYPE;  /* Cisco Report Type IE */
    buf[1] = 0x01;  /* length = 1 */
    buf[2] = report_type;
    return 3;
}

/* Build PVC Status IE - Cisco format (supports 2-byte DLCI format) */
size_t lmi_gof_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len)
{
    (void)dlci_len;
    if (max_len < 5) return 0;
    buf[0] = CISCO_IE_PVC_STATUS;  /* Cisco PVC Status IE */
    buf[1] = 0x03;  /* length = 3 */
    buf[2] = (u8)((dlci >> 4) & 0x3F);
    buf[3] = (u8)(((dlci & 0x0F) << 3) | 0x80);
    buf[4] = status | LMI_PVC_EXT;
    return 5;
}

/* ============================================================
 * Cisco/GoF LMI Stack Functions
 * ============================================================ */

int lmi_gof_init(vfr_port_t *port, int t392, int n392, int n393)
{
    vfr_lmi_state_t *lmi;

    if (!port) return -1;

    lmi = calloc(1, sizeof(vfr_lmi_state_t));
    if (!lmi) return -1;

    port->lmi_ctx = lmi;
    lmi_init_state(lmi, LMI_TYPE_CISCO);
    if (t392 > 0) lmi->t392 = t392;
    if (n392 > 0) lmi->n392 = n392;
    if (n393 > 0) lmi->n393 = n393;

    LOG_INFO("Cisco LMI initialized on %s", port->name);
    return 0;
}

/* Dedicated Cisco LMI IE Parser */
int lmi_gof_handle_frame(vfr_port_t *port, const u8 *frame, size_t len)
{
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    u8 msg_type;
    u8 send_seq = 0, recv_seq = 0;
    const u8 *p = frame;
    size_t pos = 0;

    if (!lmi || len < 4) return -1;

    /* Protocol discriminator */
    if (p[pos++] != 0x09) {
        return -1;
    }
    /* Call reference */
    if (p[pos++] != 0x00) {
        return -1;
    }

    msg_type = p[pos++];

    /* Parse Cisco IEs (no locking shifts used in GOF LMI) */
    while (pos < len - 1) {
        u8 ie_id = p[pos++];
        u8 ie_len = p[pos++];

        switch (ie_id) {
        case 0x01:  /* Cisco Report Type IE */
            if (ie_len >= 1) {
                lmi->last_report_type = p[pos];
            }
            break;
        case 0x03:  /* Cisco Link Integrity IE */
            if (ie_len >= 2) {
                send_seq = p[pos];
                recv_seq = p[pos + 1];
            }
            break;
        case 0x07:  /* Cisco PVC Status IE */
            if (ie_len >= 3) {
                u16 pvc_dlci = (((u16)(p[pos] & 0x3F)) << 4) +
                                ((p[pos + 1] & 0x78) >> 3);
                u8 pvc_status = p[pos + 2];
                vfr_dlci_entry_t *entry = port_lookup_dlci(port, pvc_dlci);
                if (entry) {
                    entry->active = (pvc_status & LMI_PVC_ACTIVE) ? 1 : 0;
                    if (entry->vc_type == VC_TYPE_PVC && entry->detail_ptr) {
                        vfr_pvc_detail_t *pvc = (vfr_pvc_detail_t *)entry->detail_ptr;
                        pvc->active = entry->active;
                        pvc->lmi_reported = 1;
                    }
                }
            }
            break;
        }
        pos += ie_len;
    }

    if (msg_type == 0x75) {  /* STATUS ENQ */
        lmi->dce_seq_recv = recv_seq;
        lmi->dte_seq_recv = send_seq;
        return 1;
    } else if (msg_type == 0x7D) {  /* STATUS */
        /* X.36 §11.4.1.6.2 Note 2: Link integrity response to a Full Status request must be ignored */
        if (lmi->dte_last_request_was_full && lmi->last_report_type == LMI_REPORT_LINK_INTEGRITY) {
            LOG_WARN("Cisco DTE on %s: Received Link Integrity response to a Full Status request - Ignoring STATUS", port->name);
            return 0;
        }

        /* D-5: per X.36 §11.4.1.6.2 NOTE 1 — invalid recv_seq means ignore entire STATUS */
        int seq_valid = (recv_seq == 0 || recv_seq == lmi->dte_seq_send);
        if (seq_valid) {
            /* ITU-T X.36 Appendix III Loopback Detection:
             * Suspect loopback if received send_seq matches our send sequence counter */
            if (send_seq != 0 && send_seq == lmi->dte_seq_send) {
                LOG_WARN("Cisco LMI on %s: Suspected physical layer loopback condition detected per ITU-T X.36 Appendix III (received send_seq=%u matches tx send_seq)",
                         port->name, send_seq);
            }

            lmi->dte_dce_seq_recv = send_seq;
            lmi->dte_status_received = 1;

            /* Check for omitted PVCs in Full Status report */
            if (lmi->last_report_type == LMI_REPORT_FULL_STATUS && g_vfrs) {
                mutex_lock(&g_vfrs->pvc_mutex);
                for (int i = 0; i < PVC_HASH_SIZE; i++) {
                    vfr_pvc_detail_t *pvc = g_vfrs->pvc_table[i];
                    while (pvc) {
                        if (strcmp(pvc->port_in, port->name) == 0) {
                            if (!pvc->lmi_reported) {
                                if (pvc->active) {
                                    pvc->active = 0;
                                    vfr_dlci_entry_t *entry = port_lookup_dlci(port, pvc->dlci_in);
                                    if (entry) entry->active = 0;
                                    LOG_INFO("Cisco DTE on %s: PVC %u marked inactive (omitted in Full Status)",
                                             port->name, pvc->dlci_in);
                                }
                            }
                        }
                        pvc = pvc->next;
                    }
                }
                mutex_unlock(&g_vfrs->pvc_mutex);
            }
        } else {
            LOG_DEBUG("Cisco DTE: ignoring STATUS with invalid recv_seq=%u (expected %u) "
                      "— error counted at next T391", recv_seq, lmi->dte_seq_send);
        }
        return 0;
    }

    return -1;
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

/* Build Cisco/Gang-of-Four LMI STATUS response. */
int lmi_cisco_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status)
{
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    vfrs_ctx_t *ctx = g_vfrs;
    size_t pos = 0;

    if (lmi) {
        lmi->dce_last_was_full = full_status;
    }

    /* LMI header: PD + call reference + message type */
    pos += lmi_build_header(port, buf, max_len, 0x09);  /* PD = 0x09 */
    if (pos + 1 > max_len) return 0;
    buf[pos++] = 0x7D;  /* STATUS */

    /* Report Type IE -- Cisco code 0x01 */
    if (pos + 3 <= max_len) {
        pos += lmi_gof_build_report_type_ie(buf + pos, max_len - pos,
                                            full_status ? LMI_REPORT_FULL_STATUS : LMI_REPORT_LINK_INTEGRITY);
    }

    /* Link Integrity IE -- Cisco code 0x03 (2 octets)
     * CB-4: bytes were swapped — octet 3 = Send Seq (DCE), octet 4 = Recv Seq (DTE).
     * CB-1: DCE increments its send seq here before placing it (§11.4.1.2 step 3). */
    if (pos + 4 <= max_len) {
        lmi->dce_seq_send = lmi_inc_seq(lmi->dce_seq_send);
        buf[pos++] = 0x03;               /* Cisco Link Integrity IE */
        buf[pos++] = 0x02;               /* length = 2 */
        buf[pos++] = lmi->dce_seq_send;  /* octet 3: Send Seq — DCE's own incremented counter */
        buf[pos++] = lmi->dte_seq_recv;  /* octet 4: Recv Seq — last DTE send seq received */
    }

    /* PVC Status IEs -- Cisco code 0x07 (3 octets) */
    if (full_status && ctx) {
        lmi_status_item_t sorted_items[MAX_PVCS];
        int num_items = 0;
        int i;

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

        for (i = 0; i < num_items && pos + 5 <= max_len; i++) {
            lmi_status_item_t *item = &sorted_items[i];
            u8 status = item->status;
            if (item->is_new) {
                status |= LMI_PVC_NEW;
                if (item->new_txsn_ptr) {
                    *item->new_txsn_ptr = lmi->dce_seq_send;
                }
            }
            pos += lmi_gof_build_pvc_status_ie(buf + pos, max_len - pos, item->dlci, status, 2);
        }
    }

    return (int)pos;
}

/* Build Cisco/Gang-of-Four LMI STATUS ENQUIRY. */
size_t lmi_cisco_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len)
{
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    size_t pos = 0;
    u8 report_type;

    if (!lmi) return 0;

    /* LMI header: PD = 0x09, dummy call reference = 0x00 */
    pos += lmi_build_header(port, buf, max_len, 0x09);
    if (pos + 1 > max_len) return 0;
    buf[pos++] = 0x75;  /* STATUS ENQ */

    /* Report Type IE -- Cisco code 0x01 */
    report_type = (lmi->polls_since_full >= lmi->n391) ?
                  LMI_REPORT_FULL_STATUS : LMI_REPORT_LINK_INTEGRITY;
    if (pos + 3 <= max_len) {
        pos += lmi_gof_build_report_type_ie(buf + pos, max_len - pos, report_type);
    }

    /* Link Integrity IE -- Cisco code 0x03 (2 octets) */
    if (pos + 4 <= max_len) {
        buf[pos++] = 0x03;               /* Cisco Link Integrity IE */
        buf[pos++] = 0x02;               /* length = 2 */
        buf[pos++] = lmi->dte_seq_send;  /* our current send seq */
        buf[pos++] = lmi->dte_dce_seq_recv;  /* last DCE seq received */
    }

    return pos;
}
