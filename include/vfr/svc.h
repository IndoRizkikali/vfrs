/*
 * svc.h - VFRS Switched Virtual Circuit (SVC) Framework
 * Virtual Frame Relay Switch - ITU-T Q.933 / X.36 / X.76
 */

#ifndef VFR_SVC_H
#define VFR_SVC_H

#include "platform.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct vfr_call_s;
typedef struct vfrs_ctx_s vfrs_ctx_t;

/* Graceful SVC teardown */
VFR_API void vfrs_shutdown_svc(vfrs_ctx_t *ctx);

/* SVC Diagnostic / Show Routines */
VFR_API void vfrs_show_svc_calls(vfrs_ctx_t *ctx, FILE *out);
VFR_API void vfrs_show_svc_call_detail(vfrs_ctx_t *ctx, const char *arg, FILE *out);
VFR_API void vfrs_show_svc_stats(vfrs_ctx_t *ctx, FILE *out);
VFR_API void vfrs_show_svc_subscribers(vfrs_ctx_t *ctx, FILE *out);
VFR_API void vfrs_show_svc_routes(vfrs_ctx_t *ctx, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* VFR_SVC_H */
