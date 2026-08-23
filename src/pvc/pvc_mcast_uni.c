/*
 * pvc_mcast_uni.c - Frame Relay PVC Multicasting for UNI
 */

#include "vfr.h"

static int is_pvc_endpoint(vfrs_ctx_t *ctx, const char *port, u32 dlci)
{
    if (!ctx || !port) return 0;
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);
    int hash = PVC_HASH(dlci);
    vfr_pvc_detail_t *pvc = ctx->pvc_table[hash];
    while (pvc) {
        if (strcmp(pvc->port_in, port) == 0 && pvc->dlci_in == dlci) {
            mutex_unlock(&ctx->pvc_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
            return 1;
        }
        pvc = pvc->next;
    }
    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
    return 0;
}

static int vfrs_mcast_port_is_active_uni(vfrs_ctx_t *ctx, const char *port_name)
{
    vfr_port_t *port = vfrs_find_port_unlocked(ctx, port_name);
    if (!port || port->status == PORT_STATUS_DOWN) return 0;
    vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
    if (lmi) {
        if (lmi->dce_link_down) return 0;
        if (lmi->dte_enabled && lmi->dte_link_down) return 0;
    }
    return 1;
}

vfr_mcast_group_t *vfrs_find_mcast_group_by_endpoint_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci, int *is_root)
{
    if (!ctx || !port) return NULL;
    vfr_mcast_group_t *g = ctx->mcast_groups;
    while (g) {
        /* Check root/source endpoint */
        if (strcmp(g->source_port, port) == 0 && g->source_dlci == dlci) {
            if (is_root) *is_root = 1;
            return g;
        }
        /* Check member endpoints */
        vfr_mcast_member_t *m = g->members;
        while (m) {
            if (strcmp(m->port_name, port) == 0 && m->dlci == dlci) {
                if (is_root) *is_root = 0;
                return g;
            }
            m = m->next;
        }
        g = g->next;
    }
    return NULL;
}

vfr_mcast_group_t *vfrs_find_mcast_group_by_endpoint(vfrs_ctx_t *ctx, const char *port, u32 dlci, int *is_root)
{
    if (!ctx) return NULL;
    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    vfr_mcast_group_t *g = vfrs_find_mcast_group_by_endpoint_unlocked(ctx, port, dlci, is_root);
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
    return g;
}

int vfrs_mcast_endpoint_is_active_uni(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, const char *port, int is_root)
{
    int active = 0;
    if (g->mode == MCAST_MODE_ONEWAY || g->mode == MCAST_MODE_TWOWAY) {
        if (is_root) {
            /* Root: active if root port is UP and at least one leaf is active */
            if (vfrs_mcast_port_is_active_uni(ctx, g->source_port)) {
                vfr_mcast_member_t *m = g->members;
                while (m) {
                    if (vfrs_mcast_port_is_active_uni(ctx, m->port_name)) {
                        active = 1;
                        break;
                    }
                    m = m->next;
                }
            }
        } else {
            /* Leaf: active if leaf port is UP and root port is UP */
            if (vfrs_mcast_port_is_active_uni(ctx, port) && vfrs_mcast_port_is_active_uni(ctx, g->source_port)) {
                active = 1;
            }
        }
    } else if (g->mode == MCAST_MODE_NWAY) {
        /* NWAY: active if local port is UP and at least one other member is active */
        if (vfrs_mcast_port_is_active_uni(ctx, port)) {
            int active_others = 0;
            if (strcmp(g->source_port, port) != 0) {
                if (vfrs_mcast_port_is_active_uni(ctx, g->source_port)) {
                    active_others = 1;
                }
            }
            vfr_mcast_member_t *m = g->members;
            while (m && !active_others) {
                if (strcmp(m->port_name, port) != 0) {
                    if (vfrs_mcast_port_is_active_uni(ctx, m->port_name)) {
                        active_others = 1;
                    }
                }
                m = m->next;
            }
            if (active_others) {
                active = 1;
            }
        }
    }
    return active;
}

int vfrs_mcast_endpoint_is_active_unlocked(vfrs_ctx_t *ctx, const char *port_name, u32 dlci)
{
    if (!ctx || !port_name) return 0;

    int is_root = 0;
    vfr_mcast_group_t *g = vfrs_find_mcast_group_by_endpoint_unlocked(ctx, port_name, dlci, &is_root);
    if (!g) {
        return 0;
    }

    vfr_port_t *port = vfrs_find_port_unlocked(ctx, port_name);
    if (port && port->type == PORT_TYPE_NNI) {
        return vfrs_mcast_endpoint_is_active_nni(ctx, g, port_name, is_root);
    } else {
        return vfrs_mcast_endpoint_is_active_uni(ctx, g, port_name, is_root);
    }
}

int vfrs_mcast_endpoint_is_active(vfrs_ctx_t *ctx, const char *port, u32 dlci)
{
    if (!ctx || !port) return 0;
    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    int active = vfrs_mcast_endpoint_is_active_unlocked(ctx, port, dlci);
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
    return active;
}

int vfrs_add_mcast_group(vfrs_ctx_t *ctx, const char *name, const char *src_port, u32 src_dlci, const char *mode_str, u32 cir, u32 bc, u32 be)
{
    if (!ctx || !name || !src_port) return -1;

    if (!fr_dlci_valid(src_dlci)) {
        LOG_ERROR("Invalid multicast DLCI: %u", src_dlci);
        return -1;
    }

    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    vfr_mcast_group_t *g = ctx->mcast_groups;
    while (g) {
        if (strcmp(g->name, name) == 0) {
            LOG_ERROR("Multicast group name '%s' already exists", name);
            mutex_unlock(&ctx->mcast_mutex);
            RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
            return -1;
        }
        g = g->next;
    }

    int dummy;
    if (vfrs_find_mcast_group_by_endpoint_unlocked(ctx, src_port, src_dlci, &dummy)) {
        LOG_ERROR("Multicast endpoint %s:%u already belongs to a multicast group", src_port, src_dlci);
        mutex_unlock(&ctx->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
        return -1;
    }
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

    if (is_pvc_endpoint(ctx, src_port, src_dlci)) {
        LOG_ERROR("Multicast endpoint %s:%u is already configured as a PVC", src_port, src_dlci);
        return -1;
    }

    int mode = MCAST_MODE_ONEWAY;
    if (mode_str) {
        if (strcmp(mode_str, "oneway") == 0) mode = MCAST_MODE_ONEWAY;
        else if (strcmp(mode_str, "twoway") == 0) mode = MCAST_MODE_TWOWAY;
        else if (strcmp(mode_str, "nway") == 0) mode = MCAST_MODE_NWAY;
        else {
            LOG_WARN("Unknown multicast mode '%s', defaulting to oneway", mode_str);
        }
    }

    vfr_mcast_group_t *new_g = calloc(1, sizeof(vfr_mcast_group_t));
    if (!new_g) {
        LOG_ERROR("Failed to allocate multicast group");
        return -1;
    }

    strncpy(new_g->name, name, sizeof(new_g->name) - 1);
    strncpy(new_g->source_port, src_port, sizeof(new_g->source_port) - 1);
    new_g->source_dlci = src_dlci;
    new_g->mode = mode;
    new_g->root_lmi_new = 1;
    new_g->cir = cir;
    new_g->bc = bc;
    new_g->be = be;

    if (cir > 0) {
        new_g->tb.cir = cir;
        new_g->tb.bc = bc;
        new_g->tb.be = be;
        new_g->tb.tokens = bc + be;
    }
    new_g->tb.last_update = get_tick_count();

    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    new_g->next = ctx->mcast_groups;
    ctx->mcast_groups = new_g;
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

    LOG_INFO("Multicast group '%s' created (mode=%s, cir=%u): root/first %s:%u",
             name, mode == MCAST_MODE_ONEWAY ? "oneway" : (mode == MCAST_MODE_TWOWAY ? "twoway" : "nway"),
             cir, src_port, src_dlci);

    return 0;
}

int vfrs_add_mcast_member(vfrs_ctx_t *ctx, const char *group_name, const char *member_port, u32 member_dlci)
{
    if (!ctx || !group_name || !member_port) return -1;

    if (!fr_dlci_valid(member_dlci)) {
        LOG_ERROR("Invalid multicast member DLCI: %u", member_dlci);
        return -1;
    }

    /* Perform PVC check BEFORE acquiring mcast_mutex to enforce global lock ordering */
    int is_pvc = is_pvc_endpoint(ctx, member_port, member_dlci);

    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    vfr_mcast_group_t *g = ctx->mcast_groups;
    while (g) {
        if (strcmp(g->name, group_name) == 0) {
            break;
        }
        g = g->next;
    }

    if (!g) {
        LOG_ERROR("Multicast group '%s' not found for member %s:%u", group_name, member_port, member_dlci);
        mutex_unlock(&ctx->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
        return -1;
    }

    int dummy;
    if (vfrs_find_mcast_group_by_endpoint_unlocked(ctx, member_port, member_dlci, &dummy)) {
        LOG_ERROR("Multicast endpoint %s:%u already belongs to a multicast group", member_port, member_dlci);
        mutex_unlock(&ctx->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
        return -1;
    }

    if (is_pvc && g->mode != MCAST_MODE_ONEWAY) {
        LOG_ERROR("Multicast endpoint %s:%u is already configured as a PVC", member_port, member_dlci);
        mutex_unlock(&ctx->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
        return -1;
    }

    if (g->mode == MCAST_MODE_ONEWAY && !is_pvc) {
        LOG_ERROR("One-Way multicast leaf member %s:%u must be a configured PVC", member_port, member_dlci);
        mutex_unlock(&ctx->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
        return -1;
    }

    vfr_mcast_member_t *new_m = calloc(1, sizeof(vfr_mcast_member_t));
    if (!new_m) {
        LOG_ERROR("Failed to allocate multicast member");
        mutex_unlock(&ctx->mcast_mutex);
        RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);
        return -1;
    }

    strncpy(new_m->port_name, member_port, sizeof(new_m->port_name) - 1);
    new_m->dlci = member_dlci;
    new_m->lmi_new = 1;

    new_m->next = g->members;
    g->members = new_m;
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

    LOG_INFO("Member %s:%u added to multicast group '%s'", member_port, member_dlci, group_name);

    return 0;
}

int vfrs_mcast_member_has_pvc_unlocked(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, vfr_mcast_member_t *m)
{
    if (!ctx || !g || !m) return 0;
    if (g->mode != MCAST_MODE_ONEWAY) return 0;

    int has_pvc = 0;
    for (int h = 0; h < PVC_HASH_SIZE; h++) {
        vfr_pvc_detail_t *pvc = ctx->pvc_table[h];
        while (pvc) {
            if ((strcmp(pvc->port_in, g->source_port) == 0 && pvc->dlci_in != g->source_dlci &&
                 strcmp(pvc->port_out, m->port_name) == 0 && pvc->dlci_out == m->dlci) ||
                (strcmp(pvc->port_out, g->source_port) == 0 && pvc->dlci_out != g->source_dlci &&
                 strcmp(pvc->port_in, m->port_name) == 0 && pvc->dlci_in == m->dlci)) {
                has_pvc = 1;
                break;
            }
            pvc = pvc->next;
        }
        if (has_pvc) break;
    }
    return has_pvc;
}

int vfrs_mcast_member_has_pvc(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, vfr_mcast_member_t *m)
{
    int has_pvc = 0;
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);
    has_pvc = vfrs_mcast_member_has_pvc_unlocked(ctx, g, m);
    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
    return has_pvc;
}
