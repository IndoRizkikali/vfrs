/*
 * pvc_mcast_uni.h - Frame Relay PVC Multicasting for UNI
 */

#ifndef PVC_MCAST_UNI_H
#define PVC_MCAST_UNI_H

/* Forward declarations (types are defined in vfr.h before including this file) */

int vfrs_add_mcast_group(vfrs_ctx_t *ctx, const char *name, const char *src_port, u32 src_dlci, const char *mode_str, u32 cir, u32 bc, u32 be);
int vfrs_add_mcast_member(vfrs_ctx_t *ctx, const char *group_name, const char *member_port, u32 member_dlci);
vfr_mcast_group_t *vfrs_find_mcast_group_by_endpoint(vfrs_ctx_t *ctx, const char *port, u32 dlci, int *is_root);
vfr_mcast_group_t *vfrs_find_mcast_group_by_endpoint_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci, int *is_root);
int vfrs_mcast_endpoint_is_active(vfrs_ctx_t *ctx, const char *port, u32 dlci);
int vfrs_mcast_endpoint_is_active_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci);
int vfrs_mcast_endpoint_is_active_uni(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, const char *port, int is_root);
int vfrs_mcast_member_has_pvc(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, vfr_mcast_member_t *m);
int vfrs_mcast_member_has_pvc_unlocked(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, vfr_mcast_member_t *m);

#endif /* PVC_MCAST_UNI_H */
