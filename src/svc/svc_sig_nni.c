/*
 * svc_sig_nni.c - ITU-T X.76 Clause 10 NNI Call State Machine Implementation
 * Virtual Frame Relay Switch
 */

#include "svc_sig_nni.h"
#include "svc_sig_uni.h"
#include "svc_spvc.h"
#include "vfr.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "switching/svc_routing_common.h"
#include "ports/lapf/port_lapf.h"

extern vfrs_ctx_t *g_vfrs;

void svc_nni_init(void) {
    LOG_INFO("ITU-T X.76 NNI Call State Machine initialized");
}

/* Helper to send an X.76 signaling frame wrapped in LAPF I-frame (or DLCI 0 UI frame if LAPF inactive) */
static int send_sig_frame_nni(vfr_port_t *port, const u8 *msg_data, size_t msg_len) {
    if (!port || !msg_data || msg_len == 0) return -1;

    if (port_get_lapf_ctx(port, 0)) {
        return port_dl_send_data(port, 0, msg_data, msg_len);
    }

    return port_dl_send_unit_data(port, 0, msg_data, msg_len, 0, 0, 0, 0);
}

/* Helper to initiate standard NNI call clearing procedure (ITU-T X.76 §10.6.2) */
void svc_nni_initiate_clearing(vfr_port_t *port, vfr_svc_ctx_t *sctx, vfr_call_t *call, u8 cause) {
    if (!port || !sctx || !call) return;

    const char *cni = sctx->network_id[0] ? sctx->network_id : NULL;
    u8 cni_tp = sctx->network_id_type_plan ? sctx->network_id_type_plan : 0x33;

    LOG_INFO("SVC NNI [Port %s CRV 0x%04X]: Initiating call clearing from state %s with Cause %u (\"%s\")",
             port->name, call->call_ref, q933_get_state_name(call->state), cause, q850_get_cause_str(cause));

    if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);

    u8 tx_buf[256];
    int tx_len = 0;

    if (call->state == SVC_NNI_STATE_NN0_NULL) {
        /* Send RELEASE COMPLETE immediately with CNI */
        tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, cause, cni, cni_tp);
        if (tx_len > 0) {
            send_sig_frame_nni(port, tx_buf, tx_len);
            LOG_INFO("SVC NNI [Port %s CRV 0x%04X]: Sent RELEASE COMPLETE -> State: NN0", port->name, call->call_ref);
        }
        svc_free_call(sctx, call);
    } else {
        /* Send RELEASE, transition to NN11 (Release Request), start T308 */
        tx_len = q933_build_nni_release(tx_buf, sizeof(tx_buf), call, cause, cni, cni_tp);
        if (tx_len > 0) {
            send_sig_frame_nni(port, tx_buf, tx_len);
            LOG_INFO("SVC NNI [Port %s CRV 0x%04X]: Sent RELEASE -> State: NN11, T308 started (%u ms)",
                     port->name, call->call_ref, sctx->t308_ms);
        }

        timer_cancel(&call->t303);
        timer_cancel(&call->t305);
        timer_cancel(&call->t308);
        timer_cancel(&call->t310);
        timer_cancel(&call->t301);
        timer_cancel(&call->t322);

        call->state = SVC_NNI_STATE_NN11_REL_REQ;
        call->t308_retries = 0;
        timer_set(&call->t308, sctx->t308_ms);
    }
}

/* Forward clearing to linked peer leg */
static void forward_clearing_to_peer(vfr_call_t *call, u8 cause, const char *clearing_net_id, u8 cni_type_plan) {
    (void)clearing_net_id;
    (void)cni_type_plan;
    if (!call || !call->peer_port) return;

    vfr_port_t *peer_port = call->peer_port;
    vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)peer_port->svc_ctx;
    if (!peer_sctx) return;

    vfr_call_t *peer_call = svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag);
    if (!peer_call) return;

    LOG_INFO("SVC NNI: Forwarding clearing to peer port %s (CRV 0x%04X, state %s)",
             peer_port->name, peer_call->call_ref, q933_get_state_name(peer_call->state));

    if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);

    if (peer_sctx->is_nni) {
        svc_nni_initiate_clearing(peer_port, peer_sctx, peer_call, cause);
    } else {
        svc_initiate_clearing_before_active(peer_port, peer_sctx, peer_call, cause);
    }
}

/* Main Q.933 / X.76 NNI message processing function */
static int svc_nni_process_msg_locked(vfr_port_t *port, const u8 *msg_data, size_t len) {
    if (!port || !msg_data || len < 3) return -1;

    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
    if (!sctx) return -1;

    q933_msg_header_t hdr;
    int hdr_len = q933_parse_header(msg_data, len, &hdr);
    if (hdr_len < 0) {
        LOG_WARN("SVC NNI [Port %s]: Failed to parse message header", port->name);
        return -1;
    }

    const u8 *ie_start = msg_data + hdr_len;
    size_t ie_total_len = len - hdr_len;

    LOG_INFO("SVC NNI [Port %s CRV 0x%04X]: Received %s message (%zu bytes)",
             port->name, hdr.call_ref_value, q933_get_msg_name(hdr.message_type), len);

    /* Global CRV Handling */
    if (hdr.call_ref_value == 0) {
        if (hdr.message_type == Q933_MSG_RESTART) {
            LOG_INFO("SVC NNI [Port %s]: Received RESTART message (Global CRV).", port->name);
            u8 restart_class = 0; /* Default: all channels / entire interface */
            u32 restart_dlci = 0;
            u8 restart_dlci_len = 0;

            size_t off = 0;
            while (off < ie_total_len) {
                u8 ie_id = ie_start[off];
                if (off + 1 >= ie_total_len) break;
                u8 ie_len = ie_start[off + 1];
                const u8 *ie_data = &ie_start[off + 2];
                if (off + 2 + ie_len > ie_total_len) break;

                if (ie_id == Q933_IE_RESTART_INDICATOR && ie_len >= 1) {
                    restart_class = ie_data[0] & 0x07;
                } else if (ie_id == Q933_IE_DLCI) {
                    q933_parse_dlci_ie(ie_data, ie_len, &restart_dlci, &restart_dlci_len);
                }
                off += 2 + ie_len;
            }

            /* Handle Restart Collision (X.76 §10.6.8.2.3): If we also sent RESTART, stop T316 */
            if (sctx->restart_state == 1) {
                LOG_INFO("SVC NNI [Port %s]: Restart collision detected. Completing restart.", port->name);
                timer_cancel(&sctx->t316);
                sctx->restart_state = 0;
                sctx->t316_retries = 0;
            }

            if (restart_class == 2 && restart_dlci > 0) {
                /* Single specified channel restart */
                for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
                    vfr_call_t *c = &sctx->calls[i];
                    if (c->in_use && (c->ingress_dlci == restart_dlci || c->egress_dlci == restart_dlci)) {
                        if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, c);
                        forward_clearing_to_peer(c, Q850_CAUSE_TEMPORARY_FAILURE, sctx->network_id, sctx->network_id_type_plan);
                        svc_free_call(sctx, c);
                        break;
                    }
                }
            } else {
                /* All virtual channels / entire interface */
                for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
                    vfr_call_t *c = &sctx->calls[i];
                    if (c->in_use) {
                        if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, c);
                        forward_clearing_to_peer(c, Q850_CAUSE_TEMPORARY_FAILURE, sctx->network_id, sctx->network_id_type_plan);
                        svc_free_call(sctx, c);
                    }
                }
            }

            u8 tx_buf[128];
            int tx_len = q933_build_restart_ack(tx_buf, sizeof(tx_buf), restart_class);
            if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
            return 0;
        } else if (hdr.message_type == Q933_MSG_RESTART_ACK) {
            LOG_INFO("SVC NNI [Port %s]: Received RESTART ACK (Global CRV).", port->name);
            timer_cancel(&sctx->t316);
            sctx->restart_state = 0;
            sctx->t316_retries = 0;
            return 0;
        } else if (hdr.message_type == Q933_MSG_STATUS) {
            return 0;
        } else {
            u8 tx_buf[128];
            int tx_len = q933_build_status(tx_buf, sizeof(tx_buf), NULL, Q850_CAUSE_INVALID_CALL_REF);
            if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
            return 0;
        }
    }

    vfr_call_t *call = svc_find_call(sctx, hdr.call_ref_value, hdr.call_ref_flag ^ 1);

    switch (hdr.message_type) {
        case Q933_MSG_SETUP: {
            if (call != NULL) {
                u8 tx_buf[128];
                int tx_len = q933_build_status(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_MSG_NOT_COMPAT_WITH_STATE);
                if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
                return 0;
            }

            call = svc_alloc_call_ref(sctx, hdr.call_ref_value, hdr.call_ref_flag ^ 1);
            if (!call) {
                u8 tx_buf[128];
                int tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, hdr.call_ref_len, Q850_CAUSE_RESOURCE_UNAVAILABLE, sctx->network_id, sctx->network_id_type_plan);
                if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
                return 0;
            }

            call->call_ref_len = hdr.call_ref_len;
            call->call_ref_flag = hdr.call_ref_flag ^ 1;
            call->is_nni = 1;
            snprintf(call->ingress_port, sizeof(call->ingress_port), "%s", port->name);

            size_t offset = 0;
            u8 bearer_cap_present = 0;
            u8 dlci_present = 0;
            u8 called_num_present = 0;
            u8 call_ident_present = 0;

            while (offset < ie_total_len) {
                u8 ie_id = ie_start[offset];
                if (ie_id & 0x80) {
                    /* Single-octet IE: consumes 1 octet */
                    offset += 1;
                    continue;
                }
                if (offset + 1 >= ie_total_len) break;
                u8 ie_len = ie_start[offset + 1];
                const u8 *ie_data = &ie_start[offset + 2];
                if (offset + 2 + ie_len > ie_total_len) break;

                switch (ie_id) {
                    case Q933_IE_BEARER_CAPABILITY:
                        if (q933_parse_bearer_capability(ie_data, ie_len) == 0) bearer_cap_present = 1;
                        break;
                    case Q933_IE_DLCI: {
                        u32 dlci_val = 0;
                        u8 dlci_len_parsed = 0;
                        if (q933_parse_dlci_ie(ie_data, ie_len, &dlci_val, &dlci_len_parsed) == 0) {
                            call->ingress_dlci = dlci_val;
                            dlci_present = 1;
                        }
                        break;
                    }
                    case Q933_IE_LLCORE_PARAMS:
                        q933_parse_llcore_params(ie_data, ie_len, &call->llcore);
                        break;
                    case Q933_IE_REVERSE_CHARGE_IND:
                        q933_parse_reverse_charge_ind(ie_data, ie_len, &call->rev_charge_requested);
                        break;
                    case Q933_IE_TRANSIT_NET_ID: {
                        if (call->tni_list.count >= SVC_MAX_TRANSIT_NETWORKS) {
                            svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_EXCESS_REPETITIONS_OF_IE);
                            return 0;
                        }
                        char net_id[16];
                        u8 tp = 0;
                        if (q933_parse_transit_net_id(ie_data, ie_len, net_id, sizeof(net_id), &tp) == 0) {
                            if (sctx->network_id[0] && strcmp(sctx->network_id, net_id) == 0) {
                                svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_INVALID_IE_CONTENTS);
                                return 0;
                            }
                            int idx = call->tni_list.count;
                            call->tni_list.entries[idx].type_plan = tp;
                            snprintf(call->tni_list.entries[idx].net_id, sizeof(call->tni_list.entries[idx].net_id), "%s", net_id);
                            call->tni_list.count++;
                        }
                        break;
                    }
                    case Q933_IE_CALL_IDENT:
                        if (q933_parse_call_ident(ie_data, ie_len, &call->call_ident) == 0) call_ident_present = 1;
                        break;
                    case Q933_IE_PRIORITY_SVC_CLASS:
                        q933_parse_priority_params(ie_data, ie_len, &call->ftp_out, &call->ftp_in, &call->fdp_out, &call->fdp_in, &call->srv_class);
                        call->priority_present = 1;
                        break;
                    case Q933_IE_CALLING_NUMBER:
                        q933_parse_number_ie(ie_data, ie_len, call->calling_number, sizeof(call->calling_number), &call->calling_number_type, &call->calling_number_plan, &call->calling_presentation, &call->calling_screening);
                        break;
                    case Q933_IE_CALLING_SUBADDR:
                        q933_parse_subaddress(ie_data, ie_len, call->calling_subaddr, sizeof(call->calling_subaddr), &call->calling_subaddr_len);
                        break;
                    case Q933_IE_CALLED_NUMBER:
                        if (q933_parse_number_ie(ie_data, ie_len, call->called_number, sizeof(call->called_number), &call->called_number_type, &call->called_number_plan, NULL, NULL) == 0) {
                            called_num_present = 1;
                        }
                        break;
                    case Q933_IE_CALLED_SUBADDR:
                        q933_parse_subaddress(ie_data, ie_len, call->called_subaddr, sizeof(call->called_subaddr), &call->called_subaddr_len);
                        break;
                    case Q933_IE_TRANSIT_NETWORK:
                        q933_parse_transit_network_selection(ie_data, ie_len, call->transit_network, sizeof(call->transit_network), &call->transit_network_type_plan);
                        call->transit_network_present = 1;
                        break;
                    case Q933_IE_LOW_LAYER_COMPAT:
                        if (ie_len <= sizeof(call->llc_data)) {
                            memcpy(call->llc_data, ie_data, ie_len);
                            call->llc_len = (u8)ie_len;
                        }
                        break;
                    case Q933_IE_USER_USER:
                        if (ie_len <= sizeof(call->uu_data)) {
                            memcpy(call->uu_data, ie_data, ie_len);
                            call->uu_len = (u8)ie_len;
                        }
                        break;
                    case Q933_IE_CALLED_SPVC: {
                        q933_spvc_ie_t spvc_called;
                        if (q933_parse_called_spvc_ie(ie_data, ie_len, &spvc_called) == 0) {
                            call->is_spvc = 1;
                            call->spvc_selection_type = spvc_called.selection_type;
                            call->spvc_target_dlci = spvc_called.dlci;
                        }
                        break;
                    }
                    case Q933_IE_CALLING_SPVC: {
                        u32 calling_dlci = 0;
                        u8 dlci_len_p = 0;
                        if (q933_parse_calling_spvc_ie(ie_data, ie_len, &calling_dlci, &dlci_len_p) == 0) {
                            call->spvc_calling_dlci = calling_dlci;
                        }
                        break;
                    }
                    default:
                        /* Unrecognized IE (X.76 §10.6.7.4 / §10.10.7.4) */
                        if ((ie_id & 0x10) == 0) {
                            LOG_WARN("SVC NNI [Port %s CRV 0x%04X]: Unrecognized IE 0x%02X with comprehension required (bit 5=0). Clearing with Cause 99.",
                                     port->name, call->call_ref, ie_id);
                            svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_IE_NONEXISTENT_OR_NOT_IMPL);
                            return 0;
                        } else {
                            LOG_DEBUG("SVC NNI [Port %s CRV 0x%04X]: Ignoring unrecognized IE 0x%02X (comprehension not required)",
                                      port->name, call->call_ref, ie_id);
                        }
                        break;
                }
                offset += 2 + ie_len;
            }

            if (!bearer_cap_present || !called_num_present || !dlci_present || !call_ident_present) {
                svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_MANDATORY_IE_MISSING);
                return 0;
            }

            if (call->rev_charge_requested && sctx->reverse_charging_prevention) {
                svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_FACILITY_REJECTED);
                return 0;
            }

            /* Check if this incoming SETUP is terminating at a local SPVC endpoint (ITU-T X.76 Annex A.4.2) */
            if (call->is_spvc && g_vfrs) {
                int spvc_rc = spvc_handle_nni_incoming_setup(g_vfrs, port, call);
                if (spvc_rc >= 0) {
                    return 0; /* Handled as local SPVC termination */
                }
            }

            if (sctx->network_id[0] && call->tni_list.count < SVC_MAX_TRANSIT_NETWORKS) {
                int idx = call->tni_list.count;
                call->tni_list.entries[idx].type_plan = sctx->network_id_type_plan ? sctx->network_id_type_plan : 0x33;
                snprintf(call->tni_list.entries[idx].net_id, sizeof(call->tni_list.entries[idx].net_id), "%s", sctx->network_id);
                call->tni_list.count++;
            }

            const char *tns = call->transit_network_present ? call->transit_network : NULL;
            vfr_port_t *p_out = svc_route_lookup_ex(g_vfrs, call->called_number, tns, &call->tni_list);
            if (!p_out || !p_out->svc_ctx) {
                svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_NO_ROUTE_TO_DESTINATION);
                return 0;
            }

            vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)p_out->svc_ctx;
            snprintf(call->egress_port, sizeof(call->egress_port), "%s", p_out->name);

            vfr_call_t *peer_call = svc_alloc_call_ref(peer_sctx, 0, 0);
            if (!peer_call) {
                svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_RESOURCE_UNAVAILABLE);
                return 0;
            }

            call->peer_port = p_out;
            call->peer_call_ref = peer_call->call_ref;
            call->peer_call_ref_flag = peer_call->call_ref_flag;

            peer_call->peer_port = port;
            peer_call->peer_call_ref = call->call_ref;
            peer_call->peer_call_ref_flag = call->call_ref_flag;
            peer_call->is_nni = peer_sctx->is_nni;

            snprintf(peer_call->calling_number, sizeof(peer_call->calling_number), "%s", call->calling_number);
            snprintf(peer_call->called_number, sizeof(peer_call->called_number), "%s", call->called_number);
            peer_call->calling_subaddr_len = call->calling_subaddr_len;
            if (call->calling_subaddr_len > 0) {
                memcpy(peer_call->calling_subaddr, call->calling_subaddr, call->calling_subaddr_len);
            }
            peer_call->called_subaddr_len = call->called_subaddr_len;
            if (call->called_subaddr_len > 0) {
                memcpy(peer_call->called_subaddr, call->called_subaddr, call->called_subaddr_len);
            }
            peer_call->calling_number_type = call->calling_number_type;
            peer_call->calling_number_plan = call->calling_number_plan;
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
            peer_call->call_ident = call->call_ident;
            peer_call->tni_list = call->tni_list;
            peer_call->llc_len = call->llc_len;
            memcpy(peer_call->llc_data, call->llc_data, call->llc_len);
            peer_call->uu_len = call->uu_len;
            memcpy(peer_call->uu_data, call->uu_data, call->uu_len);

            u32 out_dlci = svc_dlci_alloc_ex(&peer_sctx->dlci_alloc, peer_sctx->dlci_alloc_dir);
            if (out_dlci == 0) {
                svc_free_call(peer_sctx, peer_call);
                call->peer_port = NULL;
                svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_NO_CIRCUIT_AVAILABLE);
                return 0;
            }

            snprintf(peer_call->ingress_port, sizeof(peer_call->ingress_port), "%s", p_out->name);
            snprintf(peer_call->egress_port, sizeof(peer_call->egress_port), "%s", port->name);
            peer_call->ingress_dlci = out_dlci;
            peer_call->egress_dlci = call->ingress_dlci;
            call->egress_dlci = out_dlci;

            call->state = SVC_NNI_STATE_NN3_CALL_PROC_SENT;
            u8 tx_buf[512];
            int tx_len = q933_build_nni_call_proceeding(tx_buf, sizeof(tx_buf), call);
            if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);

            if (peer_sctx->is_nni) {
                peer_call->state = SVC_NNI_STATE_NN6_CALL_PRESENT;
                tx_len = q933_build_nni_setup(tx_buf, sizeof(tx_buf), peer_call, peer_sctx);
                if (tx_len > 0) send_sig_frame_nni(p_out, tx_buf, tx_len);
            } else {
                peer_call->state = SVC_STATE_CALL_PRESENT;
                tx_len = q933_build_setup(tx_buf, sizeof(tx_buf), peer_call, peer_sctx);
                if (tx_len > 0) send_sig_frame_nni(p_out, tx_buf, tx_len);
            }

            timer_set(&peer_call->t303, peer_sctx->t303_ms);
            return 0;
        }

        case Q933_MSG_CALL_PROCEEDING: {
            if (!call) return 0;
            if (call->state == SVC_NNI_STATE_NN6_CALL_PRESENT) {
                timer_cancel(&call->t303);
                call->state = SVC_NNI_STATE_NN9_CALL_PROC_RCVD;
                timer_set(&call->t310, sctx->t310_ms);
            } else {
                /* Table IV.1/X.76: Send STATUS Cause 98 on incompatible CALL PROCEEDING */
                u8 tx_buf[128];
                int tx_len = q933_build_status_raw_ex(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, hdr.call_ref_len, call->state, Q850_CAUSE_MSG_NOT_COMPAT_WITH_STATE, hdr.message_type, 1);
                if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
                LOG_WARN("SVC NNI [Port %s CRV 0x%04X]: Incompatible CALL PROCEEDING received in state %s. Sent STATUS Cause 98.",
                         port->name, call->call_ref, q933_get_state_name(call->state));
            }
            return 0;
        }

        case Q933_MSG_CONNECT: {
            if (!call) return 0;

            if (call->state != SVC_NNI_STATE_NN6_CALL_PRESENT && call->state != SVC_NNI_STATE_NN9_CALL_PROC_RCVD) {
                /* Table IV.1/X.76: Send STATUS Cause 98 on incompatible CONNECT */
                u8 tx_buf[128];
                int tx_len = q933_build_status_raw_ex(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, hdr.call_ref_len, call->state, Q850_CAUSE_MSG_NOT_COMPAT_WITH_STATE, hdr.message_type, 1);
                if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
                LOG_WARN("SVC NNI [Port %s CRV 0x%04X]: Incompatible CONNECT received in state %s. Sent STATUS Cause 98.",
                         port->name, call->call_ref, q933_get_state_name(call->state));
                return 0;
            }

            timer_cancel(&call->t303);
            timer_cancel(&call->t310);
            timer_cancel(&call->t322);
            call->t322_retries = 0;

            size_t offset = 0;
            vfr_tni_list_t reflected_tni;
            memset(&reflected_tni, 0, sizeof(reflected_tni));
            call->connected_subaddr_len = 0;
            call->llc_len = 0;
            call->uu_len = 0;

            while (offset < ie_total_len) {
                u8 ie_id = ie_start[offset];
                if (ie_id & 0x80) {
                    /* Single-octet IE: consumes 1 octet */
                    offset += 1;
                    continue;
                }
                if (offset + 1 >= ie_total_len) break;
                u8 ie_len = ie_start[offset + 1];
                const u8 *ie_data = &ie_start[offset + 2];
                if (offset + 2 + ie_len > ie_total_len) break;

                switch (ie_id) {
                    case Q933_IE_LLCORE_PARAMS:
                        q933_parse_llcore_params(ie_data, ie_len, &call->llcore);
                        break;
                    case Q933_IE_CONNECTED_NUMBER:
                        q933_parse_number_ie(ie_data, ie_len, call->connected_number, sizeof(call->connected_number), &call->connected_number_type, &call->connected_number_plan, &call->connected_presentation, &call->connected_screening);
                        break;
                    case Q933_IE_CONNECTED_SUBADDR:
                        q933_parse_subaddress(ie_data, ie_len, call->connected_subaddr, sizeof(call->connected_subaddr), &call->connected_subaddr_len);
                        break;
                    case Q933_IE_TRANSIT_NET_ID: {
                        if (reflected_tni.count < SVC_MAX_TRANSIT_NETWORKS) {
                            char net_id[16];
                            u8 tp = 0;
                            if (q933_parse_transit_net_id(ie_data, ie_len, net_id, sizeof(net_id), &tp) == 0) {
                                int idx = reflected_tni.count;
                                reflected_tni.entries[idx].type_plan = tp;
                                snprintf(reflected_tni.entries[idx].net_id, sizeof(reflected_tni.entries[idx].net_id), "%s", net_id);
                                reflected_tni.count++;
                            }
                        }
                        break;
                    }
                    case Q933_IE_LOW_LAYER_COMPAT:
                        if (ie_len <= sizeof(call->llc_data)) {
                            memcpy(call->llc_data, ie_data, ie_len);
                            call->llc_len = (u8)ie_len;
                        }
                        break;
                    case Q933_IE_USER_USER:
                        if (ie_len <= sizeof(call->uu_data)) {
                            memcpy(call->uu_data, ie_data, ie_len);
                            call->uu_len = (u8)ie_len;
                        }
                        break;
                    case Q933_IE_CALLED_SPVC: {
                        q933_spvc_ie_t spvc_called;
                        if (q933_parse_called_spvc_ie(ie_data, ie_len, &spvc_called) == 0) {
                            call->is_spvc = 1;
                            call->spvc_selection_type = spvc_called.selection_type;
                            call->spvc_target_dlci = spvc_called.dlci;
                        }
                        break;
                    }
                    default:
                        /* Unrecognized IE (X.76 §10.6.7.4 / §10.10.7.4) */
                        if ((ie_id & 0x10) == 0) {
                            LOG_WARN("SVC NNI [Port %s CRV 0x%04X]: Unrecognized IE 0x%02X with comprehension required (bit 5=0). Clearing with Cause 99.",
                                     port->name, call->call_ref, ie_id);
                            svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_IE_NONEXISTENT_OR_NOT_IMPL);
                            return 0;
                        } else {
                            LOG_DEBUG("SVC NNI [Port %s CRV 0x%04X]: Ignoring unrecognized IE 0x%02X (comprehension not required)",
                                      port->name, call->call_ref, ie_id);
                        }
                        break;
                }
                offset += 2 + ie_len;
            }

            call->state = SVC_NNI_STATE_NN10_ACTIVE;

            if (call->peer_port) {
                vfr_port_t *peer_port = call->peer_port;
                vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)peer_port->svc_ctx;
                if (peer_sctx) {
                    vfr_call_t *peer_call = svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag);
                    if (peer_call) {
                        peer_call->llcore = call->llcore;
                        snprintf(peer_call->connected_number, sizeof(peer_call->connected_number), "%s", call->connected_number);
                        peer_call->connected_subaddr_len = call->connected_subaddr_len;
                        if (call->connected_subaddr_len > 0) {
                            memcpy(peer_call->connected_subaddr, call->connected_subaddr, call->connected_subaddr_len);
                        }
                        peer_call->connected_number_type = call->connected_number_type;
                        peer_call->connected_number_plan = call->connected_number_plan;
                        peer_call->connected_presentation = call->connected_presentation;
                        peer_call->connected_screening = call->connected_screening;
                        peer_call->llc_len = call->llc_len;
                        if (call->llc_len > 0) {
                            memcpy(peer_call->llc_data, call->llc_data, call->llc_len);
                        }
                        peer_call->uu_len = call->uu_len;
                        if (call->uu_len > 0) {
                            memcpy(peer_call->uu_data, call->uu_data, call->uu_len);
                        }
                        if (reflected_tni.count > 0) {
                            peer_call->tni_list = reflected_tni;
                        }

                        u8 tx_buf[512];
                        int tx_len = 0;
                        if (peer_sctx->is_nni) {
                            peer_call->state = SVC_NNI_STATE_NN10_ACTIVE;
                            tx_len = q933_build_nni_connect(tx_buf, sizeof(tx_buf), peer_call);
                            if (tx_len > 0) send_sig_frame_nni(peer_port, tx_buf, tx_len);
                        } else {
                            peer_call->state = SVC_STATE_ACTIVE;
                            tx_len = q933_build_connect(tx_buf, sizeof(tx_buf), peer_call);
                            if (tx_len > 0) send_sig_frame_nni(peer_port, tx_buf, tx_len);
                        }

                        if (g_vfrs) {
                            svc_create_data_pvcs(g_vfrs, call);
                        }
                    }
                }
            } else if (g_vfrs) {
                spvc_handle_nni_connect(g_vfrs, port, call);
            }
            return 0;
        }

        case Q933_MSG_RELEASE: {
            u8 cause = Q850_CAUSE_NORMAL_CLEARING;
            char cni[16] = {0};
            u8 cni_tp = 0;

            size_t offset = 0;
            while (offset < ie_total_len) {
                u8 ie_id = ie_start[offset];
                if (ie_id & 0x80) {
                    offset += 1;
                    continue;
                }
                if (offset + 1 >= ie_total_len) break;
                u8 ie_len = ie_start[offset + 1];
                const u8 *ie_data = &ie_start[offset + 2];
                if (offset + 2 + ie_len > ie_total_len) break;

                if (ie_id == Q933_IE_CAUSE) {
                    q933_parse_cause(ie_data, ie_len, NULL, &cause);
                } else if (ie_id == Q933_IE_CLEARING_NET_ID) {
                    q933_parse_clearing_net_id(ie_data, ie_len, cni, sizeof(cni), &cni_tp);
                }
                offset += 2 + ie_len;
            }

            if (!call) {
                u8 tx_buf[128];
                int tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag, hdr.call_ref_len, 0, sctx->network_id, sctx->network_id_type_plan);
                if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
                return 0;
            }

            if (call->state == SVC_NNI_STATE_NN11_REL_REQ) {
                timer_cancel(&call->t308);
                u8 tx_buf[128];
                int tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, 0, sctx->network_id, sctx->network_id_type_plan);
                if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
                if (g_vfrs) spvc_handle_nni_release_cause(g_vfrs, port, call, cause);
                svc_free_call(sctx, call);
                return 0;
            }

            u8 tx_buf[128];
            int tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, 0, sctx->network_id, sctx->network_id_type_plan);
            if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);

            if (g_vfrs) {
                svc_destroy_data_pvcs(g_vfrs, call);
                spvc_handle_nni_release_cause(g_vfrs, port, call, cause);
            }
            forward_clearing_to_peer(call, cause, cni[0] ? cni : sctx->network_id, cni_tp);
            svc_free_call(sctx, call);
            return 0;
        }

        case Q933_MSG_RELEASE_COMPLETE: {
            if (!call) return 0;
            if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);

            u8 cause = Q850_CAUSE_NORMAL_UNSPECIFIED;
            char cni[16] = {0};
            u8 cni_tp = 0;

            size_t offset = 0;
            while (offset < ie_total_len) {
                u8 ie_id = ie_start[offset];
                if (ie_id & 0x80) {
                    offset += 1;
                    continue;
                }
                if (offset + 1 >= ie_total_len) break;
                u8 ie_len = ie_start[offset + 1];
                const u8 *ie_data = &ie_start[offset + 2];
                if (offset + 2 + ie_len > ie_total_len) break;

                if (ie_id == Q933_IE_CAUSE) {
                    q933_parse_cause(ie_data, ie_len, NULL, &cause);
                } else if (ie_id == Q933_IE_CLEARING_NET_ID) {
                    q933_parse_clearing_net_id(ie_data, ie_len, cni, sizeof(cni), &cni_tp);
                }
                offset += 2 + ie_len;
            }

            if (g_vfrs) {
                spvc_handle_nni_release_cause(g_vfrs, port, call, cause);
            }
            forward_clearing_to_peer(call, cause, cni[0] ? cni : sctx->network_id, cni_tp);
            svc_free_call(sctx, call);
            return 0;
        }

        case Q933_MSG_STATUS_ENQUIRY: {
            u8 current_state = call ? call->state : SVC_NNI_STATE_NN0_NULL;
            u8 tx_buf[128];
            int tx_len = q933_build_status_raw(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag ^ 1, hdr.call_ref_len, current_state, Q850_CAUSE_RESPONSE_TO_STATUS_ENQ);
            if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
            return 0;
        }

        case Q933_MSG_STATUS: {
            if (call) {
                timer_cancel(&call->t322);
                call->t322_retries = 0;
            }

            u8 reported_state = 0xFF;
            u8 cause = 0;
            size_t offset = 0;

            while (offset < ie_total_len) {
                u8 ie_id = ie_start[offset];
                if (ie_id & 0x80) {
                    offset += 1;
                    continue;
                }
                if (offset + 1 >= ie_total_len) break;
                u8 ie_len = ie_start[offset + 1];
                const u8 *ie_data = &ie_start[offset + 2];
                if (offset + 2 + ie_len > ie_total_len) break;

                if (ie_id == Q933_IE_CALL_STATE) {
                    q933_parse_call_state(ie_data, ie_len, &reported_state);
                } else if (ie_id == Q933_IE_CAUSE) {
                    q933_parse_cause(ie_data, ie_len, NULL, &cause);
                }
                offset += 2 + ie_len;
            }

            if (!call && reported_state != SVC_NNI_STATE_NN0_NULL) {
                u8 tx_buf[128];
                int tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), hdr.call_ref_value, hdr.call_ref_flag, hdr.call_ref_len, Q850_CAUSE_INVALID_CALL_REF, sctx->network_id, sctx->network_id_type_plan);
                if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
            } else if (call && reported_state == SVC_NNI_STATE_NN0_NULL) {
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);
                forward_clearing_to_peer(call, Q850_CAUSE_NORMAL_CLEARING, sctx->network_id, sctx->network_id_type_plan);
                svc_free_call(sctx, call);
            }
            return 0;
        }

        default: {
            u8 tx_buf[128];
            int tx_len = q933_build_status(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_MESSAGE_TYPE_NONEXISTENT);
            if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);
            return 0;
        }
    }
}

int svc_nni_process_msg(vfr_port_t *port, const u8 *msg_data, size_t len) {
    if (!port) return -1;
    mutex_lock(&port->mutex);
    int rc = svc_nni_process_msg_locked(port, msg_data, len);
    mutex_unlock(&port->mutex);
    return rc;
}

int svc_nni_initiate_restart(vfr_port_t *port) {
    if (!port || !port->svc_ctx) return -1;

    mutex_lock(&port->mutex);
    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
    LOG_INFO("SVC NNI [Port %s]: Initiating interface RESTART procedure", port->name);

    /* Clear all calls on this port and notify peers */
    for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
        vfr_call_t *c = &sctx->calls[i];
        if (c->in_use) {
            if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, c);
            forward_clearing_to_peer(c, Q850_CAUSE_TEMPORARY_FAILURE, sctx->network_id, sctx->network_id_type_plan);
            svc_free_call(sctx, c);
        }
    }

    sctx->restart_state = 1;
    sctx->t316_retries = 0;
    u32 t316_interval = sctx->t316_ms ? sctx->t316_ms : 120000;
    timer_set(&sctx->t316, t316_interval);

    u8 tx_buf[128];
    int tx_len = q933_build_restart(tx_buf, sizeof(tx_buf), 0);
    if (tx_len > 0) send_sig_frame_nni(port, tx_buf, tx_len);

    mutex_unlock(&port->mutex);
    return 0;
}

