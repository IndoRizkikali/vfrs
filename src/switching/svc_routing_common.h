/*
 * svc_routing_common.h - VFRS Digit-by-Digit Analysis Tree & DLCI Allocator
 * Virtual Frame Relay Switch
 */

#ifndef SVC_ROUTING_COMMON_H
#define SVC_ROUTING_COMMON_H

#include "vfr.h"
#include "svc/svc_sig_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Route Actions */
typedef enum {
    ROUTE_ACTION_NONE = 0,
    ROUTE_ACTION_LOCAL,
    ROUTE_ACTION_NNI,
    ROUTE_ACTION_TRANSLATE,
    ROUTE_ACTION_REJECT
} vfr_route_action_t;

/* Digit Trie Node for Digit-by-Digit Analysis */
typedef struct vfr_digit_node_s {
    struct vfr_digit_node_s *children[10];
    int                     is_terminal;
    vfr_route_action_t      action;
    char                    target_port_name[32];
    char                    transit_net_id[16];
    u8                      metric;
    u8                      revchg_allow;
    u8                      strip_digits;
    char                    insert_prefix[16];
    u8                      reject_cause;
} vfr_digit_node_t;

/* Digit Analysis Engine Context */
typedef struct {
    vfr_digit_node_t *root_x121;
    vfr_digit_node_t *root_e164;
    size_t           total_routes;
} vfr_digit_tree_t;

/* ============================================================
 * DLCI Range Allocator
 * ============================================================ */
int  svc_dlci_alloc_init(vfr_dlci_allocator_t *alloc, u32 range_low, u32 range_high);
int  svc_dlci_alloc_init_ex(vfr_dlci_allocator_t *alloc, u32 range_low, u32 range_high, vfr_dlci_alloc_dir_t alloc_dir);
u32  svc_dlci_alloc(vfr_dlci_allocator_t *alloc);
u32  svc_dlci_alloc_ex(vfr_dlci_allocator_t *alloc, vfr_dlci_alloc_dir_t dir);
void svc_dlci_free(vfr_dlci_allocator_t *alloc, u32 dlci);

/* ============================================================
 * Digit-by-Digit Analysis & Routing Engine
 * ============================================================ */
int  svc_route_init(vfrs_ctx_t *ctx);
void svc_route_destroy(vfrs_ctx_t *ctx);

int  svc_trie_insert(vfr_digit_node_t **root, const char *digits,
                     vfr_route_action_t action, const char *egress_port,
                     const char *transit_net_id, u8 metric, u8 revchg_allow);

const vfr_digit_node_t *svc_trie_lookup(const vfr_digit_node_t *root, const char *digits);
void                    svc_trie_destroy(vfr_digit_node_t **root);

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
