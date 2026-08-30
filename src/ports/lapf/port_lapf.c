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

/* Flush deferred callbacks OUTSIDE the port mutex (F-10 deadlock fix).
 * Called after mutex_unlock at the end of lapf_handle_frame.
 * The deferred array lives on the caller's stack, so all data pointers
 * reference the original frame buffer which is still valid at this point. */
static void lapf_flush_deferred(vfr_port_t *port, lapf_deferred_cb_t *deferred, int count,
                                void (*on_event)(vfr_port_t *, u32, int)) {
    for (int i = 0; i < count; i++) {
        switch (deferred[i].type) {
            case LAPF_CB_L3_DATA:
                lapf_l3_recv_cb(port, deferred[i].data, deferred[i].len);
                break;
            case LAPF_CB_EVENT:
                if (on_event) {
                    on_event(port, deferred[i].dlci, deferred[i].event);
                }
                break;
            default:
                break;
        }
    }
}

/* ============================================================
 * Centralized Data Link Layer Frame Builder (DL-CORE)
 * ============================================================ */
int port_dl_build_frame(vfr_port_t *port, u32 dlci, u8 ctrl_byte, const u8 *payload, size_t len,
                        int cr, int fecn, int becn, int de, u8 *out_buf, size_t max_len, size_t *out_len)
{
    if (!port || !out_buf || !out_len) return -1;

    size_t addr_len = (port->dlcibit == 23 || dlci > 63487) ? 4 :
                      (port->dlcibit == 16 || dlci > 1023) ? 3 : 2;

    if (addr_len + 1 + len + 2 > max_len) {
        LOG_WARN("Frame exceeds buffer size on port %s (DLCI %u)", port->name, dlci);
        return -1;
    }

    /* Congestion & Discard Eligibility determinations (ITU-T X.36 / X.76):
     * 1. If local port is congested, set BECN=1 on reverse traffic and FECN=1 on forward traffic
     * 2. Preserves incoming flags without ever clearing set bits */
    int eff_fecn = fecn;
    int eff_becn = becn;
    int eff_de = de;

    if (cgst_should_set_de(port)) {
        eff_becn = 1;
        eff_fecn = 1;
    }

    /* Encode Q.922 Address */
    fr_encode_dlci_with_flags(out_buf, dlci, eff_fecn, eff_becn, eff_de, cr, (int)addr_len);

    /* Encode Control Octet */
    out_buf[addr_len] = ctrl_byte;

    /* Copy Information/Payload */
    if (len > 0 && payload) {
        memcpy(out_buf + addr_len + 1, payload, len);
    }

    size_t total_len = addr_len + 1 + len;
    size_t send_len = total_len;

    /* Transport-specific FCS-16 appending */
    if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
        u16 fcs = crc16_fcs(out_buf, total_len);
        out_buf[total_len]     = fcs & 0xFF;
        out_buf[total_len + 1] = (fcs >> 8) & 0xFF;
        send_len += 2;
    }

    *out_len = send_len;
    return 0;
}

/* Transmit helper for supervisory/unnumbered frames */
static void lapf_send_control(vfr_port_t *port, vfr_lapf_state_t *lapf, u8 ctrl_type, u8 nr, int pf, int is_response) {
    u8 ctrl_buf[2];
    size_t ctrl_len = 0;
    int cr = lapf_get_tx_cr(port, is_response);

    if (ctrl_type == LAPF_CTRL_S_RR || ctrl_type == LAPF_CTRL_S_RNR || ctrl_type == LAPF_CTRL_S_REJ) {
        ctrl_buf[0] = ctrl_type;
        ctrl_buf[1] = (nr << 1) | (pf ? 1 : 0);
        ctrl_len = 2;
        if (ctrl_type == LAPF_CTRL_S_RR) lapf->rr_tx++;
        else if (ctrl_type == LAPF_CTRL_S_RNR) lapf->rnr_tx++;
        else lapf->rej_tx++;
    } else {
        ctrl_buf[0] = ctrl_type | (pf ? 0x10 : 0);
        ctrl_len = 1;
        if (ctrl_type == LAPF_CTRL_U_SABME) lapf->sabme_tx++;
        else if (ctrl_type == LAPF_CTRL_U_DISC) lapf->disc_tx++;
        else if (ctrl_type == LAPF_CTRL_U_UA) lapf->ua_tx++;
        else if (ctrl_type == LAPF_CTRL_U_DM) lapf->dm_tx++;
        else if (ctrl_type == LAPF_CTRL_U_FRMR) lapf->frmr_tx++;
        else if (ctrl_type == LAPF_CTRL_U_UI) lapf->ui_tx++;
    }

    u8 frame_buf[128];
    size_t send_len = 0;
    const u8 *payload = (ctrl_len > 1) ? &ctrl_buf[1] : NULL;
    size_t payload_len = (ctrl_len > 1) ? ctrl_len - 1 : 0;

    int ret = port_dl_build_frame(port, lapf->dlci, ctrl_buf[0], payload, payload_len,
                                  cr, 0, 0, 0, frame_buf, sizeof(frame_buf), &send_len);
    if (ret == 0 && port->ops && port->ops->send) {
        int tx_bytes = port->ops->send(port, frame_buf, send_len);
        if (tx_bytes >= 0) {
            port->stats.tx_frames++;
            port->stats.tx_bytes += (u64)tx_bytes;
        }
    }
}

/* Transmit FRMR response frame with full 5-byte information field per Q.921 §3.10.1 & Figure 6/Q.921 */
static void lapf_send_frmr(vfr_port_t *port, vfr_lapf_state_t *lapf,
                           u8 rej_ctrl1, u8 rej_ctrl2, int rej_was_response,
                           u8 error_bits) {
    u8 frmr_payload[5];
    frmr_payload[0] = rej_ctrl1;
    frmr_payload[1] = rej_ctrl2;
    frmr_payload[2] = (u8)(lapf->v_s << 1);
    frmr_payload[3] = (u8)((lapf->v_r << 1) | (rej_was_response ? 1 : 0));
    frmr_payload[4] = error_bits & 0x0F;

    u8 frame_buf[128];
    size_t send_len = 0;
    int cr = lapf_get_tx_cr(port, 1); /* FRMR is always a response */

    int ret = port_dl_build_frame(port, lapf->dlci, LAPF_CTRL_U_FRMR, frmr_payload, sizeof(frmr_payload),
                                  cr, 0, 0, 0, frame_buf, sizeof(frame_buf), &send_len);
    if (ret == 0) {
        lapf->frmr_tx++;
        if (port->ops && port->ops->send) {
            int tx_bytes = port->ops->send(port, frame_buf, send_len);
            if (tx_bytes >= 0) {
                port->stats.tx_frames++;
                port->stats.tx_bytes += (u64)tx_bytes;
            }
        }
    }

    LOG_INFO("LAPF on %s (DLCI %u): Sent FRMR response (rej_ctrl=0x%02X%02X, V(S)=%u, V(R)=%u, WXYZ=0x%X)",
             port->name, lapf->dlci, rej_ctrl1, rej_ctrl2, lapf->v_s, lapf->v_r, error_bits);
}

/* Transmit helper for I-frames */
static void lapf_send_i_frame(vfr_port_t *port, vfr_lapf_state_t *lapf, u8 ns, u8 nr, int pf, int is_response, const u8 *data, size_t len) {
    u8 ctrl1 = ns << 1;
    u8 ctrl2 = (nr << 1) | (pf ? 1 : 0);
    int cr = lapf_get_tx_cr(port, is_response);

    u8 frame_buf[FR_MAX_FRAMESZ + 32];
    size_t send_len = 0;

    u8 i_payload[FR_MAX_FRAMESZ];
    i_payload[0] = ctrl2;
    if (len > 0 && data) {
        memcpy(i_payload + 1, data, len);
    }

    int ret = port_dl_build_frame(port, lapf->dlci, ctrl1, i_payload, len + 1,
                                  cr, 0, 0, 0, frame_buf, sizeof(frame_buf), &send_len);
    if (ret == 0) {
        lapf->i_tx++;
        if (port->ops && port->ops->send) {
            int tx_bytes = port->ops->send(port, frame_buf, send_len);
            if (tx_bytes >= 0) {
                port->stats.tx_frames++;
                port->stats.tx_bytes += (u64)tx_bytes;
            }
        }
    }
}

/* Dequeue from backlog queue and transmit if dynamic window is open */
static void lapf_process_backlog(vfr_port_t *port, vfr_lapf_state_t *lapf) {
    if (!lapf) return;

    /* F-08/F-12: Do not dequeue frames when peer has indicated RNR */
    if (lapf->peer_receiver_busy) return;

    while (lapf->backlog_head != lapf->backlog_tail) {
        u8 outstanding = (lapf->v_s >= lapf->v_a) ? (lapf->v_s - lapf->v_a) : (128 - lapf->v_a + lapf->v_s);
        if (outstanding >= lapf->v_k) {
            break; /* Dynamic working window v_k is full */
        }

        /* Dequeue frame */
        lapf_tx_frame_t *f = &lapf->tx_backlog[lapf->backlog_head];
        u8 ns = lapf->v_s;

        /* Store in outstanding transmission window */
        memcpy(lapf->tx_window[ns].data, f->data, f->len);
        lapf->tx_window[ns].len = f->len;

        lapf_send_i_frame(port, lapf, ns, lapf->v_r, 0, 0, f->data, f->len);
        LOG_DEBUG("LAPF on %s (DLCI %u): Sent I-frame N(S)=%u from backlog (V(k)=%u)",
                  port->name, lapf->dlci, ns, lapf->v_k);

        lapf->v_s = (lapf->v_s + 1) % 128;
        lapf->backlog_head = (lapf->backlog_head + 1) % 64;

        timer_set(&lapf->t200_timer, lapf->t200);
        timer_cancel(&lapf->t203_timer);
    }
}

/* ============================================================
 * Unified Port Data Link Layer Primitives Implementation
 * ============================================================ */

/* Unacknowledged UI data transfer with explicit congestion/priority parameters */
int port_dl_send_unit_data(vfr_port_t *port, u32 dlci, const u8 *payload, size_t len,
                           int cr, int fecn, int becn, int de)
{
    if (!port || !payload || len == 0) return -1;

    u8 frame_buf[FR_MAX_FRAMESZ + 32];
    size_t send_len = 0;

    int ret = port_dl_build_frame(port, dlci, LAPF_CTRL_U_UI, payload, len,
                                  cr, fecn, becn, de, frame_buf, sizeof(frame_buf), &send_len);
    if (ret < 0) return -1;

    if (port->ops && port->ops->send) {
        int tx_bytes = port->ops->send(port, frame_buf, send_len);
        if (tx_bytes >= 0) {
            port->stats.tx_frames++;
            port->stats.tx_bytes += (u64)tx_bytes;
            return 0;
        }
    }
    return -1;
}

/* Sequenced acknowledged I-frame transfer via LAPF FSM */
int port_dl_send_data(vfr_port_t *port, u32 dlci, const u8 *payload, size_t len)
{
    if (!port || !payload || len == 0) return -1;
    return lapf_send_l3(port, dlci, payload, len);
}

/* Unnumbered XID management / parameter exchange */
int port_dl_send_xid(vfr_port_t *port, u32 dlci, const u8 *xid_info, size_t len, int is_response)
{
    if (!port) return -1;

    u8 frame_buf[FR_MAX_FRAMESZ + 32];
    size_t send_len = 0;
    int cr = lapf_get_tx_cr(port, is_response);

    int ret = port_dl_build_frame(port, dlci, LAPF_CTRL_U_XID, xid_info, len,
                                  cr, 0, 0, 0, frame_buf, sizeof(frame_buf), &send_len);
    if (ret < 0) return -1;

    if (port->ops && port->ops->send) {
        int tx_bytes = port->ops->send(port, frame_buf, send_len);
        if (tx_bytes >= 0) {
            port->stats.tx_frames++;
            port->stats.tx_bytes += (u64)tx_bytes;
            return 0;
        }
    }
    return -1;
}

/* Link establishment request (sends SABME with P=1) */
int port_dl_establish_req(vfr_port_t *port, u32 dlci)
{
    return lapf_establish_link(port, dlci);
}

/* Link release request (sends DISC with P=1) */
int port_dl_release_req(vfr_port_t *port, u32 dlci)
{
    return lapf_release_link(port, dlci);
}

/* Explicit DLCI initialization in dynamic hash table */
int port_dl_init_dlci(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203)
{
    if (!port) return -1;

    mutex_lock(&port->mutex);

    u32 bucket = dlci % PORT_LAPF_HASH_SIZE;
    vfr_lapf_node_t *node = port->lapf_hash[bucket];
    while (node) {
        if (node->dlci == dlci) {
            vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)node->state;
            if (lapf) {
                lapf->dlci = dlci;
                lapf->state = LAPF_STATE_TEI_ASSIGNED;
                lapf->k = k ? k : lapf_default_k_for_rate(port->cgst_ctx ? cgst_get_access_rate(port) : 0);
                lapf->v_k = lapf->k;
                lapf->n_w = 5;
                lapf->ia_ct = 0;
                lapf->n200 = n200 ? n200 : 3;
                lapf->n201 = n201 ? n201 : 1598;
                lapf->t200 = t200 ? t200 : 1500;
                lapf->t203 = t203 ? t203 : 30000;
                LOG_INFO("LAPF re-initialized on %s for DLCI %u: k=%u (v_k=%u), n200=%u, n201=%u, t200=%ums, t203=%ums",
                         port->name, dlci, lapf->k, lapf->v_k, lapf->n200, lapf->n201, lapf->t200, lapf->t203);
            }
            mutex_unlock(&port->mutex);
            return 0;
        }
        node = node->next;
    }

    /* Allocate state and node */
    vfr_lapf_state_t *lapf = calloc(1, sizeof(vfr_lapf_state_t));
    if (!lapf) {
        LOG_ERROR("Failed to allocate LAPF state for %s DLCI %u", port->name, dlci);
        mutex_unlock(&port->mutex);
        return -1;
    }

    lapf->dlci = dlci;
    lapf->state = LAPF_STATE_TEI_ASSIGNED;
    lapf->k = k ? k : lapf_default_k_for_rate(port->cgst_ctx ? cgst_get_access_rate(port) : 0);
    lapf->v_k = lapf->k;
    lapf->n_w = 5;
    lapf->ia_ct = 0;
    lapf->n200 = n200 ? n200 : 3;
    lapf->n201 = n201 ? n201 : 1598;
    lapf->t200 = t200 ? t200 : 1500;
    lapf->t203 = t203 ? t203 : 30000;

    vfr_lapf_node_t *new_node = calloc(1, sizeof(vfr_lapf_node_t));
    if (!new_node) {
        free(lapf);
        mutex_unlock(&port->mutex);
        return -1;
    }

    new_node->dlci = dlci;
    new_node->state = lapf;
    new_node->next = port->lapf_hash[bucket];
    port->lapf_hash[bucket] = new_node;
    port->lapf_count++;

    LOG_INFO("LAPF initialized on %s for DLCI %u (dynamic hash): k=%u (v_k=%u), n200=%u, n201=%u, t200=%ums, t203=%ums",
             port->name, dlci, lapf->k, lapf->v_k, lapf->n200, lapf->n201, lapf->t200, lapf->t203);

    mutex_unlock(&port->mutex);
    return 0;
}

/* Explicit DLCI teardown from dynamic hash table */
void port_dl_free_dlci(vfr_port_t *port, u32 dlci)
{
    if (!port) return;
    mutex_lock(&port->mutex);
    u32 bucket = dlci % PORT_LAPF_HASH_SIZE;
    vfr_lapf_node_t **pptr = &port->lapf_hash[bucket];
    while (*pptr) {
        if ((*pptr)->dlci == dlci) {
            vfr_lapf_node_t *del = *pptr;
            *pptr = del->next;
            if (del->state) {
                free(del->state);
            }
            free(del);
            port->lapf_count--;
            break;
        }
        pptr = &(*pptr)->next;
    }
    mutex_unlock(&port->mutex);
}

/* Initialize LAPF on a port for a specific DLCI (Legacy wrapper) */
int lapf_port_init(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203)
{
    return port_dl_init_dlci(port, dlci, k, n200, n201, t200, t203);
}

/* Free all LAPF contexts and SAP registrations on a port */
void lapf_free(vfr_port_t *port) {
    if (!port) return;
    mutex_lock(&port->mutex);
    for (int i = 0; i < PORT_LAPF_HASH_SIZE; i++) {
        vfr_lapf_node_t *node = port->lapf_hash[i];
        while (node) {
            vfr_lapf_node_t *next = node->next;
            if (node->state) free(node->state);
            free(node);
            node = next;
        }
        port->lapf_hash[i] = NULL;
    }
    port->lapf_count = 0;

    /* Free SAP list */
    port_dl_sap_t *sap = port->sap_list;
    while (sap) {
        port_dl_sap_t *next_sap = sap->next;
        free(sap);
        sap = next_sap;
    }
    port->sap_list = NULL;

    mutex_unlock(&port->mutex);
}

/* SAP registration helper */
static port_dl_sap_t *port_get_or_create_sap(vfr_port_t *port, u32 dlci) {
    port_dl_sap_t *s = port->sap_list;
    while (s) {
        if (s->dlci == dlci) return s;
        s = s->next;
    }
    s = calloc(1, sizeof(port_dl_sap_t));
    if (s) {
        s->dlci = dlci;
        s->next = port->sap_list;
        port->sap_list = s;
    }
    return s;
}

void port_dl_register_ui_handler(vfr_port_t *port, u32 dlci, port_dl_ui_cb_fn cb) {
    if (!port) return;
    mutex_lock(&port->mutex);
    port_dl_sap_t *s = port_get_or_create_sap(port, dlci);
    if (s) s->ui_cb = cb;
    mutex_unlock(&port->mutex);
}

void port_dl_register_xid_handler(vfr_port_t *port, u32 dlci, port_dl_xid_cb_fn cb) {
    if (!port) return;
    mutex_lock(&port->mutex);
    port_dl_sap_t *s = port_get_or_create_sap(port, dlci);
    if (s) s->xid_cb = cb;
    mutex_unlock(&port->mutex);
}

void port_dl_register_l3_handler(vfr_port_t *port, u32 dlci, port_dl_l3_cb_fn cb) {
    if (!port) return;
    mutex_lock(&port->mutex);
    port_dl_sap_t *s = port_get_or_create_sap(port, dlci);
    if (s) s->l3_cb = cb;
    mutex_unlock(&port->mutex);
}

/* ITU-T X.36 Appendix VII window calculation helper: k = 2 + ((Ttd * Ru) / (4 * Ld)) */
u8 port_dl_calc_k_x36(u32 transit_delay_ms, u32 throughput_bps, u16 frame_size_octets) {
    if (frame_size_octets == 0) frame_size_octets = 1598;
    if (throughput_bps == 0) throughput_bps = 64000;
    if (transit_delay_ms == 0) transit_delay_ms = 50;

    u64 numerator = (u64)transit_delay_ms * (u64)throughput_bps;
    u64 denominator = (u64)4000 * (u64)frame_size_octets;

    u64 k_val = 2 + (numerator / denominator);
    if (k_val < 1) k_val = 1;
    if (k_val > 127) k_val = 127;
    return (u8)k_val;
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

/* Set L3 event callback for DL-ESTABLISH/DL-RELEASE indications */
void lapf_set_event_cb(vfr_port_t *port, u32 dlci,
                       void (*on_event)(vfr_port_t *port, u32 dlci, int event_type)) {
    if (!port) return;
    mutex_lock(&port->mutex);
    vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)port_get_lapf_ctx(port, dlci);
    if (lapf) {
        lapf->on_event = on_event;
    }
    mutex_unlock(&port->mutex);
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
    if (outstanding >= lapf->v_k || lapf->peer_receiver_busy) {
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

        LOG_DEBUG("LAPF on %s (DLCI %u): Transmit dynamic window full/busy (outstanding=%u, V(k)=%u), queued in backlog (len=%zu)",
                  port->name, dlci, outstanding, lapf->v_k, len);
        mutex_unlock(&port->mutex);
        return 0;
    }

    /* Send immediately */
    u8 ns = lapf->v_s;
    memcpy(lapf->tx_window[ns].data, data, len);
    lapf->tx_window[ns].len = len;

    lapf_send_i_frame(port, lapf, ns, lapf->v_r, 0, 0, data, len);
    LOG_DEBUG("LAPF on %s (DLCI %u): Sent I-frame N(S)=%u, N(R)=%u (len=%zu, V(k)=%u)",
              port->name, dlci, ns, lapf->v_r, len, lapf->v_k);

    lapf->v_s = (lapf->v_s + 1) % 128;
    timer_set(&lapf->t200_timer, lapf->t200);
    timer_cancel(&lapf->t203_timer);

    mutex_unlock(&port->mutex);
    return 0;
}



#include "svc/svc_sig_common.h"

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
static void lapf_handle_xid(vfr_port_t *port, vfr_lapf_state_t *lapf,
                            const u8 *frame, size_t frame_len,
                            const u8 *payload, size_t len,
                            int pf, int is_response) {
    lapf->xid_rx++;

    if (len < 4) return;
    u8 fi = payload[0];
    u8 gi = payload[1];

    if (fi != 0x82) {
        LOG_DEBUG("LAPF on %s (DLCI %u): Unsupported XID Format Identifier (FI=0x%02X, expected 0x82)",
                  port->name, lapf->dlci, fi);
        return;
    }

    if (gi == 0x0F) {
        /* F-06: GI=0x0F is Q.922 Annex A §A.7 "private parameters" — CLLM congestion management.
         * Forward to the CLLM module. */
        LOG_DEBUG("LAPF on %s (DLCI %u): XID with GI=0x0F (CLLM private params), forwarding to congestion module",
                  port->name, lapf->dlci);
        if (port->cgst_ctx) {
            cllm_handle_frame(port, frame, frame_len);
        } else {
            LOG_DEBUG("LAPF on %s (DLCI %u): CLLM XID received but congestion management not active, ignoring",
                      port->name, lapf->dlci);
        }
        return;
    }

    if (gi != 0x80) {
        LOG_DEBUG("LAPF on %s (DLCI %u): Unsupported XID Group Identifier (GI=0x%02X)",
                  port->name, lapf->dlci, gi);
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
        u8 xid_payload[32];
        u16 gl_len = 4 + 3 + 3;

        xid_payload[0] = 0x82; /* FI */
        xid_payload[1] = 0x80; /* GI */
        xid_payload[2] = (u8)(gl_len >> 8);   /* GL MSB */
        xid_payload[3] = (u8)(gl_len & 0xFF); /* GL LSB */

        /* PI = 5: Frame size (transmit) */
        xid_payload[4] = 5;
        xid_payload[5] = 2;
        xid_payload[6] = (u8)(resp_n201 >> 8);
        xid_payload[7] = (u8)(resp_n201 & 0xFF);

        /* PI = 7: Window size */
        xid_payload[8] = 7;
        xid_payload[9] = 1;
        xid_payload[10] = resp_k;

        /* PI = 9: Retransmission timer */
        xid_payload[11] = 9;
        xid_payload[12] = 1;
        xid_payload[13] = (u8)(resp_t200 / 100);

        u8 frame_buf[128];
        size_t send_len = 0;
        int cr = lapf_get_tx_cr(port, 1); /* XID Response */
        u8 ctrl_byte = LAPF_CTRL_U_XID | (pf ? 0x10 : 0);

        int ret = port_dl_build_frame(port, lapf->dlci, ctrl_byte, xid_payload, 14,
                                      cr, 0, 0, 0, frame_buf, sizeof(frame_buf), &send_len);
        if (ret == 0 && port->ops && port->ops->send) {
            int tx_bytes = port->ops->send(port, frame_buf, send_len);
            if (tx_bytes >= 0) {
                port->stats.tx_frames++;
                port->stats.tx_bytes += (u64)tx_bytes;
            }
            lapf->xid_tx++;
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

    /* Deferred callback queue — flushed outside mutex (F-10) */
    lapf_deferred_cb_t deferred[LAPF_MAX_DEFERRED];
    int deferred_count = 0;
    void (*snapshot_on_event)(vfr_port_t *, u32, int) = NULL;

    /* Decode C/R bit from the first byte of Q.922 Address of incoming frame */
    int is_response = (frame[0] & 0x02) >> 1; /* C/R is in bit 2 of octet 1 */
    
    /* Determine Address Length using bounded helper (F8.14) */
    size_t addr_len = fr_get_addr_len(frame, len);
    if (addr_len == 0 || len < addr_len) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Verify DLCI context exists for this port */
    fr_addr_t addr;
    fr_decode_addr(frame, &addr);

    u32 dlci = addr.dlci;
    vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)port_get_lapf_ctx(port, dlci);
    if (!lapf) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Snapshot callback pointer while holding mutex */
    snapshot_on_event = lapf->on_event;

    /* Dynamic Windowing (Q.922 Appendix I §I.2.2.1.2): On BECN receipt, scale working window by 0.625 */
    if (addr.becn) {
        u8 scaled = (u8)((lapf->v_k * 5) / 8);
        lapf->v_k = (scaled > 0) ? scaled : 1;
        LOG_DEBUG("LAPF on %s (DLCI %u): BECN received, dynamic window scaled down to V(k)=%u",
                  port->name, lapf->dlci, lapf->v_k);
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

    /* UI Frame — state-independent per Q.922 §5.2 (F-09) */
    if (ctrl_type == LAPF_CTRL_U_UI) {
        /* Check registered SAP UI handler first */
        port_dl_sap_t *sap = port->sap_list;
        while (sap) {
            if (sap->dlci == lapf->dlci && sap->ui_cb) {
                sap->ui_cb(port, lapf->dlci, payload + 1, payload_len - 1);
                goto flush_and_return;
            }
            sap = sap->next;
        }

        if (payload_len > 1 && payload[1] == 0x08) {
            /* Defer L3 delivery (F-10) */
            if (deferred_count < LAPF_MAX_DEFERRED) {
                deferred[deferred_count].type = LAPF_CB_L3_DATA;
                deferred[deferred_count].data = payload + 1;
                deferred[deferred_count].len = payload_len - 1;
                deferred_count++;
            }
        } else {
            LOG_INFO("LAPF on %s (DLCI %u): Received UI frame (ignored)", port->name, lapf->dlci);
        }
        goto flush_and_return;
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

        /* F-01: Queue DL-ESTABLISH indication for L3 */
        if (deferred_count < LAPF_MAX_DEFERRED) {
            deferred[deferred_count].type = LAPF_CB_EVENT;
            deferred[deferred_count].event = LAPF_EVENT_ESTABLISHED;
            deferred[deferred_count].dlci = lapf->dlci;
            deferred_count++;
        }

        goto flush_and_return;
    }

    if (ctrl_type == LAPF_CTRL_U_DISC) {
        LOG_INFO("LAPF on %s (DLCI %u): Received DISC command", port->name, lapf->dlci);

        /* F-02: Queue DL-RELEASE indication for L3 (only if link was established) */
        if (lapf->state == LAPF_STATE_ESTABLISHED || lapf->state == LAPF_STATE_TIMER_RECOVERY) {
            if (deferred_count < LAPF_MAX_DEFERRED) {
                deferred[deferred_count].type = LAPF_CB_EVENT;
                deferred[deferred_count].event = LAPF_EVENT_RELEASED;
                deferred[deferred_count].dlci = lapf->dlci;
                deferred_count++;
            }
        }

        if (lapf->state == LAPF_STATE_TEI_ASSIGNED) {
            lapf_send_control(port, lapf, LAPF_CTRL_U_DM, 0, pf, 1); /* DM Response */
        } else {
            lapf->state = LAPF_STATE_TEI_ASSIGNED;
            lapf_send_control(port, lapf, LAPF_CTRL_U_UA, 0, pf, 1); /* UA Response */
        }
        timer_cancel(&lapf->t200_timer);
        timer_cancel(&lapf->t203_timer);
        
        LOG_INFO("LAPF on %s: Link disconnected (state=TEI_ASSIGNED)", port->name);
        goto flush_and_return;
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

                /* F-01: Queue DL-ESTABLISH confirm for L3 */
                if (deferred_count < LAPF_MAX_DEFERRED) {
                    deferred[deferred_count].type = LAPF_CB_EVENT;
                    deferred[deferred_count].event = LAPF_EVENT_ESTABLISHED;
                    deferred[deferred_count].dlci = lapf->dlci;
                    deferred_count++;
                }
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
                break;
            }

            if (ctrl_type == LAPF_CTRL_U_DM) {
                if (pf) {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_B);
                    if (lapf->state == LAPF_STATE_TIMER_RECOVERY) {
                        lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                        lapf->retransmission_count = 0;
                        lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                        timer_set(&lapf->t200_timer, lapf->t200);
                        timer_cancel(&lapf->t203_timer);
                    }
                } else {
                    lapf_mdl_error(port, lapf, LAPF_MDL_ERROR_E);
                    lapf->state = LAPF_STATE_AWAITING_ESTABLISHMENT;
                    lapf->retransmission_count = 0;
                    lapf_send_control(port, lapf, LAPF_CTRL_U_SABME, 0, 1, 0);
                    timer_set(&lapf->t200_timer, lapf->t200);
                    timer_cancel(&lapf->t203_timer);
                }
                break;
            }

            /* XID Frame detection */
            if (ctrl_type == LAPF_CTRL_U_XID) {
                lapf_handle_xid(port, lapf, frame, len, payload + 1, payload_len - 1, pf, is_response);
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

            /* Guard: if this is a U-frame, it was already handled above */
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
                u8 ack_count = (nr >= lapf->v_a) ? (nr - lapf->v_a) : (128 - lapf->v_a + nr);
                LOG_DEBUG("LAPF on %s (DLCI %u): Received N(R)=%u, acknowledging %u frames from V(A)=%u",
                          port->name, lapf->dlci, nr, ack_count, lapf->v_a);
                lapf->v_a = nr;
                
                /* Dynamic Windowing (Q.922 Appendix I §I.1.2): Step recovery */
                lapf->ia_ct += ack_count;
                while (lapf->ia_ct >= lapf->n_w) {
                    if (lapf->v_k < lapf->k) {
                        lapf->v_k++;
                        LOG_DEBUG("LAPF on %s (DLCI %u): Dynamic window stepped up to V(k)=%u (k=%u)",
                                  port->name, lapf->dlci, lapf->v_k, lapf->k);
                    }
                    lapf->ia_ct -= lapf->n_w;
                }

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
                /* Dynamic Windowing (Q.922 Appendix I §I.1.1): Reduce window on loss (REJ) */
                lapf->v_k = (lapf->v_k > 4) ? (lapf->v_k / 4) : 1;
                lapf->ia_ct = 0;
                LOG_DEBUG("LAPF on %s (DLCI %u): REJ received, dynamic window reduced to V(k)=%u",
                          port->name, lapf->dlci, lapf->v_k);

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
                if (!lapf->peer_receiver_busy) {
                    LOG_INFO("LAPF on %s (DLCI %u): Received REJ frame, retransmitting from N(R)=%u to V(S)=%u",
                             port->name, lapf->dlci, nr, lapf->v_s);
                    u8 tx_seq = nr;
                    while (tx_seq != lapf->v_s) {
                        lapf_send_i_frame(port, lapf, tx_seq, lapf->v_r, 0, 0,
                                          lapf->tx_window[tx_seq].data, lapf->tx_window[tx_seq].len);
                        tx_seq = (tx_seq + 1) % 128;
                    }
                    timer_set(&lapf->t200_timer, lapf->t200);
                    timer_cancel(&lapf->t203_timer);
                } else {
                    LOG_INFO("LAPF on %s (DLCI %u): Received REJ but peer is busy (RNR), deferring retransmission",
                             port->name, lapf->dlci);
                }
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
                            /* Defer L3 delivery (F-10) */
                            if (deferred_count < LAPF_MAX_DEFERRED) {
                                deferred[deferred_count].type = LAPF_CB_L3_DATA;
                                deferred[deferred_count].data = payload + 2;
                                deferred[deferred_count].len = payload_len - 2;
                                deferred_count++;
                            }
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

flush_and_return:
    mutex_unlock(&port->mutex);

    /* Invoke all deferred callbacks OUTSIDE the mutex (F-10) */
    if (deferred_count > 0) {
        lapf_flush_deferred(port, deferred, deferred_count, snapshot_on_event);
    }

    return 0;
}

/* Process LAPF timer ticks (called periodically from main loop) */
void lapf_poll_timer(vfr_port_t *port) {
    if (!port) return;

    mutex_lock(&port->mutex);
    for (int b = 0; b < PORT_LAPF_HASH_SIZE; b++) {
        vfr_lapf_node_t *node = port->lapf_hash[b];
        while (node) {
            vfr_lapf_state_t *lapf = (vfr_lapf_state_t *)node->state;
            if (!lapf) {
                node = node->next;
                continue;
            }

            /* Check T200 (Retransmission) Timer */
            if (timer_is_expired(&lapf->t200_timer)) {
                lapf->t200_expires++;
                lapf->retransmission_count++;

                /* Dynamic Windowing (Q.922 Appendix I §I.1.1): scale working window down on T200 loss */
                lapf->v_k = (lapf->v_k > 4) ? (lapf->v_k / 4) : 1;
                lapf->ia_ct = 0;

                LOG_WARN("LAPF on %s (DLCI %u): T200 expired (retransmission count: %u/%u) in state %d (V(k)=%u)",
                         port->name, lapf->dlci, lapf->retransmission_count, lapf->n200, lapf->state, lapf->v_k);

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

            node = node->next;
        }
    }

    mutex_unlock(&port->mutex);
}
