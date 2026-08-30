/*
 * cfg_schema.c - VFRS Configuration Schema & Strict Validation Utilities
 * Virtual Frame Relay Switch
 */

#include "vfr/cfg_schema.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>

void cfg_scoped_defaults_init(vfr_scoped_defaults_t *defs)
{
    if (!defs) return;
    memset(defs, 0, sizeof(*defs));

    /* Baseline System Standards Defaults */
    defs->interface_def.dlcibit = 10;
    defs->interface_def.ar = 0; /* 0 = unconstrained or auto-derived from baud */

    defs->lapf_def.k = 8;
    defs->lapf_def.n200 = 3;
    defs->lapf_def.n201 = 1598;
    defs->lapf_def.t200_ms = 1500;
    defs->lapf_def.t203_ms = 30000;

    defs->lmi_dce_def.n392 = 3;
    defs->lmi_dce_def.n393 = 4;
    defs->lmi_dce_def.t392_s = 15;

    defs->lmi_dte_def.n391 = 6;
    defs->lmi_dte_def.n392 = 3;
    defs->lmi_dte_def.n393 = 4;
    defs->lmi_dte_def.t391_s = 10;

    defs->pvc_def.cir = 0;
    defs->pvc_def.bc = 0;
    defs->pvc_def.be = 0;
    defs->pvc_def.ftp = 8;
    defs->pvc_def.fdp = 4;
    defs->pvc_def.srv_class = 1;

    defs->svc_def.cir = 0;
    defs->svc_def.bc = 0;
    defs->svc_def.be = 0;
    defs->svc_def.fmif = 1600;
    defs->svc_def.ftp = 8;
    defs->svc_def.fdp = 4;
    defs->svc_def.srv_class = 1;
    defs->svc_def.revchg_allow = 1;
    defs->svc_def.ogrevchg_allow = 1;
    defs->svc_def.icrevchg_allow = 1;

    defs->svc_mcast_def = defs->svc_def;

    defs->svc_uni_def.t301_ms = 180000;
    defs->svc_uni_def.t303_ms = 4000;
    defs->svc_uni_def.t305_ms = 30000;
    defs->svc_uni_def.t308_ms = 4000;
    defs->svc_uni_def.t310_ms = 35000;
    defs->svc_uni_def.t316_ms = 120000;
    defs->svc_uni_def.t317_ms = 10000;
    defs->svc_uni_def.t322_ms = 4000;

    defs->svc_nni_def.t301_ms = 180000;
    defs->svc_nni_def.t303_ms = 4000;
    defs->svc_nni_def.t305_ms = 30000;
    defs->svc_nni_def.t308_ms = 4000;
    defs->svc_nni_def.t310_ms = 40000;
    defs->svc_nni_def.t316_ms = 120000;
    defs->svc_nni_def.t317_ms = 20000;
    defs->svc_nni_def.t322_ms = 4000;
}

int cfg_parse_u32(const char *str, u32 min_val, u32 max_val, u32 *out)
{
    if (!str || !*str || !out) return -1;

    /* Reject negative sign explicitly */
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') return -1;
    if (*str == '+') str++;

    char *endptr = NULL;
    errno = 0;
    unsigned long long val = strtoull(str, &endptr, 10);

    if (errno != 0 || endptr == str) return -1;
    while (*endptr == ' ' || *endptr == '\t') endptr++;
    if (*endptr != '\0') return -1; /* Trailing non-numeric garbage */

    if (val < min_val || val > max_val || val > 0xFFFFFFFFULL) {
        return -1;
    }

    *out = (u32)val;
    return 0;
}

int cfg_parse_u16(const char *str, u16 min_val, u16 max_val, u16 *out)
{
    if (!out) return -1;
    u32 v = 0;
    if (cfg_parse_u32(str, min_val, max_val, &v) < 0) return -1;
    *out = (u16)v;
    return 0;
}

int cfg_parse_u8(const char *str, u8 min_val, u8 max_val, u8 *out)
{
    if (!out) return -1;
    u32 v = 0;
    if (cfg_parse_u32(str, min_val, max_val, &v) < 0) return -1;
    *out = (u8)v;
    return 0;
}

int cfg_parse_rate(const char *str, u32 *out_bps)
{
    if (!str || !*str || !out_bps) return -1;

    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') return -1;

    char *endptr = NULL;
    errno = 0;
    double val = strtod(str, &endptr);
    if (errno != 0 || endptr == str || val < 0) return -1;

    while (*endptr == ' ' || *endptr == '\t') endptr++;

    double mult = 1.0;
    if (*endptr != '\0') {
        if (strcasecmp(endptr, "k") == 0 || strcasecmp(endptr, "kbps") == 0) {
            mult = 1000.0;
        } else if (strcasecmp(endptr, "kib") == 0) {
            mult = 1024.0;
        } else if (strcasecmp(endptr, "m") == 0 || strcasecmp(endptr, "mbps") == 0) {
            mult = 1000000.0;
        } else if (strcasecmp(endptr, "mib") == 0) {
            mult = 1048576.0;
        } else if (strcasecmp(endptr, "g") == 0 || strcasecmp(endptr, "gbps") == 0) {
            mult = 1000000000.0;
        } else if (strcasecmp(endptr, "bps") == 0) {
            mult = 1.0;
        } else {
            return -1; /* Unknown rate unit */
        }
    }

    double res = val * mult;
    if (res > 4294967295.0) return -1;

    *out_bps = (u32)res;
    return 0;
}

int cfg_parse_time_ms(const char *str, u32 *out_ms)
{
    if (!str || !*str || !out_ms) return -1;

    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') return -1;

    char *endptr = NULL;
    errno = 0;
    double val = strtod(str, &endptr);
    if (errno != 0 || endptr == str || val < 0) return -1;

    while (*endptr == ' ' || *endptr == '\t') endptr++;

    double mult = 1000.0; /* Default time unit is seconds -> convert to ms */
    if (*endptr != '\0') {
        if (strcasecmp(endptr, "s") == 0 || strcasecmp(endptr, "sec") == 0) {
            mult = 1000.0;
        } else if (strcasecmp(endptr, "ms") == 0) {
            mult = 1.0;
        } else if (strcasecmp(endptr, "m") == 0 || strcasecmp(endptr, "min") == 0) {
            mult = 60000.0;
        } else {
            return -1;
        }
    }

    double res = val * mult;
    if (res > 4294967295.0) return -1;

    *out_ms = (u32)res;
    return 0;
}

int cfg_parse_time_s(const char *str, u16 *out_s)
{
    if (!str || !out_s) return -1;
    u32 ms = 0;
    if (cfg_parse_time_ms(str, &ms) < 0) return -1;
    u32 s = (ms + 500) / 1000;
    if (s > 65535) return -1;
    *out_s = (u16)s;
    return 0;
}

int cfg_parse_bool(const char *str, int *out)
{
    if (!str || !*str || !out) return -1;

    if (strcasecmp(str, "true") == 0 || strcasecmp(str, "1") == 0 ||
        strcasecmp(str, "on") == 0 || strcasecmp(str, "enable") == 0 ||
        strcasecmp(str, "enabled") == 0 || strcasecmp(str, "allow") == 0 ||
        strcasecmp(str, "yes") == 0) {
        *out = 1;
        return 0;
    }

    if (strcasecmp(str, "false") == 0 || strcasecmp(str, "0") == 0 ||
        strcasecmp(str, "off") == 0 || strcasecmp(str, "disable") == 0 ||
        strcasecmp(str, "disabled") == 0 || strcasecmp(str, "deny") == 0 ||
        strcasecmp(str, "no") == 0) {
        *out = 0;
        return 0;
    }

    return -1;
}

int cfg_parse_enum(const char *str, const char *const *table, int count, int *out)
{
    if (!str || !table || count <= 0 || !out) return -1;

    for (int i = 0; i < count; i++) {
        if (table[i] && strcasecmp(str, table[i]) == 0) {
            *out = i;
            return 0;
        }
    }

    return -1;
}
