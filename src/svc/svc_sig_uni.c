/*
 * svc_sig_uni.c - Q.933 DCE UNI Call State Machine Implementation
 * Virtual Frame Relay Switch - ITU-T Q.933 / X.36 Chapters 10.7 - 10.11
 */

#include "svc_sig_uni.h"
#include "svc_sig_nni.h"
#include "svc_spvc.h"
#include "vfr.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "switching/svc_routing_common.h"
#include "ports/lapf/port_lapf.h"

extern vfrs_ctx_t *g_vfrs;

void svc_uni_init(void) {
    LOG_INFO("Q.933 UNI Call State Machine initialized");
}

/* Helper to send a Q.933 signaling frame wrapped in LAPF I-frame (or DLCI 0 UI frame if LAPF inactive) */
static int send_sig_frame(vfr_port_t *port, const u8 *msg_data, size_t msg_len) {
    if (!port || !msg_data || msg_len == 0) return -1;

    if (port_get_lapf_ctx(port, 0)) {
        return port_dl_send_data(port, 0, msg_data, msg_len);
    }

    return port_dl_send_unit_data(port, 0, msg_data, msg_len, 0, 0, 0, 0);
}

static void send_status_incompatible_state(vfr_port_t *port, vfr_svc_ctx_t *sctx, vfr_call_t *call, u8 cause, u8 offending_msg_type) {
    (void)sctx;
    u8 tx_buf[128];
    int tx_len = q933_build_status_ex(tx_buf, sizeof(tx_buf), call, cause, offending_msg_type, 1);
    if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);
}

/* Helper to initiate standard clearing procedure (X.36 §10.7.4.2 & Appendix VI) */
void svc_initiate_clearing_before_active_ex(vfr_port_t *port, vfr_svc_ctx_t *sctx, vfr_call_t *call, u8 cause, u8 diag_byte, int has_diag) {
    if (!port || !sctx || !call) return;

    LOG_INFO("SVC [Port %s CRV 0x%04X]: Initiating call clearing from state %s with Cause %u (\"%s\")",
             port->name, call->call_ref, q933_get_state_name(call->state), cause, q850_get_cause_str(cause));

    if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);

    u8 tx_buf[128];
    int tx_len = 0;

    if (call->state == SVC_STATE_N10_ACTIVE || call->state == SVC_STATE_CONNECT_REQUEST) {
        /* Send DISCONNECT, enter state N12 (Disconnect Indication), start T305 */
        tx_len = q933_build_disconnect(tx_buf, sizeof(tx_buf), call, cause);
        if (tx_len > 0) {
            send_sig_frame(port, tx_buf, tx_len);
            LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent DISCONNECT -> State: %s, T305 started (%u ms)",
                     port->name, call->call_ref, q933_get_state_name(SVC_STATE_DISCONNECT_IND), sctx->t305_ms);
        }
        
        timer_cancel(&call->t303);
        timer_cancel(&call->t305);
        timer_cancel(&call->t308);
        timer_cancel(&call->t310);
        timer_cancel(&call->t301);
        timer_cancel(&call->t322);

        call->state = SVC_STATE_DISCONNECT_IND;
        timer_set(&call->t305, sctx->t305_ms);
    } else if (call->state == SVC_STATE_NULL) {
        /* Send RELEASE COMPLETE, transition to NULL immediately */
        tx_len = q933_build_release_complete_ex(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, cause, diag_byte, has_diag);
        if (tx_len > 0) {
            send_sig_frame(port, tx_buf, tx_len);
            LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent RELEASE COMPLETE -> Call cleared, State: %s",
                     port->name, call->call_ref, q933_get_state_name(SVC_STATE_NULL));
        }
        
        svc_free_call(sctx, call);
    } else {
        /* Send RELEASE, transition to N19 (Release Request), start T308 */
        tx_len = q933_build_release(tx_buf, sizeof(tx_buf), call, cause);
        if (tx_len > 0) {
            send_sig_frame(port, tx_buf, tx_len);
            LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent RELEASE -> State: %s, T308 started (%u ms)",
                     port->name, call->call_ref, q933_get_state_name(SVC_STATE_N19_REL_REQ), sctx->t308_ms);
        }

        timer_cancel(&call->t303);
        timer_cancel(&call->t305);
        timer_cancel(&call->t308);
        timer_cancel(&call->t310);
        timer_cancel(&call->t301);
        timer_cancel(&call->t322);

        call->state = SVC_STATE_N19_REL_REQ;
        call->t308_retries = 0;
        timer_set(&call->t308, sctx->t308_ms);
    }
}

void svc_initiate_clearing_before_active(vfr_port_t *port, vfr_svc_ctx_t *sctx, vfr_call_t *call, u8 cause) {
    svc_initiate_clearing_before_active_ex(port, sctx, call, cause, 0, 0);
}

/* Virtual Switch Entity for all-zeros switch self-number (e.g. 510401010000) */
int svc_uni_handle_virtual_switch_call(vfr_port_t *port, vfr_call_t *call) {
    if (!port || !call) return -1;

    u8 tx_buf[512];
    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;

    LOG_INFO("SVC [Port %s CRV 0x%04X]: Call to Virtual Switch entity accepted (DLCI=%u)",
             port->name, call->call_ref, call->ingress_dlci);

    /* 1. Send CALL PROCEEDING */
    call->call_ref_len = sctx->crv_len_cfg;
    call->call_ref_flag = 1;
    call->state = SVC_STATE_N3_CALL_PROC;
    int tx_len = q933_build_call_proceeding(tx_buf, sizeof(tx_buf), call);
    if (tx_len > 0) {
        send_sig_frame(port, tx_buf, tx_len);
        LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent CALL PROCEEDING -> State: %s",
                 port->name, call->call_ref, q933_get_state_name(call->state));
    }

    /* 2. Send CONNECT */
    call->state = SVC_STATE_N10_ACTIVE;
    call->egress_dlci = call->ingress_dlci;
    snprintf(call->ingress_port, sizeof(call->ingress_port), "%s", port->name);
    snprintf(call->egress_port, sizeof(call->egress_port), "%s", port->name);
    snprintf(call->connected_number, sizeof(call->connected_number), "%s", call->called_number);

    tx_len = q933_build_connect(tx_buf, sizeof(tx_buf), call);
    if (tx_len > 0) {
        send_sig_frame(port, tx_buf, tx_len);
        LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent CONNECT -> State: %s",
                 port->name, call->call_ref, q933_get_state_name(call->state));
    }

    /* 3. Register self-loop PVC for echo/test data phase */
    if (g_vfrs) {
        svc_create_data_pvcs(g_vfrs, call);
    }

    return 0;
}

/* Main Q.933 UNI message processing function */
static int svc_uni_process_msg_locked(vfr_port_t *port, const u8 *msg_data, size_t len) {
    if (!port || !msg_data || len < 3) return -1;

    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
    if (!sctx) {
        return -1;
    }

    q933_msg_header_t hdr;
    int hdr_len = q933_parse_header(msg_data, len, &hdr);
    if (hdr_len < 0) {
        LOG_WARN("SVC [Port %s]: Failed to parse Q.933 message header (invalid protocol disc or CRV format)", port->name);
        return -1;
    }

    const u8 *ie_start = msg_data + hdr_len;
    size_t ie_total_len = len - hdr_len;

    LOG_INFO("SVC [Port %s CRV 0x%04X]: Received %s message (%zu bytes)",
             port->name, hdr.call_ref_value, q933_get_msg_name(hdr.message_type), len);

    /* ============================================================
     * Global Call Reference Handling (X.36 §10.8.3 & §10.9)
     * ============================================================ */
    if (hdr.call_ref_value == 0) {
        if (hdr.message_type == Q933_MSG_RESTART) {
            LOG_INFO("SVC [Port %s]: Received RESTART message (Global CRV). Resetting all SVC calls.", port->name);
            /* Clear all active calls on this port */
            for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
                if (sctx->calls[i].in_use) {
                    if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, &sctx->calls[i]);
                    svc_free_call(sctx, &sctx->calls[i]);
                }
            }
            u8 tx_buf[128];
            int tx_len = q933_build_restart_ack(tx_buf, sizeof(tx_buf), 0);
            if (tx_len > 0) {
                send_sig_frame(port, tx_buf, tx_len);
                LOG_INFO("SVC [Port %s]: Sent RESTART ACKNOWLEDGE (Global CRV)", port->name);
            }
            return 0;
        } else if (hdr.message_type == Q933_MSG_RESTART_ACK) {
            LOG_INFO("SVC [Port %s]: Received RESTART ACK (Global CRV). Restart procedure completed.", port->name);
            timer_cancel(&sctx->t316);
            sctx->restart_state = 0;
            sctx->t316_retries = 0;
            return 0;
        } else if (hdr.message_type == Q933_MSG_STATUS) {
            /* X.36 §10.8.3: Ignore STATUS message with Global CRV */
            LOG_DEBUG("SVC [Port %s]: Ignoring STATUS message with Global CRV", port->name);
            return 0;
        } else {
            /* X.36 §10.8.3 & §10.10.3.2 bit 4: Return STATUS with Global CRV, Cause 81, state REST0 */
            LOG_WARN("SVC [Port %s]: Non-RESTART message 0x%02X received with Global CRV. Returning STATUS.", port->name, hdr.message_type);
            u8 tx_buf[128];
            int tx_len = q933_build_status(tx_buf, sizeof(tx_buf), NULL, Q850_CAUSE_INVALID_CALL_REF);
            if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);
            return 0;
        }
    }

    /* Find existing CCB */
    vfr_call_t *call = svc_find_call(sctx, hdr.call_ref_value, hdr.call_ref_flag ^ 1);

    /* ============================================================
     * Unrecognized Call Reference Handling (X.36 §10.10.3.2)
     * ============================================================ */
    if (!call && hdr.message_type != Q933_MSG_SETUP) {
        if (hdr.message_type == Q933_MSG_RELEASE_COMPLETE) {
            /* X.36 §10.10.3.2 bit 2: Ignore RELEASE COMPLETE with unrecognized CRV */
            LOG_DEBUG("SVC [Port %s]: Ignoring RELEASE COMPLETE for unrecognized CRV 0x%04X", port->name, hdr.call_ref_value);
            return 0;
        }
        if (hdr.message_type == Q933_MSG_STATUS_ENQUIRY) {
            /* X.36 §10.10.3.2 bit 6: Return STATUS call state Null, Cause 30 */
            LOG_WARN("SVC [Port %s]: Unrecognized CRV 0x%04X for STATUS ENQUIRY. Returning STATUS Call State Null.", port->name, hdr.call_ref_value);
            u8 tx_buf[128];
            int tx_len = q933_build_status_raw(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, sctx->crv_len_cfg, 0, Q850_CAUSE_RESPONSE_TO_STATUS_ENQ);
            if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);
            return 0;
        }
        if (hdr.message_type == Q933_MSG_STATUS) {
            /* X.36 §10.10.3.2 bit 5 & §10.8.2: STATUS with unrecognized CRV */
            u8 peer_state = 0;
            size_t offset = 0;
            while (offset + 2 <= ie_total_len) {
                u8 ie_id = ie_start[offset];
                u8 ie_len = ie_start[offset + 1];
                if (offset + 2 + ie_len > ie_total_len) break;
                if (ie_id == Q933_IE_CALL_STATE) {
                    q933_parse_call_state(&ie_start[offset + 2], ie_len, &peer_state);
                    break;
                }
                offset += 2 + ie_len;
            }
            if (peer_state != 0) {
                /* Peer indicates non-Null, we are Null -> send RELEASE COMPLETE Cause 101 */
                LOG_WARN("SVC [Port %s]: STATUS message for unrecognized CRV 0x%04X reports non-Null state %s. Returning RELEASE COMPLETE.",
                         port->name, hdr.call_ref_value, q933_get_state_name(peer_state));
                u8 tx_buf[128];
                int tx_len = q933_build_release_complete(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, sctx->crv_len_cfg, Q850_CAUSE_MSG_INCOMPAT_WITH_STATE);
                if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);
            } else {
                LOG_INFO("SVC [Port %s]: STATUS message for unrecognized CRV 0x%04X reports Null state. Discarding.", port->name, hdr.call_ref_value);
            }
            return 0;
        }

        /* X.36 §10.10.3.2 bit 1: Send RELEASE COMPLETE with Cause 81 for unrecognized CRV */
        LOG_WARN("SVC [Port %s]: Unrecognized CRV 0x%04X for message type %s (0x%02X). Returning RELEASE COMPLETE.",
                 port->name, hdr.call_ref_value, q933_get_msg_name(hdr.message_type), hdr.message_type);
        u8 tx_buf[128];
        int tx_len = q933_build_release_complete(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, sctx->crv_len_cfg, Q850_CAUSE_INVALID_CALL_REF);
        if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);
        return 0;
    }

    /* ============================================================
     * Message Processing per State Machine
     * ============================================================ */

    switch (hdr.message_type) {

    case Q933_MSG_SETUP: {
        /* X.36 §10.10.3.2 bit 3: Reject incoming SETUP with CRV flag == 1 (DCE response flag set by DTE) */
        if (hdr.call_ref_flag == 1) {
            LOG_WARN("SVC [Port %s]: SETUP received with invalid CRV flag 1 from DTE. Ignoring.", port->name);
            return 0;
        }

        if (call && call->state != SVC_STATE_NULL) {
            /* Duplicate SETUP on active CRV - X.36 §10.10.3.2 bit 3: ignore */
            LOG_WARN("SVC [Port %s]: SETUP received on active CRV 0x%04X in state %s. Ignoring.",
                     port->name, hdr.call_ref_value, q933_get_state_name(call->state));
            return 0;
        }

        /* Allocate new CCB */
        call = svc_alloc_call_ref(sctx, hdr.call_ref_value, hdr.call_ref_flag ^ 1);
        if (!call) {
            LOG_WARN("SVC [Port %s]: Failed to allocate call reference block for CRV 0x%04X. Returning RELEASE COMPLETE Cause 34.",
                     port->name, hdr.call_ref_value);
            u8 tx_buf[128];
            int tx_len = q933_build_release_complete(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, sctx->crv_len_cfg, Q850_CAUSE_NO_CIRCUIT_AVAILABLE);
            if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);
            return 0;
        }

        call->state = SVC_STATE_N1_CALL_INIT;
        call->call_ref_len = sctx->crv_len_cfg;
        call->call_ref_flag = 1; /* DCE response flag */
        snprintf(call->ingress_port, sizeof(call->ingress_port), "%s", port->name);

        /* Set default QoS values first (will be overridden if IEs present) */
        call->ftp_out = sctx->default_ftp;
        call->ftp_in = sctx->default_ftp;
        call->fdp_out = sctx->default_fdp;
        call->fdp_in = sctx->default_fdp;
        call->srv_class = sctx->default_svc_class;

        /* Parse IEs in SETUP */
        size_t offset = 0;
        int bc_valid = -1; /* -1 = missing, 0 = valid, -2 = invalid */
        int bc_seen = 0;
        int called_num_seen = 0;
        while (offset < ie_total_len) {
            u8 ie_id = ie_start[offset];
            if (ie_id & 0x80) {
                /* Single-octet IE per Q.931/X.36: consumes exactly 1 octet (no length octet) */
                offset += 1;
                continue;
            }

            if (offset + 1 >= ie_total_len) break;
            u8 ie_len = ie_start[offset + 1];
            if (offset + 2 + ie_len > ie_total_len) break;
            const u8 *ie_data = &ie_start[offset + 2];

            if (ie_id == Q933_IE_BEARER_CAPABILITY) {
                /* X.36 §10.10.5.2: Use first instance of unrepeatable IE, ignore subsequent duplicates */
                if (!bc_seen) {
                    bc_valid = q933_parse_bearer_capability(ie_data, ie_len);
                    bc_seen = 1;
                }
            } else if (ie_id == Q933_IE_CALLED_NUMBER) {
                /* X.36 §10.10.5.2: Use first instance of unrepeatable IE, ignore subsequent duplicates */
                if (!called_num_seen) {
                    q933_parse_number_ie(ie_data, ie_len, call->called_number, sizeof(call->called_number),
                                         &call->called_number_type, &call->called_number_plan, NULL, NULL);
                    called_num_seen = 1;
                }
            } else if (ie_id == Q933_IE_CALLING_NUMBER) {
                q933_parse_number_ie(ie_data, ie_len, call->calling_number, sizeof(call->calling_number),
                                     &call->calling_number_type, &call->calling_number_plan,
                                     &call->calling_presentation, &call->calling_screening);
            } else if (ie_id == Q933_IE_CALLING_SUBADDR) {
                q933_parse_subaddress(ie_data, ie_len, call->calling_subaddr, sizeof(call->calling_subaddr), &call->calling_subaddr_len);
            } else if (ie_id == Q933_IE_CALLED_SUBADDR) {
                q933_parse_subaddress(ie_data, ie_len, call->called_subaddr, sizeof(call->called_subaddr), &call->called_subaddr_len);
            } else if (ie_id == Q933_IE_DLCI) {
                q933_parse_dlci_ie(ie_data, ie_len, &call->ingress_dlci, NULL);
            } else if (ie_id == Q933_IE_LLCORE_PARAMS) {
                q933_parse_llcore_params(ie_data, ie_len, &call->llcore);
            } else if (ie_id == Q933_IE_LLPROTO_PARAMS) {
                /* Link Layer Protocol Parameters (0x49) per X.36 Table 10-9 */
                (void)ie_data;
            } else if (ie_id == Q933_IE_REVERSE_CHARGE_IND) {
                u8 rev_val = 0;
                q933_parse_reverse_charge_ind(ie_data, ie_len, &rev_val);
                if (rev_val == 0x01) {
                    call->rev_charge_requested = 1;
                }
            } else if (ie_id == Q933_IE_PRIORITY_SVC_CLASS) {
                q933_parse_priority_params(ie_data, ie_len,
                                           &call->ftp_out, &call->ftp_in,
                                           &call->fdp_out, &call->fdp_in,
                                           &call->srv_class);
                call->priority_present = 1;
            } else if (ie_id == Q933_IE_TRANSIT_NETWORK) {
                q933_parse_transit_network_selection(ie_data, ie_len, call->transit_network, sizeof(call->transit_network), &call->transit_network_type_plan);
                call->transit_network_present = 1;
            } else if (ie_id == Q933_IE_LOW_LAYER_COMPAT) {
                call->llc_len = ie_len > 32 ? 32 : ie_len;
                memcpy(call->llc_data, ie_data, call->llc_len);
            } else if (ie_id == Q933_IE_USER_USER) {
                call->uu_len = ie_len > 136 ? 136 : ie_len;
                memcpy(call->uu_data, ie_data, call->uu_len);
            } else {
                /* Unrecognized IE (X.36 §10.6.7.4 / §10.10.7.4) */
                /* Check comprehension required bit (bit 5: 0 = required, 1 = not required) */
                if ((ie_id & 0x10) == 0) {
                    LOG_WARN("SVC [Port %s CRV 0x%04X]: Unrecognized IE 0x%02X with comprehension required (bit 5=0). Rejecting with Cause 99.",
                             port->name, call->call_ref, ie_id);
                    svc_initiate_clearing_before_active_ex(port, sctx, call, Q850_CAUSE_IE_NONEXISTENT_OR_NOT_IMPL, ie_id, 1);
                    return 0;
                } else {
                    LOG_DEBUG("SVC [Port %s CRV 0x%04X]: Ignoring unrecognized IE 0x%02X (comprehension not required)",
                              port->name, call->call_ref, ie_id);
                }
            }

            offset += 2 + ie_len;
        }

        /* Check Bearer Capability validity (X.36 Annex E diagnostic reporting) */
        if (bc_valid == -1) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Bearer Capability missing in SETUP. Rejecting with Cause 96 (Diag: 0x04).", port->name, call->call_ref);
            svc_initiate_clearing_before_active_ex(port, sctx, call, Q850_CAUSE_MANDATORY_IE_MISSING, Q933_IE_BEARER_CAPABILITY, 1);
            return 0;
        } else if (bc_valid == -2) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Bearer Capability invalid in SETUP. Rejecting with Cause 100 (Diag: 0x04).", port->name, call->call_ref);
            svc_initiate_clearing_before_active_ex(port, sctx, call, Q850_CAUSE_INVALID_IE_CONTENTS, Q933_IE_BEARER_CAPABILITY, 1);
            return 0;
        }

        /* Check priority values range validity (X.36 §10.15.2, §10.16.2, §10.17.2) */
        if (call->ftp_out > 15 || call->ftp_in > 15 ||
            call->fdp_out > 7 || call->fdp_in > 7 ||
            call->srv_class > 3) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Priority params out of range (FTP_out=%d FTP_in=%d FDP_out=%d FDP_in=%d SrvCls=%d). Rejecting with Cause 49.",
                     port->name, call->call_ref, call->ftp_out, call->ftp_in, call->fdp_out, call->fdp_in, call->srv_class);
            svc_initiate_clearing_before_active(port, sctx, call, 49); /* Cause 49 = Quality of service not available */
            return 0;
        }

        /* X.36 §10.10.6.1 bit 3: Check mandatory Called Party Number IE */
        if (call->called_number[0] == '\0') {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Mandatory Called Party Number IE missing in SETUP. Rejection with Cause 96 (Diag: 0x70).", port->name, call->call_ref);
            svc_initiate_clearing_before_active_ex(port, sctx, call, Q850_CAUSE_MANDATORY_IE_MISSING, Q933_IE_CALLED_NUMBER, 1);
            return 0;
        }

        /* X.36 §10.7.1.2: Calling Party Number screening and default fallback */
        call->calling_presentation = 0; /* Presentation allowed (00) */
        if (call->calling_number[0] != '\0') {
            vfr_port_t *sub_port = g_vfrs ? svc_find_subscriber(g_vfrs, call->calling_number) : NULL;
            if (sub_port && strcmp(sub_port->name, port->name) == 0) {
                call->calling_screening = 1; /* User provided verified and passed (01) */
            } else {
                /* Invalid calling number provided by DTE. Overwrite with default network address */
                snprintf(call->calling_number, sizeof(call->calling_number), "%s", sctx->subscriber_number);
                call->calling_number_type = sctx->subscriber_type;
                call->calling_number_plan = (sctx->subscriber_type == 2) ? 1 : 3;
                call->calling_screening = 3; /* Network provided */
            }
        } else {
            /* Omitted by DTE. Fill default network address */
            snprintf(call->calling_number, sizeof(call->calling_number), "%s", sctx->subscriber_number);
            call->calling_number_type = sctx->subscriber_type;
            call->calling_number_plan = (sctx->subscriber_type == 2) ? 1 : 3;
            call->calling_screening = 3; /* Network provided */
        }

        /* Translate Called Party Number from alias if registered */
        if (g_vfrs && call->called_number[0] != '\0') {
            char primary_called[SVC_MAX_ADDR_LEN + 1];
            if (svc_get_primary_number(g_vfrs, call->called_number, primary_called, sizeof(primary_called))) {
                snprintf(call->called_number, sizeof(call->called_number), "%s", primary_called);
            }
        }

        /* Check Link Layer Core Minimum Acceptable CIR Negotiation (X.36 §10.7.1.3) */
        if (call->llcore.min_fwd_cir > 0 && call->llcore.fwd_cir < call->llcore.min_fwd_cir) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Requested CIR %u bps below Minimum Acceptable CIR %u bps. Rejecting with Cause 49.",
                     port->name, call->call_ref, call->llcore.fwd_cir, call->llcore.min_fwd_cir);
            svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_QOS_UNAVAILABLE);
            return 0;
        }

        /* DLCI Allocation at Originating UNI (X.36 §10.7.1.4) */
        if (call->ingress_dlci == 0) {
            call->ingress_dlci = svc_dlci_alloc(&sctx->dlci_alloc);
            if (call->ingress_dlci == 0) {
                /* Range exhausted */
                LOG_WARN("SVC [Port %s CRV 0x%04X]: No DLCI available for call setup. Rejection with Cause 34.", port->name, call->call_ref);
                svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_NO_CIRCUIT_AVAILABLE);
                return 0;
            }
        }

        LOG_INFO("SVC [Port %s CRV 0x%04X]: SETUP parsed (Calling='%s', Called='%s', Ingress DLCI=%u)",
                 port->name, call->call_ref, call->calling_number, call->called_number, call->ingress_dlci);

        /* Check for Virtual Switch Self-Number (all-zeros) */
        if (g_vfrs && svc_numbering_is_all_zeros(g_vfrs, call->called_number)) {
            return svc_uni_handle_virtual_switch_call(port, call);
        }

        /* Check for SPVC Crooked SVC-to-PVC Call */
        if (g_vfrs && spvc_handle_incoming_setup(g_vfrs, port, call) == 0) {
            return 0;
        }

        /* Hierarchical Multi-Tier Call Routing */
        const char *tns = call->transit_network_present ? call->transit_network : NULL;
        vfr_port_t *target_port = g_vfrs ? svc_route_lookup_ex(g_vfrs, call->called_number, tns, NULL) : NULL;

        if (target_port) {
            vfr_svc_ctx_t *target_sctx = (vfr_svc_ctx_t *)target_port->svc_ctx;
            if (!target_sctx) {
                LOG_WARN("SVC [Port %s CRV 0x%04X]: Target port %s has no SVC context. Rejecting with Cause 38.",
                         port->name, call->call_ref, target_port->name);
                svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_DESTINATION_OUT_OF_ORDER);
                return 0;
            }

            /* Check Reverse Charging prevention & acceptance policies (X.36 §10.14.2) */
            if (call->rev_charge_requested) {
                if (target_sctx->reverse_charging_prevention) {
                    LOG_INFO("SVC [Port %s CRV 0x%04X]: Target subscriber '%s' has Reverse Charging Prevention enabled. Rejecting call with Cause 29.",
                             port->name, call->call_ref, call->called_number);
                    svc_initiate_clearing_before_active(port, sctx, call, 29); /* Cause 29 = Facility rejected */
                    return 0;
                }
                if (!target_sctx->reverse_charging_acceptance) {
                    LOG_INFO("SVC [Port %s CRV 0x%04X]: Target subscriber '%s' has Reverse Charging Acceptance disabled. Rejecting call with Cause 29.",
                             port->name, call->call_ref, call->called_number);
                    svc_initiate_clearing_before_active(port, sctx, call, 29); /* Cause 29 = Facility rejected */
                    return 0;
                }
            }

            /* Allocate peer CCB on target port */
            vfr_call_t *peer_call = svc_alloc_call_ref(target_sctx, 0, 0);
            if (!peer_call) {
                LOG_WARN("SVC [Port %s CRV 0x%04X]: Peer call allocation failed on %s. Rejection with Cause 34.",
                         port->name, call->call_ref, target_port->name);
                svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_NO_CIRCUIT_AVAILABLE);
                return 0;
            }

            u32 out_dlci = svc_dlci_alloc_ex(&target_sctx->dlci_alloc, target_sctx->dlci_alloc_dir);
            peer_call->ingress_dlci = out_dlci;
            peer_call->egress_dlci = call->ingress_dlci;
            call->egress_dlci = out_dlci;
            peer_call->is_nni = target_sctx->is_nni;
            snprintf(peer_call->ingress_port, sizeof(peer_call->ingress_port), "%s", target_port->name);
            snprintf(peer_call->egress_port, sizeof(peer_call->egress_port), "%s", port->name);
            snprintf(peer_call->called_number, sizeof(peer_call->called_number), "%s", call->called_number);
            snprintf(peer_call->calling_number, sizeof(peer_call->calling_number), "%s", call->calling_number);
            peer_call->called_subaddr_len = call->called_subaddr_len;
            if (call->called_subaddr_len > 0) {
                memcpy(peer_call->called_subaddr, call->called_subaddr, call->called_subaddr_len);
            }
            peer_call->calling_subaddr_len = call->calling_subaddr_len;
            if (call->calling_subaddr_len > 0) {
                memcpy(peer_call->calling_subaddr, call->calling_subaddr, call->calling_subaddr_len);
            }
            peer_call->called_number_type = call->called_number_type;
            peer_call->called_number_plan = call->called_number_plan;
            peer_call->calling_presentation = call->calling_presentation;
            peer_call->calling_screening = call->calling_screening;
            peer_call->llcore = call->llcore;
            peer_call->ftp_out = call->ftp_out;
            peer_call->ftp_in = call->ftp_in;
            peer_call->fdp_out = call->fdp_out;
            peer_call->fdp_in = call->fdp_in;
            peer_call->srv_class = call->srv_class;
            peer_call->priority_present = call->priority_present;
            peer_call->rev_charge_requested = call->rev_charge_requested;
            peer_call->llc_len = call->llc_len;
            if (call->llc_len > 0) {
                memcpy(peer_call->llc_data, call->llc_data, call->llc_len);
            }
            peer_call->uu_len = call->uu_len;
            if (call->uu_len > 0) {
                memcpy(peer_call->uu_data, call->uu_data, call->uu_len);
            }

            if (target_sctx->is_nni) {
                peer_call->call_ident = svc_generate_call_ident();
                const char *net_id = target_sctx->network_id[0] ? target_sctx->network_id : sctx->network_id;
                if (net_id[0] != '\0') {
                    peer_call->tni_list.count = 1;
                    peer_call->tni_list.entries[0].type_plan = target_sctx->network_id_type_plan ? target_sctx->network_id_type_plan : 0x33;
                    snprintf(peer_call->tni_list.entries[0].net_id, sizeof(peer_call->tni_list.entries[0].net_id), "%s", net_id);
                }
            }

            /* Link peer calls */
            call->peer_port = target_port;
            call->peer_call_ref = peer_call->call_ref;
            call->peer_call_ref_flag = peer_call->call_ref_flag;
            peer_call->peer_port = port;
            peer_call->peer_call_ref = call->call_ref;
            peer_call->peer_call_ref_flag = call->call_ref_flag;

            call->egress_dlci = peer_call->egress_dlci;
            snprintf(call->egress_port, sizeof(call->egress_port), "%s", target_port->name);

            LOG_INFO("SVC [Port %s CRV 0x%04X]: Route found -> Egress %s (Assigned Egress DLCI=%u)",
                     port->name, call->call_ref, target_port->name, peer_call->egress_dlci);

            /* Send CALL PROCEEDING to caller, enter Outgoing Call Proceeding state (N3) */
            call->state = SVC_STATE_N3_CALL_PROC;
            u8 tx_buf[512];
            int tx_len = q933_build_call_proceeding(tx_buf, sizeof(tx_buf), call);
            if (tx_len > 0) {
                send_sig_frame(port, tx_buf, tx_len);
                LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent CALL PROCEEDING (DLCI=%u) -> State: %s, T310 started (%u ms)",
                         port->name, call->call_ref, call->ingress_dlci, q933_get_state_name(call->state), sctx->t310_ms);
            }

            /* Start timer T310 on originating DCE (awaiting CONNECT) */
            timer_set(&call->t310, sctx->t310_ms);

            /* Transition terminating side to Call Present (N6/NN6) */
            if (target_sctx->is_nni) {
                peer_call->state = SVC_NNI_STATE_NN6_CALL_PRESENT;
                u8 setup_buf[512];
                int setup_len = q933_build_nni_setup(setup_buf, sizeof(setup_buf), peer_call, target_sctx);
                if (setup_len > 0) {
                    send_sig_frame(target_port, setup_buf, setup_len);
                    LOG_INFO("SVC [Port %s CRV 0x%04X]: Forwarded NNI SETUP to STE (DLCI=%u) -> State: %s, T303 started (%u ms)",
                             target_port->name, peer_call->call_ref, peer_call->egress_dlci, q933_get_state_name(peer_call->state), target_sctx->t303_ms);
                }
                timer_set(&peer_call->t303, target_sctx->t303_ms);
            } else {
                peer_call->state = SVC_STATE_N6_CALL_PRESENT;
                if (port_get_lapf_ctx(target_port, 0)) {
                    u8 setup_buf[512];
                    int setup_len = q933_build_setup(setup_buf, sizeof(setup_buf), peer_call, target_sctx);
                    if (setup_len > 0) {
                        send_sig_frame(target_port, setup_buf, setup_len);
                        LOG_INFO("SVC [Port %s CRV 0x%04X]: Forwarded SETUP to called DTE (DLCI=%u) -> State: %s, T303 started (%u ms)",
                                 target_port->name, peer_call->call_ref, peer_call->egress_dlci, q933_get_state_name(peer_call->state), target_sctx->t303_ms);
                    }
                    timer_set(&peer_call->t303, target_sctx->t303_ms);
                }
            }
        } else {
            /* Destination / Unallocated number error */
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Call to '%s' failed - Unallocated number / No route",
                     port->name, call->call_ref, call->called_number);
            svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_UNALLOCATED_NUMBER);
        }

        break;
    }

    case Q933_MSG_CALL_PROCEEDING: {
        if (!call) return 0;

        if (call->state != SVC_STATE_N6_CALL_PRESENT) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Received CALL PROCEEDING in incompatible state %s. Returning STATUS Cause 98.",
                     port->name, call->call_ref, q933_get_state_name(call->state));
            send_status_incompatible_state(port, sctx, call, Q850_CAUSE_MSG_INCOMPAT_WITH_STATE, hdr.message_type);
            return 0;
        }

        u32 resp_dlci = 0;
        int dlci_found = 0;
        size_t off = 0;
        while (off < ie_total_len) {
            u8 ie_id = ie_start[off];
            if (ie_id & 0x80) {
                off += 1;
                continue;
            }
            if (off + 1 >= ie_total_len) break;
            u8 ie_len = ie_start[off + 1];
            if (off + 2 + ie_len > ie_total_len) break;
            if (ie_id == Q933_IE_DLCI) {
                q933_parse_dlci_ie(&ie_start[off + 2], ie_len, &resp_dlci, NULL);
                dlci_found = 1;
                break;
            }
            off += 2 + ie_len;
        }

        if (call->state == SVC_STATE_N6_CALL_PRESENT) {
            if (!dlci_found) {
                LOG_WARN("SVC [Port %s CRV 0x%04X]: Mandatory DLCI IE missing in CALL PROCEEDING. Clearing with Cause 96 (Diag: 0x19).",
                         port->name, call->call_ref);
                svc_initiate_clearing_before_active_ex(port, sctx, call, Q850_CAUSE_MANDATORY_IE_MISSING, Q933_IE_DLCI, 1);
                if (call->peer_port) {
                    vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                    vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                    if (peer_call) {
                        svc_initiate_clearing_before_active_ex(call->peer_port, peer_sctx, peer_call, Q850_CAUSE_MANDATORY_IE_MISSING, Q933_IE_DLCI, 1);
                    }
                }
                return 0;
            }
            if (resp_dlci != call->ingress_dlci && resp_dlci != call->egress_dlci) {
                LOG_WARN("SVC [Port %s CRV 0x%04X]: DLCI %u in CALL PROCEEDING differs from allocated DLCI. Clearing with Cause 100 (Diag: 0x19).",
                         port->name, call->call_ref, resp_dlci);
                svc_initiate_clearing_before_active_ex(port, sctx, call, Q850_CAUSE_INVALID_IE_CONTENTS, Q933_IE_DLCI, 1);
                if (call->peer_port) {
                    vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                    vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                    if (peer_call) {
                        svc_initiate_clearing_before_active_ex(call->peer_port, peer_sctx, peer_call, Q850_CAUSE_INVALID_IE_CONTENTS, Q933_IE_DLCI, 1);
                    }
                }
                return 0;
            }

            call->state = SVC_STATE_INCOMING_PROC; /* State N9 */
            timer_cancel(&call->t303);
            timer_set(&call->t310, sctx->t310_ms); /* Start T310 awaiting CONNECT */
            LOG_INFO("SVC [Port %s CRV 0x%04X]: Called DTE confirmed CALL PROCEEDING (DLCI=%u) -> State: %s, T310 started (%u ms)",
                     port->name, call->call_ref, resp_dlci, q933_get_state_name(call->state), sctx->t310_ms);
        }
        break;
    }

    case Q933_MSG_CONNECT: {
        if (!call) return 0;

        if (call->state != SVC_STATE_N6_CALL_PRESENT && call->state != SVC_STATE_INCOMING_PROC) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Received CONNECT in incompatible state %s. Returning STATUS Cause 98.",
                     port->name, call->call_ref, q933_get_state_name(call->state));
            send_status_incompatible_state(port, sctx, call, Q850_CAUSE_MSG_INCOMPAT_WITH_STATE, hdr.message_type);
            return 0;
        }

        /* Parse DLCI, Connected Number, and optional CONNECT IEs */
        u32 resp_dlci = 0;
        int dlci_found = 0;
        char conn_num[SVC_MAX_ADDR_LEN + 1] = "";
        u8 conn_type = 1, conn_plan = 3, conn_pres = 0, conn_scr = 3;
        u8 conn_subaddr_len = 0;
        u8 conn_subaddr[SVC_MAX_SUBADDR_LEN] = {0};
        u8 conn_llc_len = 0;
        u8 conn_llc_data[32] = {0};
        u8 conn_uu_len = 0;
        u8 conn_uu_data[136] = {0};

        size_t off = 0;
        while (off < ie_total_len) {
            u8 ie_id = ie_start[off];
            if (ie_id & 0x80) {
                /* Single-octet IE: consumes 1 octet */
                off += 1;
                continue;
            }
            if (off + 1 >= ie_total_len) break;
            u8 ie_len = ie_start[off + 1];
            if (off + 2 + ie_len > ie_total_len) break;

            if (ie_id == Q933_IE_DLCI) {
                q933_parse_dlci_ie(&ie_start[off + 2], ie_len, &resp_dlci, NULL);
                dlci_found = 1;
            } else if (ie_id == Q933_IE_LLCORE_PARAMS) {
                q933_parse_llcore_params(&ie_start[off + 2], ie_len, &call->llcore);
            } else if (ie_id == Q933_IE_CONNECTED_NUMBER) {
                q933_parse_number_ie(&ie_start[off + 2], ie_len, conn_num, sizeof(conn_num),
                                     &conn_type, &conn_plan, &conn_pres, &conn_scr);
            } else if (ie_id == Q933_IE_CONNECTED_SUBADDR) {
                q933_parse_subaddress(&ie_start[off + 2], ie_len, conn_subaddr, sizeof(conn_subaddr), &conn_subaddr_len);
            } else if (ie_id == Q933_IE_LOW_LAYER_COMPAT) {
                conn_llc_len = ie_len > 32 ? 32 : ie_len;
                memcpy(conn_llc_data, &ie_start[off + 2], conn_llc_len);
            } else if (ie_id == Q933_IE_USER_USER) {
                conn_uu_len = ie_len > 136 ? 136 : ie_len;
                memcpy(conn_uu_data, &ie_start[off + 2], conn_uu_len);
            } else {
                /* Unrecognized IE (X.36 §10.6.7.4 / §10.10.7.4) */
                if ((ie_id & 0x10) == 0) {
                    LOG_WARN("SVC [Port %s CRV 0x%04X]: Unrecognized IE 0x%02X in CONNECT with comprehension required (bit 5=0). Rejecting with Cause 99.",
                             port->name, call->call_ref, ie_id);
                    svc_initiate_clearing_before_active_ex(port, sctx, call, Q850_CAUSE_IE_NONEXISTENT_OR_NOT_IMPL, ie_id, 1);
                    return 0;
                } else {
                    LOG_DEBUG("SVC [Port %s CRV 0x%04X]: Ignoring unrecognized IE 0x%02X in CONNECT (comprehension not required)",
                              port->name, call->call_ref, ie_id);
                }
            }
            off += 2 + ie_len;
        }

        /* Update local call with received CONNECT IEs */
        call->connected_subaddr_len = conn_subaddr_len;
        if (conn_subaddr_len > 0) {
            memcpy(call->connected_subaddr, conn_subaddr, conn_subaddr_len);
        }
        call->llc_len = conn_llc_len;
        if (conn_llc_len > 0) {
            memcpy(call->llc_data, conn_llc_data, conn_llc_len);
        }
        call->uu_len = conn_uu_len;
        if (conn_uu_len > 0) {
            memcpy(call->uu_data, conn_uu_data, conn_uu_len);
        }

        /* Validate DLCI */
        if (!dlci_found && call->state == SVC_STATE_N6_CALL_PRESENT) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Mandatory DLCI IE missing in CONNECT. Clearing with Cause 96.", port->name, call->call_ref);
            svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_MANDATORY_IE_MISSING);
            if (call->peer_port) {
                vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                if (peer_call) {
                    svc_initiate_clearing_before_active(call->peer_port, peer_sctx, peer_call, Q850_CAUSE_MANDATORY_IE_MISSING);
                }
            }
            return 0;
        }

        if (dlci_found && resp_dlci != call->ingress_dlci && resp_dlci != call->egress_dlci) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: DLCI %u in CONNECT differs from allocated DLCI. Clearing with Cause 100.", port->name, call->call_ref, resp_dlci);
            svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_INVALID_IE_CONTENTS);
            if (call->peer_port) {
                vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                if (peer_call) {
                    svc_initiate_clearing_before_active(call->peer_port, peer_sctx, peer_call, Q850_CAUSE_INVALID_IE_CONTENTS);
                }
            }
            return 0;
        }

        /* Screen the connected number */
        if (conn_num[0] != '\0') {
            vfr_port_t *sub_port = g_vfrs ? svc_find_subscriber(g_vfrs, conn_num) : NULL;
            if (sub_port && strcmp(sub_port->name, port->name) == 0) {
                conn_scr = 1; /* User provided verified and passed */
            } else {
                snprintf(conn_num, sizeof(conn_num), "%s", sctx->subscriber_number);
                conn_type = sctx->subscriber_type;
                conn_plan = (sctx->subscriber_type == 2) ? 1 : 3;
                conn_scr = 3; /* Network provided fallback */
            }
        } else {
            snprintf(conn_num, sizeof(conn_num), "%s", sctx->subscriber_number);
            conn_type = sctx->subscriber_type;
            conn_plan = (sctx->subscriber_type == 2) ? 1 : 3;
            conn_scr = 3; /* Network provided fallback */
        }

        /* Transition called side to ACTIVE N10 (X.36 §10.7.2.1) */
        timer_cancel(&call->t303);
        timer_cancel(&call->t310);
        call->state = SVC_STATE_N10_ACTIVE;
        LOG_INFO("SVC [Port %s CRV 0x%04X]: Called DTE accepted call with CONNECT (Connected='%s', DLCI=%u) -> Terminating state: %s",
                 port->name, call->call_ref, conn_num, resp_dlci, q933_get_state_name(call->state));

        /* Connect peer (calling) call and propagate CONNECT (X.36 §10.7.1.2) */
        if (call->peer_port) {
            vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
            vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
            if (peer_call) {
                timer_cancel(&peer_call->t310);
                /* Transition peer (calling side) to ACTIVE (N10) per X.36 §10.7.1.2 */
                peer_call->state = SVC_STATE_N10_ACTIVE;
                
                /* Propagate screened connected number details and connected subaddress */
                snprintf(peer_call->connected_number, sizeof(peer_call->connected_number), "%s", conn_num);
                peer_call->connected_subaddr_len = conn_subaddr_len;
                if (conn_subaddr_len > 0) {
                    memcpy(peer_call->connected_subaddr, conn_subaddr, conn_subaddr_len);
                }
                peer_call->connected_number_type = conn_type;
                peer_call->connected_number_plan = conn_plan;
                peer_call->connected_presentation = conn_pres;
                peer_call->connected_screening = conn_scr;

                /* Propagate optional IEs (LLC/UU) to peer CONNECT only if present in incoming CONNECT (X.36 Table 10-3 Note 4/5) */
                peer_call->llc_len = conn_llc_len;
                if (conn_llc_len > 0) {
                    memcpy(peer_call->llc_data, conn_llc_data, conn_llc_len);
                }
                peer_call->uu_len = conn_uu_len;
                if (conn_uu_len > 0) {
                    memcpy(peer_call->uu_data, conn_uu_data, conn_uu_len);
                }

                u8 tx_buf[512];
                int tx_len = 0;
                if (peer_sctx->is_nni) {
                    peer_call->state = SVC_NNI_STATE_NN10_ACTIVE;
                    tx_len = q933_build_nni_connect(tx_buf, sizeof(tx_buf), peer_call);
                    if (tx_len > 0) send_sig_frame(call->peer_port, tx_buf, tx_len);
                } else {
                    tx_len = q933_build_connect(tx_buf, sizeof(tx_buf), peer_call);
                    if (tx_len > 0) send_sig_frame(call->peer_port, tx_buf, tx_len);
                }
                LOG_INFO("SVC [Port %s CRV 0x%04X]: Propagated CONNECT to calling DTE/STE (Connected='%s', DLCI=%u) -> Originating state: %s",
                         call->peer_port->name, peer_call->call_ref, peer_call->connected_number, peer_call->ingress_dlci, q933_get_state_name(peer_call->state));

                /* Create dynamic data PVC forwarding table entries immediately */
                if (g_vfrs) {
                    svc_create_data_pvcs(g_vfrs, call);
                }

                LOG_INFO("SVC Call Established: %s (DLCI %u) <--> %s (DLCI %u) | Call is ACTIVE (N10)",
                         call->ingress_port, call->ingress_dlci, call->egress_port, call->egress_dlci);
            }
        }
        break;
    }

    case Q933_MSG_CONNECT_ACK: {
        if (!call) return 0;
        LOG_DEBUG("SVC [Port %s CRV 0x%04X]: Received legacy/diagnostic CONNECT ACK", port->name, call->call_ref);
        call->state = SVC_STATE_N10_ACTIVE;
        if (g_vfrs) {
            svc_create_data_pvcs(g_vfrs, call);
        }
        break;
    }

    case Q933_MSG_DISCONNECT: {
        if (!call) return 0;

        if (call->state != SVC_STATE_N10_ACTIVE &&
            call->state != SVC_STATE_N12_DISC_IND) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Received DISCONNECT in incompatible state %s. Returning STATUS Cause 98.",
                     port->name, call->call_ref, q933_get_state_name(call->state));
            send_status_incompatible_state(port, sctx, call, Q850_CAUSE_MSG_INCOMPAT_WITH_STATE, hdr.message_type);
            return 0;
        }

        /* Parse Cause code(s) from DISCONNECT if present (X.36 Table 10-5 Note 2) */
        u8 disc_cause = Q850_CAUSE_NORMAL_CLEARING;
        call->clearing_cause_count = 0;
        size_t off = 0;
        while (off < ie_total_len) {
            u8 ie_id = ie_start[off];
            if (ie_id & 0x80) {
                off += 1;
                continue;
            }
            if (off + 1 >= ie_total_len) break;
            u8 ie_len = ie_start[off + 1];
            if (off + 2 + ie_len > ie_total_len) break;
            if (ie_id == Q933_IE_CAUSE && ie_len >= 2) {
                u8 loc = 0, cv = 0, dlen = 0;
                u8 dbuf[32] = {0};
                if (q933_parse_cause_full(&ie_start[off + 2], ie_len, &loc, &cv, dbuf, sizeof(dbuf), &dlen) == 0) {
                    if (call->clearing_cause_count < 2) {
                        u8 idx = call->clearing_cause_count;
                        call->clearing_causes[idx] = cv;
                        call->clearing_locations[idx] = loc;
                        call->clearing_diag_len[idx] = dlen;
                        if (dlen > 0) memcpy(call->clearing_diags[idx], dbuf, dlen);
                        call->clearing_cause_count++;
                    }
                }
            }
            off += 2 + ie_len;
        }
        if (call->clearing_cause_count > 0) {
            disc_cause = call->clearing_causes[0];
        }

        LOG_INFO("SVC [Port %s CRV 0x%04X]: Received DISCONNECT (Cause %u: \"%s\", State: %s). Clearing call.",
                 port->name, call->call_ref, disc_cause, q850_get_cause_str(disc_cause), q933_get_state_name(call->state));

        /* Check clearing collision: cancel T305 if running (X.36 §10.7.4.3) */
        if (call->state == SVC_STATE_N11_DISC_REQ || call->state == SVC_STATE_N12_DISC_IND) {
            timer_cancel(&call->t305);
        }

        /* Tear down active data fast path */
        if (g_vfrs) {
            svc_destroy_data_pvcs(g_vfrs, call);
        }

        /* Clear peer call if active */
        if (call->peer_port) {
            vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
            vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
            if (peer_call) {
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                
                if (peer_sctx->is_nni) {
                    svc_nni_initiate_clearing(call->peer_port, peer_sctx, peer_call, disc_cause);
                } else {
                    u8 peer_tx[128];
                    int peer_len = q933_build_disconnect(peer_tx, sizeof(peer_tx), peer_call, disc_cause);
                    if (peer_len > 0) {
                        send_sig_frame(call->peer_port, peer_tx, peer_len);
                        LOG_INFO("SVC [Port %s CRV 0x%04X]: Propagated DISCONNECT to peer -> State: %s, T305 started (%u ms)",
                                 call->peer_port->name, peer_call->call_ref, q933_get_state_name(SVC_STATE_N12_DISC_IND), peer_sctx->t305_ms);
                    }
                    
                    /* Cancel all peer timers before entering N12 */
                    timer_cancel(&peer_call->t303);
                    timer_cancel(&peer_call->t305);
                    timer_cancel(&peer_call->t308);
                    timer_cancel(&peer_call->t310);

                    /* Enter state N12 (Disconnect Indication), start T305 and await RELEASE */
                    peer_call->state = SVC_STATE_N12_DISC_IND;
                    timer_set(&peer_call->t305, peer_sctx->t305_ms);
                }
            }
        }

        /* Send RELEASE to initiator, start T308, enter state N19 (Release Request) */
        u8 tx_buf[128];
        int tx_len = q933_build_release(tx_buf, sizeof(tx_buf), call, disc_cause);
        if (tx_len > 0) {
            send_sig_frame(port, tx_buf, tx_len);
            LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent RELEASE -> State: %s, T308 started (%u ms)",
                     port->name, call->call_ref, q933_get_state_name(SVC_STATE_N19_REL_REQ), sctx->t308_ms);
        }

        call->state = SVC_STATE_N19_REL_REQ;
        timer_set(&call->t308, sctx->t308_ms);
        break;
    }

    case Q933_MSG_RELEASE: {
        if (!call) return 0;

        /* Parse Cause code(s) from RELEASE if present (X.36 Table 10-5 Note 2) */
        u8 rel_cause = Q850_CAUSE_NORMAL_CLEARING;
        call->clearing_cause_count = 0;
        size_t off = 0;
        while (off < ie_total_len) {
            u8 ie_id = ie_start[off];
            if (ie_id & 0x80) {
                off += 1;
                continue;
            }
            if (off + 1 >= ie_total_len) break;
            u8 ie_len = ie_start[off + 1];
            if (off + 2 + ie_len > ie_total_len) break;
            if (ie_id == Q933_IE_CAUSE && ie_len >= 2) {
                u8 loc = 0, cv = 0, dlen = 0;
                u8 dbuf[32] = {0};
                if (q933_parse_cause_full(&ie_start[off + 2], ie_len, &loc, &cv, dbuf, sizeof(dbuf), &dlen) == 0) {
                    if (call->clearing_cause_count < 2) {
                        u8 idx = call->clearing_cause_count;
                        call->clearing_causes[idx] = cv;
                        call->clearing_locations[idx] = loc;
                        call->clearing_diag_len[idx] = dlen;
                        if (dlen > 0) memcpy(call->clearing_diags[idx], dbuf, dlen);
                        call->clearing_cause_count++;
                    }
                }
            }
            off += 2 + ie_len;
        }
        if (call->clearing_cause_count > 0) {
            rel_cause = call->clearing_causes[0];
        }

        LOG_INFO("SVC [Port %s CRV 0x%04X]: Received RELEASE (Cause %u: \"%s\", State: %s)",
                 port->name, call->call_ref, rel_cause, q850_get_cause_str(rel_cause), q933_get_state_name(call->state));

        /* Check clearing collision (X.36 §10.7.4.3): If in N19, send RELEASE COMPLETE, stop T308, and release */
        if (call->state == SVC_STATE_N19_REL_REQ) {
            timer_cancel(&call->t308);
            if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);

            /* Send RELEASE COMPLETE to acknowledge clearing collision per X.36 §10.7.4.3 */
            u8 tx_buf[128];
            int tx_len = q933_build_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, sctx->crv_len_cfg, rel_cause);
            if (tx_len > 0) {
                send_sig_frame(port, tx_buf, tx_len);
                LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent RELEASE COMPLETE on clearing collision", port->name, call->call_ref);
            }
            
            /* Propagate clearing to peer if it exists */
            if (call->peer_port) {
                vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                if (peer_call) {
                    if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                    if (peer_call->state != SVC_STATE_N19_REL_REQ) {
                        u8 peer_tx[128];
                        int peer_len = q933_build_release(peer_tx, sizeof(peer_tx), peer_call, rel_cause);
                        if (peer_len > 0) {
                            send_sig_frame(call->peer_port, peer_tx, peer_len);
                            LOG_INFO("SVC [Port %s CRV 0x%04X]: Propagated RELEASE to peer -> State: %s, T308 started (%u ms)",
                                     call->peer_port->name, peer_call->call_ref, q933_get_state_name(SVC_STATE_N19_REL_REQ), peer_sctx->t308_ms);
                        }
                        
                        timer_cancel(&peer_call->t303);
                        timer_cancel(&peer_call->t305);
                        timer_cancel(&peer_call->t308);
                        timer_cancel(&peer_call->t310);

                        peer_call->state = SVC_STATE_N19_REL_REQ;
                        peer_call->t308_retries = 0;
                        timer_set(&peer_call->t308, peer_sctx->t308_ms);
                    }
                }
            }
            svc_free_call(sctx, call);
            break;
        }
        
        timer_cancel(&call->t303);
        timer_cancel(&call->t305);
        timer_cancel(&call->t308);
        timer_cancel(&call->t310);

        if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);

        /* Propagate clearing to peer if it exists */
        if (call->peer_port) {
            vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
            vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
            if (peer_call) {
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                if (peer_call->state != SVC_STATE_N19_REL_REQ) {
                    u8 peer_tx[128];
                    int peer_len = q933_build_release(peer_tx, sizeof(peer_tx), peer_call, rel_cause);
                    if (peer_len > 0) {
                        send_sig_frame(call->peer_port, peer_tx, peer_len);
                        LOG_INFO("SVC [Port %s CRV 0x%04X]: Propagated RELEASE to peer -> State: %s, T308 started (%u ms)",
                                 call->peer_port->name, peer_call->call_ref, q933_get_state_name(SVC_STATE_N19_REL_REQ), peer_sctx->t308_ms);
                    }
                    
                    timer_cancel(&peer_call->t303);
                    timer_cancel(&peer_call->t305);
                    timer_cancel(&peer_call->t308);
                    timer_cancel(&peer_call->t310);

                    peer_call->state = SVC_STATE_N19_REL_REQ;
                    peer_call->t308_retries = 0;
                    timer_set(&peer_call->t308, peer_sctx->t308_ms);
                }
            }
        }

        /* Send RELEASE COMPLETE to finalize clearing */
        u8 tx_buf[128];
        int tx_len = q933_build_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, sctx->crv_len_cfg, rel_cause);
        if (tx_len > 0) {
            send_sig_frame(port, tx_buf, tx_len);
            LOG_INFO("SVC [Port %s CRV 0x%04X]: Sent RELEASE COMPLETE -> Call reference cleared to %s",
                     port->name, call->call_ref, q933_get_state_name(SVC_STATE_NULL));
        }

        /* Free local call reference context */
        svc_free_call(sctx, call);
        break;
    }

    case Q933_MSG_RELEASE_COMPLETE: {
        if (!call) return 0;
        timer_cancel(&call->t308);
        if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);
        
        /* Parse Cause code(s) from RELEASE COMPLETE if present (X.36 Table 10-6 Note 2) */
        u8 rel_comp_cause = Q850_CAUSE_NORMAL_UNSPECIFIED; /* Default per §10.10.6.1.1 if absent */
        call->clearing_cause_count = 0;
        size_t off = 0;
        while (off < ie_total_len) {
            u8 ie_id = ie_start[off];
            if (ie_id & 0x80) {
                off += 1;
                continue;
            }
            if (off + 1 >= ie_total_len) break;
            u8 ie_len = ie_start[off + 1];
            if (off + 2 + ie_len > ie_total_len) break;
            if (ie_id == Q933_IE_CAUSE && ie_len >= 2) {
                u8 loc = 0, cv = 0, dlen = 0;
                u8 dbuf[32] = {0};
                if (q933_parse_cause_full(&ie_start[off + 2], ie_len, &loc, &cv, dbuf, sizeof(dbuf), &dlen) == 0) {
                    if (call->clearing_cause_count < 2) {
                        u8 idx = call->clearing_cause_count;
                        call->clearing_causes[idx] = cv;
                        call->clearing_locations[idx] = loc;
                        call->clearing_diag_len[idx] = dlen;
                        if (dlen > 0) memcpy(call->clearing_diags[idx], dbuf, dlen);
                        call->clearing_cause_count++;
                    }
                }
            }
            off += 2 + ie_len;
        }
        if (call->clearing_cause_count > 0) {
            rel_comp_cause = call->clearing_causes[0];
        }

        LOG_INFO("SVC [Port %s CRV 0x%04X]: Received RELEASE COMPLETE (Cause %u: \"%s\", State: %s) -> Call reference cleared to %s",
                 port->name, call->call_ref, rel_comp_cause, q850_get_cause_str(rel_comp_cause), q933_get_state_name(call->state), q933_get_state_name(SVC_STATE_NULL));

        /* Propagate clearing to peer if it exists with exact received rejection cause */
        if (call->peer_port) {
            vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
            vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
            if (peer_call) {
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                if (peer_call->state != SVC_STATE_N19_REL_REQ) {
                    u8 peer_tx[128];
                    int peer_len = q933_build_release(peer_tx, sizeof(peer_tx), peer_call, rel_comp_cause);
                    if (peer_len > 0) {
                        send_sig_frame(call->peer_port, peer_tx, peer_len);
                        LOG_INFO("SVC [Port %s CRV 0x%04X]: Propagated RELEASE (Cause %u: \"%s\") to peer -> State: %s, T308 started (%u ms)",
                                 call->peer_port->name, peer_call->call_ref, rel_comp_cause, q850_get_cause_str(rel_comp_cause), q933_get_state_name(SVC_STATE_N19_REL_REQ), peer_sctx->t308_ms);
                    }
                    
                    timer_cancel(&peer_call->t303);
                    timer_cancel(&peer_call->t305);
                    timer_cancel(&peer_call->t308);
                    timer_cancel(&peer_call->t310);

                    peer_call->state = SVC_STATE_N19_REL_REQ;
                    peer_call->t308_retries = 0;
                    timer_set(&peer_call->t308, peer_sctx->t308_ms);
                }
            }
        }

        svc_free_call(sctx, call);
        break;
    }

    case Q933_MSG_STATUS_ENQUIRY: {
        u8 tx_buf[128];
        u8 cause = Q850_CAUSE_RESPONSE_TO_STATUS_ENQ;
        u8 current_state = call ? call->state : SVC_STATE_NULL;
        int tx_len = q933_build_status_raw(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, hdr.call_ref_len, current_state, cause);
        if (tx_len > 0) {
            send_sig_frame(port, tx_buf, tx_len);
            LOG_INFO("SVC [Port %s CRV 0x%04X]: Received STATUS ENQUIRY -> Responded with STATUS (State: %s)",
                     port->name, hdr.call_ref_value, q933_get_state_name(current_state));
        }
        break;
    }

    case Q933_MSG_STATUS: {
        /* X.36 §10.8.2: STATUS message processing */
        if (call) {
            timer_cancel(&call->t322);
            call->t322_retries = 0;
        }

        u8 peer_state = 0;
        u8 cause_val = 0;
        size_t offset = 0;
        while (offset < ie_total_len) {
            u8 ie_id = ie_start[offset];
            if (ie_id & 0x80) {
                offset += 1;
                continue;
            }
            if (offset + 1 >= ie_total_len) break;
            u8 ie_len = ie_start[offset + 1];
            if (offset + 2 + ie_len > ie_total_len) break;
            if (ie_id == Q933_IE_CALL_STATE) {
                q933_parse_call_state(&ie_start[offset + 2], ie_len, &peer_state);
            } else if (ie_id == Q933_IE_CAUSE && ie_len >= 2) {
                cause_val = ie_start[offset + 3] & 0x7F;
            }
            offset += 2 + ie_len;
        }

        LOG_INFO("SVC [Port %s CRV 0x%04X]: STATUS message received. Reported state=%s (%u), Cause %u (\"%s\"), Local state=%s (%u)",
                 port->name, call->call_ref, q933_get_state_name(peer_state), peer_state,
                 cause_val, q850_get_cause_str(cause_val), q933_get_state_name(call->state), call->state);

        /* Rule 3: If receiver is in Release request state (N19) and STATUS indicates any state except Null, take no action */
        if (call->state == SVC_STATE_N19_REL_REQ) {
            if (peer_state != 0) {
                LOG_INFO("SVC [Port %s CRV 0x%04X]: Local call in Release Request state, peer reported state %s. No action taken.",
                         port->name, call->call_ref, q933_get_state_name(peer_state));
                return 0;
            }
        }

        /* Rule 2: If receiver is in any state except Null, and STATUS indicates Null state, release all resources and move to Null */
        if (peer_state == 0) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: Peer reported Null state. Releasing call locally.", port->name, call->call_ref);
            if (g_vfrs) {
                svc_destroy_data_pvcs(g_vfrs, call);
            }
            /* Propagate clearing to peer if it exists */
            if (call->peer_port) {
                vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                if (peer_call) {
                    if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                    u8 peer_tx[128];
                    int peer_len = q933_build_release(peer_tx, sizeof(peer_tx), peer_call, Q850_CAUSE_NORMAL_CLEARING);
                    if (peer_len > 0) send_sig_frame(call->peer_port, peer_tx, peer_len);
                    peer_call->state = SVC_STATE_N19_REL_REQ;
                    peer_call->t308_retries = 0;
                    timer_set(&peer_call->t308, peer_sctx->t308_ms);
                }
            }
            svc_free_call(sctx, call);
            return 0;
        }

        /* Check for state incompatibility / mismatch */
        int compatible = 0;
        if (call->state == SVC_STATE_N10_ACTIVE && peer_state == 10) compatible = 1;
        else if (call->state == SVC_STATE_N1_CALL_INIT && peer_state == 1) compatible = 1;
        else if (call->state == SVC_STATE_N3_CALL_PROC && peer_state == 3) compatible = 1;
        else if (call->state == SVC_STATE_N6_CALL_PRESENT && peer_state == 6) compatible = 1;
        else if (call->state == SVC_STATE_INCOMING_PROC && peer_state == 9) compatible = 1;
        else if (call->state == SVC_STATE_N11_DISC_REQ && peer_state == 11) compatible = 1;
        else if (call->state == SVC_STATE_N12_DISC_IND && peer_state == 12) compatible = 1;
        else if (call->state == SVC_STATE_N19_REL_REQ && peer_state == 19) compatible = 1;

        if (!compatible) {
            LOG_WARN("SVC [Port %s CRV 0x%04X]: State mismatch detected (Local %s vs Peer %s). Clearing call with Cause 101.",
                     port->name, call->call_ref, q933_get_state_name(call->state), q933_get_state_name(peer_state));
            if (g_vfrs) {
                svc_destroy_data_pvcs(g_vfrs, call);
            }
            
            /* Clear call with Cause 101 */
            u8 tx_buf[128];
            int tx_len = q933_build_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, sctx->crv_len_cfg, Q850_CAUSE_MSG_INCOMPAT_WITH_STATE);
            if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);

            /* Propagate clearing to peer if it exists */
            if (call->peer_port) {
                vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                if (peer_call) {
                    if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                    u8 peer_tx[128];
                    int peer_len = q933_build_release(peer_tx, sizeof(peer_tx), peer_call, Q850_CAUSE_MSG_INCOMPAT_WITH_STATE);
                    if (peer_len > 0) send_sig_frame(call->peer_port, peer_tx, peer_len);
                    peer_call->state = SVC_STATE_N19_REL_REQ;
                    peer_call->t308_retries = 0;
                    timer_set(&peer_call->t308, peer_sctx->t308_ms);
                }
            }
            svc_free_call(sctx, call);
        }
        break;
    }

    default:
        /* X.36 §10.10.4 bit 3: Unexpected or unrecognized message in non-Null state -> Send STATUS Cause 97/101 */
        LOG_WARN("SVC [Port %s CRV 0x%04X]: Unhandled/Unexpected Q.933 message %s (0x%02X) in state %s",
                 port->name, call ? call->call_ref : 0, q933_get_msg_name(hdr.message_type), hdr.message_type,
                 call ? q933_get_state_name(call->state) : "Null (N0)");
        if (call) {
            u8 tx_buf[128];
            int tx_len = q933_build_status(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_MESSAGE_TYPE_NONEXISTENT);
            if (tx_len > 0) send_sig_frame(port, tx_buf, tx_len);
        }
        break;
    }

    return 0;
}

int svc_uni_process_msg(vfr_port_t *port, const u8 *msg_data, size_t len) {
    if (!port || !msg_data || len < 3) return -1;
    mutex_lock(&port->mutex);
    int ret = svc_uni_process_msg_locked(port, msg_data, len);
    mutex_unlock(&port->mutex);
    return ret;
}

/* ============================================================
 * SVC CLI Inspection & Monitoring Commands
 * ============================================================ */

void vfrs_show_svc_calls(vfrs_ctx_t *ctx, FILE *out) {
    if (!ctx) return;
    if (!out) out = stdout;

    fprintf(out, "====================================================================================================================================\n");
    fprintf(out, " ACTIVE SVC CALLS TABLE\n");
    fprintf(out, "====================================================================================================================================\n");
    fprintf(out, "%-10s %-5s %-8s %-6s %-12s %-16s %-10s %-16s %-16s %-20s %-10s\n",
            "PORT", "TYPE", "CRV", "DIR", "LOCAL DLCI", "PEER DEST", "CALL ID", "CALLING NUM", "CALLED NUM", "STATE", "CIR (bps)");
    fprintf(out, "------------------------------------------------------------------------------------------------------------------------------------\n");

    int active_count = 0;
    mutex_lock(&ctx->port_mutex);
    for (int p = 0; p < ctx->port_count; p++) {
        vfr_port_t *port = ctx->ports[p];
        if (!port || !port->svc_ctx) continue;
        vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;

        for (int c = 0; c < SVC_MAX_CALLS_PER_PORT; c++) {
            vfr_call_t *call = &sctx->calls[c];
            if (!call->in_use) continue;

            char local_dlci_str[16], peer_dest_str[64], cid_str[16];
            snprintf(local_dlci_str, sizeof(local_dlci_str), "DLCI %u", call->ingress_dlci);
            snprintf(peer_dest_str, sizeof(peer_dest_str), "%s:%u",
                     call->egress_port[0] ? call->egress_port : "-", call->egress_dlci);
            if (call->call_ident > 0) {
                snprintf(cid_str, sizeof(cid_str), "%u", call->call_ident);
            } else {
                snprintf(cid_str, sizeof(cid_str), "-");
            }

            const char *type_str = (call->is_nni || sctx->is_nni) ? "NNI" : "UNI";
            const char *dir_str = (call->call_ref_flag == 1) ? "ORIG" : "TERM";
            u32 cir = call->llcore.present ? call->llcore.fwd_cir : sctx->default_cir;

            fprintf(out, "%-10s %-5s 0x%04X   %-6s %-12s %-16s %-10s %-16s %-16s %-20s %-10u\n",
                    port->name, type_str, call->call_ref, dir_str, local_dlci_str, peer_dest_str, cid_str,
                    call->calling_number[0] ? call->calling_number : "-",
                    call->called_number[0] ? call->called_number : "-",
                    q933_get_state_name(call->state),
                    cir);
            active_count++;
        }
    }
    mutex_unlock(&ctx->port_mutex);

    if (active_count == 0) {
        fprintf(out, "  (No active SVC calls)\n");
    }
    fprintf(out, "====================================================================================================================================\n\n");
}

void vfrs_show_svc_call_detail(vfrs_ctx_t *ctx, const char *arg, FILE *out) {
    if (!ctx) return;
    if (!out) out = stdout;

    if (!arg || arg[0] == '\0') {
        fprintf(out, "Usage: show svc call <crv> or show svc call <port> <crv>\n");
        return;
    }

    char port_name[32] = "";
    char crv_str[32] = "";
    if (sscanf(arg, "%31s %31s", port_name, crv_str) == 2) {
        /* Both port and CRV provided */
    } else {
        /* Only CRV provided */
        snprintf(crv_str, sizeof(crv_str), "%s", port_name);
        port_name[0] = '\0';
    }

    u16 target_crv = (u16)strtoul(crv_str, NULL, 0);

    int found = 0;
    mutex_lock(&ctx->port_mutex);
    for (int p = 0; p < ctx->port_count; p++) {
        vfr_port_t *port = ctx->ports[p];
        if (!port || !port->svc_ctx) continue;
        if (port_name[0] != '\0' && strcmp(port->name, port_name) != 0) continue;

        vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
        for (int c = 0; c < SVC_MAX_CALLS_PER_PORT; c++) {
            vfr_call_t *call = &sctx->calls[c];
            if (!call->in_use) continue;
            if (target_crv != 0 && call->call_ref != target_crv) continue;

            found++;
            fprintf(out, "================================================================================\n");
            fprintf(out, " SVC CALL DETAILS: Port %s, CRV 0x%04X (%s)\n",
                    port->name, call->call_ref, (call->is_nni || sctx->is_nni) ? "ITU-T X.76 NNI" : "ITU-T X.36 UNI");
            fprintf(out, "================================================================================\n");
            fprintf(out, "  Role / Direction    : %s\n", (call->call_ref_flag == 1) ? "Originating DTE/STE side" : "Terminating DTE/STE side");
            fprintf(out, "  Call State          : %s (State Code: %d)\n", q933_get_state_name(call->state), call->state);
            if (call->call_ident > 0) {
                fprintf(out, "  Call Identification : %u (0x%08X) [IE 0x69]\n", call->call_ident, call->call_ident);
            }
            fprintf(out, "  Ingress Interface   : %s, Ingress DLCI: %u\n", call->ingress_port[0] ? call->ingress_port : port->name, call->ingress_dlci);
            fprintf(out, "  Egress Destination  : %s, Egress DLCI: %u\n", call->egress_port[0] ? call->egress_port : "(none)", call->egress_dlci);
            fprintf(out, "  Calling Address     : '%s' (Type: %d, Plan: %d, Screen: %d, Pres: %d)\n",
                    call->calling_number[0] ? call->calling_number : "(none)",
                    call->calling_number_type, call->calling_number_plan, call->calling_screening, call->calling_presentation);
            fprintf(out, "  Called Address      : '%s' (Type: %d, Plan: %d)\n",
                    call->called_number[0] ? call->called_number : "(none)",
                    call->called_number_type, call->called_number_plan);
            fprintf(out, "  Connected Address   : '%s' (Type: %d, Plan: %d, Screen: %d, Pres: %d)\n",
                    call->connected_number[0] ? call->connected_number : "(none)",
                    call->connected_number_type, call->connected_number_plan, call->connected_screening, call->connected_presentation);
            
            if (call->tni_list.count > 0) {
                fprintf(out, "  Transit Networks    : %u network(s) traversed [TNI Chain]:\n", call->tni_list.count);
                for (u8 t = 0; t < call->tni_list.count; t++) {
                    fprintf(out, "    Hop %u: '%s' (Type/Plan: 0x%02X)\n",
                            t + 1, call->tni_list.entries[t].net_id, call->tni_list.entries[t].type_plan);
                }
            }

            if (call->llcore.present) {
                fprintf(out, "  QoS / LLCORE Params : Fwd CIR=%u, Bwd CIR=%u, Fwd Bc=%u, Bwd Bc=%u, Fwd Be=%u, Bwd Be=%u\n",
                        call->llcore.fwd_cir, call->llcore.bwd_cir,
                        call->llcore.fwd_bc, call->llcore.bwd_bc,
                        call->llcore.fwd_be, call->llcore.bwd_be);
            } else {
                fprintf(out, "  QoS Parameters      : Default Port CIR=%u bps, Bc=%u, Be=%u\n",
                        sctx->default_cir, sctx->default_bc, sctx->default_be);
            }
            fprintf(out, "  Priority / Classes  : FTP out=%d in=%d, FDP out=%d in=%d, Service Class=%d\n",
                    call->ftp_out, call->ftp_in, call->fdp_out, call->fdp_in, call->srv_class);
            fprintf(out, "  Facilities          : Reverse Charging=%s, Transit Network Sel='%s'\n",
                    call->rev_charge_requested ? "Requested" : "None",
                    call->transit_network_present ? call->transit_network : "None");
            if (call->llc_len > 0) {
                fprintf(out, "  Low Layer Compat    : %u bytes encoded\n", call->llc_len);
            }
            if (call->uu_len > 0) {
                fprintf(out, "  User-to-User Info   : %u bytes encoded\n", call->uu_len);
            }
            fprintf(out, "  Fast-Path Data PVCs : %s\n", (call->fwd_pvc && call->rev_pvc) ? "Installed & Operational" : "Not Active");
            fprintf(out, "--------------------------------------------------------------------------------\n\n");
        }
    }
    mutex_unlock(&ctx->port_mutex);

    if (found == 0) {
        fprintf(out, "No matching SVC call found for CRV 0x%04X%s%s\n",
                target_crv, port_name[0] ? " on port " : "", port_name[0] ? port_name : "");
    }
}

void vfrs_show_svc_subscribers(vfrs_ctx_t *ctx, FILE *out) {
    if (!ctx) return;
    if (!out) out = stdout;

    fprintf(out, "================================================================================\n");
    fprintf(out, " REGISTERED SVC SUBSCRIBERS TABLE\n");
    fprintf(out, "================================================================================\n");
    fprintf(out, "%-12s %-22s %-12s %-16s\n", "PORT", "SUBSCRIBER NUMBER", "TYPE", "ALIAS NUMBER");
    fprintf(out, "--------------------------------------------------------------------------------\n");

    int sub_count = 0;
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);
    for (int p = 0; p < ctx->port_count; p++) {
        vfr_port_t *port = ctx->ports[p];
        if (!port || !port->svc_ctx) continue;
        vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;

        if (sctx->subscriber_number[0]) {
            const char *type_str = (sctx->subscriber_type == 2) ? "E.164" : "X.121";
            fprintf(out, "%-12s %-22s %-12s %-16s\n",
                    port->name, sctx->subscriber_number, type_str,
                    sctx->alias_number[0] ? sctx->alias_number : "-");
            sub_count++;
        }
    }
    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);

    if (sub_count == 0) {
        fprintf(out, "  (No registered subscribers)\n");
    }
    fprintf(out, "================================================================================\n\n");
}

void vfrs_show_svc_routes(vfrs_ctx_t *ctx, FILE *out) {
    if (!ctx) return;
    if (!out) out = stdout;

    fprintf(out, "====================================================================================================\n");
    fprintf(out, " SVC NNI ROUTING TABLE\n");
    fprintf(out, "====================================================================================================\n");
    fprintf(out, " Local Switch Address Prefix: DNIC=%u, SGC=%u, SIC=%u\n", ctx->dnic, ctx->sgc, ctx->sic);
    fprintf(out, "----------------------------------------------------------------------------------------------------\n");
    fprintf(out, "%-20s %-16s %-16s %-8s %-12s\n", "PREFIX / ALIAS", "EGRESS PORT", "TNS BINDING", "METRIC", "ROUTE TYPE");
    fprintf(out, "----------------------------------------------------------------------------------------------------\n");

    int route_count = 0;
    for (int i = 0; i < ctx->svc_route_count; i++) {
        const char *tns_str = ctx->svc_routes[i].transit_net_id[0] ? ctx->svc_routes[i].transit_net_id : "-";
        u8 metric = ctx->svc_routes[i].metric ? ctx->svc_routes[i].metric : 10;
        fprintf(out, "%-20s %-16s %-16s %-8u %-12s\n",
                ctx->svc_routes[i].prefix, ctx->svc_routes[i].egress_port, tns_str, metric, "STATIC");
        route_count++;
    }

    if (route_count == 0) {
        fprintf(out, "  (No static NNI routes configured)\n");
    }
    fprintf(out, "====================================================================================================\n\n");
}

void vfrs_show_svc_stats(vfrs_ctx_t *ctx, FILE *out) {
    if (!ctx) return;
    if (!out) out = stdout;

    fprintf(out, "================================================================================\n");
    fprintf(out, " SVC SIGNALING STATISTICS SUMMARY\n");
    fprintf(out, "================================================================================\n");

    int total_ports = 0;
    int svc_ports = 0;
    int total_active_calls = 0;

    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);
    for (int p = 0; p < ctx->port_count; p++) {
        vfr_port_t *port = ctx->ports[p];
        if (!port) continue;
        total_ports++;
        if (!port->svc_ctx) continue;
        vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
        svc_ports++;

        int port_calls = 0;
        for (int c = 0; c < SVC_MAX_CALLS_PER_PORT; c++) {
            if (sctx->calls[c].in_use) {
                port_calls++;
                total_active_calls++;
            }
        }
        fprintf(out, " Port %-10s : SVC Active=%s, Active Calls=%d, Alloc DLCI Free=%d/%d\n",
                port->name, sctx->enabled ? "YES" : "NO", port_calls,
                sctx->dlci_alloc.count, (sctx->dlci_alloc.range_high - sctx->dlci_alloc.range_low + 1));
    }
    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);

    fprintf(out, "--------------------------------------------------------------------------------\n");
    fprintf(out, " Total Switch Ports : %d\n", total_ports);
    fprintf(out, " SVC-Enabled Ports  : %d\n", svc_ports);
    fprintf(out, " Total Active Calls : %d\n", total_active_calls);
    fprintf(out, "================================================================================\n\n");
}
