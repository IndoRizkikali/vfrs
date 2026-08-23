/*
 * lapf.h - VFRS LAPF (Q.922 Core) Interface
 * Virtual Frame Relay Switch
 */

#ifndef VFR_LAPF_H
#define VFR_LAPF_H

#include "platform.h"
#include "types.h"
#include "ports.h"

#ifdef __cplusplus
extern "C" {
#endif

VFR_API int lapf_port_init(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203);
VFR_API void lapf_poll_timer(vfr_port_t *port);
VFR_API int lapf_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
VFR_API int lapf_send_l3(vfr_port_t *port, u32 dlci, const u8 *data, size_t len);
VFR_API int lapf_establish_link(vfr_port_t *port, u32 dlci);
VFR_API int lapf_release_link(vfr_port_t *port, u32 dlci);
VFR_API void lapf_l3_recv_cb(vfr_port_t *port, const u8 *data, size_t len);
VFR_API void lapf_free(vfr_port_t *port);
VFR_API void svc_free_port(vfr_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* VFR_LAPF_H */
