/*
 * svc_spvc.h - ITU-T X.76 Annex A Switched PVC (SPVC) & Service Interworking Engine
 * Virtual Frame Relay Switch
 */

#ifndef SVC_SPVC_H
#define SVC_SPVC_H

#include "vfr.h"
#include "svc/svc_sig_common.h"
#include "svc/svc_sig_iel.h"
#include "svc/svc_sig_iep.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPVC_MODE_INTER_PVC = 0,    /* Standard X.76 Annex A PVC-to-PVC across NNI */
    SPVC_MODE_SVC_TO_PVC = 1,   /* Crooked: SVC subscriber calling legacy PVC endpoint */
    SPVC_MODE_PVC_TO_SVC = 2    /* Crooked: PVC endpoint auto-dialing out to remote SVC subscriber */
} spvc_mode_t;

typedef enum {
    SPVC_STATE_IDLE = 0,
    SPVC_STATE_SETUP_SENT = 1,
    SPVC_STATE_CONNECTED = 2,
    SPVC_STATE_CLEARING = 3,
    SPVC_STATE_RETRY_WAIT = 4
} spvc_state_t;

typedef struct vfr_spvc_entry_s {
    spvc_mode_t      mode;
    char             local_port[VFR_MAX_NAME_LEN];
    u32              local_dlci;
    char             target_addr[MAX_ADDR_STR];   /* Remote subscriber / destination X.121/E.164 */
    u32              target_dlci;                 /* Destination PVC DLCI (inter-pvc) */
    char             svc_addr[MAX_ADDR_STR];      /* Registered virtual address (svc-pvc) */
    u32              cir;
    u32              bc;
    u32              be;

    spvc_state_t     state;
    u16              active_crv;
    char             nni_port[VFR_MAX_NAME_LEN];
    u32              nni_dlci;
    u32              retry_interval_ms;
    u64              last_attempt_ms;
    u8               last_cause;

    struct vfr_spvc_entry_s *next;
} vfr_spvc_entry_t;

/* Initialize SPVC subsystem in switch context */
void spvc_init(vfrs_ctx_t *ctx);

/* Add standard Inter-PVC SPVC definition (X.76 Annex A) */
int spvc_add_inter_pvc(vfrs_ctx_t *ctx, const char *local_port, u32 local_dlci,
                       const char *target_addr, u32 target_dlci,
                       u32 cir, u32 bc, u32 be);

/* Add SVC-to-PVC Crooked Interworking definition */
int spvc_add_svc_to_pvc(vfrs_ctx_t *ctx, const char *local_port, u32 local_dlci,
                        const char *svc_addr);

/* Add PVC-to-SVC Crooked Auto-Dial Interworking definition */
int spvc_add_pvc_to_svc(vfrs_ctx_t *ctx, const char *local_port, u32 local_dlci,
                        const char *target_addr,
                        u32 cir, u32 bc, u32 be);

/* Periodic timer tick (called from slow-path control plane thread) */
void spvc_poll_timers(vfrs_ctx_t *ctx);

/* Check if a given DLCI on a port is backed by SPVC and whether SPVC is connected (X.76 Annex A.4.5.3) */
int spvc_is_dlci_active(vfrs_ctx_t *ctx, const char *port_name, u32 dlci);

/* Check if a given DLCI on a port is configured as an SPVC */
int spvc_is_spvc_dlci(vfrs_ctx_t *ctx, const char *port_name, u32 dlci);

/* Handle incoming SETUP on UNI targeting an SVC-to-PVC crooked endpoint */
int spvc_handle_incoming_setup(vfrs_ctx_t *ctx, vfr_port_t *ingress_port, vfr_call_t *call);

/* Handle incoming SETUP on NNI with Called Party SPVC IE (X.76 Annex A.4.2) */
int spvc_handle_nni_incoming_setup(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call);

/* Handle NNI CONNECT for active SPVC session (X.76 Annex A.4.3) */
int spvc_handle_nni_connect(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call);

/* Handle NNI RELEASE / clearing for SPVC session (X.76 Annex A.4.4) */
int spvc_handle_nni_release(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call);
int spvc_handle_nni_release_cause(vfrs_ctx_t *ctx, vfr_port_t *nni_port, vfr_call_t *call, u8 cause);

/* Notify SPVC engine when an access PVC state changes */
void spvc_notify_pvc_status_change(vfrs_ctx_t *ctx, const char *port_name, u32 dlci, int is_active);

/* Show SPVC status table */
void vfrs_show_spvcs(vfrs_ctx_t *ctx, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* SVC_SPVC_H */
