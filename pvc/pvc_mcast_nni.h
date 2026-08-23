/*
 * pvc_mcast_nni.h - Frame Relay PVC Multicasting for NNI
 */

#ifndef PVC_MCAST_NNI_H
#define PVC_MCAST_NNI_H

/* Forward declarations (types are defined in vfr.h before including this file) */

int vfrs_mcast_endpoint_is_active_nni(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, const char *port, int is_root);

#endif /* PVC_MCAST_NNI_H */
