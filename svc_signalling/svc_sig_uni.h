/*
 * svc_sig_uni.h - Q.933 DCE UNI Call State Machine
 * Virtual Frame Relay Switch - Phase 1C
 */

#ifndef SVC_SIG_UNI_H
#define SVC_SIG_UNI_H

#include "svc_sig_common.h"
#include "svc_sig_iel.h"
#include "svc_sig_iep.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize UNI state machine */
void svc_uni_init(void);

/* Process an incoming Q.933 Layer 3 message on a port */
int svc_uni_process_msg(vfr_port_t *port, const u8 *msg_data, size_t len);

/* Virtual Switch Entity handler for all-zeros switch number */
int svc_uni_handle_virtual_switch_call(vfr_port_t *port, vfr_call_t *call);

/* State-compliant call clearing procedure helper */
void svc_initiate_clearing_before_active(vfr_port_t *port, vfr_svc_ctx_t *sctx, vfr_call_t *call, u8 cause);

#ifdef __cplusplus
}
#endif

#endif /* SVC_SIG_UNI_H */
