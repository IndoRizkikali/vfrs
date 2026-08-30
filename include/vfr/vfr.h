/*
 * vfr.h - VFRS Public Master Umbrella Header
 * Virtual Frame Relay Switch - Platform Foundation
 */

#ifndef VFRS_H
#define VFRS_H

#include "export.h"
#include "platform.h"
#include "types.h"
#include "logger.h"
#include "config.h"
#include "pcap.h"
#include "switching.h"
#include "pvc.h"
#include "ports.h"
#include "congestion.h"
#include "lapf.h"
#include "svc.h"
#include "fragment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Global Switch Context
 * ============================================================ */

struct vfrs_ctx_s {
    char            swid[64];       /* Switch identifier */

    /* Numbering plan for SVC */
    u16             dcc;            /* Data Country Code */
    u16             nd;             /* Network Digit */
    u16             dnic;           /* Data Network ID */
    u32             pnic;           /* Private Data Network ID */
    u8              sgclen;         /* SGC length */
    u8              sgc;            /* System Group Code */
    u8              siclen;         /* SIC length */
    u8              sic;            /* System Identification Code */
    u8              subnumlen;      /* Subscriber number length */

    /* SVC subscriber table */
    struct {
        char port_name[VFR_MAX_NAME_LEN];
        char x121_number[21];
        u8   reverse_charging_acceptance;
        u8   reverse_charging_prevention;
        u8   number_type;
    } svc_subscribers[512];
    int             svc_subscriber_count;

    /* SVC NNI route table */
    struct {
        char prefix[21];
        char egress_port[VFR_MAX_NAME_LEN];
        char transit_net_id[16];
        u8   metric;
    } svc_routes[256];
    int             svc_route_count;

    /* Global DLCI Table */
    vfr_dlci_entry_t *dlci_table[PVC_HASH_SIZE];
    mutex_t         dlci_mutex;

    /* PVC table */
    vfr_pvc_detail_t *pvc_table[PVC_HASH_SIZE];
    mutex_t         pvc_mutex;

    /* SVC table (forward declared struct vfr_call_s) */
    struct vfr_call_s *svc_table[PVC_HASH_SIZE];
    mutex_t         svc_mutex;

    /* Multicast groups */
    vfr_mcast_group_t *mcast_groups;
    mutex_t         mcast_mutex;

    /* Port list */
    vfr_port_t      *ports[MAX_PORTS];
    int             port_count;
    mutex_t         port_mutex;

    /* Global capture */
    pcap_writer_t   *global_capture;

    /* Default parameters */
    u16             default_n391;
    u16             default_n392;
    u16             default_n393;
    u16             default_t391;
    u16             default_t392;
    u16             default_n392_dte;
    u16             default_n393_dte;

    u16             default_lapf_k;
    u16             default_lapf_n200;
    u16             default_lapf_n201;
    u16             default_lapf_t200;
    u16             default_lapf_t203;

    u32             default_svc_t301;
    u32             default_svc_t303;
    u32             default_svc_t305;
    u32             default_svc_t308;
    u32             default_svc_t310;
    u32             default_svc_t316;
    u32             default_svc_t317;
    u32             default_svc_t322;

    u32             default_cir;
    u32             default_bc;
    u32             default_be;
    u16             default_svc_fmif;
    u8              default_svc_ftp;
    u8              default_svc_fdp;
    u8              default_svc_class;
    u32             access_rate;

    /* SPSC signaling queue & slow-path thread (Phase 3) */
    _Atomic u16     next_crv;
    vfr_spsc_queue_t sig_queue;
    thread_t        slow_path_thread;
};

/* Global context (singleton) */
extern vfrs_ctx_t  *g_vfrs;
extern _Atomic int  g_running;   /* 1 = running; 0 = shutdown requested */

/* Context initialization */
VFR_API vfrs_ctx_t *vfrs_create(const char *swid);
VFR_API void vfrs_destroy(vfrs_ctx_t *ctx);

/* Statistics / Show functions */
VFR_API void vfrs_show_ports(vfrs_ctx_t *ctx, FILE *out);
VFR_API void vfrs_show_pvcs(vfrs_ctx_t *ctx, FILE *out);
VFR_API void vfrs_show_stats(vfrs_ctx_t *ctx, const char *port_name, FILE *out);
VFR_API void vfrs_show_config(vfrs_ctx_t *ctx, FILE *out);
VFR_API void vfrs_show_defaults(vfrs_ctx_t *ctx, FILE *out);
VFR_API void vfrs_show_swconfig(vfrs_ctx_t *ctx, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* VFRS_H */
