/*
 * svc_sig_common.c - VFRS SVC Signaling Core Common Engine
 * Virtual Frame Relay Switch
 */

#include "svc_sig_common.h"
#include "svc_sig_uni.h"
#include "svc_sig_nni.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "switching/svc_routing_common.h"

static _Atomic uint32_t g_call_ident_counter = 1;

u32 svc_generate_call_ident(void) {
    u32 val = atomic_fetch_add(&g_call_ident_counter, 1);
    if (val == 0) {
        val = atomic_fetch_add(&g_call_ident_counter, 1);
    }
    return val;
}

int svc_init_port(vfr_port_t *port, u32 dlci_low, u32 dlci_high, u8 crv_len_cfg) {
    return svc_init_port_ex(port, dlci_low, dlci_high, crv_len_cfg, 0, SVC_DLCI_ALLOC_ASCENDING);
}

int svc_init_port_ex(vfr_port_t *port, u32 dlci_low, u32 dlci_high, u8 crv_len_cfg, u8 is_nni, vfr_dlci_alloc_dir_t alloc_dir) {
    if (!port) return -1;

    if (port->svc_ctx) {
        /* Already initialized */
        return 0;
    }

    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)calloc(1, sizeof(vfr_svc_ctx_t));
    if (!sctx) {
        LOG_ERROR("SVC Init: Failed to allocate memory for port %s", port->name);
        return -1;
    }

    sctx->enabled = 1;
    sctx->is_nni = is_nni;
    sctx->dlci_alloc_dir = alloc_dir;
    sctx->crv_len_cfg = crv_len_cfg ? crv_len_cfg : 2;

    /* Initialize DLCI Range Allocator */
    if (dlci_low == 0 || dlci_high == 0) {
        if (port->dlcibit == 23) {
            dlci_low = 512;
            dlci_high = 991;
        } else {
            dlci_low = 512;
            dlci_high = 991;
        }
    }
    svc_dlci_alloc_init_ex(&sctx->dlci_alloc, dlci_low, dlci_high, alloc_dir);

    /* Timer defaults (milliseconds) */
    sctx->t301_ms = g_vfrs ? g_vfrs->default_svc_t301 : 180000;
    sctx->t303_ms = g_vfrs ? g_vfrs->default_svc_t303 : 4000;
    sctx->t305_ms = g_vfrs ? g_vfrs->default_svc_t305 : 30000;
    sctx->t308_ms = g_vfrs ? g_vfrs->default_svc_t308 : 4000;
    sctx->t310_ms = g_vfrs ? g_vfrs->default_svc_t310 : (is_nni ? 40000 : 35000);
    sctx->t316_ms = g_vfrs ? g_vfrs->default_svc_t316 : 120000;
    sctx->t317_ms = g_vfrs ? g_vfrs->default_svc_t317 : (is_nni ? 20000 : 10000);
    sctx->t322_ms = g_vfrs ? g_vfrs->default_svc_t322 : 4000;

    /* QoS defaults */
    sctx->default_cir = g_vfrs ? (g_vfrs->default_cir ? g_vfrs->default_cir : 32000) : 32000;
    sctx->default_bc  = g_vfrs ? (g_vfrs->default_bc ? g_vfrs->default_bc : 32000) : 32000;
    sctx->default_be  = g_vfrs ? g_vfrs->default_be  : 0;
    sctx->default_fmif = (g_vfrs && g_vfrs->default_svc_fmif) ? g_vfrs->default_svc_fmif : FR_MAX_FRAMESZ;
    sctx->default_ftp = (g_vfrs && g_vfrs->default_svc_ftp) ? g_vfrs->default_svc_ftp : 8;
    sctx->default_fdp = (g_vfrs && g_vfrs->default_svc_fdp) ? g_vfrs->default_svc_fdp : 4;
    sctx->default_svc_class = (g_vfrs && g_vfrs->default_svc_class) ? g_vfrs->default_svc_class : 1;

    sctx->reverse_charging_acceptance = 1; /* Default is accepted */
    sctx->reverse_charging_prevention = 0; /* Default is not prevented */

    /* Set switch own network ID (TNI/CNI) */
    if (g_vfrs && g_vfrs->dnic > 0) {
        snprintf(sctx->network_id, sizeof(sctx->network_id), "%04u", g_vfrs->dnic);
        sctx->network_id_type_plan = 0x33; /* International, X.121 DNIC */
    }

    /* Query the global subscriber numbering register to copy pre-configured properties (order-independence) */
    if (g_vfrs) {
        for (int i = 0; i < g_vfrs->svc_subscriber_count; i++) {
            if (strcmp(g_vfrs->svc_subscribers[i].port_name, port->name) == 0) {
                snprintf(sctx->subscriber_number, sizeof(sctx->subscriber_number), "%s", g_vfrs->svc_subscribers[i].x121_number);
                sctx->subscriber_type = g_vfrs->svc_subscribers[i].number_type;
                sctx->reverse_charging_acceptance = g_vfrs->svc_subscribers[i].reverse_charging_acceptance;
                sctx->reverse_charging_prevention = g_vfrs->svc_subscribers[i].reverse_charging_prevention;
                break;
            }
        }
    }

    port->svc_ctx = sctx;
    LOG_INFO("SVC Init: Enabled on port %s (%s, DLCI range %u-%u [%s], CRV len cfg=%u)",
             port->name, is_nni ? "NNI" : "UNI", dlci_low, dlci_high,
             alloc_dir == SVC_DLCI_ALLOC_DESCENDING ? "high-side" : "low-side", crv_len_cfg);
    return 0;
}

void svc_free_port(vfr_port_t *port) {
    if (!port || !port->svc_ctx) return;

    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
    for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
        if (sctx->calls[i].in_use) {
            svc_destroy_data_pvcs(g_vfrs, &sctx->calls[i]);
            sctx->calls[i].in_use = 0;
        }
    }

    free(sctx);
    port->svc_ctx = NULL;
}

vfr_call_t *svc_alloc_call_ref(vfr_svc_ctx_t *svc_ctx, u16 requested_crv, u8 crv_flag) {
    if (!svc_ctx) return NULL;

    /* Find unused CCB */
    vfr_call_t *call = NULL;
    for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
        if (!svc_ctx->calls[i].in_use) {
            call = &svc_ctx->calls[i];
            memset(call, 0, sizeof(*call));
            call->in_use = 1;
            break;
        }
    }

    if (!call) {
        LOG_ERROR("SVC Call Pool: Out of CCBs on port (max %d)", SVC_MAX_CALLS_PER_PORT);
        return NULL;
    }

    if (requested_crv != 0) {
        call->call_ref = requested_crv & 0x7FFF;
        call->call_ref_flag = crv_flag;
    } else {
        /* Generate unique 15-bit CRV using atomic counter in switch context (Issue #2.20) */
        u16 crv = 1;
        if (g_vfrs) {
            int retries = 0;
            do {
                crv = atomic_fetch_add(&g_vfrs->next_crv, 1);
                crv = crv % 0x7FFF;
                if (crv == 0) crv = 1;
                /* Ensure generated CRV is not already active on this port (both flags) */
                if (svc_find_call(svc_ctx, crv, 0) == NULL &&
                    svc_find_call(svc_ctx, crv, 1) == NULL) {
                    break;
                }
                retries++;
            } while (retries < SVC_MAX_CALLS_PER_PORT);
        }

        call->call_ref = crv;
        call->call_ref_flag = 0; /* We originated */
    }

    call->calling_number_type = 0xFF;
    call->calling_number_plan = 0xFF;
    call->called_number_type = 0xFF;
    call->called_number_plan = 0xFF;
    call->connected_number_type = 0xFF;
    call->connected_number_plan = 0xFF;

    call->state = SVC_STATE_NULL;
    return call;
}

vfr_call_t *svc_find_call(vfr_svc_ctx_t *svc_ctx, u16 call_ref, u8 call_ref_flag) {
    if (!svc_ctx) return NULL;

    u16 search_crv = call_ref & 0x7FFF;
    for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
        if (svc_ctx->calls[i].in_use &&
            svc_ctx->calls[i].call_ref == search_crv &&
            svc_ctx->calls[i].call_ref_flag == call_ref_flag) {
            return &svc_ctx->calls[i];
        }
    }

    return NULL;
}

void svc_free_call(vfr_svc_ctx_t *svc_ctx, vfr_call_t *call) {
    if (!svc_ctx || !call) return;

    /* Cancel all timers */
    timer_cancel(&call->t303);
    timer_cancel(&call->t305);
    timer_cancel(&call->t308);
    timer_cancel(&call->t310);
    timer_cancel(&call->t301);
    timer_cancel(&call->t322);

    /* Free DLCIs if allocated */
    if (call->ingress_dlci > 0) {
        svc_dlci_free(&svc_ctx->dlci_alloc, call->ingress_dlci);
        call->ingress_dlci = 0;
    }

    /* Destroy fast-path PVCs if active */
    svc_destroy_data_pvcs(g_vfrs, call);

    call->in_use = 0;
    call->state = SVC_STATE_NULL;
}

int svc_create_data_pvcs(vfrs_ctx_t *ctx, vfr_call_t *call) {
    if (!ctx || !call || !call->in_use) return -1;
    if (call->ingress_dlci == 0 || call->egress_dlci == 0) return -1;

    /* If already created, do not recreate */
    if (call->fwd_pvc != NULL || call->rev_pvc != NULL) {
        return 0;
    }

    vfr_port_t *p_in = vfrs_find_port(ctx, call->ingress_port);
    vfr_port_t *p_out = vfrs_find_port(ctx, call->egress_port);
    if (!p_in || !p_out) return -1;

    vfr_svc_ctx_t *sctx_in = (vfr_svc_ctx_t *)p_in->svc_ctx;
    u32 cir = call->llcore.present ? call->llcore.fwd_cir : (sctx_in ? sctx_in->default_cir : ctx->default_cir);
    u32 bc  = call->llcore.present ? call->llcore.fwd_bc  : (sctx_in ? sctx_in->default_bc  : ctx->default_bc);
    u32 be  = call->llcore.present ? call->llcore.fwd_be  : (sctx_in ? sctx_in->default_be  : ctx->default_be);

    /* Allocate and initialize forward DLCI entry (ingress_port:ingress_dlci -> egress_port:egress_dlci) */
    vfr_dlci_entry_t *entry_fwd = calloc(1, sizeof(vfr_dlci_entry_t));
    if (!entry_fwd) return -1;
    snprintf(entry_fwd->port_in, sizeof(entry_fwd->port_in), "%s", call->ingress_port);
    snprintf(entry_fwd->port_out, sizeof(entry_fwd->port_out), "%s", call->egress_port);
    entry_fwd->dlci_in = call->ingress_dlci;
    entry_fwd->dlci_out = call->egress_dlci;
    entry_fwd->active = 1;
    entry_fwd->vc_type = VC_TYPE_SVC;
    entry_fwd->detail_ptr = call;
    entry_fwd->ftp = call->ftp_out;
    entry_fwd->fdp = call->fdp_out;
    entry_fwd->srvcls = call->srv_class;
    cgst_dlci_tb_init(entry_fwd, p_in, cir, bc, be);

    entry_fwd->link_type = p_in->type;
    dlci_table_add(ctx, entry_fwd);
    port_add_dlci_entry(p_in, entry_fwd);

    /* Allocate and initialize reverse DLCI entry (egress_port:egress_dlci -> ingress_port:ingress_dlci) */
    vfr_dlci_entry_t *entry_rev = calloc(1, sizeof(vfr_dlci_entry_t));
    if (!entry_rev) {
        port_del_dlci_entry(p_in, call->ingress_dlci);
        dlci_table_del(ctx, call->ingress_port, call->ingress_dlci);
        return -1;
    }
    snprintf(entry_rev->port_in, sizeof(entry_rev->port_in), "%s", call->egress_port);
    snprintf(entry_rev->port_out, sizeof(entry_rev->port_out), "%s", call->ingress_port);
    entry_rev->dlci_in = call->egress_dlci;
    entry_rev->dlci_out = call->ingress_dlci;
    entry_rev->active = 1;
    entry_rev->vc_type = VC_TYPE_SVC;
    entry_rev->detail_ptr = call;
    entry_rev->ftp = call->ftp_in;
    entry_rev->fdp = call->fdp_in;
    entry_rev->srvcls = call->srv_class;
    cgst_dlci_tb_init(entry_rev, p_out, cir, bc, be);

    p_out = vfrs_find_port(ctx, call->egress_port);
    if (p_out) {
        entry_rev->link_type = p_out->type;
        dlci_table_add(ctx, entry_rev);
        port_add_dlci_entry(p_out, entry_rev);
    } else {
        free(entry_rev);
        port_del_dlci_entry(p_in, call->ingress_dlci);
        dlci_table_del(ctx, call->ingress_port, call->ingress_dlci);
        return -1;
    }

    entry_fwd->reverse_entry = entry_rev;
    entry_rev->reverse_entry = entry_fwd;

    call->fwd_pvc = entry_fwd;
    call->rev_pvc = entry_rev;

    /* Also link peer CCB if present */
    if (call->peer_port) {
        vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
        vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
        if (peer_call) {
            peer_call->fwd_pvc = entry_rev;
            peer_call->rev_pvc = entry_fwd;
        }
    }

    LOG_INFO("SVC Data path created: %s:%u <-> %s:%u (CIR=%u)",
             call->ingress_port, call->ingress_dlci, call->egress_port, call->egress_dlci, cir);

    return 0;
}

void svc_destroy_data_pvcs(vfrs_ctx_t *ctx, vfr_call_t *call) {
    if (!ctx || !call) return;

    if (call->fwd_pvc == NULL && call->rev_pvc == NULL) {
        return;
    }

    if (call->ingress_dlci > 0 && call->ingress_port[0] != '\0') {
        vfr_port_t *p_in = vfrs_find_port(ctx, call->ingress_port);
        if (p_in) port_del_dlci_entry(p_in, call->ingress_dlci);
        dlci_table_del(ctx, call->ingress_port, call->ingress_dlci);
    }
    if (call->egress_dlci > 0 && call->egress_port[0] != '\0') {
        vfr_port_t *p_out = vfrs_find_port(ctx, call->egress_port);
        if (p_out) port_del_dlci_entry(p_out, call->egress_dlci);
        dlci_table_del(ctx, call->egress_port, call->egress_dlci);
    }

    LOG_INFO("SVC Data path destroyed: %s:%u <-> %s:%u",
             call->ingress_port, call->ingress_dlci, call->egress_port, call->egress_dlci);

    call->fwd_pvc = NULL;
    call->rev_pvc = NULL;

    if (call->peer_port) {
        vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
        vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
        if (peer_call) {
            peer_call->fwd_pvc = NULL;
            peer_call->rev_pvc = NULL;
        }
    }
}

#include "svc_sig_iep.h"
#include "svc_sig_iel.h"
#include "ports/lapf/port_lapf.h"

/* Helper to transmit signaling frame */
static int send_sig_frame_common(vfr_port_t *port, const u8 *msg_data, size_t msg_len) {
    if (!port || !msg_data || msg_len == 0) return -1;

    if (port_get_lapf_ctx(port, 0)) {
        return port_dl_send_data(port, 0, msg_data, msg_len);
    }

    return port_dl_send_unit_data(port, 0, msg_data, msg_len, 0, 0, 0, 0);
}

void svc_poll_timers(vfr_port_t *port) {
    if (!port || !port->svc_ctx) return;

    mutex_lock(&port->mutex);
    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
    for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
        vfr_call_t *call = &sctx->calls[i];
        if (!call->in_use) continue;

        if (timer_is_expired(&call->t303)) {
            timer_cancel(&call->t303);
            call->t303_retries++;
            if (call->t303_retries < 2) {
                LOG_WARN("SVC Timer T303 Expired for CRV 0x%04X on port %s — retransmitting SETUP (attempt %d)",
                         call->call_ref, port->name, call->t303_retries + 1);
                u8 setup_buf[512];
                int setup_len = sctx->is_nni ?
                    q933_build_nni_setup(setup_buf, sizeof(setup_buf), call, sctx) :
                    q933_build_setup(setup_buf, sizeof(setup_buf), call, sctx);
                if (setup_len > 0) send_sig_frame_common(port, setup_buf, setup_len);
                timer_set(&call->t303, sctx->t303_ms);
                continue;
            } else {
                LOG_WARN("SVC Timer T303 Expired second time for CRV 0x%04X on port %s — clearing call",
                         call->call_ref, port->name);
                u8 tx_buf[128];
                int tx_len = sctx->is_nni ?
                    q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY, sctx->network_id, sctx->network_id_type_plan) :
                    q933_build_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, sctx->crv_len_cfg, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY);
                if (tx_len > 0) send_sig_frame_common(port, tx_buf, tx_len);
                
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);
                if (call->peer_port) {
                    vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                    vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                    if (peer_call) {
                        if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                        if (peer_sctx->is_nni) {
                            tx_len = q933_build_nni_release(tx_buf, sizeof(tx_buf), peer_call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY, peer_sctx->network_id, peer_sctx->network_id_type_plan);
                            if (tx_len > 0) send_sig_frame_common(call->peer_port, tx_buf, tx_len);
                            peer_call->state = SVC_NNI_STATE_NN11_REL_REQ;
                            peer_call->t308_retries = 0;
                            timer_set(&peer_call->t308, peer_sctx->t308_ms);
                        } else {
                            tx_len = q933_build_disconnect(tx_buf, sizeof(tx_buf), peer_call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY);
                            if (tx_len > 0) send_sig_frame_common(call->peer_port, tx_buf, tx_len);
                            svc_free_call(peer_sctx, peer_call);
                        }
                    }
                }
                svc_free_call(sctx, call);
                continue;
            }
        } else if (timer_is_expired(&call->t305)) {
            timer_cancel(&call->t305);
            LOG_WARN("SVC Timer T305 Expired for CRV 0x%04X on port %s — sending RELEASE",
                     call->call_ref, port->name);
            
            if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);
            
            u8 tx_buf[128];
            int tx_len = sctx->is_nni ?
                q933_build_nni_release(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY, sctx->network_id, sctx->network_id_type_plan) :
                q933_build_release(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY);
            if (tx_len > 0) send_sig_frame_common(port, tx_buf, tx_len);
            
            call->state = sctx->is_nni ? SVC_NNI_STATE_NN11_REL_REQ : SVC_STATE_N19_REL_REQ;
            call->t308_retries = 0;
            timer_set(&call->t308, sctx->t308_ms);
            continue;
        } else if (timer_is_expired(&call->t308)) {
            timer_cancel(&call->t308);
            call->t308_retries++;
            if (call->t308_retries < 2) {
                LOG_WARN("SVC Timer T308 Expired for CRV 0x%04X on port %s — retransmitting RELEASE (attempt %d)",
                         call->call_ref, port->name, call->t308_retries + 1);
                u8 tx_buf[128];
                int tx_len = sctx->is_nni ?
                    q933_build_nni_release(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY, sctx->network_id, sctx->network_id_type_plan) :
                    q933_build_release(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY);
                if (tx_len > 0) send_sig_frame_common(port, tx_buf, tx_len);
                timer_set(&call->t308, sctx->t308_ms);
                continue;
            } else {
                LOG_WARN("SVC Timer T308 Expired second time for CRV 0x%04X on port %s — releasing resources",
                         call->call_ref, port->name);
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);
                
                if (call->peer_port) {
                    vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                    vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                    if (peer_call) {
                        if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                        svc_free_call(peer_sctx, peer_call);
                    }
                }
                
                svc_free_call(sctx, call);
                continue;
            }
        } else if (timer_is_expired(&call->t310)) {
            timer_cancel(&call->t310);
            LOG_WARN("SVC Timer T310 Expired for CRV 0x%04X on port %s — clearing call",
                     call->call_ref, port->name);
            
            if (sctx->is_nni) {
                u8 tx_buf[128];
                int tx_len = q933_build_nni_release(tx_buf, sizeof(tx_buf), call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY, sctx->network_id, sctx->network_id_type_plan);
                if (tx_len > 0) send_sig_frame_common(port, tx_buf, tx_len);
                call->state = SVC_NNI_STATE_NN11_REL_REQ;
                call->t308_retries = 0;
                timer_set(&call->t308, sctx->t308_ms);
            } else {
                if (call->peer_port) {
                    vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                    vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                    if (peer_call) {
                        svc_initiate_clearing_before_active(call->peer_port, peer_sctx, peer_call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY);
                    }
                }
                svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY);
            }
            continue;
        } else if (timer_is_expired(&call->t301)) {
            timer_cancel(&call->t301);
            LOG_WARN("SVC Timer T301 Expired (Alerting timeout) for CRV 0x%04X on port %s",
                     call->call_ref, port->name);
            
            if (call->peer_port) {
                vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                if (peer_call) {
                    svc_initiate_clearing_before_active(call->peer_port, peer_sctx, peer_call, Q850_CAUSE_NO_USER_RESPONDING);
                }
            }
            svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_NO_USER_RESPONDING);
            continue;
        } else if (timer_is_expired(&call->t322)) {
            timer_cancel(&call->t322);
            call->t322_retries++;
            if (call->t322_retries < 2) {
                LOG_WARN("SVC Timer T322 Expired for CRV 0x%04X on port %s — retransmitting STATUS ENQUIRY (attempt %d)",
                         call->call_ref, port->name, call->t322_retries + 1);
                u8 tx_buf[128];
                int tx_len = q933_build_status_enquiry(tx_buf, sizeof(tx_buf), call);
                if (tx_len > 0) send_sig_frame_common(port, tx_buf, tx_len);
                timer_set(&call->t322, sctx->t322_ms);
                continue;
            } else {
                LOG_WARN("SVC Timer T322 Expired second time for CRV 0x%04X on port %s — clearing call with Cause 41 (Temporary failure)",
                         call->call_ref, port->name);
                /* Release call with Cause 41 (Temporary failure) per X.36 §10.5.5 / X.76 §10.5.5 */
                u8 tx_buf[128];
                int tx_len = sctx->is_nni ?
                    q933_build_nni_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, call->call_ref_len, Q850_CAUSE_TEMPORARY_FAILURE, sctx->network_id, sctx->network_id_type_plan) :
                    q933_build_release_complete(tx_buf, sizeof(tx_buf), call->call_ref, call->call_ref_flag, sctx->crv_len_cfg, Q850_CAUSE_TEMPORARY_FAILURE);
                if (tx_len > 0) send_sig_frame_common(port, tx_buf, tx_len);
                
                /* Destroy active fast-path PVCs */
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);
                
                /* Clear peer call if connected */
                if (call->peer_port) {
                    vfr_svc_ctx_t *peer_sctx = (vfr_svc_ctx_t *)call->peer_port->svc_ctx;
                    vfr_call_t *peer_call = peer_sctx ? svc_find_call(peer_sctx, call->peer_call_ref, call->peer_call_ref_flag) : NULL;
                    if (peer_call) {
                        if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, peer_call);
                        if (peer_sctx->is_nni) {
                            tx_len = q933_build_nni_release(tx_buf, sizeof(tx_buf), peer_call, Q850_CAUSE_TEMPORARY_FAILURE, peer_sctx->network_id, peer_sctx->network_id_type_plan);
                            if (tx_len > 0) send_sig_frame_common(call->peer_port, tx_buf, tx_len);
                            peer_call->state = SVC_NNI_STATE_NN11_REL_REQ;
                            peer_call->t308_retries = 0;
                            timer_set(&peer_call->t308, peer_sctx->t308_ms);
                        } else {
                            tx_len = q933_build_disconnect(tx_buf, sizeof(tx_buf), peer_call, Q850_CAUSE_TEMPORARY_FAILURE);
                            if (tx_len > 0) send_sig_frame_common(call->peer_port, tx_buf, tx_len);
                            svc_free_call(peer_sctx, peer_call);
                        }
                    }
                }
                svc_free_call(sctx, call);
                continue;
            }
        }
    }

    /* Check per-port RESTART timers T316 and T317 (Issue #2.21) */
    if (timer_is_expired(&sctx->t316)) {
        sctx->t316_retries++;
        if (sctx->t316_retries >= 3) { /* N316 = 3 max retries */
            timer_cancel(&sctx->t316);
            LOG_ERROR("SVC Port %s: T316 Expired %d times. Aborting restart procedure.",
                      port->name, sctx->t316_retries);
            sctx->restart_state = 0; /* Abort */
        } else {
            LOG_WARN("SVC Port %s: T316 Expired (retransmit RESTART attempt %d)",
                     port->name, sctx->t316_retries);
            /* Re-send RESTART message */
            u8 tx_buf[128];
            int tx_len = q933_build_restart(tx_buf, sizeof(tx_buf), 0);
            if (tx_len > 0) send_sig_frame_common(port, tx_buf, tx_len);
            timer_set(&sctx->t316, sctx->t316_ms ? sctx->t316_ms : 120000);
        }
    }

    if (timer_is_expired(&sctx->t317)) {
        timer_cancel(&sctx->t317);
        LOG_WARN("SVC Port %s: T317 Expired (Remote failed to acknowledge RESTART) — clearing data PVCs",
                 port->name);
        /* Clear all active calls on this port since restart failed / timed out */
        for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
            vfr_call_t *call = &sctx->calls[i];
            if (call->in_use) {
                if (g_vfrs) svc_destroy_data_pvcs(g_vfrs, call);
                svc_free_call(sctx, call);
            }
        }
        sctx->restart_state = 0;
    }
    mutex_unlock(&port->mutex);
}

void svc_handle_l3_message(vfr_port_t *port, const u8 *data, size_t len) {
    if (!port || !port->svc_ctx || !data || len < 3) return;

    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
    LOG_DEBUG("SVC L3 Recv on %s (%s, len=%zu, byte0=0x%02X)", port->name, sctx->is_nni ? "NNI" : "UNI", len, data[0]);

    if (sctx->is_nni) {
        svc_nni_process_msg(port, data, len);
    } else {
        svc_uni_process_msg(port, data, len);
    }
}

void vfrs_shutdown_svc(vfrs_ctx_t *ctx)
{
    if (!ctx) return;

    LOG_INFO("Gracefully shutting down all active SVC calls");

    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);
    for (int p = 0; p < ctx->port_count; p++) {
        vfr_port_t *port = ctx->ports[p];
        if (port && port->svc_ctx) {
            vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
            for (int i = 0; i < SVC_MAX_CALLS_PER_PORT; i++) {
                vfr_call_t *call = &sctx->calls[i];
                if (call->in_use) {
                    /* Destroy active data PVCs */
                    svc_destroy_data_pvcs(ctx, call);

                    /* Send RELEASE COMPLETE (Cause 102 - Recovery on timer expiry / shutdown) */
                    u8 tx_buf[128];
                    int tx_len = q933_build_release_complete(tx_buf, sizeof(tx_buf), call->call_ref,
                                                             call->call_ref_flag, sctx->crv_len_cfg,
                                                             Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY);
                    if (tx_len > 0) {
                        send_sig_frame_common(port, tx_buf, tx_len);
                    }

                    /* Free local call */
                    svc_free_call(sctx, call);
                }
            }
        }
    }
    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);
}
