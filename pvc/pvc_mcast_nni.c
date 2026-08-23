/*
 * pvc_mcast_nni.c - Frame Relay PVC Multicasting for NNI
 */

#include "vfr.h"

static int vfrs_mcast_port_is_active_nni(vfrs_ctx_t *ctx, const char *port_name)
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

int vfrs_mcast_endpoint_is_active_nni(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, const char *port, int is_root)
{
    int active = 0;
    if (g->mode == MCAST_MODE_ONEWAY) {
        if (is_root) {
            /* Downstream side of NNI: active if root port is UP and at least one leaf is active */
            if (vfrs_mcast_port_is_active_nni(ctx, g->source_port)) {
                vfr_mcast_member_t *m = g->members;
                while (m) {
                    if (vfrs_mcast_port_is_active_nni(ctx, m->port_name)) {
                        active = 1;
                        break;
                    }
                    m = m->next;
                }
            }
        } else {
            /* Upstream side of NNI (One-Way): active if active to the root (root port is UP) */
            if (vfrs_mcast_port_is_active_nni(ctx, g->source_port)) {
                active = 1;
            }
        }
    } else if (g->mode == MCAST_MODE_TWOWAY) {
        if (is_root) {
            /* Root (downstream): active if root port is UP and at least one leaf is active */
            if (vfrs_mcast_port_is_active_nni(ctx, g->source_port)) {
                vfr_mcast_member_t *m = g->members;
                while (m) {
                    if (vfrs_mcast_port_is_active_nni(ctx, m->port_name)) {
                        active = 1;
                        break;
                    }
                    m = m->next;
                }
            }
        } else {
            /* Leaf (upstream): active if leaf port is UP and root port is UP */
            if (vfrs_mcast_port_is_active_nni(ctx, port) && vfrs_mcast_port_is_active_nni(ctx, g->source_port)) {
                active = 1;
            }
        }
    } else if (g->mode == MCAST_MODE_NWAY) {
        /* NWAY: active if local port is UP and at least one other member is active */
        if (vfrs_mcast_port_is_active_nni(ctx, port)) {
            int active_others = 0;
            if (strcmp(g->source_port, port) != 0) {
                if (vfrs_mcast_port_is_active_nni(ctx, g->source_port)) {
                    active_others = 1;
                }
            }
            vfr_mcast_member_t *m = g->members;
            while (m && !active_others) {
                if (strcmp(m->port_name, port) != 0) {
                    if (vfrs_mcast_port_is_active_nni(ctx, m->port_name)) {
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
