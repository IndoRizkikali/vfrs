/*
 * svc_sig_nni.h - ITU-T X.76 Clause 10 NNI Call State Machine & Signalling Engine
 * Virtual Frame Relay Switch
 */

#ifndef SVC_SIG_NNI_H
#define SVC_SIG_NNI_H

#include "svc_sig_common.h"
#include "svc_sig_iel.h"
#include "svc_sig_iep.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize NNI state machine */
void svc_nni_init(void);

/* Process an incoming ITU-T X.76 Layer 3 message on an NNI port */
int svc_nni_process_msg(vfr_port_t *port, const u8 *msg_data, size_t len);

/* Helper to initiate standard NNI call clearing procedure (X.76 §10.6.2 & §10.6.7) */
void svc_nni_initiate_clearing(vfr_port_t *port, vfr_svc_ctx_t *sctx, vfr_call_t *call, u8 cause);

/* Initiate ITU-T X.76 Clause 10 interface RESTART procedure */
int svc_nni_initiate_restart(vfr_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* SVC_SIG_NNI_H */
