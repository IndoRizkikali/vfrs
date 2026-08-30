/*
 * cfg_ast.h - VFRS Configuration Abstract Syntax Tree (AST) Definitions
 * Virtual Frame Relay Switch
 */

#ifndef VFR_CFG_AST_H
#define VFR_CFG_AST_H

#include "types.h"
#include "vfr/cfg_schema.h"
#include "vfr/cfg_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAX_PORTS       256
#define CFG_MAX_PVCS        1024
#define CFG_MAX_ROUTES      512
#define CFG_MAX_MCAST_MEMBERS 64
#define CFG_MAX_MCAST_GROUPS 32
#define CFG_MAX_SPVCS       128
#define CFG_MAX_ALIASES     16

/* Numbering Plan Selection */
typedef enum {
    NSPF_X121 = 1,
    NSPF_E164 = 2
} cfg_nspf_t;

/* AST Node: swconfig */
typedef struct {
    int         configured;
    char        swid[64];
    cfg_nspf_t  nspf;

    /* X.121 Dial Plan */
    char        dcc[4];             /* 3 digits */
    char        nd[2];              /* 1 digit or empty */
    char        dnic[5];            /* 4 digits */
    char        pnic[8];            /* up to 6 digits */
    char        x121_ind[32];       /* unbounded */
    int         sgclen;             /* -1 = none */
    char        sgc[8];
    int         siclen;             /* -1 = none */
    char        sic[8];
    int         sublen;             /* -1 = auto */

    /* E.164 Dial Plan */
    char        cc[4];              /* 1-3 digits */
    char        ic[5];              /* 1-4 digits */
    char        ndc[8];
    char        area[8];
    char        e164_ind[32];       /* unbounded */
} ast_swconfig_t;

/* AST Node: log */
typedef struct {
    char con_level[16];
    char txt_level[16];
    char file_path[256];
    u32  rot_size_mb;
    u32  rot_files;
} ast_log_t;

/* AST Node: port */
typedef struct {
    char port_name[32];
    char transport[32];
    char pipe_name[128];
    char serial_dev[128];
    u32  baud;
    char local_host[64];
    u16  local_port;
    char remote_host[64];
    u16  remote_port;
    u32  vcid;
    u8   dlcibit;
    u32  ar;
    int  line;
} ast_port_t;

/* AST Node: pvc */
typedef struct {
    char port1[32];
    u32  dlci1;
    char port2[32];
    u32  dlci2;
    u32  cir;
    u32  bc;
    u32  be;
    u8   ftp;
    u8   fdp;
    u8   srv_class;
    int  line;
} ast_pvc_t;

/* AST Node: pvc mcast */
typedef struct {
    char name[32];
    u32  cir;
    u32  bc;
    u32  be;
    u8   ftp;
    u8   fdp;
    u8   srv_class;
    char src_port[32];
    u32  src_dlci;
    char mode[16]; /* oneway, twoway, nway */
    int  num_members;
    struct {
        char port[32];
        u32  dlci;
        u32  cir;
        u32  bc;
        u32  be;
        u8   ftp;
        u8   fdp;
        u8   srv_class;
    } members[CFG_MAX_MCAST_MEMBERS];
    int line;
} ast_pvc_mcast_t;

/* AST Node: lmi */
typedef struct {
    char port[32];
    char role[16];     /* dce, dte */
    char standard[16]; /* ansi, cisco, q933a, none */
    u16  t391_s;
    u16  t392_s;
    u8   n391;
    u8   n392;
    u8   n393;
    int  async_enabled;
    int  line;
} ast_lmi_t;

/* AST Node: lapf */
typedef struct {
    char port[32];
    char sabme[16];    /* active, passive, bidirectional */
    char xid[16];      /* active, passive, disable */
    u8   k;
    u8   n200;
    u16  n201;
    u32  t200_ms;
    u32  t203_ms;
    int  line;
} ast_lapf_t;

/* AST Node: svc int */
typedef struct {
    char port[32];
    u32  dlci_low;
    u32  dlci_high;
    u32  t301_ms;
    u32  t303_ms;
    u32  t305_ms;
    u32  t308_ms;
    u32  t310_ms;
    u32  t316_ms;
    u32  t317_ms;
    u32  t322_ms;
    u32  cirdef;
    u32  bcdef;
    u32  bedef;
    u16  fmifdef;
    u8   ftpdef;
    u8   fdpdef;
    u8   clsdef;
    u8   revchg_allow;
    u8   ogrevchg_allow;
    u8   icrevchg_allow;
    int  line;
} ast_svc_int_t;

/* AST Node: svc addr */
typedef struct {
    char port[32];
    char mode[16];     /* manual, autoprefix, autonumber */
    char plan[16];     /* x121, e164 */
    char number_primary[32];
    char autonumber_scheme[32]; /* global, group, global-reverse, group-reverse */
    int  num_aliases;
    struct {
        char plan[16];
        char number[32];
        u8   revchg_allow;
    } aliases[CFG_MAX_ALIASES];
    int  line;
} ast_svc_addr_t;

/* AST Node: svc route */
typedef struct {
    char port[32];
    char mode[16];     /* x121, e164, prefix */
    char plan[16];

    /* X.121 fields */
    char dcc[4];
    char nd[2];
    char dnic[5];
    char pnic[8];
    char x121_ind[32];

    /* E.164 fields */
    char cc[4];
    char ic[5];
    char ndc[8];
    char area[8];
    char e164_ind[32];

    /* Common */
    int  sgclen;
    char sgc[8];
    int  siclen;
    char sic[8];
    char prefix_regex[64];

    u8   revchg_allow;
    u32  metric;
    int  line;
} ast_svc_route_t;

/* AST Node: svc mcast */
typedef struct {
    char group_name[32];
    char conf_plan[16];
    char conf_number[32];
    char subaddr_number[32];
    char src_plan[16];
    char src_number_or_port[32];
    char src_subaddr[32];
    char mode[16];
    int  num_members;
    struct {
        char plan[16];
        char number_or_port[32];
        char subaddr[32];
    } members[CFG_MAX_MCAST_MEMBERS];
    int  line;
} ast_svc_mcast_t;

/* AST Node: spvc */
typedef struct {
    char port1[32];
    u32  local_dlci;
    u32  lspvcid;
    char dest_vfrs_number[32];
    char target_type[16]; /* specific, correlator */
    u32  tdlci;
    u32  tspvcid;
    u32  cir;
    u32  bc;
    u32  be;
    u8   ftp;
    u8   fdp;
    u8   srv_class;
    u8   revchg_allow;
    int  line;
} ast_spvc_t;

/* AST Node: issmp */
typedef struct {
    char port[32];
    int  enabled;
    u32  as;
    u32  isic;
    u32  cost;
    char pw[64];
    int  line;
} ast_issmp_t;

/* AST Node: cgst */
typedef struct {
    char port[32];
    u32  rate_fps;
    u32  clear_fps;
    u32  threshold;
    int  cllm_enabled;
    u32  cllm_txint_ms;
    int  line;
} ast_cgst_t;

/* AST Node: capture */
typedef struct {
    char target[64]; /* port name, group, or 'global'/'all' */
    char pcap_file[256];
    char svccap[16]; /* iframe, uiframe */
    int  line;
} ast_capture_t;

/* Unified AST Container */
typedef struct {
    ast_swconfig_t        swconfig;
    ast_log_t             log;
    vfr_scoped_defaults_t defaults;

    int                   num_ports;
    ast_port_t            ports[CFG_MAX_PORTS];

    int                   num_pvcs;
    ast_pvc_t             pvcs[CFG_MAX_PVCS];

    int                   num_pvc_mcasts;
    ast_pvc_mcast_t       pvc_mcasts[CFG_MAX_MCAST_GROUPS];

    int                   num_lmis;
    ast_lmi_t             lmis[CFG_MAX_PORTS];

    int                   num_lapfs;
    ast_lapf_t            lapfs[CFG_MAX_PORTS];

    int                   num_svc_ints;
    ast_svc_int_t         svc_ints[CFG_MAX_PORTS];

    int                   num_svc_addrs;
    ast_svc_addr_t        svc_addrs[CFG_MAX_PORTS];

    int                   num_svc_routes;
    ast_svc_route_t       svc_routes[CFG_MAX_ROUTES];

    int                   num_svc_mcasts;
    ast_svc_mcast_t       svc_mcasts[CFG_MAX_MCAST_GROUPS];

    int                   num_spvcs;
    ast_spvc_t            spvcs[CFG_MAX_SPVCS];

    int                   num_issmps;
    ast_issmp_t           issmps[CFG_MAX_PORTS];

    int                   num_cgsts;
    ast_cgst_t            cgsts[CFG_MAX_PORTS];

    int                   num_captures;
    ast_capture_t         captures[CFG_MAX_PORTS];

    int                   has_error;
    char                  err_msg[512];
    int                   err_line;
} cfg_ast_t;

VFR_API cfg_ast_t *cfg_ast_create(void);
VFR_API void       cfg_ast_destroy(cfg_ast_t *ast);
VFR_API int        cfg_parse_tokens(cfg_lexer_t *lex, cfg_ast_t *ast);

#ifdef __cplusplus
}
#endif

#endif /* VFR_CFG_AST_H */
