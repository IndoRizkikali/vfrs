/*
 * svc_routing_common.h - VFRS Two-Tier Routing Engine & DLCI Allocator
 * Virtual Frame Relay Switch
 */

#ifndef SVC_ROUTING_COMMON_H
#define SVC_ROUTING_COMMON_H

#include "vfr.h"
#include "svc/svc_sig_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * DLCI Range Allocator
 * ============================================================ */
int  svc_dlci_alloc_init(vfr_dlci_allocator_t *alloc, u32 range_low, u32 range_high);
int  svc_dlci_alloc_init_ex(vfr_dlci_allocator_t *alloc, u32 range_low, u32 range_high, vfr_dlci_alloc_dir_t alloc_dir);
u32  svc_dlci_alloc(vfr_dlci_allocator_t *alloc);
u32  svc_dlci_alloc_ex(vfr_dlci_allocator_t *alloc, vfr_dlci_alloc_dir_t dir);
void svc_dlci_free(vfr_dlci_allocator_t *alloc, u32 dlci);

/* ============================================================
 * Hierarchical Multi-Tier & Multi-Hop Routing Engine
 * ============================================================ */
int  svc_route_init(vfrs_ctx_t *ctx);
int  svc_route_add(vfrs_ctx_t *ctx, const char *prefix, const char *egress_port);
int  svc_route_add_ex(vfrs_ctx_t *ctx, const char *prefix, const char *egress_port,
                      const char *transit_net_id, u8 metric);
int  svc_route_add_structural(vfrs_ctx_t *ctx, const char *egress_port,
                              const char *dnic, const char *sgc, const char *sic);
int  svc_route_add_structural_ex(vfrs_ctx_t *ctx, const char *egress_port,
                                 const char *dnic, const char *sgc, const char *sic,
                                 const char *transit_net_id, u8 metric);
vfr_port_t *svc_route_lookup(vfrs_ctx_t *ctx, const char *called_number);
vfr_port_t *svc_route_lookup_ex(vfrs_ctx_t *ctx, const char *called_number,
                                const char *transit_net_sel, const vfr_tni_list_t *tni_list);
int  svc_route_is_local(vfrs_ctx_t *ctx, const char *called_number);

#ifdef __cplusplus
}
#endif

#endif /* SVC_ROUTING_COMMON_H */
