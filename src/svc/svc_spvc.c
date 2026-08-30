/*
 * svc_spvc.c - ITU-T X.76 Annex A Switched PVC (SPVC) & Service Interworking Engine
 * Virtual Frame Relay Switch
 */

#include "svc_spvc.h"
#include "switching/svc_routing_common.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "svc/svc_sig_uni.h"
#include "svc/svc_sig_nni.h"
#include <time.h>

static vfr_spvc_entry_t *g_spvc_list = NULL;
static mutex_t           g_spvc_mutex;

void spvc_init(vfrs_ctx_t *ctx)
{
    (void)ctx;
    mutex_init(&g_spvc_mutex);
}

int spvc_add_inter_pvc(vfrs_ctx_t *ctx, const char *local_port, u32 local_dlci,
                       const char *target_addr, u32 target_dlci,
                       u32 cir, u32 bc, u32 be)
{
    if (!ctx || !local_port || !target_addr) return -1;

    vfr_spvc_entry_t *spvc = calloc(1, sizeof(vfr_spvc_entry_t));
    if (!spvc) return -1;

    spvc->mode = SPVC_MODE_INTER_PVC;
    strncpy(spvc->local_port, local_port, sizeof(spvc->local_port) - 1);
    spvc->local_dlci = local_dlci;
    strncpy(spvc->target_addr, target_addr, sizeof(spvc->target_addr) - 1);
    spvc->target_dlci = target_dlci;
    spvc->cir = cir;
    spvc->bc = bc;
    spvc->be = be;
    spvc->state = SPVC_STATE_IDLE;
    spvc->retry_interval_ms = 5000;

    mutex_lock(&g_spvc_mutex);
    spvc->next = g_spvc_list;
    g_spvc_list = spvc;
    mutex_unlock(&g_spvc_mutex);

    LOG_INFO("SPVC: Registered Inter-PVC SPVC on %s DLCI %u -> target '%s' DLCI %u (CIR=%u)",
             local_port, local_dlci, target_addr, target_dlci, cir);
    return 0;
}

int spvc_add_svc_to_pvc(vfrs_ctx_t *ctx, const char *local_port, u32 local_dlci,
                        const char *svc_addr)
{
    if (!ctx || !local_port || !svc_addr) return -1;

    vfr_spvc_entry_t *spvc = calloc(1, sizeof(vfr_spvc_entry_t));
    if (!spvc) return -1;

    spvc->mode = SPVC_MODE_SVC_TO_PVC;
    strncpy(spvc->local_port, local_port, sizeof(spvc->local_port) - 1);
    spvc->local_dlci = local_dlci;
    strncpy(spvc->svc_addr, svc_addr, sizeof(spvc->svc_addr) - 1);
    spvc->state = SPVC_STATE_IDLE;

    mutex_lock(&g_spvc_mutex);
    spvc->next = g_spvc_list;
    g_spvc_list = spvc;
    mutex_unlock(&g_spvc_mutex);

    /* Register subscriber address in switch directory pointing to local_port */
    svc_numbering_add(ctx, local_port, "x121", svc_addr, NULL, NULL, 1, 0);

    LOG_INFO("SPVC: Registered SVC-to-PVC Crooked Interworking on %s DLCI %u at svc_addr '%s'",
             local_port, local_dlci, svc_addr);
    return 0;
}

int spvc_add_pvc_to_svc(vfrs_ctx_t *ctx, const char *local_port, u32 local_dlci,
                        const char *target_addr,
                        u32 cir, u32 bc, u32 be)
{
    if (!ctx || !local_port || !target_addr) return -1;

    vfr_spvc_entry_t *spvc = calloc(1, sizeof(vfr_spvc_entry_t));
    if (!spvc) return -1;

    spvc->mode = SPVC_MODE_PVC_TO_SVC;
    strncpy(spvc->local_port, local_port, sizeof(spvc->local_port) - 1);
    spvc->local_dlci = local_dlci;
    strncpy(spvc->target_addr, target_addr, sizeof(spvc->target_addr) - 1);
    spvc->cir = cir;
    spvc->bc = bc;
    spvc->be = be;
    spvc->state = SPVC_STATE_IDLE;
    spvc->retry_interval_ms = 5000;

    mutex_lock(&g_spvc_mutex);
    spvc->next = g_spvc_list;
    g_spvc_list = spvc;
    mutex_unlock(&g_spvc_mutex);

    LOG_INFO("SPVC: Registered PVC-to-SVC Crooked Auto-Dial on %s DLCI %u -> target '%s' (CIR=%u)",
             local_port, local_dlci, target_addr, cir);
    return 0;
}

int spvc_is_spvc_dlci(vfrs_ctx_t *ctx, const char *port_name, u32 dlci)
{
    if (!ctx || !port_name) return 0;
    int found = 0;
    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;
    while (spvc) {
        if (strcmp(spvc->local_port, port_name) == 0 && spvc->local_dlci == dlci) {
            found = 1;
            break;
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
    return found;
}

int spvc_is_dlci_active(vfrs_ctx_t *ctx, const char *port_name, u32 dlci)
{
    if (!ctx || !port_name) return 0;
    int is_act = 0;
    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;
    while (spvc) {
        if (strcmp(spvc->local_port, port_name) == 0 && spvc->local_dlci == dlci) {
            if (spvc->state == SPVC_STATE_CONNECTED) {
                is_act = 1;
            }
            break;
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
    return is_act;
}

static void spvc_trigger_call(vfrs_ctx_t *ctx, vfr_spvc_entry_t *spvc)
{
    if (!ctx || !spvc) return;

    /* Perform LPM routing lookup to find egress port */
    vfr_port_t *egress_port = svc_route_lookup(ctx, spvc->target_addr);
    if (!egress_port || egress_port->status != PORT_STATUS_UP || !egress_port->svc_ctx) {
        LOG_DEBUG("SPVC: Egress port not UP or SVC not ready for target '%s' (local %s:%u)",
                  spvc->target_addr, spvc->local_port, spvc->local_dlci);
        spvc->state = SPVC_STATE_RETRY_WAIT;
        spvc->last_attempt_ms = (u64)(time(NULL) * 1000);
        return;
    }

    vfr_svc_ctx_t *egress_sctx = (vfr_svc_ctx_t *)egress_port->svc_ctx;
    vfr_call_t *call = svc_alloc_call_ref(egress_sctx, 0, 0);
    if (!call) {
        LOG_ERROR("SPVC: Failed to allocate call control block on egress port %s", egress_port->name);
        spvc->state = SPVC_STATE_RETRY_WAIT;
        spvc->last_attempt_ms = (u64)(time(NULL) * 1000);
        return;
    }

    /* Allocate egress DLCI */
    u32 allocated_dlci = svc_dlci_alloc_ex(&egress_sctx->dlci_alloc, egress_sctx->dlci_alloc_dir);
    if (allocated_dlci == 0) {
        LOG_ERROR("SPVC: Failed to allocate DLCI on egress port %s", egress_port->name);
        svc_free_call(egress_sctx, call);
        spvc->state = SPVC_STATE_RETRY_WAIT;
        spvc->last_attempt_ms = (u64)(time(NULL) * 1000);
        return;
    }

    call->state = SVC_STATE_CALL_INITIATED;
    call->ingress_dlci = allocated_dlci;
    snprintf(call->ingress_port, sizeof(call->ingress_port), "%s", egress_port->name);
    snprintf(call->egress_port, sizeof(call->egress_port), "%s", spvc->local_port);
    call->egress_dlci = spvc->local_dlci;
    snprintf(call->called_number, sizeof(call->called_number), "%.*s", (int)(sizeof(call->called_number) - 1), spvc->target_addr);
    call->is_nni = (egress_port->type == PORT_TYPE_NNI);

    /* SPVC Protocol Flags (ITU-T X.76 Annex A.4.1) */
    call->is_spvc = 1;
    call->spvc_selection_type = (spvc->target_dlci > 0) ? 2 /* Specific DLCI */ : 1 /* Any DLCI */;
    call->spvc_target_dlci = spvc->target_dlci;
    call->spvc_calling_dlci = spvc->local_dlci;

    /* Populate QoS */
    call->llcore.present = 1;
    call->llcore.fwd_cir = spvc->cir;
    call->llcore.bwd_cir = spvc->cir;
    call->llcore.fwd_bc = spvc->bc;
    call->llcore.bwd_bc = spvc->bc;
    call->llcore.fwd_be = spvc->be;
    call->llcore.bwd_be = spvc->be;

    /* Build SETUP buffer */
    u8 setup_buf[512];
    int setup_len;

    if (call->is_nni) {
        setup_len = q933_build_nni_setup(setup_buf, sizeof(setup_buf), call, egress_sctx);
    } else {
        setup_len = q933_build_setup(setup_buf, sizeof(setup_buf), call, egress_sctx);
    }

    if (setup_len > 0) {
        lapf_send_l3(egress_port, 0, setup_buf, setup_len);
        timer_set(&call->t303, egress_sctx->t303_ms);

        spvc->state = SPVC_STATE_SETUP_SENT;
        spvc->active_crv = call->call_ref;
        snprintf(spvc->nni_port, sizeof(spvc->nni_port), "%s", egress_port->name);
        spvc->nni_dlci = allocated_dlci;

        LOG_INFO("SPVC: Sent SETUP on %s for SPVC %s:%u -> '%s' (CRV=0x%04X, DLCI=%u)",
                 egress_port->name, spvc->local_port, spvc->local_dlci, spvc->target_addr,
                 call->call_ref, allocated_dlci);
    } else {
        svc_free_call(egress_sctx, call);
        spvc->state = SPVC_STATE_RETRY_WAIT;
        spvc->last_attempt_ms = (u64)(time(NULL) * 1000);
    }
}

void spvc_poll_timers(vfrs_ctx_t *ctx)
{
    if (!ctx) return;

    u64 now_ms = (u64)(time(NULL) * 1000);

    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;

    while (spvc) {
        if (spvc->mode == SPVC_MODE_INTER_PVC || spvc->mode == SPVC_MODE_PVC_TO_SVC) {
            vfr_port_t *lport = vfrs_find_port(ctx, spvc->local_port);
            int local_active = 0;

            if (lport && lport->status == PORT_STATUS_UP) {
                vfr_dlci_entry_t *entry = port_lookup_dlci(lport, spvc->local_dlci);
                if (entry) {
                    /* Access interface is physically UP and LMI is verified */
                    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)lport->lmi_ctx;
                    if (!lmi || !lmi->dce_link_down) {
                        local_active = 1;
                    }
                }
            }

            if (spvc->state == SPVC_STATE_IDLE && local_active) {
                spvc_trigger_call(ctx, spvc);
            } else if (spvc->state == SPVC_STATE_RETRY_WAIT && local_active) {
                if (now_ms - spvc->last_attempt_ms >= spvc->retry_interval_ms) {
                    spvc_trigger_call(ctx, spvc);
                }
            } else if (!local_active && spvc->state == SPVC_STATE_CONNECTED) {
                /* Local PVC dropped: tear down call per X.76 Annex A.4.5.3 */
                LOG_INFO("SPVC: Local PVC %s:%u down, clearing active call on %s",
                         spvc->local_port, spvc->local_dlci, spvc->nni_port);
                vfr_port_t *nport = vfrs_find_port(ctx, spvc->nni_port);
                if (nport && nport->svc_ctx) {
                    vfr_svc_ctx_t *nsctx = (vfr_svc_ctx_t *)nport->svc_ctx;
                    vfr_call_t *call = svc_find_call(nsctx, spvc->active_crv, 0);
                    if (call) {
                        svc_nni_initiate_clearing(nport, nsctx, call, Q850_CAUSE_PERM_CONN_OUT_OF_SERVICE);
                    }
                }
                spvc->state = SPVC_STATE_IDLE;
            }
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
}

int spvc_handle_incoming_setup(vfrs_ctx_t *ctx, vfr_port_t *ingress_port, vfr_call_t *call)
{
    if (!ctx || !ingress_port || !call) return -1;

    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;

    while (spvc) {
        if (spvc->mode == SPVC_MODE_SVC_TO_PVC && strcmp(spvc->svc_addr, call->called_number) == 0) {
            /* Matched SVC-to-PVC crooked interworking endpoint */
            vfr_port_t *pvc_port = vfrs_find_port(ctx, spvc->local_port);
            if (!pvc_port || pvc_port->status != PORT_STATUS_UP) {
                mutex_unlock(&g_spvc_mutex);
                return -1;
            }

            vfr_dlci_entry_t *pvc_entry = port_lookup_dlci(pvc_port, spvc->local_dlci);
            if (!pvc_entry) {
                mutex_unlock(&g_spvc_mutex);
                return -1;
            }

            /* Bind call to local PVC */
            snprintf(call->egress_port, sizeof(call->egress_port), "%s", spvc->local_port);
            call->egress_dlci = spvc->local_dlci;
            spvc->state = SPVC_STATE_CONNECTED;
            spvc->active_crv = call->call_ref;
            snprintf(spvc->nni_port, sizeof(spvc->nni_port), "%s", ingress_port->name);
            spvc->nni_dlci = call->ingress_dlci;

            /* Add bidirectional fast-path forwarding entry */
            vfrs_add_pvc(ctx, ingress_port->name, call->ingress_dlci,
                         pvc_port->name, spvc->local_dlci,
                         spvc->cir, spvc->bc, spvc->be, 0, 0, 0);

            /* Return CONNECT */
            u8 conn_buf[256];
            int conn_len = q933_build_connect(conn_buf, sizeof(conn_buf), call);
            if (conn_len > 0) {
                lapf_send_l3(ingress_port, 0, conn_buf, conn_len);
            }
            call->state = SVC_STATE_ACTIVE;

            LOG_INFO("SPVC: Connected Crooked SVC-to-PVC call: %s DLCI %u <-> %s DLCI %u (svc_addr='%s')",
                     ingress_port->name, call->ingress_dlci, pvc_port->name, spvc->local_dlci, spvc->svc_addr);

            mutex_unlock(&g_spvc_mutex);
            return 0;
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
    return -1;
}

int spvc_handle_nni_incoming_setup(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call)
{
    if (!ctx || !nni_port || !call) return -1;

    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;

    while (spvc) {
        int addr_match = 0;
        if (spvc->mode == SPVC_MODE_INTER_PVC) {
            /* Match destination subscriber number / switch address */
            if (strcmp(spvc->target_addr, call->called_number) == 0 ||
                (spvc->target_dlci > 0 && spvc->target_dlci == call->spvc_target_dlci)) {
                addr_match = 1;
            }
        }

        if (addr_match) {
            /* Validate called endpoint availability (ITU-T X.76 Annex A.4.2.3 & A.4.2.4) */
            vfr_port_t *lport = vfrs_find_port(ctx, spvc->local_port);
            if (!lport || lport->status != PORT_STATUS_UP) {
                LOG_WARN("SPVC: Local called port '%s' not UP for incoming SPVC SETUP. Rejecting with Cause 27.",
                         spvc->local_port);
                mutex_unlock(&g_spvc_mutex);
                u8 tx_buf[128];
                int tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, Q850_CAUSE_DESTINATION_OUT_OF_ORDER, NULL, 0);
                if (tx_len > 0) lapf_send_l3(nni_port, 0, tx_buf, tx_len);
                return 1;
            }

            vfr_dlci_entry_t *pentry = port_lookup_dlci(lport, spvc->local_dlci);
            if (!pentry) {
                LOG_WARN("SPVC: Local called DLCI %u not provisioned on port %s. Rejecting with Cause 21.",
                         spvc->local_dlci, lport->name);
                mutex_unlock(&g_spvc_mutex);
                u8 tx_buf[128];
                int tx_len = q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, Q850_CAUSE_CALL_REJECTED, NULL, 0);
                if (tx_len > 0) lapf_send_l3(nni_port, 0, tx_buf, tx_len);
                return 1;
            }

            /* Cross-connect data plane */
            spvc->state = SPVC_STATE_CONNECTED;
            spvc->active_crv = call->call_ref;
            snprintf(spvc->nni_port, sizeof(spvc->nni_port), "%s", nni_port->name);
            spvc->nni_dlci = call->ingress_dlci;

            vfrs_add_pvc(ctx, nni_port->name, call->ingress_dlci,
                         lport->name, spvc->local_dlci,
                         spvc->cir, spvc->bc, spvc->be, 0, 0, 0);

            /* Return NNI CONNECT with Called Party SPVC IE (Assigned DLCI = spvc->local_dlci) */
            call->is_spvc = 1;
            call->spvc_selection_type = 3; /* Assigned DLCI */
            call->spvc_target_dlci = spvc->local_dlci;
            call->state = SVC_NNI_STATE_NN10_ACTIVE;

            u8 conn_buf[512];
            int conn_len = q933_build_nni_connect(conn_buf, sizeof(conn_buf), call);
            if (conn_len > 0) {
                lapf_send_l3(nni_port, 0, conn_buf, conn_len);
            }

            LOG_INFO("SPVC: Accepted and Connected NNI SPVC: %s DLCI %u <-> %s DLCI %u (CRV=0x%04X)",
                     nni_port->name, call->ingress_dlci, lport->name, spvc->local_dlci, call->call_ref);

            mutex_unlock(&g_spvc_mutex);
            return 0;
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
    return -1;
}

int spvc_handle_nni_connect(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call)
{
    if (!ctx || !nni_port || !call) return -1;

    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;

    while (spvc) {
        if (spvc->active_crv == call->call_ref && strcmp(spvc->nni_port, nni_port->name) == 0) {
            /* Verify Called Party SPVC IE assigned DLCI (X.76 Annex A.4.3) */
            if (call->is_spvc && spvc->target_dlci > 0 && call->spvc_target_dlci > 0) {
                if (call->spvc_target_dlci != spvc->target_dlci) {
                    LOG_WARN("SPVC: Assigned DLCI %u mismatch with requested %u. Clearing with Cause 21.",
                             call->spvc_target_dlci, spvc->target_dlci);
                    mutex_unlock(&g_spvc_mutex);
                    svc_nni_initiate_clearing(nni_port, (vfr_svc_ctx_t *)nni_port->svc_ctx, call, Q850_CAUSE_CALL_REJECTED);
                    return -1;
                }
            }

            spvc->state = SPVC_STATE_CONNECTED;
            vfr_port_t *lport = vfrs_find_port(ctx, spvc->local_port);

            if (lport) {
                /* Dynamically bridge access PVC and NNI SVC in fast-path forwarding tables */
                vfrs_add_pvc(ctx, lport->name, spvc->local_dlci,
                             nni_port->name, call->ingress_dlci,
                             spvc->cir, spvc->bc, spvc->be, 0, 0, 0);

                LOG_INFO("SPVC: Established bidirectional data path: %s DLCI %u <-> %s DLCI %u",
                         lport->name, spvc->local_dlci, nni_port->name, call->ingress_dlci);
            }
            mutex_unlock(&g_spvc_mutex);
            return 0;
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
    return -1;
}

int spvc_handle_nni_release(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call)
{
    return spvc_handle_nni_release_cause(ctx, nni_port, call, Q850_CAUSE_NORMAL_CLEARING);
}

int spvc_handle_nni_release_cause(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call, u8 cause)
{
    if (!ctx || !nni_port || !call) return -1;

    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;

    while (spvc) {
        if (spvc->active_crv == call->call_ref && strcmp(spvc->nni_port, nni_port->name) == 0) {
            vfr_port_t *lport = vfrs_find_port(ctx, spvc->local_port);
            if (lport) {
                vfrs_del_pvc(ctx, lport->name, spvc->local_dlci);
                vfrs_del_pvc(ctx, nni_port->name, call->ingress_dlci);
            }

            spvc->state = SPVC_STATE_RETRY_WAIT;
            spvc->last_attempt_ms = (u64)(time(NULL) * 1000);
            spvc->active_crv = 0;
            spvc->last_cause = cause;

            /* Retry timing based on standard ITU-T X.76 Annex A.4.4 */
            if (cause == Q850_CAUSE_NO_CIRCUIT_AVAILABLE || cause == 34) {
                /* Cause 34: Random seconds backoff */
                spvc->retry_interval_ms = (u32)(5000 + (rand() % 10000));
            } else if (cause == Q850_CAUSE_DESTINATION_OUT_OF_ORDER || cause == 27) {
                /* Cause 27: Minimum 60s backoff */
                spvc->retry_interval_ms = 60000;
            } else {
                spvc->retry_interval_ms = 5000;
            }

            LOG_INFO("SPVC: Cleared data path for %s DLCI %u (Cause %u, retry in %u ms)",
                     spvc->local_port, spvc->local_dlci, cause, spvc->retry_interval_ms);
            mutex_unlock(&g_spvc_mutex);
            return 0;
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
    return -1;
}

void spvc_notify_pvc_status_change(vfrs_ctx_t *ctx, const char *port_name, u32 dlci, int is_active)
{
    if (!ctx || !port_name) return;

    mutex_lock(&g_spvc_mutex);
    vfr_spvc_entry_t *spvc = g_spvc_list;
    while (spvc) {
        if (strcmp(spvc->local_port, port_name) == 0 && spvc->local_dlci == dlci) {
            if (!is_active && spvc->state == SPVC_STATE_CONNECTED) {
                /* Local access PVC dropped: tear down call per X.76 Annex A.4.5.3 with Cause 39 */
                LOG_INFO("SPVC: Async notification - Local PVC %s:%u down, clearing active call on %s",
                         spvc->local_port, spvc->local_dlci, spvc->nni_port);
                vfr_port_t *nport = vfrs_find_port(ctx, spvc->nni_port);
                if (nport && nport->svc_ctx) {
                    vfr_svc_ctx_t *nsctx = (vfr_svc_ctx_t *)nport->svc_ctx;
                    vfr_call_t *call = svc_find_call(nsctx, spvc->active_crv, 0);
                    if (call) {
                        svc_nni_initiate_clearing(nport, nsctx, call, Q850_CAUSE_PERM_CONN_OUT_OF_SERVICE);
                    }
                }
                spvc->state = SPVC_STATE_RETRY_WAIT;
                spvc->last_attempt_ms = (u64)(time(NULL) * 1000);
            } else if (is_active && (spvc->state == SPVC_STATE_IDLE || spvc->state == SPVC_STATE_RETRY_WAIT)) {
                LOG_INFO("SPVC: Async notification - Local PVC %s:%u up, triggering immediate auto-dial",
                         spvc->local_port, spvc->local_dlci);
                spvc_trigger_call(ctx, spvc);
            }
        }
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
}

void vfrs_show_spvcs(vfrs_ctx_t *ctx, FILE *out)
{
    (void)ctx;
    if (!out) out = stdout;

    mutex_lock(&g_spvc_mutex);
    fprintf(out, "\n%-10s %-10s %-8s %-16s %-12s %-12s %-10s\n",
            "Mode", "Port", "DLCI", "Target/SVC-Addr", "Target-DLCI", "State", "Active-DLCI");
    fprintf(out, "--------------------------------------------------------------------------------\n");

    vfr_spvc_entry_t *spvc = g_spvc_list;
    while (spvc) {
        const char *mode_str = (spvc->mode == SPVC_MODE_INTER_PVC) ? "INTER-PVC" :
                               ((spvc->mode == SPVC_MODE_SVC_TO_PVC) ? "SVC->PVC" : "PVC->SVC");
        const char *state_str = (spvc->state == SPVC_STATE_CONNECTED) ? "ACTIVE" :
                                ((spvc->state == SPVC_STATE_SETUP_SENT) ? "SETUP" :
                                 ((spvc->state == SPVC_STATE_RETRY_WAIT) ? "RETRY" : "IDLE"));

        const char *addr_str = (spvc->mode == SPVC_MODE_SVC_TO_PVC) ? spvc->svc_addr : spvc->target_addr;
        char tgt_dlci_str[16];
        if (spvc->mode == SPVC_MODE_INTER_PVC) {
            snprintf(tgt_dlci_str, sizeof(tgt_dlci_str), "%u", spvc->target_dlci);
        } else {
            snprintf(tgt_dlci_str, sizeof(tgt_dlci_str), "-");
        }

        char act_dlci_str[64];
        if (spvc->state == SPVC_STATE_CONNECTED) {
            snprintf(act_dlci_str, sizeof(act_dlci_str), "%s:%u", spvc->nni_port, spvc->nni_dlci);
        } else {
            snprintf(act_dlci_str, sizeof(act_dlci_str), "-");
        }

        fprintf(out, "%-10s %-10s %-8u %-16s %-12s %-12s %-10s\n",
                mode_str, spvc->local_port, spvc->local_dlci, addr_str, tgt_dlci_str, state_str, act_dlci_str);
        spvc = spvc->next;
    }
    mutex_unlock(&g_spvc_mutex);
}
