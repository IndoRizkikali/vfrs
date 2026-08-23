/*
 * svc_sig_common.h - VFRS SVC Signaling Shared Common Header
 * Virtual Frame Relay Switch - ITU-T X.36 Clause 10 & Q.933 Signaling Framework
 */

#ifndef SVC_SIG_COMMON_H
#define SVC_SIG_COMMON_H

#include "vfr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Call States per ITU-T X.36 Table 10-26 (DCE Side) & ITU-T X.76 §10.3.1 (NNI STE) */
#define SVC_STATE_NULL              0   /* U0/N0/NN0: Null / Idle */
#define SVC_STATE_CALL_INITIATED    1   /* N1/NN1: SETUP received from calling DTE / calling STE */
#define SVC_STATE_OUTGOING_PROC     3   /* N3/NN3: CALL PROCEEDING sent */
#define SVC_STATE_CALL_DELIVERED    4   /* N4: SETUP sent to called DTE (UNI only) */
#define SVC_STATE_CALL_PRESENT      6   /* N6/NN6: SETUP sent to called DTE / called STE */
#define SVC_STATE_CALL_RECEIVED     7   /* N7: Called DTE alerting (UNI only) */
#define SVC_STATE_CONNECT_REQUEST   8   /* N8: CONNECT received from called DTE */
#define SVC_STATE_INCOMING_PROC     9   /* N9/NN9: CALL PROCEEDING received */
#define SVC_STATE_ACTIVE           10   /* N10/NN10: Call active, data transfer */
#define SVC_STATE_DISCONNECT_REQ   11   /* N11/NN11: DISCONNECT sent / RELEASE sent (NNI) */
#define SVC_STATE_DISCONNECT_IND   12   /* N12/NN12: DISCONNECT received / RELEASE received (NNI) */
#define SVC_STATE_RELEASE_REQ      19   /* N19: RELEASE sent (UNI) */
#define SVC_STATE_RESTART_REQ      61   /* Restart request (Rest1) */
#define SVC_STATE_RESTART          62   /* Restart received (Rest2) */

/* Standard State Aliases */
#define SVC_STATE_N0_NULL           SVC_STATE_NULL
#define SVC_STATE_N1_CALL_INIT      SVC_STATE_CALL_INITIATED
#define SVC_STATE_N3_CALL_PROC      SVC_STATE_OUTGOING_PROC
#define SVC_STATE_N6_CALL_PRESENT   SVC_STATE_CALL_PRESENT
#define SVC_STATE_N10_ACTIVE        SVC_STATE_ACTIVE
#define SVC_STATE_N11_DISC_REQ      SVC_STATE_DISCONNECT_REQ
#define SVC_STATE_N12_DISC_IND      SVC_STATE_DISCONNECT_IND
#define SVC_STATE_N19_REL_REQ       SVC_STATE_RELEASE_REQ

/* NNI Call States per ITU-T X.76 §10.3.1 */
#define SVC_NNI_STATE_NN0_NULL           SVC_STATE_NULL
#define SVC_NNI_STATE_NN1_CALL_INIT      SVC_STATE_CALL_INITIATED
#define SVC_NNI_STATE_NN3_CALL_PROC_SENT SVC_STATE_OUTGOING_PROC
#define SVC_NNI_STATE_NN6_CALL_PRESENT   SVC_STATE_CALL_PRESENT
#define SVC_NNI_STATE_NN9_CALL_PROC_RCVD SVC_STATE_INCOMING_PROC
#define SVC_NNI_STATE_NN10_ACTIVE        SVC_STATE_ACTIVE
#define SVC_NNI_STATE_NN11_REL_REQ       SVC_STATE_DISCONNECT_REQ
#define SVC_NNI_STATE_NN12_REL_IND       SVC_STATE_DISCONNECT_IND

/* NNI Restart States per ITU-T X.76 §10.3.2 */
#define SVC_NNI_REST_STATE_REST0_NULL    0
#define SVC_NNI_REST_STATE_REST1_REQ     1
#define SVC_NNI_REST_STATE_REST2_RCVD    2

/* ============================================================
 * Q.933 Message Types (ITU-T X.36 Table 10-5 & X.76 Clause 10)
 * ============================================================ */
#define Q933_MSG_ALERTING          0x01
#define Q933_MSG_CALL_PROCEEDING   0x02
#define Q933_MSG_CONNECT           0x07
#define Q933_MSG_CONNECT_ACK       0x0F
#define Q933_MSG_SETUP             0x05
#define Q933_MSG_DISCONNECT        0x45
#define Q933_MSG_RELEASE           0x4D
#define Q933_MSG_RELEASE_COMPLETE  0x5A
#define Q933_MSG_RESTART           0x46
#define Q933_MSG_RESTART_ACK       0x4E
#define Q933_MSG_STATUS            0x7D
#define Q933_MSG_STATUS_ENQUIRY    0x75

/* ============================================================
 * Q.933 / X.76 Information Element Identifiers
 * ============================================================ */
#define Q933_IE_BEARER_CAPABILITY  0x04
#define Q933_IE_CAUSE              0x08
#define Q933_IE_CALLED_SPVC        0x0A
#define Q933_IE_CALLING_SPVC       0x0B
#define Q933_IE_CALL_STATE         0x14
#define Q933_IE_DLCI               0x19
#define Q933_IE_CUG                0x47
#define Q933_IE_LLCORE_PARAMS      0x48
#define Q933_IE_LLPROTO_PARAMS     0x49
#define Q933_IE_REVERSE_CHARGE_IND 0x4A
#define Q933_IE_CONNECTED_NUMBER   0x4C
#define Q933_IE_CONNECTED_SUBADDR  0x4D
#define Q933_IE_TRANSIT_NET_ID     0x67  /* X.76 Table 10.5 / Fig 34: 0110 0111 */
#define Q933_IE_CUG_INTERLOCK      0x68  /* X.76 Table 10.5 / Fig 21: 0110 1000 */
#define Q933_IE_CALL_IDENT         0x69  /* X.76 Table 10.5 / Fig 13: 0110 1001 */
#define Q933_IE_PRIORITY_SVC_CLASS 0x6A  /* X.76 Table 10.5 / Fig 31: 0110 1010 */
#define Q933_IE_CLEARING_NET_ID    0x6B  /* X.76 Table 10.5 / Fig 20: 0110 1011 */
#define Q933_IE_CALLING_NUMBER     0x6C
#define Q933_IE_CALLING_SUBADDR    0x6D
#define Q933_IE_GAT                0x6E  /* X.76 Table 10.5 / FRF.10.1: 0110 1110 */
#define Q933_IE_CALLED_NUMBER      0x70
#define Q933_IE_CALLED_SUBADDR     0x71
#define Q933_IE_TRANSIT_NETWORK    0x78
#define Q933_IE_RESTART_INDICATOR  0x79
#define Q933_IE_LOW_LAYER_COMPAT   0x7C
#define Q933_IE_USER_USER          0x7E

/* Protocol Discriminator */
#define Q933_PROTOCOL_DISC         0x08

/* ============================================================
 * Maximum Capacities (Doubled per Annotation 28)
 * ============================================================ */
#define SVC_MAX_CALLS_PER_PORT     512  /* Pre-allocated CCB pool size */
#define SVC_MAX_ADDR_LEN           20   /* Max X.121 / E.164 number digits */
#define SVC_MAX_SUBADDR_LEN        32   /* Max subaddress length (NSAP / user-specified) */
#define SVC_MAX_ROUTES             256
#define SVC_MAX_SUBSCRIBERS        512
#define SVC_MAX_FREE_RANGES        64
#define SVC_MAX_TRANSIT_NETWORKS   6    /* Max 6 transit networks per X.76 §10.6.9.1 */

/* ============================================================
 * DLCI Range Allocator Structures
 * ============================================================ */
typedef enum {
    SVC_DLCI_ALLOC_ASCENDING  = 0,  /* Allocate from lowest unused DLCI (low side / default) */
    SVC_DLCI_ALLOC_DESCENDING = 1   /* Allocate from highest unused DLCI (high side) */
} vfr_dlci_alloc_dir_t;

typedef struct {
    u32 start;
    u32 end;
} vfr_dlci_range_t;

typedef struct {
    vfr_dlci_range_t ranges[SVC_MAX_FREE_RANGES];
    int              count;
    u32              range_low;    /* Configured lower bound (e.g. 512) */
    u32              range_high;   /* Configured upper bound (e.g. 991) */
    vfr_dlci_alloc_dir_t alloc_dir; /* Allocation direction */
} vfr_dlci_allocator_t;

/* Transit Network Identification entry per X.76 §10.5.26 */
typedef struct {
    u8   type_plan;                                  /* Network ID type & numbering plan */
    char net_id[16];                                 /* Network ID digits string (e.g. "5104") */
} vfr_tni_entry_t;

typedef struct {
    vfr_tni_entry_t entries[SVC_MAX_TRANSIT_NETWORKS];
    u8              count;                           /* Number of transit networks (0-6) */
} vfr_tni_list_t;

/* ============================================================
 * Parsed Signaling Message Structures
 * ============================================================ */

/* Parsed Q.933 message header */
typedef struct {
    u8  protocol_disc;
    u8  call_ref_len;          /* 1 or 2 bytes */
    u16 call_ref_value;        /* Internal 15-bit CRV */
    u8  call_ref_flag;         /* 0=originator, 1=destination */
    u8  message_type;
} q933_msg_header_t;

/* Parsed Link Layer Core Parameters */
typedef struct {
    u32 fwd_cir;        /* Forward CIR (bps) */
    u32 bwd_cir;        /* Backward CIR (bps) */
    u32 fwd_bc;         /* Forward Bc (bits) */
    u32 bwd_bc;         /* Backward Bc (bits) */
    u32 fwd_be;         /* Forward Be (bits) */
    u32 bwd_be;         /* Backward Be (bits) */
    u16 fwd_fmif;       /* Forward max frame info field (octets) */
    u16 bwd_fmif;       /* Backward max frame info field (octets) */
    u8  present;        /* 1 if LLCORE IE was present */
} q933_llcore_params_t;

/* Parsed Priority and Service Class */
typedef struct {
    u8  ftp;            /* Frame Transfer Priority (0-15) */
    u8  fdp;            /* Frame Discard Priority (0-7) */
    u8  svc_class;      /* Service Class (0-3) */
    u8  present;
} q933_priority_params_t;

/* ============================================================
 * Call Control Block (CCB)
 * ============================================================ */
typedef struct vfr_call_s {
    u8      state;                          /* Current call state (SVC_STATE_*) */
    u8      in_use;                         /* 1 = CCB active */
    u16     call_ref;                       /* Call Reference Value (15-bit) */
    u8      call_ref_flag;                  /* 0=we originated, 1=peer originated */
    u8      call_ref_len;                   /* 1 or 2 (DTE's CRV octet length) */

    /* Addressing */
    char    calling_number[SVC_MAX_ADDR_LEN + 1];
    char    called_number[SVC_MAX_ADDR_LEN + 1];
    char    connected_number[SVC_MAX_ADDR_LEN + 1];
    u8      calling_subaddr[SVC_MAX_SUBADDR_LEN];
    u8      calling_subaddr_len;
    u8      called_subaddr[SVC_MAX_SUBADDR_LEN];
    u8      called_subaddr_len;
    u8      connected_subaddr[SVC_MAX_SUBADDR_LEN];
    u8      connected_subaddr_len;
    u8      calling_number_type;     /* 0=unknown, 1=international, 2=national */
    u8      calling_number_plan;     /* 0=unknown, 1=E.164, 3=X.121 */
    u8      called_number_type;      /* 0=unknown, 1=international, 2=national */
    u8      called_number_plan;      /* 0=unknown, 1=E.164, 3=X.121 */
    u8      connected_number_type;
    u8      connected_number_plan;
    u8      connected_presentation;
    u8      connected_screening;
    u8      calling_presentation;    /* 0=allowed, 1=restricted */
    u8      calling_screening;       /* 0=user-provided, 1=user-verified, 3=network */
    u8      rev_charge_requested;
    u8      priority_present;
    u8      ftp_out;
    u8      ftp_in;
    u8      fdp_out;
    u8      fdp_in;
    u8      srv_class;

    /* Port references */
    char    ingress_port[VFR_MAX_NAME_LEN];     /* Calling DTE's port */
    char    egress_port[VFR_MAX_NAME_LEN];      /* Called DTE's port */
    u32     ingress_dlci;                    /* SVC data DLCI on ingress port */
    u32     egress_dlci;                     /* SVC data DLCI on egress port */

    /* Peer Call Linking */
    vfr_port_t *peer_port;
    u16         peer_call_ref;
    u8          peer_call_ref_flag;

    /* Negotiated parameters */
    q933_llcore_params_t  llcore;
    q933_priority_params_t priority;

    /* Facility services */
    u8      reverse_charging;               /* 1 = reverse charging requested */
    u8      transit_network_present;
    u8      transit_network_type_plan;
    char    transit_network[16];

    /* NNI-Specific Tracking Fields (ITU-T X.76) */
    u8              is_nni;                 /* 1 = NNI signaling leg */
    u32             call_ident;             /* Fixed 4-octet Call Identification (IE 0x69) */
    vfr_tni_list_t  tni_list;               /* Transit Network Identification chain (max 6) */
    char            clearing_net_id[16];    /* Clearing Network Identification (IE 0x6B) */
    u8              clearing_net_type_plan;

    /* Optional user details propagation */
    u8      llc_data[32];
    u8      llc_len;
    u8      uu_data[136];
    u8      uu_len;

    /* Dynamic DLCI entries for active data phase */
    vfr_dlci_entry_t *fwd_pvc;             /* ingress->egress data entry */
    vfr_dlci_entry_t *rev_pvc;             /* egress->ingress data entry */

    /* Timers (X.36 / X.76 Tables 10-27/10-28 / Table 27/X.76) */
    vfr_timer_t t303;       /* 4s: SETUP sent, awaiting response */
    vfr_timer_t t305;       /* 30s: DISCONNECT sent, awaiting RELEASE */
    vfr_timer_t t308;       /* 4s: RELEASE sent (retransmit once) */
    vfr_timer_t t310;       /* 10s (UNI) / 40s (NNI): CALL PROC received, awaiting CONNECT */
    vfr_timer_t t301;       /* 180s: Alerting, awaiting CONNECT (UNI only) */
    vfr_timer_t t322;       /* 4s: STATUS ENQUIRY sent */

    /* Retransmission counters */
    u8      t303_retries;
    u8      t308_retries;
    u8      t322_retries;
} vfr_call_t;

/* ============================================================
 * Per-Port SVC Context
 * ============================================================ */
typedef struct {
    int                  enabled;           /* 1 = SVC active on this port */
    u8                   is_nni;            /* 1 = NNI interface, 0 = UNI */
    u8                   crv_len_cfg;       /* 0=auto, 1=1-byte, 2=2-byte */
    vfr_call_t           calls[SVC_MAX_CALLS_PER_PORT]; /* CCB pool */
    vfr_dlci_allocator_t dlci_alloc;        /* DLCI range allocator */
    vfr_dlci_alloc_dir_t dlci_alloc_dir;    /* Direction: 0=low/ascending, 1=high/descending */
    char                 subscriber_number[SVC_MAX_ADDR_LEN + 1]; /* Primary X.121 number */
    char                 alias_number[SVC_MAX_ADDR_LEN + 1];      /* Alias number */
    char                 network_id[16];        /* Own Network Identification (for TNI/CNI) */
    char                 remote_network_id[16]; /* Connected Remote Network Identification */
    u8                   network_id_type_plan;
    u8                   subscriber_type;   /* 1=x121, 2=e164 */
    u8                   alias_type;        /* 1=x121, 2=e164 */
    u8                   presentation;      /* 0=allowed, 1=restricted */
    u8                   reverse_charging_acceptance;
    u8                   reverse_charging_prevention;

    /* Per-port SVC defaults */
    u32     default_cir;
    u32     default_bc;
    u32     default_be;
    u16     default_fmif;
    u8      default_ftp;
    u8      default_fdp;
    u8      default_svc_class;

    /* Per-port timer defaults (milliseconds) */
    u32     t303_ms;
    u32     t305_ms;
    u32     t308_ms;
    u32     t310_ms;
    u32     t301_ms;
    u32     t316_ms;
    u32     t317_ms;
    u32     t322_ms;

    /* RESTART state */
    u8      restart_state;
    vfr_timer_t t316;       /* 120s: RESTART sent */
    vfr_timer_t t317;       /* 10s (UNI) / 20s (NNI): RESTART received, clearing watchdog */
    u8      t316_retries;
} vfr_svc_ctx_t;

/* Function prototypes for svc_sig_common.c */
int  svc_init_port(vfr_port_t *port, u32 dlci_low, u32 dlci_high, u8 crv_len_cfg);
int  svc_init_port_ex(vfr_port_t *port, u32 dlci_low, u32 dlci_high, u8 crv_len_cfg, u8 is_nni, vfr_dlci_alloc_dir_t alloc_dir);
void svc_free_port(vfr_port_t *port);
vfr_call_t *svc_alloc_call_ref(vfr_svc_ctx_t *svc_ctx, u16 requested_crv, u8 crv_flag);
vfr_call_t *svc_find_call(vfr_svc_ctx_t *svc_ctx, u16 call_ref, u8 call_ref_flag);
void svc_free_call(vfr_svc_ctx_t *svc_ctx, vfr_call_t *call);
void svc_poll_timers(vfr_port_t *port);
u32  svc_dlci_alloc_ex(vfr_dlci_allocator_t *alloc, vfr_dlci_alloc_dir_t dir);
u32  svc_generate_call_ident(void);
int  svc_create_data_pvcs(vfrs_ctx_t *ctx, vfr_call_t *call);
void svc_destroy_data_pvcs(vfrs_ctx_t *ctx, vfr_call_t *call);
void svc_handle_l3_message(vfr_port_t *port, const u8 *data, size_t len);

/* SVC CLI & Inspection Functions */
void vfrs_show_svc_calls(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_svc_call_detail(vfrs_ctx_t *ctx, const char *arg, FILE *out);
void vfrs_show_svc_subscribers(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_svc_routes(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_svc_stats(vfrs_ctx_t *ctx, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* SVC_SIG_COMMON_H */
