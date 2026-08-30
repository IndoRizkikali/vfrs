/*
 * cfg_schema.h - VFRS Configuration Schema & Strict Validation Utilities
 * Virtual Frame Relay Switch
 */

#ifndef VFR_CFG_SCHEMA_H
#define VFR_CFG_SCHEMA_H

#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u32 cir;
    u32 bc;
    u32 be;
    u16 fmif;
    u8  ftp;
    u8  fdp;
    u8  srv_class;
    u8  revchg_allow;
    u8  ogrevchg_allow;
    u8  icrevchg_allow;
    u8  set_cir:1;
    u8  set_bc:1;
    u8  set_be:1;
    u8  set_fmif:1;
    u8  set_ftp:1;
    u8  set_fdp:1;
    u8  set_srv_class:1;
    u8  set_revchg:1;
    u8  set_ogrevchg:1;
    u8  set_icrevchg:1;
} vfr_scoped_svc_def_t;

/* Scoped defaults container */
typedef struct {
    struct {
        u8  dlcibit;        /* 10 or 23 */
        u32 ar;             /* Access rate bps */
        u8  set_dlcibit:1;
        u8  set_ar:1;
    } interface_def;

    struct {
        u8  k;              /* Window size 1-127 */
        u8  n200;           /* Max retries */
        u16 n201;           /* Max frame size */
        u32 t200_ms;        /* Retransmit timer ms */
        u32 t203_ms;        /* Idle timer ms */
        u8  set_k:1;
        u8  set_n200:1;
        u8  set_n201:1;
        u8  set_t200:1;
        u8  set_t203:1;
    } lapf_def;

    struct {
        u8  n392;           /* Error threshold 1-10 */
        u8  n393;           /* Monitored window 1-10 */
        u16 t392_s;         /* DCE polling timer 5-30s */
        u8  set_n392:1;
        u8  set_n393:1;
        u8  set_t392:1;
    } lmi_dce_def;

    struct {
        u8  n391;           /* Full status counter 1-255 */
        u8  n392;           /* DTE Error threshold 1-10 */
        u8  n393;           /* DTE Monitored window 1-10 */
        u16 t391_s;         /* DTE polling timer 5-30s */
        u8  set_n391:1;
        u8  set_n392:1;
        u8  set_n393:1;
        u8  set_t391:1;
    } lmi_dte_def;

    struct {
        u32 cir;
        u32 bc;
        u32 be;
        u8  ftp;            /* 0-15 */
        u8  fdp;            /* 0-7 */
        u8  srv_class;      /* 0-3 */
        u8  set_cir:1;
        u8  set_bc:1;
        u8  set_be:1;
        u8  set_ftp:1;
        u8  set_fdp:1;
        u8  set_srv_class:1;
    } pvc_def;

    vfr_scoped_svc_def_t svc_def;
    vfr_scoped_svc_def_t svc_mcast_def;

    struct {
        u32 t301_ms;
        u32 t303_ms;
        u32 t305_ms;
        u32 t308_ms;
        u32 t310_ms;
        u32 t316_ms;
        u32 t317_ms;
        u32 t322_ms;
        u8  set_t301:1;
        u8  set_t303:1;
        u8  set_t305:1;
        u8  set_t308:1;
        u8  set_t310:1;
        u8  set_t316:1;
        u8  set_t317:1;
        u8  set_t322:1;
    } svc_uni_def;

    struct {
        u32 t301_ms;
        u32 t303_ms;
        u32 t305_ms;
        u32 t308_ms;
        u32 t310_ms;
        u32 t316_ms;
        u32 t317_ms;
        u32 t322_ms;
        u8  set_t301:1;
        u8  set_t303:1;
        u8  set_t305:1;
        u8  set_t308:1;
        u8  set_t310:1;
        u8  set_t316:1;
        u8  set_t317:1;
        u8  set_t322:1;
    } svc_nni_def;
} vfr_scoped_defaults_t;

/* Schema Parser Functions */
VFR_API void cfg_scoped_defaults_init(vfr_scoped_defaults_t *defs);

VFR_API int cfg_parse_u32(const char *str, u32 min_val, u32 max_val, u32 *out_val);
VFR_API int cfg_parse_u16(const char *str, u16 min_val, u16 max_val, u16 *out_val);
VFR_API int cfg_parse_u8(const char *str, u8 min_val, u8 max_val, u8 *out_val);
VFR_API int cfg_parse_rate(const char *str, u32 *out_bps);
VFR_API int cfg_parse_time_ms(const char *str, u32 *out_ms);
VFR_API int cfg_parse_time_s(const char *str, u16 *out_s);
VFR_API int cfg_parse_bool(const char *str, int *out_bool);
VFR_API int cfg_parse_enum(const char *str, const char *const table[], int count, int *out_idx);

#ifdef __cplusplus
}
#endif

#endif /* VFR_CFG_SCHEMA_H */
