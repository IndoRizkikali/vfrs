/*
 * congestion.h - VFRS Congestion Management & CLLM Subsystem
 * Virtual Frame Relay Switch - ITU-T X.36 / Q.922 / Q.933
 */

#ifndef VFR_CONGESTION_H
#define VFR_CONGESTION_H

#include "platform.h"
#include "types.h"
#include "switching.h"
#include "pvc.h"
#include "ports.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Congestion levels */
#define CLLM_LEVEL_NONE        0x00
#define CLLM_LEVEL_RECEIVED     0x01   /* FECN received */
#define CLLM_LEVEL_INDICATED    0x02   /* BECN or DE received */
#define CLLM_LEVEL_PERSISTENT   0x03   /* Sustained congestion */

/* Q.922 §A.7.3 — Parameter Identifiers inside group value */
#define CLLM_PI_PARAM_SET_ID   0x00   /* PI=0: Parameter Set Identifier */
#define CLLM_PI_CAUSE          0x02   /* PI=2: Cause Identifier */
#define CLLM_PI_DLCI_LIST      0x03   /* PI=3: DLCI List */

VFR_API int cgst_init(vfr_port_t *port, u32 access_rate, u32 rate_threshold);
VFR_API u32 cgst_get_access_rate(vfr_port_t *port);
VFR_API void cgst_dlci_tb_init(vfr_dlci_entry_t *entry, struct vfr_port_s *port, u32 cir, u32 bc, u32 be);
VFR_API int cgst_process_frame(vfr_port_t *port, vfr_dlci_entry_t *entry, u8 *frame, size_t len, fr_addr_t *addr);
VFR_API int cgst_process_frame_tb(vfr_port_t *port, token_bucket_t *tb, u8 *frame, size_t len, fr_addr_t *addr);
VFR_API int cgst_should_set_de(vfr_port_t *port);
VFR_API void cgst_set_congested(vfr_port_t *port, int congested);
VFR_API void cgst_free(vfr_port_t *port);
VFR_API void cgst_poll_timer(vfr_port_t *port);
VFR_API void cgst_set_clear_threshold(vfr_port_t *port, u32 clear_threshold);
VFR_API void cgst_set_cllm_enabled(vfr_port_t *port, int enabled);
VFR_API void cgst_set_cllm_tx_interval(vfr_port_t *port, u32 interval_ms);
VFR_API void cgst_set_write_failure_threshold(vfr_port_t *port, u32 threshold);
VFR_API void cgst_handle_write_failure(vfr_port_t *port);
VFR_API void cgst_handle_write_success(vfr_port_t *port);
VFR_API size_t cllm_build_xid(u8 *buf, size_t max_len, u8 congestion_level,
                              const u32 *dlci_list, int dlci_count, int dlci_len);
VFR_API int cllm_parse_xid(const u8 *buf, size_t len, u8 *congestion_level,
                           u32 *dlci_list, int *dlci_count, int max_dlcis);
VFR_API int cllm_send_notification(vfr_port_t *port, u8 congestion_level,
                                   const u32 *dlci_list, int dlci_count);
VFR_API int cllm_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
VFR_API int cllm_should_include_dlci(u32 dlci, u8 congestion_level);

#ifdef __cplusplus
}
#endif

#endif /* VFR_CONGESTION_H */
