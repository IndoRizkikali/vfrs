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

/* LAPF event types for on_event callback (F-01/F-02) */
#define LAPF_EVENT_ESTABLISHED   1   /* DL-ESTABLISH indication: link (re-)established by peer */
#define LAPF_EVENT_RELEASED      2   /* DL-RELEASE indication: link released by peer */

/* ============================================================
 * Unified Port Data Link Layer Primitives (DL-CORE & DL-CONTROL)
 * ============================================================ */

/* Unacknowledged UI data transfer with explicit congestion & priority parameters */
VFR_API int port_dl_send_unit_data(vfr_port_t *port, u32 dlci, const u8 *payload, size_t len,
                                   int cr, int fecn, int becn, int de);

/* Sequenced acknowledged I-frame transfer via LAPF FSM */
VFR_API int port_dl_send_data(vfr_port_t *port, u32 dlci, const u8 *payload, size_t len);

/* Unnumbered XID management / parameter exchange */
VFR_API int port_dl_send_xid(vfr_port_t *port, u32 dlci, const u8 *xid_info, size_t len, int is_response);

/* Link establishment & release primitives */
VFR_API int port_dl_establish_req(vfr_port_t *port, u32 dlci);
VFR_API int port_dl_release_req(vfr_port_t *port, u32 dlci);

/* Explicit DLCI initialization / teardown in dynamic hash table */
VFR_API int port_dl_init_dlci(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203);
VFR_API void port_dl_free_dlci(vfr_port_t *port, u32 dlci);

/* SAP Callback Registration */
VFR_API void port_dl_register_ui_handler(vfr_port_t *port, u32 dlci, port_dl_ui_cb_fn cb);
VFR_API void port_dl_register_xid_handler(vfr_port_t *port, u32 dlci, port_dl_xid_cb_fn cb);
VFR_API void port_dl_register_l3_handler(vfr_port_t *port, u32 dlci, port_dl_l3_cb_fn cb);

/* ITU-T X.36 Appendix VII window calculation helper: k = 2 + ((Ttd * Ru) / (4 * Ld)) */
VFR_API u8 port_dl_calc_k_x36(u32 transit_delay_ms, u32 throughput_bps, u16 frame_size_octets);

/* Legacy / Convenience LAPF APIs */
VFR_API int lapf_port_init(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203);
VFR_API void lapf_poll_timer(vfr_port_t *port);
VFR_API int lapf_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
VFR_API int lapf_send_l3(vfr_port_t *port, u32 dlci, const u8 *data, size_t len);
VFR_API int lapf_establish_link(vfr_port_t *port, u32 dlci);
VFR_API int lapf_release_link(vfr_port_t *port, u32 dlci);
VFR_API void lapf_set_event_cb(vfr_port_t *port, u32 dlci,
                               void (*on_event)(vfr_port_t *port, u32 dlci, int event_type));
VFR_API void lapf_l3_recv_cb(vfr_port_t *port, const u8 *data, size_t len);
VFR_API void lapf_free(vfr_port_t *port);
VFR_API void svc_free_port(vfr_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* VFR_LAPF_H */
