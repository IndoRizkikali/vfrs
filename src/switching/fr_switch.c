/*
 * fr_switch.c - Frame Relay Switching Engine
 * VFRS - Virtual Frame Relay Switch
 */

#include "vfr.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "svc_routing_common.h"
#include "svc/svc_spvc.h"
#include "ports/lapf/port_lapf.h"

/* ============================================================
 * Context Management
 * ============================================================ */

vfrs_ctx_t *g_vfrs = NULL;

vfrs_ctx_t *vfrs_create(const char *swid)
{
    vfrs_ctx_t *ctx;

    ctx = calloc(1, sizeof(vfrs_ctx_t));
    if (!ctx) {
        LOG_ERROR("Failed to allocate VFRS context");
        return NULL;
    }

    if (swid && *swid) {
        strncpy(ctx->swid, swid, sizeof(ctx->swid) - 1);
    } else {
        strcpy(ctx->swid, "vfrs0");
    }

    mutex_init(&ctx->dlci_mutex);
    mutex_init(&ctx->pvc_mutex);
    mutex_init(&ctx->svc_mutex);
    mutex_init(&ctx->port_mutex);
    mutex_init(&ctx->mcast_mutex);
    ctx->mcast_groups = NULL;
    spsc_queue_init(&ctx->sig_queue);
    ctx->slow_path_thread = 0;

    /* Default parameters */
    ctx->default_n391 = 6;
    ctx->default_n392 = 3;
    ctx->default_n393 = 4;
    ctx->default_t391 = 10;
    ctx->default_t392 = 15;
    ctx->default_n392_dte = 3;
    ctx->default_n393_dte = 4;

    ctx->default_lapf_k = 0;  /* 0 = auto-derive from port access rate at init time */
    ctx->default_lapf_n200 = 3;
    ctx->default_lapf_n201 = 1598;
    ctx->default_lapf_t200 = 1500;
    ctx->default_lapf_t203 = 30000;

    ctx->default_svc_t301 = 180000;
    ctx->default_svc_t303 = 4000;
    ctx->default_svc_t305 = 30000;
    ctx->default_svc_t308 = 4000;
    ctx->default_svc_t310 = 35000;
    ctx->default_svc_t316 = 120000;
    ctx->default_svc_t317 = 10000;
    ctx->default_svc_t322 = 4000;

    ctx->default_cir = 0;
    ctx->default_bc = 0;
    ctx->default_be = 0;
    ctx->access_rate = 0;

    /* Default SVC numbering parameters */
    ctx->dcc = 100;
    ctx->dnic = 1000;
    ctx->sgclen = 1;
    ctx->sgc = 1;
    ctx->siclen = 1;
    ctx->sic = 1;
    ctx->subnumlen = 4;

    svc_numbering_init(ctx);
    svc_route_init(ctx);
    ctx->next_crv = 1;

    g_vfrs = ctx;
    LOG_INFO("VFRS context created: %s", ctx->swid);

    return ctx;
}

void vfrs_destroy(vfrs_ctx_t *ctx)
{
    if (!ctx) return;

    /* Collect and clear ports under ctx->port_mutex to avoid holding it during port_free */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);
    int count = ctx->port_count;
    vfr_port_t *ports_to_free[MAX_PORTS];
    for (int i = 0; i < count; i++) {
        ports_to_free[i] = ctx->ports[i];
        ctx->ports[i] = NULL;
    }
    ctx->port_count = 0;
    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);
    mutex_destroy(&ctx->port_mutex);

    /* Free ports outside ctx->port_mutex to avoid lock inversion deadlocks */
    for (int i = 0; i < count; i++) {
        if (ports_to_free[i]) {
            port_free(ports_to_free[i]);
        }
    }

    /* Free all PVCs */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);
    for (int i = 0; i < PVC_HASH_SIZE; i++) {
        vfr_pvc_detail_t *pvc = ctx->pvc_table[i];
        while (pvc) {
            vfr_pvc_detail_t *next = pvc->next;
            free(pvc);
            pvc = next;
        }
        ctx->pvc_table[i] = NULL;
    }
    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
    mutex_destroy(&ctx->pvc_mutex);

    /* Free all DLCI entries */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_DLCI);
    mutex_lock(&ctx->dlci_mutex);
    for (int i = 0; i < PVC_HASH_SIZE; i++) {
        vfr_dlci_entry_t *entry = ctx->dlci_table[i];
        while (entry) {
            vfr_dlci_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
        ctx->dlci_table[i] = NULL;
    }
    mutex_unlock(&ctx->dlci_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_DLCI);
    mutex_destroy(&ctx->dlci_mutex);

    /* Clean SVC mutex */
    mutex_destroy(&ctx->svc_mutex);

    /* Free all multicast groups */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    vfr_mcast_group_t *g = ctx->mcast_groups;
    while (g) {
        vfr_mcast_group_t *next_g = g->next;
        vfr_mcast_member_t *m = g->members;
        while (m) {
            vfr_mcast_member_t *next_m = m->next;
            free(m);
            m = next_m;
        }
        free(g);
        g = next_g;
    }
    ctx->mcast_groups = NULL;
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
    mutex_destroy(&ctx->mcast_mutex);

    /* Free global capture */
    if (ctx->global_capture) {
        pcap_writer_close(ctx->global_capture);
        free(ctx->global_capture);
        ctx->global_capture = NULL;
    }

    spsc_queue_destroy(&ctx->sig_queue);

    LOG_INFO("VFRS context destroyed: %s", ctx->swid);
    free(ctx);
    g_vfrs = NULL;
}

/* ============================================================
 * Global DLCI Table Operations
 * ============================================================ */

vfr_dlci_entry_t *dlci_table_lookup(vfrs_ctx_t *ctx, const char *port, u32 dlci)
{
    if (!ctx || !port) return NULL;
    u32 hash = DLCI_HASH(port, dlci);
    ASSERT_LOCK_ORDER(LOCK_LEVEL_DLCI);
    mutex_lock(&ctx->dlci_mutex);
    vfr_dlci_entry_t *entry = ctx->dlci_table[hash];
    while (entry) {
        if (entry->dlci_in == dlci && strcmp(entry->port_in, port) == 0) {
            mutex_unlock(&ctx->dlci_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_DLCI);
            return entry;
        }
        entry = entry->next;
    }
    mutex_unlock(&ctx->dlci_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_DLCI);
    return NULL;
}

int dlci_table_add(vfrs_ctx_t *ctx, vfr_dlci_entry_t *entry)
{
    if (!ctx || !entry) return -1;
    u32 hash = DLCI_HASH(entry->port_in, entry->dlci_in);
    ASSERT_LOCK_ORDER(LOCK_LEVEL_DLCI);
    mutex_lock(&ctx->dlci_mutex);
    
    /* Ensure no duplicate */
    vfr_dlci_entry_t *curr = ctx->dlci_table[hash];
    while (curr) {
        if (curr->dlci_in == entry->dlci_in && strcmp(curr->port_in, entry->port_in) == 0) {
            mutex_unlock(&ctx->dlci_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_DLCI);
            return -1;
        }
        curr = curr->next;
    }
    
    entry->next = ctx->dlci_table[hash];
    ctx->dlci_table[hash] = entry;
    mutex_unlock(&ctx->dlci_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_DLCI);
    return 0;
}

int dlci_table_del(vfrs_ctx_t *ctx, const char *port, u32 dlci)
{
    if (!ctx || !port) return -1;
    u32 hash = DLCI_HASH(port, dlci);
    ASSERT_LOCK_ORDER(LOCK_LEVEL_DLCI);
    mutex_lock(&ctx->dlci_mutex);
    vfr_dlci_entry_t **pptr = &ctx->dlci_table[hash];
    while (*pptr) {
        if ((*pptr)->dlci_in == dlci && strcmp((*pptr)->port_in, port) == 0) {
            vfr_dlci_entry_t *entry = *pptr;
            *pptr = entry->next;
            mutex_unlock(&ctx->dlci_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_DLCI);
            free(entry);
            return 0;
        }
        pptr = &(*pptr)->next;
    }
    mutex_unlock(&ctx->dlci_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_DLCI);
    return -1;
}



/* ============================================================
 * PVC Management
 * ============================================================ */

int vfrs_add_pvc(vfrs_ctx_t *ctx, const char *port_in, u32 dlci_in,
                 const char *port_out, u32 dlci_out,
                 u32 cir, u32 bc, u32 be,
                 u8 ftp, u8 fdp, u8 srvcls)
{
    vfr_pvc_detail_t *pvc;
    int hash_in, hash_out;

    if (!ctx || !port_in || !port_out) return -1;

    /* Fail early if either source or destination port does not exist to prevent leaks */
    vfr_port_t *p_in = vfrs_find_port(ctx, port_in);
    vfr_port_t *p_out = vfrs_find_port(ctx, port_out);
    if (!p_in || !p_out) {
        LOG_ERROR("Cannot add PVC: port '%s' or '%s' not found", port_in, port_out);
        return -1;
    }

    /* Check if PVC already exists to prevent duplicate entries (F1) */
    if (port_lookup_dlci(p_in, dlci_in) != NULL) {
        LOG_WARN("PVC %s:%u already exists, skipping", port_in, dlci_in);
        return 0;
    }

    /* Validate DLCIs */
    if (!fr_dlci_valid(dlci_in) || !fr_dlci_valid(dlci_out)) {
        LOG_ERROR("Invalid DLCI range: %u -> %u", dlci_in, dlci_out);
        return -1;
    }

    /* Range check warnings (but keep storing them) */
    if (ftp > 7) {
        LOG_WARN("PVC %s:%u -> %s:%u: Frame Transfer Priority (ftp=%u) exceeds standard range (0-7)",
                 port_in, dlci_in, port_out, dlci_out, ftp);
    }
    if (fdp > 7) {
        LOG_WARN("PVC %s:%u -> %s:%u: Frame Discard Priority (fdp=%u) exceeds standard range (0-7)",
                 port_in, dlci_in, port_out, dlci_out, fdp);
    }
    if (srvcls > 3) {
        LOG_WARN("PVC %s:%u -> %s:%u: Service Class (srvcls=%u) exceeds standard range (0-3)",
                 port_in, dlci_in, port_out, dlci_out, srvcls);
    }

    pvc = calloc(1, sizeof(vfr_pvc_detail_t));
    if (!pvc) {
        LOG_ERROR("Failed to allocate PVC configuration");
        return -1;
    }

    /* Initialize PVC configuration detail */
    strncpy(pvc->port_in, port_in, sizeof(pvc->port_in) - 1);
    strncpy(pvc->port_out, port_out, sizeof(pvc->port_out) - 1);
    pvc->dlci_in = dlci_in;
    pvc->dlci_out = dlci_out;
    pvc->active = 1;
    pvc->lmi_new = 1;  /* Mark as newly provisioned for LMI */
    pvc->ftp = ftp;
    pvc->fdp = fdp;
    pvc->srvcls = srvcls;
    pvc->cir = cir ? cir : (p_in->transport == PORT_TRANS_SERIAL ? cgst_get_access_rate(p_in) : ctx->default_cir);
    pvc->bc = bc ? bc : ctx->default_bc;
    pvc->be = be ? be : ctx->default_be;
    pvc->access_rate = cgst_get_access_rate(p_in);

    /* Link into global PVC hash table */
    hash_in = PVC_HASH(dlci_in);
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);
    pvc->next = ctx->pvc_table[hash_in];
    ctx->pvc_table[hash_in] = pvc;
    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);

    /* Allocate Global DLCI Table forward entry */
    vfr_dlci_entry_t *entry_fwd = calloc(1, sizeof(vfr_dlci_entry_t));
    if (!entry_fwd) {
        LOG_ERROR("Failed to allocate DLCI entry");
        return -1;
    }
    strncpy(entry_fwd->port_in, port_in, sizeof(entry_fwd->port_in) - 1);
    strncpy(entry_fwd->port_out, port_out, sizeof(entry_fwd->port_out) - 1);
    entry_fwd->dlci_in = dlci_in;
    entry_fwd->dlci_out = dlci_out;
    entry_fwd->active = 1;
    entry_fwd->vc_type = VC_TYPE_PVC;
    entry_fwd->detail_ptr = pvc;
    entry_fwd->ftp = pvc->ftp;
    entry_fwd->fdp = pvc->fdp;
    entry_fwd->srvcls = pvc->srvcls;
    cgst_dlci_tb_init(entry_fwd, p_in, pvc->cir, pvc->bc, pvc->be);

    entry_fwd->link_type = p_in->type;
    dlci_table_add(ctx, entry_fwd);
    port_add_dlci_entry(p_in, entry_fwd);

    LOG_INFO("PVC added: %s:%u -> %s:%u (CIR=%u, FTP=%u, FDP=%u, SrvCls=%u)",
             port_in, dlci_in, port_out, dlci_out, pvc->cir, ftp, fdp, srvcls);

    /* Add reverse PVC for bidirectional traffic */
    vfr_pvc_detail_t *pvc_rev = calloc(1, sizeof(vfr_pvc_detail_t));
    if (!pvc_rev) {
        LOG_WARN("Failed to allocate reverse PVC detail");
        return 0;  /* Not a fatal error */
    }

    strncpy(pvc_rev->port_in, port_out, sizeof(pvc_rev->port_in) - 1);
    strncpy(pvc_rev->port_out, port_in, sizeof(pvc_rev->port_out) - 1);
    pvc_rev->dlci_in = dlci_out;
    pvc_rev->dlci_out = dlci_in;
    pvc_rev->active = 1;
    pvc_rev->lmi_new = 1;  /* Mark as newly provisioned for LMI */
    pvc_rev->ftp = ftp;
    pvc_rev->fdp = fdp;
    pvc_rev->srvcls = srvcls;
    pvc_rev->cir = cir ? cir : (p_out->transport == PORT_TRANS_SERIAL ? cgst_get_access_rate(p_out) : ctx->default_cir);
    pvc_rev->bc = bc ? bc : ctx->default_bc;
    pvc_rev->be = be ? be : ctx->default_be;
    pvc_rev->access_rate = cgst_get_access_rate(p_out);

    hash_out = PVC_HASH(dlci_out);
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);
    pvc_rev->next = ctx->pvc_table[hash_out];
    ctx->pvc_table[hash_out] = pvc_rev;
    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);

    /* Allocate Global DLCI Table reverse entry */
    vfr_dlci_entry_t *entry_rev = calloc(1, sizeof(vfr_dlci_entry_t));
    if (entry_rev) {
        strncpy(entry_rev->port_in, port_out, sizeof(entry_rev->port_in) - 1);
        strncpy(entry_rev->port_out, port_in, sizeof(entry_rev->port_out) - 1);
        entry_rev->dlci_in = dlci_out;
        entry_rev->dlci_out = dlci_in;
        entry_rev->active = 1;
        entry_rev->vc_type = VC_TYPE_PVC;
        entry_rev->detail_ptr = pvc_rev;
        entry_rev->ftp = pvc_rev->ftp;
        entry_rev->fdp = pvc_rev->fdp;
        entry_rev->srvcls = pvc_rev->srvcls;
        cgst_dlci_tb_init(entry_rev, p_out, pvc_rev->cir, pvc_rev->bc, pvc_rev->be);

        entry_rev->link_type = p_out->type;
        dlci_table_add(ctx, entry_rev);
        port_add_dlci_entry(p_out, entry_rev);
    }

    if (entry_fwd && entry_rev) {
        entry_fwd->reverse_entry = entry_rev;
        entry_rev->reverse_entry = entry_fwd;
    }

    return 0;
}

int vfrs_del_pvc(vfrs_ctx_t *ctx, const char *port_in, u32 dlci_in)
{
    int hash_in, hash_out;
    vfr_pvc_detail_t **pptr;
    vfr_port_t *port = NULL;
    vfr_port_t *port_rev = NULL;
    char port_out[VFR_MAX_NAME_LEN] = {0};
    u32 dlci_out = 0;
    vfr_pvc_detail_t *pvc = NULL;
    vfr_pvc_detail_t *pvc_rev = NULL;

    if (!ctx || !port_in) return -1;

    /* Look up the port first, outside of the pvc_mutex lock to avoid lock order reversal */
    port = vfrs_find_port(ctx, port_in);

    hash_in = PVC_HASH(dlci_in);
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);

    /* Find and unlink forward PVC configuration detail */
    for (pptr = &ctx->pvc_table[hash_in]; *pptr; pptr = &(*pptr)->next) {
        if ((*pptr)->dlci_in == dlci_in && strcmp((*pptr)->port_in, port_in) == 0) {
            pvc = *pptr;
            *pptr = pvc->next;
            memcpy(port_out, pvc->port_out, sizeof(port_out));
            dlci_out = pvc->dlci_out;
            break;
        }
    }

    if (!pvc) {
        mutex_unlock(&ctx->pvc_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
        return -1;
    }

    /* Find and unlink reverse PVC configuration detail */
    hash_out = PVC_HASH(dlci_out);
    for (pptr = &ctx->pvc_table[hash_out]; *pptr; pptr = &(*pptr)->next) {
        if ((*pptr)->dlci_in == dlci_out && strcmp((*pptr)->port_in, port_out) == 0) {
            pvc_rev = *pptr;
            *pptr = pvc_rev->next;
            break;
        }
    }

    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);

    LOG_INFO("PVC deleted: %s:%u -> %s:%u", port_in, dlci_in, port_out, dlci_out);

    /* Send asynchronous LMI STATUS notification indicating PVC deletion */
    if (port) {
        if (port->lmi_ctx) {
            mutex_lock(&port->mutex);
            lmi_send_async_status(port, dlci_in, LMI_PVC_DELETE);
            mutex_unlock(&port->mutex);
        }
        port_del_dlci_entry(port, dlci_in);
    }
    dlci_table_del(ctx, port_in, dlci_in);

    if (pvc_rev) {
        LOG_INFO("Reverse PVC deleted: %s:%u -> %s:%u", port_out, dlci_out, port_in, dlci_in);
        port_rev = vfrs_find_port(ctx, port_out);
        if (port_rev) {
            if (port_rev->lmi_ctx) {
                mutex_lock(&port_rev->mutex);
                lmi_send_async_status(port_rev, dlci_out, LMI_PVC_DELETE);
                mutex_unlock(&port_rev->mutex);
            }
            port_del_dlci_entry(port_rev, dlci_out);
        }
        dlci_table_del(ctx, port_out, dlci_out);
        free(pvc_rev);
    }

    free(pvc);
    return 0;
}

vfr_pvc_detail_t *vfrs_lookup_pvc(vfrs_ctx_t *ctx, const char *port, u32 dlci)
{
    int hash;
    vfr_pvc_detail_t *pvc;

    if (!ctx) return NULL;

    hash = PVC_HASH(dlci);
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);

    for (pvc = ctx->pvc_table[hash]; pvc; pvc = pvc->next) {
        /* Match DLCI */
        if (pvc->dlci_in != dlci) continue;
        /* Match source port — required for correct disambiguation */
        if (port && strcmp(pvc->port_in, port) != 0) continue;
        mutex_unlock(&ctx->pvc_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
        return pvc;
    }

    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
    return NULL;
}

vfr_pvc_detail_t *vfrs_lookup_pvc_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci)
{
    int hash;
    vfr_pvc_detail_t *pvc;

    if (!ctx) return NULL;

    hash = PVC_HASH(dlci);

    for (pvc = ctx->pvc_table[hash]; pvc; pvc = pvc->next) {
        /* Match DLCI */
        if (pvc->dlci_in != dlci) continue;
        /* Match source port */
        if (port && strcmp(pvc->port_in, port) != 0) continue;
        return pvc;
    }
    return NULL;
}

int vfrs_dlci_is_active_unlocked(vfrs_ctx_t *ctx, vfr_port_t *port, vfr_dlci_entry_t *entry)
{
    if (!ctx || !port || !entry) return 0;

    /* 1. Check local port status */
    if (port->status == PORT_STATUS_DOWN) return 0;
    
    /* 2. Check local LMI states (DCE, and DTE if enabled) */
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    if (lmi) {
        if (lmi->dce_link_down) return 0;
        if (lmi->dte_enabled && lmi->dte_link_down) return 0;
    }

    /* 2a. Check SPVC endpoint state (ITU-T X.76 Annex A.4.5.3) */
    if (spvc_is_spvc_dlci(ctx, port->name, entry->dlci_in)) {
        return spvc_is_dlci_active(ctx, port->name, entry->dlci_in);
    }

    /* 3. Check destination port status */
    vfr_port_t *dst_port = vfrs_find_port_unlocked(ctx, entry->port_out);
    if (!dst_port || dst_port->status == PORT_STATUS_DOWN) return 0;

    /* 4. Check destination LMI states (DCE, and DTE if enabled) */
    vfr_lmi_state_t *dst_lmi = (vfr_lmi_state_t *)dst_port->lmi_ctx;
    if (dst_lmi) {
        if (dst_lmi->dce_link_down) return 0;
        if (dst_lmi->dte_enabled && dst_lmi->dte_link_down) return 0;
    }

    /* 5. Check if destination DTE LMI marked peer active */
    vfr_dlci_entry_t *peer_entry = port_lookup_dlci(dst_port, entry->dlci_out);
    if (peer_entry) {
        if (dst_lmi && dst_lmi->dte_enabled) {
            if (!peer_entry->active) return 0;
        }
    }

    /* 6. Check multicast dependency */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    vfr_mcast_group_t *mcast_g = ctx->mcast_groups;
    while (mcast_g) {
        if (mcast_g->mode == MCAST_MODE_ONEWAY) {
            vfr_mcast_member_t *m = mcast_g->members;
            while (m) {
                if ((strcmp(entry->port_in, mcast_g->source_port) == 0 && entry->dlci_in != mcast_g->source_dlci &&
                     strcmp(entry->port_out, m->port_name) == 0 && entry->dlci_out == m->dlci) ||
                    (strcmp(entry->port_out, mcast_g->source_port) == 0 && entry->dlci_out != mcast_g->source_dlci &&
                     strcmp(entry->port_in, m->port_name) == 0 && entry->dlci_in == m->dlci)) {
                    
                    if (!vfrs_mcast_endpoint_is_active_unlocked(ctx, mcast_g->source_port, mcast_g->source_dlci)) {
                        mutex_unlock(&ctx->mcast_mutex);
                        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
                        return 0;
                    }
                }
                m = m->next;
            }
        }
        mcast_g = mcast_g->next;
    }
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

    return 1;
}

int vfrs_dlci_is_active(vfrs_ctx_t *ctx, vfr_port_t *port, vfr_dlci_entry_t *entry)
{
    int active;
    if (!ctx) return 0;
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);
    active = vfrs_dlci_is_active_unlocked(ctx, port, entry);
    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
    return active;
}

typedef struct {
    vfr_port_t *port;
    u32 dlci;
    u8 status;
} async_notif_t;

void vfrs_propagate_port_status_change(vfrs_ctx_t *ctx, vfr_port_t *changed_port)
{
    if (!ctx || !changed_port) return;

    LOG_DEBUG("Propagating port status change for %s", changed_port->name);

    async_notif_t *notifs = malloc(sizeof(async_notif_t) * MAX_PVCS);
    if (!notifs) {
        LOG_ERROR("Failed to allocate memory for async status propagation");
        return;
    }
    int notif_count = 0;

    /* 1. Collect PVC status changes under pvc_mutex */
    mutex_lock(&ctx->pvc_mutex);
    for (int i = 0; i < PVC_HASH_SIZE; i++) {
        vfr_pvc_detail_t *pvc = ctx->pvc_table[i];
        while (pvc) {
            /* If this PVC's destination is the changed port, we must notify the source port's DTE */
            if (strcmp(pvc->port_out, changed_port->name) == 0) {
                vfr_port_t *src_port = vfrs_find_port_unlocked(ctx, pvc->port_in);
                if (src_port && src_port->lmi_ctx) {
                    vfr_lmi_state_t *src_lmi = (vfr_lmi_state_t *)src_port->lmi_ctx;
                    /* Send async status only if the source port is UP and its LMI is active. */
                    if (src_port->status == PORT_STATUS_UP && !src_lmi->dce_link_down) {
                        vfr_dlci_entry_t *entry = port_lookup_dlci(src_port, pvc->dlci_in);
                        if (entry) {
                            u8 status = vfrs_dlci_is_active_unlocked(ctx, src_port, entry) ? LMI_PVC_ACTIVE : 0;
                            if (notif_count < MAX_PVCS) {
                                notifs[notif_count].port = src_port;
                                notifs[notif_count].dlci = pvc->dlci_in;
                                notifs[notif_count].status = status;
                                notif_count++;
                            }
                        }
                    }
                }
            }
            pvc = pvc->next;
        }
    }
    mutex_unlock(&ctx->pvc_mutex);

    /* 2. Collect multicast status changes under mcast_mutex */
    mutex_lock(&ctx->mcast_mutex);
    vfr_mcast_group_t *g = ctx->mcast_groups;
    while (g) {
        if (strcmp(g->source_port, changed_port->name) == 0) {
            vfr_port_t *src_port = vfrs_find_port_unlocked(ctx, g->source_port);
            if (src_port && src_port->status == PORT_STATUS_UP && src_port->lmi_ctx) {
                vfr_lmi_state_t *src_lmi = (vfr_lmi_state_t *)src_port->lmi_ctx;
                if (!src_lmi->dce_link_down) {
                    u8 status = vfrs_mcast_endpoint_is_active_unlocked(ctx, g->source_port, g->source_dlci) ? LMI_PVC_ACTIVE : 0;
                    if (notif_count < MAX_PVCS) {
                        notifs[notif_count].port = src_port;
                        notifs[notif_count].dlci = g->source_dlci;
                        notifs[notif_count].status = status;
                        notif_count++;
                    }
                }
            }
            vfr_mcast_member_t *m = g->members;
            while (m) {
                vfr_port_t *m_port = vfrs_find_port_unlocked(ctx, m->port_name);
                if (m_port && m_port->status == PORT_STATUS_UP && m_port->lmi_ctx) {
                    vfr_lmi_state_t *m_lmi = (vfr_lmi_state_t *)m_port->lmi_ctx;
                    if (!m_lmi->dce_link_down) {
                        u8 status = vfrs_mcast_endpoint_is_active_unlocked(ctx, m->port_name, m->dlci) ? LMI_PVC_ACTIVE : 0;
                        if (notif_count < MAX_PVCS) {
                            notifs[notif_count].port = m_port;
                            notifs[notif_count].dlci = m->dlci;
                            notifs[notif_count].status = status;
                            notif_count++;
                        }
                    }
                }
                m = m->next;
            }
        } else {
            vfr_mcast_member_t *m = g->members;
            int is_member = 0;
            while (m) {
                if (strcmp(m->port_name, changed_port->name) == 0) {
                    is_member = 1;
                    vfr_port_t *m_port = vfrs_find_port_unlocked(ctx, m->port_name);
                    if (m_port && m_port->status == PORT_STATUS_UP && m_port->lmi_ctx) {
                        vfr_lmi_state_t *m_lmi = (vfr_lmi_state_t *)m_port->lmi_ctx;
                        if (!m_lmi->dce_link_down) {
                            u8 status = vfrs_mcast_endpoint_is_active_unlocked(ctx, m->port_name, m->dlci) ? LMI_PVC_ACTIVE : 0;
                            if (notif_count < MAX_PVCS) {
                                notifs[notif_count].port = m_port;
                                notifs[notif_count].dlci = m->dlci;
                                notifs[notif_count].status = status;
                                notif_count++;
                            }
                        }
                    }
                }
                m = m->next;
            }
            if (is_member) {
                vfr_port_t *src_port = vfrs_find_port_unlocked(ctx, g->source_port);
                if (src_port && src_port->status == PORT_STATUS_UP && src_port->lmi_ctx) {
                    vfr_lmi_state_t *src_lmi = (vfr_lmi_state_t *)src_port->lmi_ctx;
                    if (!src_lmi->dce_link_down) {
                        u8 status = vfrs_mcast_endpoint_is_active_unlocked(ctx, g->source_port, g->source_dlci) ? LMI_PVC_ACTIVE : 0;
                        if (notif_count < MAX_PVCS) {
                            notifs[notif_count].port = src_port;
                            notifs[notif_count].dlci = g->source_dlci;
                            notifs[notif_count].status = status;
                            notif_count++;
                        }
                    }
                }
            }
        }
        g = g->next;
    }
    mutex_unlock(&ctx->mcast_mutex);

    /* 3. Send async updates outside global locks, locking port->mutex individually */
    for (int i = 0; i < notif_count; i++) {
        vfr_port_t *p = notifs[i].port;
        if (p) {
            mutex_lock(&p->mutex);
            lmi_send_async_status(p, notifs[i].dlci, notifs[i].status);
            mutex_unlock(&p->mutex);
        }
    }

    free(notifs);
}

/* ============================================================
 * Port Management
 * ============================================================ */

vfr_port_t *vfrs_find_port(vfrs_ctx_t *ctx, const char *name)
{
    int i;
    if (!ctx || !name) return NULL;

    mutex_lock(&ctx->port_mutex);
    for (i = 0; i < ctx->port_count; i++) {
        if (ctx->ports[i] && strcmp(ctx->ports[i]->name, name) == 0) {
            vfr_port_t *port = ctx->ports[i];
            mutex_unlock(&ctx->port_mutex);
            return port;
        }
    }
    mutex_unlock(&ctx->port_mutex);
    return NULL;
}

vfr_port_t *vfrs_find_port_unlocked(vfrs_ctx_t *ctx, const char *name)
{
    int i;
    if (!ctx || !name) return NULL;
    for (i = 0; i < ctx->port_count; i++) {
        if (ctx->ports[i] && strcmp(ctx->ports[i]->name, name) == 0) {
            return ctx->ports[i];
        }
    }
    return NULL;
}

int vfrs_add_port(vfrs_ctx_t *ctx, vfr_port_t *port)
{
    if (!ctx || !port) return -1;

    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);
    if (ctx->port_count >= MAX_PORTS) {
        mutex_unlock(&ctx->port_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);
        LOG_ERROR("Maximum ports reached: %d", MAX_PORTS);
        return -1;
    }

    ctx->ports[ctx->port_count++] = port;
    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);

    LOG_INFO("Port added: %s (type=%s, transport=%d)",
             port->name,
             port->type == PORT_TYPE_UNI ? "UNI" : "NNI",
             port->transport);

    return 0;
}

int vfrs_remove_port(vfrs_ctx_t *ctx, vfr_port_t *port)
{
    int i;
    if (!ctx || !port) return -1;

    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);
    for (i = 0; i < ctx->port_count; i++) {
        if (ctx->ports[i] == port) {
            /* Shift remaining ports */
            for (; i < ctx->port_count - 1; i++) {
                ctx->ports[i] = ctx->ports[i + 1];
            }
            ctx->port_count--;
            mutex_unlock(&ctx->port_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);
            LOG_INFO("Port removed: %s", port->name);
            return 0;
        }
    }
    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);
    return -1;
}

/* ============================================================
 * Frame Switching
 * ============================================================ */

int vfrs_switch_frame(vfrs_ctx_t *ctx, vfr_port_t *src_port,
                      u32 dlci, const u8 *frame, size_t len)
{
    vfr_port_t *dst_port;
    u8 out_frame[8192];
    fr_addr_t addr;

    if (!ctx || !src_port || !frame) return -1;

    /* Multicast switching */
    int is_root = 0;
    vfr_mcast_group_t *g = vfrs_find_mcast_group_by_endpoint(ctx, src_port->name, dlci, &is_root);
    if (g) {
        u64 now = get_tick_count();
        if (g->peer_congested && (now - g->last_cllm_time > 11000)) {
            g->peer_congested = 0;
        }
        ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
        mutex_lock(&ctx->mcast_mutex);
        vfr_mcast_member_t *m_temp = g->members;
        while (m_temp) {
            if (m_temp->peer_congested && (now - m_temp->last_cllm_time > 11000)) {
                m_temp->peer_congested = 0;
            }
            m_temp = m_temp->next;
        }
        mutex_unlock(&ctx->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

        LOG_DEBUG("Multicast frame received on %s DLCI %u (group=%s, mode=%d, is_root=%d)",
                  src_port->name, dlci, g->name, g->mode, is_root);

        if (!vfrs_mcast_endpoint_is_active(ctx, src_port->name, dlci)) {
            src_port->stats.dropped++;
            LOG_DEBUG("Multicast frame dropped: endpoint %s:%u inactive", src_port->name, dlci);
            return -1;
        }

        if (g->mode == MCAST_MODE_ONEWAY || g->mode == MCAST_MODE_TWOWAY) {
            if (is_root) {
                fr_decode_addr(frame, &addr);
                /* Ingress policing using the multicast group's token bucket */
                ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
                mutex_lock(&ctx->mcast_mutex);
                int cgst_action = cgst_process_frame_tb(src_port, &g->tb, (u8 *)frame, len, &addr);
                mutex_unlock(&ctx->mcast_mutex);
                RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
                if (cgst_action == 2) {
                    src_port->stats.dropped++;
                    LOG_DEBUG("Multicast frame dropped on ingress %s:%u (policer discard)", src_port->name, dlci);
                    return -1;
                } else if (cgst_action == 1) {
                    addr.de = 1;
                }

                src_port->stats.switched++;

                /* Determine actual multicast group size dynamically (F15) */
                int mcast_count = 0;
                ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
                mutex_lock(&ctx->mcast_mutex);
                vfr_mcast_member_t *m_cnt = g->members;
                while (m_cnt) {
                    mcast_count++;
                    m_cnt = m_cnt->next;
                }
                mutex_unlock(&ctx->mcast_mutex);
                RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
 
                typedef struct {
                    vfr_port_t *d_port;
                    u32 dlci;
                    u8 m_peer_congested;
                } mcast_dest_t;
                mcast_dest_t *dests = alloca(sizeof(mcast_dest_t) * (mcast_count > 0 ? mcast_count : 1));
                int dest_count = 0;
                ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
                mutex_lock(&ctx->mcast_mutex);
                vfr_mcast_member_t *m = g->members;
                while (m && dest_count < mcast_count) {
                    vfr_port_t *d_port = vfrs_find_port_unlocked(ctx, m->port_name);
                    if (d_port && d_port->status == PORT_STATUS_UP) {
                        vfr_lmi_state_t *d_lmi = (vfr_lmi_state_t *)d_port->lmi_ctx;
                        int lmi_ok = 1;
                        if (d_lmi) {
                            if (d_lmi->dce_link_down || (d_lmi->dte_enabled && d_lmi->dte_link_down)) {
                                lmi_ok = 0;
                            }
                        }
                        if (lmi_ok) {
                            dests[dest_count].d_port = d_port;
                            dests[dest_count].dlci = m->dlci;
                            dests[dest_count].m_peer_congested = m->peer_congested;
                            dest_count++;
                        }
                    }
                    m = m->next;
                }
                mutex_unlock(&ctx->mcast_mutex);
                RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

                /* Transmit replicated frames outside mcast_mutex to avoid lock contention */
                for (int i = 0; i < dest_count; i++) {
                    vfr_port_t *d_port = dests[i].d_port;
                    u32 out_dlci = dests[i].dlci;
                    u8 m_peer_congested = dests[i].m_peer_congested;

                    u8 o_frame[8192];
                    int fecn = addr.fecn;
                    int becn = addr.becn;

                    /* Decoupled, asymmetric FECN/BECN setting */
                    if (cgst_should_set_de(src_port) || cgst_should_set_de(d_port) || m_peer_congested || cgst_action == 1) {
                        fecn = 1;
                    }
                    if (cgst_should_set_de(d_port) || m_peer_congested) {
                        becn = 1;
                    }

                    size_t in_addr_len = fr_get_addr_len(frame, len);
                    size_t out_addr_len = (d_port->dlcibit == 23 || out_dlci > 63487) ? 4 : (d_port->dlcibit == 16 ? 3 : ((out_dlci > 1023) ? 3 : 2));
                    size_t payload_len = len - in_addr_len;
                    size_t new_len = out_addr_len + payload_len;

                    if (new_len + 2 <= sizeof(o_frame)) {
                        fr_encode_dlci_with_flags(o_frame, out_dlci, fecn, becn, addr.de, addr.cr, (int)out_addr_len);
                        if (payload_len > 0) {
                            memcpy(o_frame + out_addr_len, frame + in_addr_len, payload_len);
                        }

                        if (d_port->transport == PORT_TRANS_SERIAL || d_port->transport == PORT_TRANS_PIPE) {
                            u16 fcs = crc16_fcs(o_frame, new_len);
                            o_frame[new_len + 0] = fcs & 0xFF;
                            o_frame[new_len + 1] = (fcs >> 8) & 0xFF;
                        }

                        size_t send_len = (d_port->transport == PORT_TRANS_SERIAL || d_port->transport == PORT_TRANS_PIPE)
                                          ? new_len + 2 : new_len;

                        if (d_port->ops && d_port->ops->send) {
                            int ret = d_port->ops->send(d_port, o_frame, send_len);
                            if (ret >= 0) {
                                d_port->stats.tx_frames++;
                                d_port->stats.tx_bytes += (u64)ret;
                                d_port->stats.switched++;
                            }
                        }
                    }
                }
                return 0;
            } else {
                /* Leaf member */
                if (g->mode == MCAST_MODE_ONEWAY) {
                    src_port->stats.dropped++;
                    LOG_DEBUG("One-way multicast frame from leaf %s:%u dropped", src_port->name, dlci);
                    return -1;
                }

                /* Twoway leaf forwards to root */
                vfr_port_t *d_port = vfrs_find_port(ctx, g->source_port);
                if (!d_port || d_port->status != PORT_STATUS_UP) {
                    src_port->stats.dropped++;
                    LOG_DEBUG("Twoway leaf frame dropped: root port %s not available", g->source_port);
                    return -1;
                }

                vfr_lmi_state_t *d_lmi = (vfr_lmi_state_t *)d_port->lmi_ctx;
                if (d_lmi) {
                    if (d_lmi->dce_link_down || (d_lmi->dte_enabled && d_lmi->dte_link_down)) {
                        src_port->stats.dropped++;
                        LOG_DEBUG("Twoway leaf frame dropped: root port %s LMI down", g->source_port);
                        return -1;
                    }
                }

                fr_decode_addr(frame, &addr);
                /* Ingress policing using the group's token bucket */
                ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
                mutex_lock(&ctx->mcast_mutex);
                int cgst_action = cgst_process_frame_tb(src_port, &g->tb, (u8 *)frame, len, &addr);
                mutex_unlock(&ctx->mcast_mutex);
                RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
                if (cgst_action == 2) {
                    src_port->stats.dropped++;
                    LOG_DEBUG("Twoway leaf frame dropped on ingress %s:%u (policer discard)", src_port->name, dlci);
                    return -1;
                } else if (cgst_action == 1) {
                    addr.de = 1;
                }

                src_port->stats.switched++;
                d_port->stats.switched++;

                u8 o_frame[2048];
                int fecn = addr.fecn;
                int becn = addr.becn;
                
                /* Set FECN/BECN flags */
                u8 leaf_peer_congested = 0;
                ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
                mutex_lock(&ctx->mcast_mutex);
                vfr_mcast_member_t *m_search = g->members;
                while (m_search) {
                    if (strcmp(m_search->port_name, src_port->name) == 0 && m_search->dlci == dlci) {
                        leaf_peer_congested = m_search->peer_congested;
                        break;
                    }
                    m_search = m_search->next;
                }
                u8 g_peer_congested = g->peer_congested;
                mutex_unlock(&ctx->mcast_mutex);
                RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

                /* Decoupled, asymmetric FECN/BECN setting */
                if (cgst_should_set_de(src_port) || cgst_should_set_de(d_port) || g_peer_congested || cgst_action == 1) {
                    fecn = 1;
                }
                if (cgst_should_set_de(src_port) || leaf_peer_congested) {
                    becn = 1;
                }

                size_t in_addr_len = fr_get_addr_len(frame, len);
                size_t out_addr_len = (d_port->dlcibit == 23 || g->source_dlci > 63487) ? 4 : (d_port->dlcibit == 16 ? 3 : ((g->source_dlci > 1023) ? 3 : 2));
                size_t payload_len = len - in_addr_len;
                size_t new_len = out_addr_len + payload_len;

                if (new_len + 2 > sizeof(o_frame)) {
                    src_port->stats.dropped++;
                    return -1;
                }

                fr_encode_dlci_with_flags(o_frame, g->source_dlci, fecn, becn, addr.de, addr.cr, (int)out_addr_len);
                if (payload_len > 0) {
                    memcpy(o_frame + out_addr_len, frame + in_addr_len, payload_len);
                }

                if (d_port->transport == PORT_TRANS_SERIAL || d_port->transport == PORT_TRANS_PIPE) {
                    u16 fcs = crc16_fcs(o_frame, new_len);
                    o_frame[new_len + 0] = fcs & 0xFF;
                    o_frame[new_len + 1] = (fcs >> 8) & 0xFF;
                }

                size_t send_len = (d_port->transport == PORT_TRANS_SERIAL || d_port->transport == PORT_TRANS_PIPE)
                                  ? new_len + 2 : new_len;

                if (d_port->ops && d_port->ops->send) {
                    int ret = d_port->ops->send(d_port, o_frame, send_len);
                    if (ret < 0) {
                        LOG_WARN("Failed to send twoway multicast frame to root %s", d_port->name);
                        cgst_handle_write_failure(d_port);
                        return -1;
                    }
                    d_port->stats.tx_frames++;
                    d_port->stats.tx_bytes += (u64)ret;
                    cgst_handle_write_success(d_port);
                }
                return 0;
            }
        } else if (g->mode == MCAST_MODE_NWAY) {
            fr_decode_addr(frame, &addr);
            /* Ingress policing using the group's token bucket */
            ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
            mutex_lock(&ctx->mcast_mutex);
            int cgst_action = cgst_process_frame_tb(src_port, &g->tb, (u8 *)frame, len, &addr);
            mutex_unlock(&ctx->mcast_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
            if (cgst_action == 2) {
                src_port->stats.dropped++;
                LOG_DEBUG("N-way frame dropped on ingress %s:%u (policer discard)", src_port->name, dlci);
                return -1;
            } else if (cgst_action == 1) {
                addr.de = 1;
            }

            src_port->stats.switched++;

            /* Determine actual multicast group size dynamically (F15) */
            int mcast_count = 0;
            ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
            mutex_lock(&ctx->mcast_mutex);
            vfr_mcast_member_t *m_cnt = g->members;
            while (m_cnt) {
                mcast_count++;
                m_cnt = m_cnt->next;
            }
            mutex_unlock(&ctx->mcast_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
 
            typedef struct {
                vfr_port_t *d_port;
                u32 dlci;
                u8 peer_congested;
            } nway_dest_t;
            nway_dest_t *dests = alloca(sizeof(nway_dest_t) * (mcast_count + 1));
            int dest_count = 0;
            ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
            mutex_lock(&ctx->mcast_mutex);

            /* Replicate to root/first member if not sender */
            if (strcmp(g->source_port, src_port->name) != 0 || g->source_dlci != dlci) {
                vfr_port_t *d_port = vfrs_find_port_unlocked(ctx, g->source_port);
                if (d_port && d_port->status == PORT_STATUS_UP) {
                    vfr_lmi_state_t *d_lmi = (vfr_lmi_state_t *)d_port->lmi_ctx;
                    int lmi_ok = 1;
                    if (d_lmi) {
                        if (d_lmi->dce_link_down || (d_lmi->dte_enabled && d_lmi->dte_link_down)) {
                            lmi_ok = 0;
                        }
                    }
                    if (lmi_ok) {
                        dests[dest_count].d_port = d_port;
                        dests[dest_count].dlci = g->source_dlci;
                        dests[dest_count].peer_congested = g->peer_congested;
                        dest_count++;
                    }
                }
            }

            /* Replicate to other members */
            vfr_mcast_member_t *m = g->members;
            while (m && dest_count < (mcast_count + 1)) {
                if (strcmp(m->port_name, src_port->name) != 0 || m->dlci != dlci) {
                    vfr_port_t *d_port = vfrs_find_port_unlocked(ctx, m->port_name);
                    if (d_port && d_port->status == PORT_STATUS_UP) {
                        vfr_lmi_state_t *d_lmi = (vfr_lmi_state_t *)d_port->lmi_ctx;
                        int lmi_ok = 1;
                        if (d_lmi) {
                            if (d_lmi->dce_link_down || (d_lmi->dte_enabled && d_lmi->dte_link_down)) {
                                lmi_ok = 0;
                            }
                        }
                        if (lmi_ok) {
                            dests[dest_count].d_port = d_port;
                            dests[dest_count].dlci = m->dlci;
                            dests[dest_count].peer_congested = m->peer_congested;
                            dest_count++;
                        }
                    }
                }
                m = m->next;
            }
            mutex_unlock(&ctx->mcast_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

            /* Transmit outside of mcast_mutex */
            for (int i = 0; i < dest_count; i++) {
                vfr_port_t *d_port = dests[i].d_port;
                u32 out_dlci = dests[i].dlci;
                u8 peer_congested = dests[i].peer_congested;

                u8 o_frame[8192];
                int fecn = addr.fecn;
                int becn = addr.becn;

                /* Decoupled, asymmetric FECN/BECN setting */
                if (cgst_should_set_de(src_port) || cgst_should_set_de(d_port) || peer_congested || cgst_action == 1) {
                    fecn = 1;
                }
                if (cgst_should_set_de(d_port) || peer_congested) {
                    becn = 1;
                }

                size_t in_addr_len = fr_get_addr_len(frame, len);
                size_t out_addr_len = (d_port->dlcibit == 23 || out_dlci > 63487) ? 4 : (d_port->dlcibit == 16 ? 3 : ((out_dlci > 1023) ? 3 : 2));
                size_t payload_len = len - in_addr_len;
                size_t new_len = out_addr_len + payload_len;

                if (new_len + 2 <= sizeof(o_frame)) {
                    fr_encode_dlci_with_flags(o_frame, out_dlci, fecn, becn, addr.de, addr.cr, (int)out_addr_len);
                    if (payload_len > 0) {
                        memcpy(o_frame + out_addr_len, frame + in_addr_len, payload_len);
                    }

                    if (d_port->transport == PORT_TRANS_SERIAL || d_port->transport == PORT_TRANS_PIPE) {
                        u16 fcs = crc16_fcs(o_frame, new_len);
                        o_frame[new_len + 0] = fcs & 0xFF;
                        o_frame[new_len + 1] = (fcs >> 8) & 0xFF;
                    }

                    size_t send_len = (d_port->transport == PORT_TRANS_SERIAL || d_port->transport == PORT_TRANS_PIPE)
                                      ? new_len + 2 : new_len;

                    if (d_port->ops && d_port->ops->send) {
                        int ret = d_port->ops->send(d_port, o_frame, send_len);
                        if (ret >= 0) {
                            d_port->stats.tx_frames++;
                            d_port->stats.tx_bytes += (u64)ret;
                            d_port->stats.switched++;
                            cgst_handle_write_success(d_port);
                        } else {
                            cgst_handle_write_failure(d_port);
                        }
                    }
                }
            }
            return 0;
        }
    }

    /* Look up DLCI entry by (DLCI, source-port) pair — using port-local unified table */
    vfr_dlci_entry_t *entry = port_lookup_dlci(src_port, dlci);
    if (!entry || !vfrs_dlci_is_active(ctx, src_port, entry)) {
        src_port->stats.dropped++;
        LOG_DEBUG("Frame dropped: %s DLCI %u (%s)", src_port->name, dlci,
                  !entry ? "no DLCI entry" : "DLCI entry inactive");
        return -1;
    }

    /* Find destination port */
    dst_port = vfrs_find_port(ctx, entry->port_out);
    if (!dst_port || dst_port->status != PORT_STATUS_UP) {
        src_port->stats.dropped++;
        LOG_DEBUG("Frame dropped: %s destination port not available", src_port->name);
        return -1;
    }

    /* Decode congestion flags from incoming frame's address field */
    fr_decode_addr(frame, &addr);

    /* Process frame through congestion management / traffic policing */
    int cgst_action = cgst_process_frame(src_port, entry, (u8 *)frame, len, &addr);
    if (cgst_action == 2) {
        src_port->stats.dropped++;
        LOG_DEBUG("Frame dropped on %s DLCI %u: traffic policing discard", src_port->name, dlci);
        return -1;
    } else if (cgst_action == 1) {
        addr.de = 1;
    }

    /* Set FECN/BECN flags if ports are congested or PVC peer congestion is active */
    int fecn = addr.fecn;
    int becn = addr.becn;

    /* FECN is set if the forward path is congested */
    if (cgst_should_set_de(dst_port) || entry->peer_congested || cgst_action == 1) {
        fecn = 1;
    }

    /* BECN is set if the reverse path is congested */
    if (cgst_should_set_de(src_port) || (entry->reverse_entry && (entry->reverse_entry->peer_congested || 
        (entry->reverse_entry->tb.cir > 0 && entry->reverse_entry->tb.tokens < entry->reverse_entry->tb.be)))) {
        becn = 1;
    }

    /* Determine incoming and outgoing address field lengths */
    size_t in_addr_len = fr_get_addr_len(frame, len);
    if (len < in_addr_len) {
        src_port->stats.dropped++;
        return -1;
    }

    size_t out_addr_len = (dst_port->dlcibit == 23 || entry->dlci_out > 63487) ? 4 : (dst_port->dlcibit == 16 ? 3 : ((entry->dlci_out > 1023) ? 3 : 2));
    size_t payload_len = len - in_addr_len;
    size_t new_len = out_addr_len + payload_len;

    if (new_len + 2 > sizeof(out_frame)) {  /* +2 for new FCS */
        LOG_WARN("Frame too large: %zu bytes", new_len);
        src_port->stats.dropped++;
        return -1;
    }

    /* Rewrite the Q.922 address field with the output DLCI at the beginning of out_frame.
     * Note: fr_encode_dlci_with_flags correctly preserves the C/R bit */
    int dlci_len = (int)out_addr_len;
    fr_encode_dlci_with_flags(out_frame, entry->dlci_out,
                              fecn, becn, addr.de, addr.cr, dlci_len);

    /* Copy payload (control + data) after the new address field */
    if (payload_len > 0) {
        memcpy(out_frame + out_addr_len, frame + in_addr_len, payload_len);
    }

    /* Check for FRF.12 Egress Fragmentation */
    size_t eff_frag_size = entry->fragment_size > 0 ? entry->fragment_size : dst_port->fragment_size;
    if (eff_frag_size > 0 && payload_len > eff_frag_size) {
        u8 frags[FRF12_MAX_FRAGMENTS][FR_MAX_FRAMESZ + 32];
        size_t frag_lens[FRF12_MAX_FRAGMENTS];
        int num_frags = frf12_fragment_frame(out_frame, new_len, eff_frag_size, frags, frag_lens, FRF12_MAX_FRAGMENTS, &entry->frag_seq);
        if (num_frags > 0) {
            for (int f = 0; f < num_frags; f++) {
                size_t flen = frag_lens[f];
                if (dst_port->transport == PORT_TRANS_SERIAL || dst_port->transport == PORT_TRANS_PIPE) {
                    u16 fcs = crc16_fcs(frags[f], flen);
                    frags[f][flen + 0] = fcs & 0xFF;
                    frags[f][flen + 1] = (fcs >> 8) & 0xFF;
                    flen += 2;
                }
                if (dst_port->ops && dst_port->ops->send) {
                    int ret = dst_port->ops->send(dst_port, frags[f], flen);
                    if (ret >= 0) {
                        dst_port->stats.tx_frames++;
                        dst_port->stats.tx_bytes += (u64)ret;
                    }
                }
            }
            src_port->stats.switched++;
            dst_port->stats.switched++;
            entry->tx_frames++;
            entry->tx_bytes += new_len;
            cgst_handle_write_success(dst_port);
            return 0;
        }
    }

    /* Recompute FCS after DLCI rewrite — X.36 §9.2.4.
     * FCS covers [address..data] = out_frame[0..new_len-1].
     * crc16_fcs uses non-reflected CRC-16-CCITT (init=0xFFFF, final XOR).
     * Written LSB-first: out_frame[new_len]=LSB, out_frame[new_len+1]=MSB.
     */
    {
        u16 fcs = crc16_fcs(out_frame, new_len);
        out_frame[new_len + 0] = fcs & 0xFF;         /* FCS LSB */
        out_frame[new_len + 1] = (fcs >> 8) & 0xFF;  /* FCS MSB */
    }

    /* Update statistics */
    src_port->stats.switched++;
    dst_port->stats.switched++;
    entry->tx_frames++;
    entry->tx_bytes += new_len;

    /* Send the frame to the destination port.
     *
     * Serial and named pipe use full HDLC framing (Flag + stuffing + FCS).
     * The transport send function (port_serial_send / port_pipe_send) handles
     * stuffing and flag wrapping, so the frame body (including FCS) is passed raw.
     *
     * UDP/TCP send raw Frame Relay bytes without FCS (Dynamips-compatible).
     * Ref: dynamips net_io.c netio_udp_send / netio_tcp_send pass pkt as-is,
     * and frame_relay.c frsw_handle_pkt directly addresses pkt[0]/pkt[1] as
     * the Q.922 address field with no FCS bytes.
     */
    size_t send_len = (dst_port->transport == PORT_TRANS_SERIAL ||
                       dst_port->transport == PORT_TRANS_PIPE)
                      ? new_len + 2   /* address + payload + 2-byte FCS */
                      : new_len;      /* address + payload, no FCS */

    if (dst_port->ops && dst_port->ops->send) {
        int ret = dst_port->ops->send(dst_port, out_frame, send_len);
        if (ret < 0) {
            LOG_WARN("Failed to send frame to %s", dst_port->name);
            cgst_handle_write_failure(dst_port);
            return -1;
        }
        dst_port->stats.tx_frames++;
        dst_port->stats.tx_bytes += (u64)ret;
        cgst_handle_write_success(dst_port);
    }

    return 0;
}

/* Forward declarations */
static int send_lmi_status_response(vfr_port_t *port, const u8 *frame, size_t frame_len, int lmi_type);
/* lmi_cisco_build_status is declared in vfr.h */

/* Main frame input processing */
int fr_switch_input(vfrs_ctx_t *ctx, vfr_port_t *port, u8 *frame, size_t len)
{
    fr_addr_t addr;
    int offset;
    size_t frame_len;  /* destuffed frame body length (excl. FCS bytes) */
    if (!ctx || !port || !frame || len < 3) return -1;

    /* Parse frame: strip HDLC flags, destuff, decode Q.922 address.
     * fr_parse_frame modifies frame in-place for the destuffed data.
     * frame_len = total destuffed length (address + control + data, excl. FCS).
     *
     * Named pipe uses full HDLC framing (same as serial): Flag + stuffing + FCS.
     */
    int use_hdlc = (port->transport == PORT_TRANS_SERIAL ||
                    port->transport == PORT_TRANS_PIPE);
    offset = fr_parse_frame(frame, len, &addr, &frame_len, use_hdlc);
    if (offset < 0) {
        LOG_DEBUG("Invalid frame from %s", port->name);
        port->stats.fcs_errors++;
        return -1;
    }

    /* Standard frame body must be at least 2 bytes (the Q.922 address). 
     * Zero-payload congestion frames (§9.4.5 Note 1) are valid. */
    if (frame_len < 2) {
        LOG_DEBUG("Frame too short from %s: %zu bytes (min 2)", port->name, frame_len);
        port->stats.dropped++;
        return -1;
    }

    /* FCS validation — only for serial and named pipe transport.
     * UDP/TCP/raw-socket transports carry no FCS (Dynamips-compatible).
     */
    if (port->transport == PORT_TRANS_SERIAL ||
        port->transport == PORT_TRANS_PIPE) {
        if (!crc16_check(frame, frame_len + 2)) {
            LOG_DEBUG("FCS error on %s", port->name);
            port->stats.fcs_errors++;
            return -1;
        }
    }

    return fr_switch_input_processed(ctx, port, &addr, frame, frame_len);
}

int fr_switch_input_processed(vfrs_ctx_t *ctx, vfr_port_t *port, const fr_addr_t *addr_ptr, u8 *frame, size_t frame_len)
{
    fr_addr_t addr = *addr_ptr;

    /* Link Plane Routing Decision based on DLCI and LAPF Control Field */
    int route_to_control = 0;
    int drop_frame = 0;

    size_t addr_len = fr_get_addr_len(frame, frame_len);
    if (frame_len < addr_len) {
        port->stats.dropped++;
        return -1;
    }
    u8 ctrl_field = (frame_len > addr_len) ? frame[addr_len] : 0;

    if (addr.dlci == 0) {
        /* DLCI 0: LMI (UI control field 0x03) or SVC/LAPF (non-UI control field) */
        route_to_control = 1;
    } else if (addr.dlci == 1023) {
        /* Cisco LMI (UI control field 0x03 only) */
        if ((ctrl_field & ~0x10) == 0x03) {
            route_to_control = 1;
        } else {
            drop_frame = 1;
        }
    } else if (addr.dlci == 1007) {
        /* CLLM (XID control field 0xAF only) */
        if (ctrl_field == 0xAF) {
            route_to_control = 1;
        } else {
            drop_frame = 1;
        }
    } else if (addr.dlci == 1015) {
        /* Future VFRNS protocol: only for NNI ports */
        if (port->type == PORT_TYPE_NNI) {
            route_to_control = 1;
        } else {
            drop_frame = 1;
        }
    } else if ((addr.dlci >= 1 && addr.dlci <= 15) || (addr.dlci >= 1008 && addr.dlci <= 1022)) {
        /* Reserved DLCIs (Clause 9): dropped instantly */
        drop_frame = 1;
    } else {
        /* User DLCIs (16-991, 1024-8388607) */
        if ((ctrl_field & ~0x10) != 0x03) {
            /* Non-UI control frame on user DLCI (e.g. LAPF signaling on user VC) */
            route_to_control = 1;
        }
    }

    if (drop_frame) {
        port->stats.dropped++;
        LOG_DEBUG("Frame on DLCI %u dropped (reserved/invalid control field 0x%02X) on %s",
                  addr.dlci, ctrl_field, port->name);
        return -1;
    }

    /* Check if thread context requires queueing to slow path */
    int is_slow_path = 0;
    if (ctx->slow_path_thread != 0) {
#ifdef _WIN32
        if (GetCurrentThreadId() == GetThreadId(ctx->slow_path_thread)) {
            is_slow_path = 1;
        }
#else
        if (pthread_equal(pthread_self(), ctx->slow_path_thread)) {
            is_slow_path = 1;
        }
#endif
    }

    if (route_to_control && !is_slow_path) {
        int port_idx = -1;
        ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
        mutex_lock(&ctx->port_mutex);
        for (int p = 0; p < ctx->port_count; p++) {
            if (ctx->ports[p] == port) {
                port_idx = p;
                break;
            }
        }
        mutex_unlock(&ctx->port_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);

        if (port_idx != -1) {
            if (spsc_queue_push(&ctx->sig_queue, frame, frame_len, (u16)port_idx) < 0) {
                LOG_WARN("Signaling queue full, dropped control frame from %s", port->name);
            }
            return 0;
        }
    }

    /* Update port stats */
    port->stats.rx_frames++;
    port->stats.rx_bytes += frame_len;

    /* Capture destuffed frame bytes starting at Q.922 Address in standard DLT_FRELAY (107) format */
    size_t cap_len = frame_len;
    if (port->capture) {
        pcap_writer_write(port->capture, frame, cap_len);
    }
    if (ctx->global_capture) {
        pcap_writer_write(ctx->global_capture, frame, cap_len);
    }

    /* ================================================================
     * LMI DLCI 0 / 1023 — demultiplex to appropriate LMI handler
     * ================================================================ */
    if (addr.dlci == FR_DLCI_LMI || addr.dlci == FR_DLCI_CISCO) {
        size_t addr_len = fr_get_addr_len(frame, frame_len);
        int is_lmi = 0;

        if (frame_len >= addr_len + 4 && (frame[addr_len] & ~0x10) == 0x03) {
            u8 expected_pd = (addr.dlci == FR_DLCI_CISCO) ? 0x09 : 0x08;
            if (frame[addr_len + 1] == expected_pd && frame[addr_len + 2] == 0x00) {
                is_lmi = 1;
            }
        }

        if (is_lmi) {
            if (!port->lmi_ctx) return 0;  /* No LMI — drop silently */

            mutex_lock(&port->mutex);

            u8 msg_type = frame[addr_len + 3];

            /* Route to correct LMI type handler.
             * Cisco LMI uses the same message types as ANSI (0x75/0x7D) but with
             * different Information Element codes. Detect by the port's configured
             * type; also detect from the first IE if the port has no explicit config.
             */
            if (msg_type == 0x75 || msg_type == 0xA3) {
                vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
                int detected_type = lmi->type;

                if (msg_type == 0xA3) {
                    detected_type = LMI_TYPE_Q933A;
                } else if (addr.dlci == FR_DLCI_CISCO) {
                    detected_type = LMI_TYPE_CISCO;
                } else if (msg_type == 0x75 && addr.dlci == 0) {
                    /* Auto-detect ANSI LMI if locking shift to codeset 5 (0x95) IE is present */
                    if (frame_len >= addr_len + 5 && frame[addr_len + 4] == 0x95) {
                        detected_type = LMI_TYPE_ANSI;
                    } else {
                        detected_type = LMI_TYPE_Q933A;
                    }
                }

                LOG_DEBUG("LMI STATUS ENQUIRY on %s: type=0x%02X, LMI_type=%d",
                          port->name, msg_type, detected_type);

                send_lmi_status_response(port, frame, frame_len, detected_type);
                mutex_unlock(&port->mutex);
                return 0;
            } else if (msg_type == 0x7D) {
                vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
                int detected_type = lmi->type;

                if (addr.dlci == FR_DLCI_CISCO) {
                    detected_type = LMI_TYPE_CISCO;
                } else if (addr.dlci == 0) {
                    /* Auto-detect ANSI LMI if locking shift to codeset 5 (0x95) IE is present */
                    if (frame_len >= addr_len + 5 && frame[addr_len + 4] == 0x95) {
                        detected_type = LMI_TYPE_ANSI;
                    } else {
                        detected_type = LMI_TYPE_Q933A;
                    }
                }

                LOG_DEBUG("LMI STATUS on %s: type=0x%02X, LMI_type=%d",
                          port->name, msg_type, detected_type);

                const u8 *body = frame + addr_len + 1;  /* skip Address bytes and 1 UI control byte */
                size_t body_len = frame_len - addr_len - 1;
                int handler_ret = -1;

                switch (detected_type) {
                case LMI_TYPE_Q933A:
                    handler_ret = lmi_q933a_handle_frame(port, body, body_len);
                    break;
                case LMI_TYPE_CISCO:
                    handler_ret = lmi_gof_handle_frame(port, body, body_len);
                    break;
                case LMI_TYPE_ANSI:
                default:
                    handler_ret = lmi_ansi_handle_frame(port, body, body_len);
                    break;
                }

                if (handler_ret == 2) {
                    /* DTE Annex G: Full Status Continued received.
                     * Immediately increment sequence numbers, build next continued STATUS ENQUIRY,
                     * transmit it, and restart the T391 timer.
                     */
                    lmi->dte_seq_send = lmi_inc_seq(lmi->dte_seq_send);
                    lmi->dte_last_request_was_full = 1;
                    u8 reply_buf[256];
                    int reply_len = 0;

                    if (detected_type == LMI_TYPE_Q933A) {
                        reply_len = lmi_q933a_build_enquiry(port, reply_buf, sizeof(reply_buf) - 2);
                    } else if (detected_type == LMI_TYPE_ANSI) {
                        reply_len = lmi_ansi_build_enquiry(port, reply_buf, sizeof(reply_buf) - 2);
                    }

                    if (reply_len > 0 && reply_len + 2 <= (int)sizeof(reply_buf)) {
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
                                LOG_DEBUG("LMI DTE Continued STATUS ENQUIRY sent on %s (seq=%u, len=%d)",
                                          port->name, lmi->dte_seq_send, reply_len);
                            } else {
                                LOG_WARN("Failed to send DTE Continued STATUS ENQUIRY on %s", port->name);
                            }
                        }
                    }
                    timer_set(&lmi->t391_timer, lmi->t391 * 1000);
                }
                mutex_unlock(&port->mutex);
                return 0;
            }
        }
    }

    /* LAPF processing on any DLCI (such as DLCI 0, 1015, or SVC channels) */
    {
        /* Decode control byte to see if it is SABME command on any DLCI */
        size_t addr_len = fr_get_addr_len(frame, frame_len);
        int needs_init = 0;

        mutex_lock(&port->mutex);
        if (!port_get_lapf_ctx(port, addr.dlci)) {
            if (frame_len >= addr_len + 1) {
                u8 ctrl = frame[addr_len];
                if ((ctrl & ~0x10) == LAPF_CTRL_U_SABME) {
                    if (addr.dlci == 0 && !port->svc_ctx) {
                        /* Do not auto-activate DLCI 0 LAPF when SVC service is not active */
                    } else if (addr.dlci == 1015 && port->type != PORT_TYPE_NNI) {
                        /* Do not auto-activate DLCI 1015 LAPF on non-NNI ports */
                    } else {
                        needs_init = 1;
                    }
                }
            }
        }
        mutex_unlock(&port->mutex);

        if (needs_init) {
            LOG_INFO("LAPF on %s: Auto-activating LAPF stack for DLCI %u on SABME receipt", port->name, addr.dlci);
            lapf_port_init(port, addr.dlci, ctx->default_lapf_k, ctx->default_lapf_n200,
                           ctx->default_lapf_n201, ctx->default_lapf_t200, ctx->default_lapf_t203);
        }

        int has_lapf = 0;
        mutex_lock(&port->mutex);
        if (port_get_lapf_ctx(port, addr.dlci)) {
            has_lapf = 1;
        }
        mutex_unlock(&port->mutex);

        if (has_lapf) {
            /* Frame on a DLCI with LAPF enabled: process locally with LAPF stack */
            lapf_handle_frame(port, frame, frame_len);
            return 0;
        }
    }

    /* ================================================================
     * CLLM DLCI 1007 — Consolidated Link Layer Management
     * ================================================================ */
    if (addr.dlci == FR_DLCI_RESERVED) {
        size_t addr_len = fr_get_addr_len(frame, frame_len);
        if (frame_len >= addr_len + 1 && (frame[addr_len] & ~0x10) == 0xAF) {
            cllm_handle_frame(port, frame, frame_len);
        } else {
            LOG_DEBUG("CLLM DLCI 1007 received non-XID frame — dropped");
        }
        return 0;
    }

    /* ================================================================
     * User traffic — validate and switch
     * ================================================================ */

    /* Verify LMI link is operational before forwarding user traffic */
    if (port->lmi_ctx) {
        vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
        if (lmi->dce_link_down || (lmi->dte_enabled && lmi->dte_link_down)) {
            LOG_DEBUG("User frame dropped on %s: LMI link down", port->name);
            port->stats.dropped++;
            return -1;
        }
    }

    /* Check for FRF.12 Fragmentation reassembly */
    size_t in_addr_len = fr_get_addr_len(frame, frame_len);
    vfr_dlci_entry_t *in_entry = port_lookup_dlci(port, addr.dlci);
    if (in_entry && (in_entry->fragment_size > 0 || port->fragment_size > 0 || (frame_len > in_addr_len + 2 && (frame[in_addr_len] & 0xC0) != 0))) {
        if (!in_entry->reasm_ctx) {
            in_entry->reasm_ctx = calloc(1, sizeof(fr_reasm_ctx_t));
            if (in_entry->reasm_ctx) frf12_reasm_init((fr_reasm_ctx_t *)in_entry->reasm_ctx);
        }
        if (in_entry->reasm_ctx) {
            u8 reasm_out[FR_MAX_FRAMESZ + 128];
            size_t reasm_len = 0;
            int is_comp = 0;
            if (frf12_reassemble_frame((fr_reasm_ctx_t *)in_entry->reasm_ctx, frame, frame_len, reasm_out, &reasm_len, &is_comp) == 0) {
                if (!is_comp) {
                    return 0; /* Middle or initial fragment buffered, awaiting more fragments */
                }
                return vfrs_switch_frame(ctx, port, addr.dlci, reasm_out, reasm_len);
            }
        }
    }

    /* Switch the frame.
     * Pass the destuffed frame body (frame_len bytes, no FCS). The switching
     * function will re-encode the FCS after rewriting the DLCI address.
     */
    return vfrs_switch_frame(ctx, port, addr.dlci, frame, frame_len);
}

/* Send LMI STATUS response.
 *
 * Architecture:
 *   1. Call the appropriate LMI-type handler (lmi_ansi_handle_frame,
 *      lmi_q933a_handle_frame, or lmi_cisco_handle_frame) to parse IEs
 *      and update LMI state. Each handler stores send_seq/recv_seq in the
 *      LMI state struct and returns 1 (= STATUS ENQUIRY, respond required).
 *   2. Use the LMI state's dte_seq_send/recv values to build the STATUS.
 *   3. Append FCS-16 and wrap in HDLC flags/stuffing for serial transport.
 *
 * This eliminates the duplicate IE parsing that previously existed inside
 * send_lmi_status_response, replacing it with a single call to the handler.
 *
 * frame       — the destuffed frame body (from fr_parse_frame), starting at
 *               the Q.922 address field. The 2-byte FCS is at
 *               frame[frame_len] and frame[frame_len+1].
 * frame_len   — length of the destuffed frame body (excl. the 2 FCS bytes).
 * detected_type — LMI_TYPE_ANSI, LMI_TYPE_Q933A, or LMI_TYPE_CISCO.
 */
static int send_lmi_status_response(vfr_port_t *port, const u8 *frame,
                                     size_t frame_len, int detected_type)
{
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    vfrs_ctx_t *vfrs = g_vfrs;
    u8 response[512];
    int resp_len = 0;
    int use_lmi_type;
    int full_status = 1;

    if (!lmi || !port->ops) return -1;

    use_lmi_type = (detected_type != 0) ? detected_type : lmi->type;

    /* Call the appropriate LMI-type handler.
     * Handlers parse IEs from the frame (which starts at the Q.922 address field)
     * and update LMI state: dte_seq_send, dte_seq_recv, dce_seq_recv, report_type,
     * and the PVC status cache. They return 1 for STATUS ENQUIRY (= respond).
     *
     * NOTE: The handler expects frame starting at the PD byte (after Q.922 address and UI control).
     * frame_len covers the entire destuffed body incl. address. The handler parses
     * from frame+offset onward, where offset = addr_len + 1 (skip Q.922 address + UI control).
     */
    {
        size_t addr_len = fr_get_addr_len(frame, frame_len);
        const u8 *body = frame + addr_len + 1;  /* skip Address bytes and 1 UI control byte */
        size_t body_len = frame_len - addr_len - 1;
        int handler_ret = -1;

        switch (use_lmi_type) {
        case LMI_TYPE_Q933A:
            handler_ret = lmi_q933a_handle_frame(port, body, body_len);
            break;
        case LMI_TYPE_CISCO:
            handler_ret = lmi_gof_handle_frame(port, body, body_len);
            break;
        case LMI_TYPE_ANSI:
        default:
            handler_ret = lmi_ansi_handle_frame(port, body, body_len);
            break;
        }

        if (handler_ret != 1) {
            /* Not a STATUS ENQUIRY or handler error — no response needed */
            LOG_DEBUG("LMI handler ret=%d on %s — no response", handler_ret, port->name);
            return 0;
        }
    }

    /* DCE error monitoring (X.36 §11.4.1.6.1): STATUS ENQ receipt is an event.
     * It is an error if recv_seq != dce_seq_send (and recv_seq != 0).
     * recv_seq == 0 is only valid as an initialization sentinel when the link is down. */
    int is_error = lmi->dce_link_down ? (lmi->dce_seq_recv != 0 && lmi->dce_seq_recv != lmi->dce_seq_send)
                                      : (lmi->dce_seq_recv != lmi->dce_seq_send);
    int force_full_status = 0;

    if (is_error) {
        LOG_DEBUG("DCE LMI seq mismatch on %s: expect %u (our TxSN), got %u (DTE RxSN)",
                  port->name, lmi->dce_seq_send, lmi->dce_seq_recv);
        if (lmi->dce_last_was_full) {
            force_full_status = 1;
            LOG_INFO("Re-transmitting lost LMI Full Status report on %s due to RxSN mismatch", port->name);
        }
        lmi->dce_errors++;
    }
    lmi->dce_events++;

    /* Update sliding window error history (X.36 §11.6.2) */
    lmi->dce_history = ((lmi->dce_history << 1) | (u16)is_error) & (u16)((1u << lmi->n393) - 1u);

    /* Count errors in the last n393 events */
    int error_count = 0;
    for (int bits = 0; bits < lmi->n393; bits++) {
        if ((lmi->dce_history >> bits) & 1) {
            error_count++;
        }
    }

    /* Check thresholds to declare link state */
    if (error_count >= lmi->n392) {
        if (!lmi->dce_link_down) {
            lmi->dce_link_down = 1;
            LOG_WARN("LMI DCE channel down on %s (sequence error threshold reached: %d/%d)",
                      port->name, error_count, lmi->n393);
            vfrs_propagate_port_status_change(vfrs, port);
        }
    } else {
        /* Recovery: N392 consecutive error-free events clear the error state */
        u16 recovery_mask = (u16)((1u << lmi->n392) - 1u);
        if ((lmi->dce_history & recovery_mask) == 0 && lmi->dce_link_down) {
            lmi->dce_link_down = 0;
            LOG_INFO("LMI DCE channel recovered on %s", port->name);
            vfrs_propagate_port_status_change(vfrs, port);
        }
    }

    /* Clear New (N) flag on PVCs if the DTE acknowledges the sequence in which it was sent */
    if (vfrs && lmi->dce_seq_recv != 0) {
        ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
        mutex_lock(&vfrs->pvc_mutex);
        for (int i = 0; i < PVC_HASH_SIZE; i++) {
            vfr_pvc_detail_t *pvc = vfrs->pvc_table[i];
            while (pvc) {
                if (strcmp(pvc->port_in, port->name) == 0) {
                    if (pvc->lmi_new && pvc->lmi_new_txsn == lmi->dce_seq_recv) {
                        pvc->lmi_new = 0;
                        LOG_DEBUG("LMI PVC New bit acknowledged for DLCI %u on %s", pvc->dlci_in, port->name);
                    }
                }
                pvc = pvc->next;
            }
        }
        mutex_unlock(&vfrs->pvc_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);

        ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
        mutex_lock(&vfrs->mcast_mutex);
        vfr_mcast_group_t *g = vfrs->mcast_groups;
        while (g) {
            if (strcmp(g->source_port, port->name) == 0) {
                if (g->root_lmi_new && g->root_lmi_new_txsn == lmi->dce_seq_recv) {
                    g->root_lmi_new = 0;
                    LOG_DEBUG("LMI Multicast Root New bit acknowledged for DLCI %u on %s", g->source_dlci, port->name);
                }
            }
            vfr_mcast_member_t *m = g->members;
            while (m) {
                if (strcmp(m->port_name, port->name) == 0) {
                    if (m->lmi_new && m->lmi_new_txsn == lmi->dce_seq_recv) {
                        m->lmi_new = 0;
                        LOG_DEBUG("LMI Multicast Leaf/Member New bit acknowledged for DLCI %u on %s", m->dlci, port->name);
                    }
                }
                m = m->next;
            }
            g = g->next;
        }
        mutex_unlock(&vfrs->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
    }

    /* Reset T392 timer — DCE has responded to the STATUS ENQUIRY */
    timer_set(&lmi->t392_timer, lmi->t392 * 1000);

    /* Pacing reset: a fresh Full Status request from the DTE restarts segmentation */
    if (lmi->last_report_type == LMI_REPORT_FULL_STATUS) {
        lmi->segment_active = 0;
        lmi->pvc_send_index = 0;
    }

    /* Determine if full status is needed (DTE explicit request, Annex G continuation, or forced) */
    if (lmi->last_report_type == LMI_REPORT_FULL_STATUS || lmi->last_report_type == 0x04 || force_full_status) {
        full_status = 1;
    } else {
        full_status = 0;
        lmi->dce_last_was_full = 0;
    }

    /* Build STATUS response using the appropriate LMI-type builder.
     * NOTE: Each builder increments lmi->dce_seq_send internally just before
     * placing it in the Link Integrity Verification IE (CB-1 fix). */
    switch (use_lmi_type) {
    case LMI_TYPE_Q933A:
        resp_len = lmi_q933a_build_status(port, response, sizeof(response), full_status);
        break;
    case LMI_TYPE_CISCO:
        resp_len = lmi_cisco_build_status(port, response, sizeof(response), full_status);
        break;
    case LMI_TYPE_ANSI:
    default:
        resp_len = lmi_ansi_build_status(port, response, sizeof(response), full_status);
        break;
    }

    if (resp_len <= 0) {
        LOG_WARN("LMI build failed on %s", port->name);
        return -1;
    }

    /* Compute and append FCS-16 per X.36 §9.2.4 only for serial and named pipe transports.
     * FCS covers [address..data] = response[0..resp_len-1], LSB-first. */
    if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
        u16 fcs = crc16_fcs(response, (size_t)resp_len);
        if (resp_len + 2 > (int)sizeof(response)) {
            LOG_WARN("Response too long for FCS on %s", port->name);
            return -1;
        }
        response[resp_len++] = fcs & 0xFF;
        response[resp_len++] = (fcs >> 8) & 0xFF;
    }

    if (vfrs) {
        int ret = port->ops->send(port, response, (size_t)resp_len);
        if (ret >= 0) {
            port->stats.tx_frames++;
            port->stats.tx_bytes += (u64)ret;
            LOG_DEBUG("LMI STATUS sent on %s (type=%d, seq=%u, full=%d)",
                      port->name, use_lmi_type, lmi->dce_seq_send, full_status);
            return 0;
        } else {
            LOG_WARN("Failed to send LMI STATUS on %s", port->name);
        }
    }
    return -1;
}

/* ============================================================
 * Statistics
 * ============================================================ */

void vfrs_show_ports(vfrs_ctx_t *ctx, FILE *out)
{
    int i;
    if (!ctx || !out) return;

    fprintf(out, "\n=== Port Status ===\n");
    fprintf(out, "%-12s %-4s %-8s %-10s %-10s %-10s %-10s\n",
            "Name", "Type", "Status", "TX Frames", "RX Frames", "TX Bytes", "RX Bytes");
    fprintf(out, "------------------------------------------------------------------------\n");

    mutex_lock(&ctx->port_mutex);
    for (i = 0; i < ctx->port_count; i++) {
        vfr_port_t *p = ctx->ports[i];
        fprintf(out, "%-12s %-4s %-8s %10llu %10llu %10llu %10llu\n",
                p->name,
                p->type == PORT_TYPE_UNI ? "UNI" : "NNI",
                p->status == PORT_STATUS_UP ? "UP" :
                (p->status == PORT_STATUS_CONN ? "CONNECTED" : "DOWN"),
                (unsigned long long)p->stats.tx_frames,
                (unsigned long long)p->stats.rx_frames,
                (unsigned long long)p->stats.tx_bytes,
                (unsigned long long)p->stats.rx_bytes);
    }
    mutex_unlock(&ctx->port_mutex);
}

void vfrs_show_pvcs(vfrs_ctx_t *ctx, FILE *out)
{
    int i;
    if (!ctx || !out) return;

    fprintf(out, "\n=== PVC Table ===\n");
    fprintf(out, "%-12s %-6s %-12s %-6s %-8s %-10s %-10s\n",
            "In Port", "In DLCI", "Out Port", "Out DLCI", "Active", "TX Frames", "RX Frames");
    fprintf(out, "------------------------------------------------------------------------\n");

    mutex_lock(&ctx->pvc_mutex);
    for (i = 0; i < PVC_HASH_SIZE; i++) {
        vfr_pvc_detail_t *pvc = ctx->pvc_table[i];
        while (pvc) {
            u64 tx_f = 0, rx_f = 0;
            vfr_dlci_entry_t *entry = dlci_table_lookup(ctx, pvc->port_in, pvc->dlci_in);
            if (entry) {
                tx_f = entry->tx_frames;
                rx_f = entry->rx_frames;
            }
            fprintf(out, "%-12s %-6u %-12s %-6u %-8s %10llu %10llu\n",
                    pvc->port_in, pvc->dlci_in,
                    pvc->port_out, pvc->dlci_out,
                    pvc->active ? "YES" : "NO",
                    (unsigned long long)tx_f,
                    (unsigned long long)rx_f);
            pvc = pvc->next;
        }
    }
    mutex_unlock(&ctx->pvc_mutex);
}

void vfrs_show_dlci_table(vfrs_ctx_t *ctx, FILE *out)
{
    int i;
    if (!ctx || !out) return;

    fprintf(out, "\n=== Global DLCI Routing Table ===\n");
    fprintf(out, "%-12s %-6s %-12s %-6s %-6s %-8s %-10s %-10s\n",
            "In Port", "In DLCI", "Out Port", "Out DLCI", "Type", "Active", "TX Frames", "RX Frames");
    fprintf(out, "----------------------------------------------------------------------------------------\n");

    mutex_lock(&ctx->dlci_mutex);
    for (i = 0; i < PVC_HASH_SIZE; i++) {
        vfr_dlci_entry_t *entry = ctx->dlci_table[i];
        while (entry) {
            const char *type_str = "Unknown";
            if (entry->vc_type == VC_TYPE_PVC) type_str = "PVC";
            else if (entry->vc_type == VC_TYPE_SVC) type_str = "SVC";
            else if (entry->vc_type == VC_TYPE_MCAST) type_str = "MCAST";
            else if (entry->vc_type == VC_TYPE_RESERVED) type_str = "RES";

            fprintf(out, "%-12s %-6u %-12s %-6u %-6s %-8s %10llu %10llu\n",
                    entry->port_in, entry->dlci_in,
                    entry->port_out, entry->dlci_out,
                    type_str,
                    entry->active ? "YES" : "NO",
                    (unsigned long long)entry->tx_frames,
                    (unsigned long long)entry->rx_frames);
            entry = entry->next;
        }
    }
    mutex_unlock(&ctx->dlci_mutex);
}

void vfrs_show_stats(vfrs_ctx_t *ctx, const char *port_name, FILE *out)
{
    vfr_port_t *port;
    if (!ctx || !out) return;

    if (port_name) {
        port = vfrs_find_port(ctx, port_name);
        if (!port) {
            fprintf(out, "Port '%s' not found\n", port_name);
            return;
        }
        fprintf(out, "\n=== Statistics for %s ===\n", port_name);
    } else {
        vfrs_show_ports(ctx, out);
        vfrs_show_pvcs(ctx, out);
        vfrs_show_dlci_table(ctx, out);
        return;
    }

    fprintf(out, "Status: %s\n",
            port->status == PORT_STATUS_UP ? "UP" :
            (port->status == PORT_STATUS_CONN ? "CONNECTED" : "DOWN"));
    fprintf(out, "Type: %s\n", port->type == PORT_TYPE_UNI ? "UNI" : "NNI");
    fprintf(out, "Transport: %d\n\n", port->transport);

    fprintf(out, "TX Frames: %llu\n", (unsigned long long)port->stats.tx_frames);
    fprintf(out, "RX Frames: %llu\n", (unsigned long long)port->stats.rx_frames);
    fprintf(out, "TX Bytes: %llu\n", (unsigned long long)port->stats.tx_bytes);
    fprintf(out, "RX Bytes: %llu\n", (unsigned long long)port->stats.rx_bytes);
    fprintf(out, "FCS Errors: %llu\n", (unsigned long long)port->stats.fcs_errors);
    fprintf(out, "Dropped: %llu\n", (unsigned long long)port->stats.dropped);
    fprintf(out, "Switched: %llu\n", (unsigned long long)port->stats.switched);
}

void vfrs_show_swconfig(vfrs_ctx_t *ctx, FILE *out)
{
    if (!ctx || !out) return;
    fprintf(out, "\n=== Switch Identity & SVC Numbering (swconfig) ===\n");
    fprintf(out, "Switch ID (swid):       %s\n", ctx->swid);
    fprintf(out, "Data Network ID (dnic): %04u\n", ctx->dnic);
    fprintf(out, "Data Country Code (dcc):%u\n", ctx->dcc);
    fprintf(out, "Network Digit (nd):     %u\n", ctx->nd);
    fprintf(out, "Private DNIC (pnic):    %u\n", ctx->pnic);
    fprintf(out, "System Group Code (sgc):%u (len=%u)\n", ctx->sgc, ctx->sgclen);
    fprintf(out, "System Ident Code (sic):%u (len=%u)\n", ctx->sic, ctx->siclen);
    fprintf(out, "Subscriber Number Len:  %u\n\n", ctx->subnumlen);
}

void vfrs_show_defaults(vfrs_ctx_t *ctx, FILE *out)
{
    if (!ctx || !out) return;
    fprintf(out, "\n=== Global Parameter Defaults ===\n");
    fprintf(out, "LAPF:\n");
    fprintf(out, "  k=%u, n200=%u, n201=%u octets, t200=%ums, t203=%ums\n",
            ctx->default_lapf_k, ctx->default_lapf_n200, ctx->default_lapf_n201,
            ctx->default_lapf_t200, ctx->default_lapf_t203);
    fprintf(out, "LMI (DCE):\n");
    fprintf(out, "  t392=%us, n392=%u, n393=%u\n",
            ctx->default_t392, ctx->default_n392, ctx->default_n393);
    fprintf(out, "LMI (DTE):\n");
    fprintf(out, "  t391=%us, n391=%u, n392=%u, n393=%u\n",
            ctx->default_t391, ctx->default_n391, ctx->default_n392_dte, ctx->default_n393_dte);
    fprintf(out, "SVC Timers:\n");
    fprintf(out, "  t301=%ums, t303=%ums, t305=%ums, t308=%ums, t310=%ums, t316=%ums, t317=%ums, t322=%ums\n",
            ctx->default_svc_t301, ctx->default_svc_t303, ctx->default_svc_t305,
            ctx->default_svc_t308, ctx->default_svc_t310, ctx->default_svc_t316,
            ctx->default_svc_t317, ctx->default_svc_t322);
    fprintf(out, "Service & QoS Defaults:\n");
    fprintf(out, "  Physical Access Rate (ar): %u bps\n", ctx->access_rate);
    fprintf(out, "  SVC CIR=%u bps, Bc=%u bits, Be=%u bits\n",
            ctx->default_cir, ctx->default_bc, ctx->default_be);
    fprintf(out, "  SVC fmif=%u octets, ftp=%u, fdp=%u, srv_class=%u\n\n",
            ctx->default_svc_fmif ? ctx->default_svc_fmif : FR_MAX_FRAMESZ,
            ctx->default_svc_ftp ? ctx->default_svc_ftp : 8,
            ctx->default_svc_fdp ? ctx->default_svc_fdp : 4,
            ctx->default_svc_class ? ctx->default_svc_class : 1);
}

void vfrs_show_config(vfrs_ctx_t *ctx, FILE *out)
{
    if (!ctx || !out) return;

    fprintf(out, "\n# ======================================================================\n");
    fprintf(out, "# VFRS Running Configuration (Dump)\n");
    fprintf(out, "# ======================================================================\n\n");

    /* 1. swconfig */
    fprintf(out, "swconfig swid=%s dnic=%u dcc=%u nd=%u pnic=%u sgclen=%u sgc=%u siclen=%u sic=%u subnumlen=%u\n\n",
            ctx->swid, ctx->dnic, ctx->dcc, ctx->nd, ctx->pnic,
            ctx->sgclen, ctx->sgc, ctx->siclen, ctx->sic, ctx->subnumlen);

    /* 2. defaults */
    fprintf(out, "defaults lapf_k=%u lapf_n200=%u lapf_n201=%u lapf_t200=%.1f lapf_t203=%.1f \\\n",
            ctx->default_lapf_k, ctx->default_lapf_n200, ctx->default_lapf_n201,
            (double)ctx->default_lapf_t200 / 1000.0, (double)ctx->default_lapf_t203 / 1000.0);
    fprintf(out, "         lmi_t392=%u lmi_n392=%u lmi_n393=%u lmi_dte_t391=%u lmi_dte_n391=%u lmi_dte_n392=%u lmi_dte_n393=%u \\\n",
            ctx->default_t392, ctx->default_n392, ctx->default_n393,
            ctx->default_t391, ctx->default_n391, ctx->default_n392_dte, ctx->default_n393_dte);
    fprintf(out, "         ar=%u svc_default_cir=%u svc_default_bc=%u svc_default_be=%u svc_default_fmif=%u svc_default_ftp=%u svc_default_fdp=%u svc_default_svc_class=%u\n\n",
            ctx->access_rate, ctx->default_cir, ctx->default_bc, ctx->default_be,
            ctx->default_svc_fmif ? ctx->default_svc_fmif : FR_MAX_FRAMESZ,
            ctx->default_svc_ftp ? ctx->default_svc_ftp : 8,
            ctx->default_svc_fdp ? ctx->default_svc_fdp : 4,
            ctx->default_svc_class ? ctx->default_svc_class : 1);

    /* 3. ports */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);
    for (int i = 0; i < ctx->port_count; i++) {
        vfr_port_t *port = ctx->ports[i];
        if (!port) continue;
        const char *tname = "unknown";
        switch (port->transport) {
            case PORT_TRANS_UDP: tname = "udp"; break;
            case PORT_TRANS_TCP: tname = "tcp"; break;
            case PORT_TRANS_TCP_CLI: tname = "tcp-client"; break;
            case PORT_TRANS_TCP_SER: tname = "tcp-server"; break;
            case PORT_TRANS_SERIAL: tname = "serial"; break;
            case PORT_TRANS_PIPE: tname = "pipe"; break;
            case PORT_TRANS_UDP_CLI: tname = "udp-client"; break;
            case PORT_TRANS_UDP_SER: tname = "udp-server"; break;
            case PORT_TRANS_L2TPV3_FR: tname = "l2tpv3-fr"; break;
            case PORT_TRANS_L2TPV3_HDLC: tname = "l2tpv3-hdlc"; break;
        }
        fprintf(out, "# Port: %s (type=%s, transport=%s, dlcibit=%u, status=%s)\n",
                port->name, port->type == PORT_TYPE_UNI ? "UNI" : "NNI",
                tname, port->dlcibit, port->status == PORT_STATUS_UP ? "UP" : "DOWN");
    }
    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);

    /* 4. PVCs */
    fprintf(out, "\n");
    vfrs_show_pvcs(ctx, out);

    /* 5. SVC Subscribers and Routes */
    fprintf(out, "\n");
    vfrs_show_svc_subscribers(ctx, out);
    fprintf(out, "\n");
    vfrs_show_svc_routes(ctx, out);
    fprintf(out, "\n");
}
