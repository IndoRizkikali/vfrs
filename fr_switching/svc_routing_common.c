/*
 * svc_routing_common.c - VFRS Two-Tier Routing Engine & DLCI Allocator
 * Virtual Frame Relay Switch
 */

#include "svc_routing_common.h"
#include "../ports/svc_numbering/svc_numbering.h"

/* ============================================================
 * DLCI Range Allocator Implementation
 * ============================================================ */

int svc_dlci_alloc_init(vfr_dlci_allocator_t *alloc, u32 range_low, u32 range_high) {
    return svc_dlci_alloc_init_ex(alloc, range_low, range_high, SVC_DLCI_ALLOC_ASCENDING);
}

int svc_dlci_alloc_init_ex(vfr_dlci_allocator_t *alloc, u32 range_low, u32 range_high, vfr_dlci_alloc_dir_t alloc_dir) {
    if (!alloc) return -1;

    memset(alloc, 0, sizeof(*alloc));
    if (range_low > range_high) {
        range_low = 512;
        range_high = 991;
    }

    alloc->range_low = range_low;
    alloc->range_high = range_high;
    alloc->alloc_dir = alloc_dir;
    alloc->ranges[0].start = range_low;
    alloc->ranges[0].end = range_high;
    alloc->count = 1;

    return 0;
}

u32 svc_dlci_alloc(vfr_dlci_allocator_t *alloc) {
    if (!alloc) return 0;
    return svc_dlci_alloc_ex(alloc, alloc->alloc_dir);
}

u32 svc_dlci_alloc_ex(vfr_dlci_allocator_t *alloc, vfr_dlci_alloc_dir_t dir) {
    if (!alloc || alloc->count <= 0) {
        return 0; /* Exhausted */
    }

    if (dir == SVC_DLCI_ALLOC_DESCENDING) {
        /* Allocate from the highest range, highest DLCI */
        int idx = alloc->count - 1;
        u32 dlci = alloc->ranges[idx].end;
        if (alloc->ranges[idx].end > alloc->ranges[idx].start) {
            alloc->ranges[idx].end--;
        } else {
            /* Range completely exhausted */
            alloc->count--;
        }
        return dlci;
    } else {
        /* Default: Ascending from lowest range, lowest DLCI */
        u32 dlci = alloc->ranges[0].start;
        alloc->ranges[0].start++;

        if (alloc->ranges[0].start > alloc->ranges[0].end) {
            /* Range exhausted, shift remaining ranges left */
            for (int i = 0; i < alloc->count - 1; i++) {
                alloc->ranges[i] = alloc->ranges[i + 1];
            }
            alloc->count--;
        }
        return dlci;
    }
}

void svc_dlci_free(vfr_dlci_allocator_t *alloc, u32 dlci) {
    if (!alloc || dlci < alloc->range_low || dlci > alloc->range_high) {
        return;
    }

    /* Insert [dlci, dlci] in sorted order by start */
    int insert_idx = 0;
    while (insert_idx < alloc->count && alloc->ranges[insert_idx].start < dlci) {
        insert_idx++;
    }

    /* Check if already contained in any range */
    for (int i = 0; i < alloc->count; i++) {
        if (dlci >= alloc->ranges[i].start && dlci <= alloc->ranges[i].end) {
            return; /* Duplicate free ignored */
        }
    }

    if (alloc->count >= SVC_MAX_FREE_RANGES) {
        LOG_WARN("SVC DLCI Allocator: Max free ranges reached (%d), cannot free DLCI %u",
                 SVC_MAX_FREE_RANGES, dlci);
        return;
    }

    /* Make room for new range */
    for (int i = alloc->count; i > insert_idx; i--) {
        alloc->ranges[i] = alloc->ranges[i - 1];
    }

    alloc->ranges[insert_idx].start = dlci;
    alloc->ranges[insert_idx].end = dlci;
    alloc->count++;

    /* Merge adjacent or overlapping ranges */
    int i = 0;
    while (i < alloc->count - 1) {
        if (alloc->ranges[i].end + 1 >= alloc->ranges[i + 1].start) {
            if (alloc->ranges[i + 1].end > alloc->ranges[i].end) {
                alloc->ranges[i].end = alloc->ranges[i + 1].end;
            }
            /* Remove range i+1 */
            for (int j = i + 1; j < alloc->count - 1; j++) {
                alloc->ranges[j] = alloc->ranges[j + 1];
            }
            alloc->count--;
        } else {
            i++;
        }
    }
}

/* ============================================================
 * Hierarchical Multi-Tier & Multi-Hop Routing Engine Implementation
 * ============================================================ */

static int is_network_in_tni_list(const char *net_id, const vfr_tni_list_t *tni_list) {
    if (!net_id || net_id[0] == '\0' || !tni_list) return 0;
    for (int i = 0; i < tni_list->count && i < SVC_MAX_TRANSIT_NETWORKS; i++) {
        if (strcmp(tni_list->entries[i].net_id, net_id) == 0) {
            return 1;
        }
    }
    return 0;
}

int svc_route_init(vfrs_ctx_t *ctx) {
    if (!ctx) return -1;
    ctx->svc_route_count = 0;
    memset(ctx->svc_routes, 0, sizeof(ctx->svc_routes));
    return 0;
}

int svc_route_add_ex(vfrs_ctx_t *ctx, const char *raw_prefix, const char *egress_port,
                      const char *transit_net_id, u8 metric) {
    if (!ctx || !raw_prefix || !egress_port) return -1;

    char expanded_prefix[SVC_MAX_ADDR_LEN + 1];
    svc_numbering_expand(ctx, raw_prefix, expanded_prefix, sizeof(expanded_prefix));

    if (ctx->svc_route_count >= SVC_MAX_ROUTES) {
        LOG_ERROR("SVC Routing: Route table full (max %d)", SVC_MAX_ROUTES);
        return -1;
    }

    int idx = ctx->svc_route_count;
    snprintf(ctx->svc_routes[idx].prefix, sizeof(ctx->svc_routes[idx].prefix), "%.*s",
             (int)(sizeof(ctx->svc_routes[idx].prefix) - 1), expanded_prefix);
    snprintf(ctx->svc_routes[idx].egress_port, sizeof(ctx->svc_routes[idx].egress_port), "%.*s",
             (int)(sizeof(ctx->svc_routes[idx].egress_port) - 1), egress_port);
    if (transit_net_id && transit_net_id[0] != '\0') {
        snprintf(ctx->svc_routes[idx].transit_net_id, sizeof(ctx->svc_routes[idx].transit_net_id), "%.*s",
                 (int)(sizeof(ctx->svc_routes[idx].transit_net_id) - 1), transit_net_id);
    } else {
        ctx->svc_routes[idx].transit_net_id[0] = '\0';
    }
    ctx->svc_routes[idx].metric = metric > 0 ? metric : 10;
    ctx->svc_route_count++;

    LOG_INFO("SVC Routing: Added route '%s' -> egress port %s (transit_net=%s, metric=%u)",
             expanded_prefix, egress_port,
             ctx->svc_routes[idx].transit_net_id[0] ? ctx->svc_routes[idx].transit_net_id : "any",
             ctx->svc_routes[idx].metric);
    return 0;
}

int svc_route_add(vfrs_ctx_t *ctx, const char *raw_prefix, const char *egress_port) {
    return svc_route_add_ex(ctx, raw_prefix, egress_port, NULL, 10);
}

int svc_route_add_structural_ex(vfrs_ctx_t *ctx, const char *egress_port,
                                 const char *dnic, const char *sgc, const char *sic,
                                 const char *transit_net_id, u8 metric) {
    if (!ctx || !egress_port || !dnic || dnic[0] == '\0') return -1;

    char raw_builder[64] = {0};
    snprintf(raw_builder, sizeof(raw_builder), "%s", dnic);

    if (sgc && sgc[0] != '\0') {
        int sgclen = (ctx->sgclen > 0 && ctx->sgclen <= 8) ? ctx->sgclen : 1;
        /* If sgc is a numeric string and shorter than sgclen, zero-pad it */
        int is_all_digits = 1;
        for (int i = 0; sgc[i]; i++) {
            if (sgc[i] < '0' || sgc[i] > '9') { is_all_digits = 0; break; }
        }
        char sgc_padded[16];
        if (is_all_digits && (int)strlen(sgc) < sgclen) {
            snprintf(sgc_padded, sizeof(sgc_padded), "%0*u", sgclen, (unsigned int)atoi(sgc));
        } else {
            snprintf(sgc_padded, sizeof(sgc_padded), "%s", sgc);
        }
        strncat(raw_builder, sgc_padded, sizeof(raw_builder) - strlen(raw_builder) - 1);

        if (sic && sic[0] != '\0') {
            int siclen = (ctx->siclen > 0 && ctx->siclen <= 8) ? ctx->siclen : 1;
            int sic_all_digits = 1;
            for (int i = 0; sic[i]; i++) {
                if (sic[i] < '0' || sic[i] > '9') { sic_all_digits = 0; break; }
            }
            char sic_padded[16];
            if (sic_all_digits && (int)strlen(sic) < siclen) {
                snprintf(sic_padded, sizeof(sic_padded), "%0*u", siclen, (unsigned int)atoi(sic));
            } else {
                snprintf(sic_padded, sizeof(sic_padded), "%s", sic);
            }
            strncat(raw_builder, sic_padded, sizeof(raw_builder) - strlen(raw_builder) - 1);
        }
    }

    return svc_route_add_ex(ctx, raw_builder, egress_port, transit_net_id, metric);
}

int svc_route_add_structural(vfrs_ctx_t *ctx, const char *egress_port,
                              const char *dnic, const char *sgc, const char *sic) {
    return svc_route_add_structural_ex(ctx, egress_port, dnic, sgc, sic, NULL, 10);
}

int svc_route_is_local(vfrs_ctx_t *ctx, const char *called_number) {
    return (svc_find_subscriber(ctx, called_number) != NULL);
}

vfr_port_t *svc_route_lookup_ex(vfrs_ctx_t *ctx, const char *called_number,
                                const char *transit_net_sel, const vfr_tni_list_t *tni_list) {
    if (!ctx || !called_number || called_number[0] == '\0') return NULL;

    char expanded[SVC_MAX_ADDR_LEN + 1];
    svc_numbering_expand(ctx, called_number, expanded, sizeof(expanded));

    /* Tier 1: Local subscriber table match (when no transit network selection requested) */
    if (!transit_net_sel || transit_net_sel[0] == '\0') {
        vfr_port_t *local_port = svc_find_subscriber(ctx, expanded);
        if (local_port) {
            return local_port;
        }
    }

    /* Tier 2: Transit Network Selection match */
    if (transit_net_sel && transit_net_sel[0] != '\0') {
        int best_tns_len = -1;
        int best_tns_metric = 255;
        vfr_port_t *best_tns_port = NULL;

        /* Look for explicitly bound transit network routes */
        for (int i = 0; i < ctx->svc_route_count; i++) {
            if (ctx->svc_routes[i].transit_net_id[0] != '\0' &&
                strcmp(ctx->svc_routes[i].transit_net_id, transit_net_sel) == 0) {
                size_t plen = strlen(ctx->svc_routes[i].prefix);
                if (strncmp(expanded, ctx->svc_routes[i].prefix, plen) == 0) {
                    vfr_port_t *p = vfrs_find_port(ctx, ctx->svc_routes[i].egress_port);
                    if (p) {
                        if (!is_network_in_tni_list(transit_net_sel, tni_list)) {
                            u8 metric = ctx->svc_routes[i].metric ? ctx->svc_routes[i].metric : 10;
                            if ((int)plen > best_tns_len || ((int)plen == best_tns_len && metric < best_tns_metric)) {
                                best_tns_len = (int)plen;
                                best_tns_metric = metric;
                                best_tns_port = p;
                            }
                        }
                    }
                }
            }
        }

        if (best_tns_port) return best_tns_port;

        /* If no explicit route, check NNI ports directly whose network_id matches */
        for (int p = 0; p < ctx->port_count; p++) {
            vfr_port_t *port = ctx->ports[p];
            if (port && port->svc_ctx) {
                vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
                const char *net_id = sctx->remote_network_id[0] ? sctx->remote_network_id : sctx->network_id;
                if (sctx->is_nni && strcmp(net_id, transit_net_sel) == 0) {
                    if (!is_network_in_tni_list(net_id, tni_list)) {
                        return port;
                    }
                }
            }
        }
    }

    /* Tier 3: Hierarchical Longest Prefix Match (LPM) on NNI prefix route table */
    int best_match_len = -1;
    int best_metric = 255;
    vfr_port_t *best_port = NULL;

    for (int i = 0; i < ctx->svc_route_count; i++) {
        size_t plen = strlen(ctx->svc_routes[i].prefix);
        if (strncmp(expanded, ctx->svc_routes[i].prefix, plen) == 0) {
            vfr_port_t *p = vfrs_find_port(ctx, ctx->svc_routes[i].egress_port);
            if (!p) continue;

            vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)p->svc_ctx;
            /* Multi-hop Loop Detection: Skip route if egress target network ID is already in TNI list */
            const char *target_net = ctx->svc_routes[i].transit_net_id[0] ? ctx->svc_routes[i].transit_net_id :
                                     (sctx && sctx->remote_network_id[0] ? sctx->remote_network_id : NULL);
            if (target_net && is_network_in_tni_list(target_net, tni_list)) {
                LOG_DEBUG("SVC Routing: Skipping route '%s' -> %s (target network %s in TNI list)",
                          ctx->svc_routes[i].prefix, p->name, target_net);
                continue;
            }

            u8 metric = ctx->svc_routes[i].metric ? ctx->svc_routes[i].metric : 10;
            if ((int)plen > best_match_len || ((int)plen == best_match_len && metric < best_metric)) {
                best_match_len = (int)plen;
                best_metric = metric;
                best_port = p;
            }
        }
    }

    if (best_port) return best_port;

    return NULL; /* Unroutable / Cause #1 */
}

vfr_port_t *svc_route_lookup(vfrs_ctx_t *ctx, const char *called_number) {
    return svc_route_lookup_ex(ctx, called_number, NULL, NULL);
}
