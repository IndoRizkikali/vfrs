/*
 * pvc.h - VFRS PVC, Multicast & LMI Subsystems
 * Virtual Frame Relay Switch
 */

#ifndef VFR_PVC_H
#define VFR_PVC_H

#include "platform.h"
#include "types.h"
#include "switching.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct vfr_port_s vfr_port_t;
typedef struct vfrs_ctx_s vfrs_ctx_t;

/* ============================================================
 * DLCI & PVC Tables
 * ============================================================ */

typedef struct vfr_dlci_entry_s {
    char        port_in[VFR_MAX_NAME_LEN];
    char        port_out[VFR_MAX_NAME_LEN];
    u32         dlci_in;
    u32         dlci_out;
    int         active;

    u8          link_type;       /* UNI or NNI */
    u8          vc_type;         /* VC_TYPE_PVC, VC_TYPE_SVC, VC_TYPE_MCAST, VC_TYPE_RESERVED */
    void        *detail_ptr;     /* Pointer to vfr_pvc_detail_t or vfr_call_t */

    token_bucket_t tb;           /* Token bucket state for traffic policing */

    u8          peer_congested;  /* Congestion state of the peer link */
    u64         last_cllm_time;  /* Timestamp of the last received CLLM frame */
    struct vfr_dlci_entry_s *reverse_entry; /* Reverse path routing entry */

    u8          ftp;             /* Frame Transfer Priority */
    u8          fdp;             /* Frame Discard Priority */
    u8          srvcls;          /* Service Class */

    /* Counters */
    u64         tx_frames;
    u64         rx_frames;
    u64         tx_bytes;
    u64         rx_bytes;

    struct vfr_dlci_entry_s *next; /* Hash table linkage */
} vfr_dlci_entry_t;

typedef struct vfr_pvc_detail_s {
    char        port_in[VFR_MAX_NAME_LEN];
    char        port_out[VFR_MAX_NAME_LEN];
    u32         dlci_in;
    u32         dlci_out;
    int         active;

    /* Traffic parameters */
    u32         cir;             /* Committed Information Rate (bps) */
    u32         bc;              /* Committed Burst Size (bits) */
    u32         be;              /* Excess Burst Size (bits) */
    u8          ftp;             /* Frame Transfer Priority */
    u8          fdp;             /* Frame Discard Priority */
    u8          srvcls;          /* Service Class */
    u32         access_rate;     /* Physical access rate */

    /* LMI compliance fields */
    u8          lmi_new;         /* 1 if newly added, 0 if acknowledged */
    u8          lmi_new_txsn;    /* The DCE TxSN at which the new status was last sent */
    u8          lmi_reported;    /* 1 if reported in current LMI Full Status cycle */

    struct vfr_pvc_detail_s *next; /* Hash table linkage */
} vfr_pvc_detail_t;

/* PVC / DLCI hash table parameters */
#define PVC_HASH_SIZE    256
#define PVC_HASH(dlci)   (((dlci) ^ ((dlci) >> 8)) & (PVC_HASH_SIZE - 1))

/*
 * DLCI_HASH: FNV-1a 32-bit hash over port name bytes XOR'd with DLCI.
 * Including the port name prevents hash collisions when multiple ports
 * share overlapping DLCI namespaces (e.g. uni0/0:dlci=100 vs uni0/1:dlci=100).
 * Spec rationale: X.36 §9 — DLCIs are locally significant per port.
 */
static inline uint32_t dlci_hash_fn(const char *port_name, u32 dlci)
{
    uint32_t h = 2166136261u;           /* FNV-1a 32-bit offset basis */
    const unsigned char *p = (const unsigned char *)port_name;
    while (*p) { h ^= *p++; h *= 16777619u; }  /* FNV-1a prime */
    h ^= (uint8_t)(dlci);        h *= 16777619u;
    h ^= (uint8_t)(dlci >> 8);   h *= 16777619u;
    h ^= (uint8_t)(dlci >> 16);  h *= 16777619u;
    return h & (PVC_HASH_SIZE - 1u);
}
#define DLCI_HASH(port, dlci) dlci_hash_fn(port, dlci)

/* ============================================================
 * Multicast Service Types and Structs
 * ============================================================ */

#define MCAST_MODE_ONEWAY 1
#define MCAST_MODE_TWOWAY 2
#define MCAST_MODE_NWAY   3

typedef struct vfr_mcast_member_s {
    char port_name[VFR_MAX_NAME_LEN];
    u32 dlci;
    u8 lmi_new;
    u8 lmi_new_txsn;
    u8 peer_congested;
    u64 last_cllm_time;
    struct vfr_mcast_member_s *next;
} vfr_mcast_member_t;

typedef struct vfr_mcast_group_s {
    char name[VFR_MAX_NAME_LEN];
    char source_port[VFR_MAX_NAME_LEN]; /* Root port for ONEWAY, TWOWAY */
    u32 source_dlci;                /* Root DLCI */
    int mode;                       /* ONEWAY, TWOWAY, NWAY */
    u8 root_lmi_new;
    u8 root_lmi_new_txsn;
    u8 peer_congested;
    u64 last_cllm_time;
    u32 cir;                        /* Committed Information Rate in bps */
    u32 bc;                         /* Committed Burst size in bits */
    u32 be;                         /* Excess Burst size in bits */
    token_bucket_t tb;              /* Token bucket state for traffic policing */
    vfr_mcast_member_t *members;
    struct vfr_mcast_group_s *next;
} vfr_mcast_group_t;

/* Multicast API */
VFR_API int vfrs_add_mcast_group(vfrs_ctx_t *ctx, const char *name, const char *src_port, u32 src_dlci, const char *mode_str, u32 cir, u32 bc, u32 be);
VFR_API int vfrs_add_mcast_member(vfrs_ctx_t *ctx, const char *group_name, const char *member_port, u32 member_dlci);
VFR_API vfr_mcast_group_t *vfrs_find_mcast_group_by_endpoint(vfrs_ctx_t *ctx, const char *port, u32 dlci, int *is_root);
VFR_API vfr_mcast_group_t *vfrs_find_mcast_group_by_endpoint_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci, int *is_root);
VFR_API int vfrs_mcast_endpoint_is_active(vfrs_ctx_t *ctx, const char *port, u32 dlci);
VFR_API int vfrs_mcast_endpoint_is_active_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci);
VFR_API int vfrs_mcast_endpoint_is_active_uni(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, const char *port, int is_root);
VFR_API int vfrs_mcast_endpoint_is_active_nni(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, const char *port, int is_root);
VFR_API int vfrs_mcast_member_has_pvc(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, vfr_mcast_member_t *m);
VFR_API int vfrs_mcast_member_has_pvc_unlocked(vfrs_ctx_t *ctx, vfr_mcast_group_t *g, vfr_mcast_member_t *m);

/* ============================================================
 * LMI Types & Constants
 * ============================================================ */

#define LMI_TYPE_NONE     0
#define LMI_TYPE_ANSI     1   /* ANSI T1.617 Annex D */
#define LMI_TYPE_Q933A    2   /* ITU-T Q.933 Annex A */
#define LMI_TYPE_CISCO    3   /* Cisco/Gang of Four */

/* LMI message types */
#define LMI_MSG_STATUS_ENQ    0x75    /* ANSI/Cisco/Q.933A */
#define LMI_MSG_STATUS        0x7D    /* ANSI/Cisco/Q.933A */

/* Information Element codes */
#define LMI_IE_REPORT_TYPE_ANSI    0x01   /* Codeset 5 */
#define LMI_IE_LINK_INTEGRITY_ANSI 0x03   /* Codeset 5 */
#define LMI_IE_PVC_STATUS_ANSI     0x07   /* Codeset 5 */

#define LMI_IE_REPORT_TYPE_Q933A    0x51   /* Codeset 0 */
#define LMI_IE_LINK_INTEGRITY_Q933A 0x53   /* Codeset 0 */
#define LMI_IE_PVC_STATUS_Q933A     0x57   /* Codeset 0 */

/* Report types */
#define LMI_REPORT_FULL_STATUS        0x00
#define LMI_REPORT_LINK_INTEGRITY     0x01
#define LMI_REPORT_ASYNC_STATUS       0x02

/* PVC status flags (Q.933 Annex A / X.36)
 * Bit 8 (MSB) = extension bit (1)
 * Bit 4 = New (N)
 * Bit 3 = Delete (D)
 * Bit 2 = Active (A)
 */
#define LMI_PVC_EXT      0x80
#define LMI_PVC_NEW      0x08
#define LMI_PVC_DELETE   0x04
#define LMI_PVC_ACTIVE   0x02

/* LMI state structure */
typedef struct {
    int         type;               /* LMI type */

    /* DCE parameters (network side) */
    u8          n392;              /* Error threshold (default: 3) */
    u8          n393;              /* Monitored events (default: 4) */
    u16         t392;              /* Poll verification interval (default: 15s) */

    /* DTE parameters (user side) */
    u8          n391;              /* Full status poll counter (default: 6) */
    u8          n392_dte;          /* DTE N392 (default: 3) */
    u8          n393_dte;          /* DTE N393 (default: 4) */
    u16         t391;              /* DTE poll interval (default: 10s) */

    /* State variables */
    u8          dce_errors;
    u8          dce_events;
    u8          dte_errors;
    u8          dte_events;
    u8          dte_seq_send;
    u8          dte_seq_recv;
    u8          dce_seq_send;
    u8          dce_seq_recv;
    u8          dte_dce_seq_recv;   /* Stores the remote DCE's TxSN received by our DTE-side */

    u8          polls_since_full;
    u8          last_report_type;  /* Last Report Type IE from DTE */

    /* LMI compliance state variables */
    u16         dce_history;       /* Error history bitmask of last n393 events */
    u8          dce_link_down;     /* 1 if LMI link is declared down */
    u8          segment_active;    /* 1 if segmented Full Status is active */
    u16         pvc_send_index;    /* Next PVC to send in segmented Full Status */
    u8          dce_last_was_full; /* 1 if the last STATUS was Full Status */

    /* DTE-side error monitoring (X.36 §11.4.1.6.2) */
    u8          dte_status_received; /* 1 if a valid STATUS was received since last T391 */
    u16         dte_history;         /* DTE error-event sliding window (N393 bits) */
    u8          dte_link_down;       /* 1 = DTE declares service-affecting condition */
    u8          dte_enabled;         /* 1 = DTE polling is enabled on this port */
    u8          dte_immediate_poll;  /* 1 = Annex G: immediate re-poll on Full-Status-Continued */
    u8          async_enabled;       /* 1 = DCE is allowed to transmit asynchronous PVC status */
    u8          dte_last_request_was_full; /* 1 = last STATUS ENQUIRY requested Full Status */

    /* Timers (DCE side) */
    vfr_timer_t t392_timer;
    vfr_timer_t t391_timer;

} vfr_lmi_state_t;

/* LMI Operations */
VFR_API void lmi_init_state(vfr_lmi_state_t *lmi, int type);
VFR_API size_t lmi_build_header(vfr_port_t *port, u8 *buf, size_t max_len, int protocol_disc);

/* ANSI LMI IE Builders/Parsers */
VFR_API size_t lmi_ansi_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type);
VFR_API size_t lmi_ansi_build_link_integrity_ie(u8 *buf, size_t max_len, u8 send_seq, u8 recv_seq);
VFR_API size_t lmi_ansi_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len);
VFR_API int lmi_ansi_parse_pvc_status_ie(const u8 *buf, size_t len, u32 *dlci, u8 *status);

/* Q.933 Annex A LMI IE Builders/Parsers */
VFR_API size_t lmi_q933a_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type);
VFR_API size_t lmi_q933a_build_link_integrity_ie(u8 *buf, size_t max_len, u8 send_seq, u8 recv_seq);
VFR_API size_t lmi_q933a_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len);
VFR_API int lmi_q933a_parse_pvc_status_ie(const u8 *buf, size_t len, u32 *dlci, u8 *status);

/* Cisco / GoF LMI IE Builders/Parsers */
VFR_API size_t lmi_gof_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type);
VFR_API size_t lmi_gof_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len);

/* ANSI LMI */
VFR_API int lmi_ansi_init(vfr_port_t *port, int t392, int n392, int n393);
VFR_API int lmi_ansi_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
VFR_API size_t lmi_ansi_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len);

/* Q.933 Annex A LMI */
VFR_API int lmi_q933a_init(vfr_port_t *port, int t392, int n392, int n393);
VFR_API int lmi_q933a_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
VFR_API size_t lmi_q933a_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len);

/* Cisco/Gang of Four LMI */
VFR_API int lmi_gof_init(vfr_port_t *port, int t392, int n392, int n393);
VFR_API int lmi_gof_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
VFR_API size_t lmi_cisco_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len);

/* LMI polling (shared timer handler) */
VFR_API int lmi_poll_timer(vfr_port_t *port);

/* LMI state management */
VFR_API void lmi_reset_dce_state(vfr_lmi_state_t *lmi);
VFR_API int  lmi_send_async_status(vfr_port_t *port, u32 dlci, u8 status_flags);
VFR_API void lmi_free(vfr_port_t *port);
VFR_API u8 lmi_inc_seq(u8 seq);
VFR_API int lmi_dte_enable(vfr_port_t *port, u8 n391, u16 t391, u8 n392_dte, u8 n393_dte);
VFR_API size_t lmi_build_enquiry(u8 *buf, size_t max_len, int type);
VFR_API size_t lmi_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int type, int full_status);
VFR_API size_t lmi_ansi_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status);
VFR_API size_t lmi_q933a_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status);
VFR_API int lmi_cisco_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status);
VFR_API int lmi_port_dlci_len(vfr_port_t *port);

/* Global PVC / DLCI management operations */
VFR_API int vfrs_add_pvc(vfrs_ctx_t *ctx, const char *port_in, u32 dlci_in,
                         const char *port_out, u32 dlci_out, u32 cir, u32 bc, u32 be,
                         u8 ftp, u8 fdp, u8 srvcls);
VFR_API int vfrs_del_pvc(vfrs_ctx_t *ctx, const char *port_in, u32 dlci_in);
VFR_API vfr_pvc_detail_t *vfrs_lookup_pvc(vfrs_ctx_t *ctx, const char *port, u32 dlci);
VFR_API vfr_pvc_detail_t *vfrs_lookup_pvc_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci);
VFR_API vfr_dlci_entry_t *dlci_table_lookup(vfrs_ctx_t *ctx, const char *port, u32 dlci);
VFR_API int dlci_table_add(vfrs_ctx_t *ctx, vfr_dlci_entry_t *entry);
VFR_API int dlci_table_del(vfrs_ctx_t *ctx, const char *port, u32 dlci);
VFR_API int vfrs_dlci_is_active(vfrs_ctx_t *ctx, vfr_port_t *port, vfr_dlci_entry_t *entry);
VFR_API int vfrs_dlci_is_active_unlocked(vfrs_ctx_t *ctx, vfr_port_t *port, vfr_dlci_entry_t *entry);
VFR_API void vfrs_propagate_port_status_change(vfrs_ctx_t *ctx, vfr_port_t *changed_port);

#ifdef __cplusplus
}
#endif

#endif /* VFR_PVC_H */
