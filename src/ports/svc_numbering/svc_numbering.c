/*
 * svc_numbering.c - VFRS SVC X.121 / E.164 Numbering Register & Expansion Engine
 * Virtual Frame Relay Switch
 */

#include "svc_numbering.h"
#include <ctype.h>

int svc_numbering_init(vfrs_ctx_t *ctx) {
    if (!ctx) return -1;
    ctx->svc_subscriber_count = 0;
    memset(ctx->svc_subscribers, 0, sizeof(ctx->svc_subscribers));
    return 0;
}

const char *svc_numbering_expand(vfrs_ctx_t *ctx, const char *raw, char *out_buf, size_t out_len) {
    if (!ctx || !raw || !out_buf || out_len == 0) return "";

    char dnic_str[16];
    char sgc_str[16];
    char sic_str[16];
    char dge_str[48];

    snprintf(dnic_str, sizeof(dnic_str), "%04u", (unsigned int)ctx->dnic);

    /* Form SGC string zero-padded to sgclen */
    int sgclen = (ctx->sgclen > 0 && ctx->sgclen <= 8) ? (int)ctx->sgclen : 1;
    if (sgclen > 8) sgclen = 8;
    snprintf(sgc_str, sizeof(sgc_str), "%0*u", sgclen & 0x0F, (unsigned int)ctx->sgc);

    /* Form SIC string zero-padded to siclen */
    int siclen = (ctx->siclen > 0 && ctx->siclen <= 8) ? (int)ctx->siclen : 1;
    if (siclen > 8) siclen = 8;
    snprintf(sic_str, sizeof(sic_str), "%0*u", siclen & 0x0F, (unsigned int)ctx->sic);

    /* Form combined DGE string */
    snprintf(dge_str, sizeof(dge_str), "%s%s%s", dnic_str, sgc_str, sic_str);

    out_buf[0] = '\0';
    size_t out_idx = 0;
    const char *p = raw;

    while (*p != '\0') {
        if (*p == ',') {
            /* Skip commas used as separator */
            p++;
            continue;
        }

        /* Check multi-character macro keywords first */
        if (strncasecmp(p, "dnic", 4) == 0) {
            size_t len = strlen(dnic_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], dnic_str);
                out_idx += len;
            }
            p += 4;
        }
        else if (strncasecmp(p, "sgrc", 4) == 0) {
            size_t len = strlen(sgc_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], sgc_str);
                out_idx += len;
            }
            p += 4;
        }
        else if (strncasecmp(p, "sgc", 3) == 0) {
            size_t len = strlen(sgc_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], sgc_str);
                out_idx += len;
            }
            p += 3;
        }
        else if (strncasecmp(p, "sysc", 4) == 0) {
            size_t len = strlen(sic_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], sic_str);
                out_idx += len;
            }
            p += 4;
        }
        else if (strncasecmp(p, "sic", 3) == 0) {
            size_t len = strlen(sic_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], sic_str);
                out_idx += len;
            }
            p += 3;
        }
        /* Single-character mnemonics: D, G, E (can stand alone or combine in any order/combinations: DG, DE, GE, EG, ED, GD, DGE, etc.) */
        else if (*p == 'D' || *p == 'd') {
            size_t len = strlen(dnic_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], dnic_str);
                out_idx += len;
            }
            p++;
        }
        else if (*p == 'G' || *p == 'g') {
            size_t len = strlen(sgc_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], sgc_str);
                out_idx += len;
            }
            p++;
        }
        else if (*p == 'E' || *p == 'e') {
            size_t len = strlen(sic_str);
            if (out_idx + len < out_len) {
                strcpy(&out_buf[out_idx], sic_str);
                out_idx += len;
            }
            p++;
        }
        else {
            /* Copy literal digit/character */
            if (out_idx + 1 < out_len) {
                out_buf[out_idx++] = *p;
                out_buf[out_idx] = '\0';
            }
            p++;
        }
    }

    return out_buf;
}

int svc_numbering_is_all_zeros(vfrs_ctx_t *ctx, const char *number) {
    if (!number || number[0] == '\0') return 0;

    /* Expand first if raw contains macros */
    char expanded[SVC_MAX_ADDR_LEN + 1];
    svc_numbering_expand(ctx, number, expanded, sizeof(expanded));

    char dge_str[48];
    char dnic_str[16];
    char sgc_str[16];
    char sic_str[16];

    snprintf(dnic_str, sizeof(dnic_str), "%04u", (unsigned int)ctx->dnic);
    int sgclen = (ctx->sgclen > 0 && ctx->sgclen <= 8) ? (int)ctx->sgclen : 1;
    if (sgclen > 8) sgclen = 8;
    int siclen = (ctx->siclen > 0 && ctx->siclen <= 8) ? (int)ctx->siclen : 1;
    if (siclen > 8) siclen = 8;
    snprintf(sgc_str, sizeof(sgc_str), "%0*u", sgclen & 0x0F, (unsigned int)ctx->sgc);
    snprintf(sic_str, sizeof(sic_str), "%0*u", siclen & 0x0F, (unsigned int)ctx->sic);
    snprintf(dge_str, sizeof(dge_str), "%s%s%s", dnic_str, sgc_str, sic_str);

    size_t prefix_len = strlen(dge_str);

    const char *suffix = expanded;
    if (strncmp(expanded, dge_str, prefix_len) == 0) {
        suffix = expanded + prefix_len;
    }

    if (*suffix == '\0') return 0;

    /* Check if all remaining digits in suffix are '0' */
    for (const char *c = suffix; *c != '\0'; c++) {
        if (*c != '0') return 0;
    }

    return 1;
}

int svc_numbering_add_mode(vfrs_ctx_t *ctx, const char *port_name,
                           const char *mode, const char *num_type,
                           const char *raw_number, const char *alias_type,
                           const char *raw_alias, int rev_charge_acc, int rev_charge_prev) {
    if (!ctx || !port_name || !raw_number) return -1;

    char input_number[SVC_MAX_ADDR_LEN + 1];
    int is_autoprefix = (mode && strcasecmp(mode, "autoprefix") == 0);

    if (is_autoprefix) {
        /* Enforce subnumlen strictly on the primary sub-number */
        u8 max_sublen = (ctx->subnumlen > 0 && ctx->subnumlen <= 10) ? ctx->subnumlen : 4;
        if (strlen(raw_number) > max_sublen) {
            LOG_ERROR("SVC Numbering: Autoprefix subscriber number '%s' length (%zu) exceeds subnumlen (%u)",
                      raw_number, strlen(raw_number), max_sublen);
            return -1;
        }
        /* Prepend DGE macro */
        snprintf(input_number, sizeof(input_number), "DGE%s", raw_number);
    } else {
        snprintf(input_number, sizeof(input_number), "%s", raw_number);
    }

    char expanded_num[SVC_MAX_ADDR_LEN + 1];
    svc_numbering_expand(ctx, input_number, expanded_num, sizeof(expanded_num));

    /* Validate that number contains only decimal digits */
    for (int i = 0; expanded_num[i] != '\0'; i++) {
        if (!isdigit((unsigned char)expanded_num[i])) {
            LOG_ERROR("SVC Numbering: Invalid number format '%s' (non-digit characters prohibited)", expanded_num);
            return -1;
        }
    }

    /* Validate length against ITU standards (14 for X.121, 15 for E.164) */
    size_t num_len = strlen(expanded_num);
    int is_e164_num = (num_type && strcasecmp(num_type, "e164") == 0);
    if (is_e164_num && num_len > 15) {
        LOG_ERROR("SVC Numbering: E.164 number '%s' exceeds 15-digit limit", expanded_num);
        return -1;
    } else if (!is_e164_num && num_len > 14) {
        LOG_ERROR("SVC Numbering: X.121 number '%s' exceeds 14-digit limit", expanded_num);
        return -1;
    }

    /* Prohibit all-zeros subscriber suffix on user ports */
    if (svc_numbering_is_all_zeros(ctx, expanded_num)) {
        LOG_ERROR("SVC Numbering: Cannot assign switch all-zeros reserved number '%s' to port %s",
                  expanded_num, port_name);
        return -1;
    }

    /* Check for duplicate registration */
    for (int i = 0; i < ctx->svc_subscriber_count; i++) {
        if (strcmp(ctx->svc_subscribers[i].x121_number, expanded_num) == 0) {
            LOG_WARN("SVC Numbering: Number %s already registered to port %s",
                     expanded_num, ctx->svc_subscribers[i].port_name);
            return -1;
        }
    }

    if (ctx->svc_subscriber_count >= SVC_MAX_SUBSCRIBERS) {
        LOG_ERROR("SVC Numbering: Subscriber table full (max %d)", SVC_MAX_SUBSCRIBERS);
        return -1;
    }

    int idx = ctx->svc_subscriber_count;
    snprintf(ctx->svc_subscribers[idx].port_name, sizeof(ctx->svc_subscribers[idx].port_name), "%s", port_name);
    snprintf(ctx->svc_subscribers[idx].x121_number, sizeof(ctx->svc_subscribers[idx].x121_number), "%s", expanded_num);
    ctx->svc_subscribers[idx].reverse_charging_acceptance = (u8)rev_charge_acc;
    ctx->svc_subscribers[idx].reverse_charging_prevention = (u8)rev_charge_prev;
    ctx->svc_subscribers[idx].number_type = is_e164_num ? 2 : 1;
    ctx->svc_subscriber_count++;

    LOG_INFO("SVC Numbering: Registered subscriber '%s' on port %s (mode=%s, charge_acc=%d, charge_prev=%d)", 
             expanded_num, port_name, is_autoprefix ? "autoprefix" : "manual", rev_charge_acc, rev_charge_prev);

    /* Also update port's svc_ctx if port is already created */
    for (int i = 0; i < ctx->port_count; i++) {
        if (ctx->ports[i] && strcmp(ctx->ports[i]->name, port_name) == 0) {
            if (ctx->ports[i]->svc_ctx) {
                vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)ctx->ports[i]->svc_ctx;
                snprintf(sctx->subscriber_number, sizeof(sctx->subscriber_number), "%s", expanded_num);
                sctx->reverse_charging_acceptance = (u8)rev_charge_acc;
                sctx->reverse_charging_prevention = (u8)rev_charge_prev;
                if (num_type && strcasecmp(num_type, "e164") == 0) {
                    sctx->subscriber_type = 2;
                } else {
                    sctx->subscriber_type = 1;
                }

                if (raw_alias && raw_alias[0] != '\0') {
                    char expanded_alias[SVC_MAX_ADDR_LEN + 1];
                    svc_numbering_expand(ctx, raw_alias, expanded_alias, sizeof(expanded_alias));
                    snprintf(sctx->alias_number, sizeof(sctx->alias_number), "%s", expanded_alias);
                    if (alias_type && strcasecmp(alias_type, "e164") == 0) {
                        sctx->alias_type = 2;
                    } else {
                        sctx->alias_type = 1;
                    }

                    /* Also register alias in subscriber table */
                    if (ctx->svc_subscriber_count < SVC_MAX_SUBSCRIBERS) {
                        int aidx = ctx->svc_subscriber_count;
                        snprintf(ctx->svc_subscribers[aidx].port_name, sizeof(ctx->svc_subscribers[aidx].port_name), "%s", port_name);
                        snprintf(ctx->svc_subscribers[aidx].x121_number, sizeof(ctx->svc_subscribers[aidx].x121_number), "%s", expanded_alias);
                        ctx->svc_subscribers[aidx].reverse_charging_acceptance = (u8)rev_charge_acc;
                        ctx->svc_subscribers[aidx].reverse_charging_prevention = (u8)rev_charge_prev;
                        ctx->svc_subscribers[aidx].number_type = (alias_type && strcasecmp(alias_type, "e164") == 0) ? 2 : 1;
                        ctx->svc_subscriber_count++;
                        LOG_INFO("SVC Numbering: Registered alias subscriber '%s' on port %s", expanded_alias, port_name);
                    }
                }
            }
            break;
        }
    }

    return 0;
}

int svc_numbering_add(vfrs_ctx_t *ctx, const char *port_name,
                      const char *num_type, const char *raw_number,
                      const char *alias_type, const char *raw_alias,
                      int rev_charge_acc, int rev_charge_prev) {
    return svc_numbering_add_mode(ctx, port_name, "manual", num_type, raw_number,
                                 alias_type, raw_alias, rev_charge_acc, rev_charge_prev);
}

vfr_port_t *svc_find_subscriber(vfrs_ctx_t *ctx, const char *called_number) {
    if (!ctx || !called_number || called_number[0] == '\0') return NULL;

    char expanded[SVC_MAX_ADDR_LEN + 1];
    svc_numbering_expand(ctx, called_number, expanded, sizeof(expanded));

    /* Linear scan subscriber table */
    for (int i = 0; i < ctx->svc_subscriber_count; i++) {
        if (strcmp(ctx->svc_subscribers[i].x121_number, expanded) == 0) {
            /* Find matching port */
            for (int p = 0; p < ctx->port_count; p++) {
                if (ctx->ports[p] && strcmp(ctx->ports[p]->name, ctx->svc_subscribers[i].port_name) == 0) {
                    return ctx->ports[p];
                }
            }
        }
    }

    return NULL;
}

int svc_get_primary_number(vfrs_ctx_t *ctx, const char *number, char *primary_buf, size_t max_len) {
    if (!primary_buf || max_len == 0) return 0;
    primary_buf[0] = '\0';
    if (!ctx || !number || number[0] == '\0') return 0;

    char expanded[SVC_MAX_ADDR_LEN + 1];
    svc_numbering_expand(ctx, number, expanded, sizeof(expanded));

    vfr_port_t *port = svc_find_subscriber(ctx, expanded);
    if (port && port->svc_ctx) {
        vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
        if (sctx->subscriber_number[0] != '\0') {
            snprintf(primary_buf, max_len, "%s", sctx->subscriber_number);
            if (strcmp(expanded, sctx->subscriber_number) != 0) {
                LOG_INFO("SVC Numbering: Alias number '%s' translated to primary subscriber number '%s'",
                         expanded, sctx->subscriber_number);
                return 1; /* Translated from alias */
            }
            return 0; /* Already primary */
        }
    }

    snprintf(primary_buf, max_len, "%s", expanded);
    return 0;
}
