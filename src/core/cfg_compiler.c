/*
 * cfg_compiler.c - VFRS Configuration AST Semantic Compiler & Validator
 * Virtual Frame Relay Switch
 */

#include "vfr.h"
#include "vfr/config.h"
#include "vfr/cfg_lexer.h"
#include "vfr/cfg_ast.h"
#include "vfr/cfg_schema.h"
#include "vfr/ports.h"
#include "vfr/pvc.h"
#include "vfr/lapf.h"
#include "vfr/pcap.h"
#include "vfr/congestion.h"
#include "vfr/logger.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "switching/svc_routing_common.h"
#include "svc/svc_sig_common.h"
#include "svc/svc_spvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum log_level parse_log_level(const char *s)
{
    if (!s) return LOG_INFO;
    if (strcasecmp(s, "trace") == 0) return LOG_TRACE;
    if (strcasecmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(s, "info") == 0) return LOG_INFO;
    if (strcasecmp(s, "warn") == 0) return LOG_WARN;
    if (strcasecmp(s, "error") == 0) return LOG_ERROR;
    return LOG_INFO;
}

/* Helper to find a port in context */
static vfr_port_t *find_port(vfrs_ctx_t *ctx, const char *name)
{
    if (!ctx || !name) return NULL;
    return vfrs_find_port(ctx, name);
}

/* Compile swconfig node */
static int compile_swconfig(vfrs_ctx_t *ctx, const ast_swconfig_t *sw)
{
    if (!sw->configured) return 0;

    if (sw->swid[0] != '\0') {
        snprintf(ctx->swid, sizeof(ctx->swid), "%s", sw->swid);
    }

    if (sw->nspf == NSPF_X121) {
        if (sw->dnic[0] != '\0') {
            ctx->dnic = (u16)atoi(sw->dnic);
            ctx->dcc = (u16)(ctx->dnic / 10);
            ctx->nd = (u8)(ctx->dnic % 10);
        } else if (sw->dcc[0] != '\0') {
            ctx->dcc = (u16)atoi(sw->dcc);
            ctx->nd = (sw->nd[0] != '\0' && strcasecmp(sw->nd, "none") != 0) ? (u8)atoi(sw->nd) : 0;
            ctx->dnic = (u16)(ctx->dcc * 10 + ctx->nd);
        }

        if (sw->pnic[0] != '\0' && strcasecmp(sw->pnic, "none") != 0) {
            ctx->pnic = (u32)atoi(sw->pnic);
        } else {
            ctx->pnic = 0;
        }

        ctx->sgclen = (sw->sgclen >= 0) ? (u8)sw->sgclen : 0;
        ctx->sgc = (sw->sgc[0] != '\0') ? (u16)atoi(sw->sgc) : 0;

        ctx->siclen = (sw->siclen >= 0) ? (u8)sw->siclen : 0;
        ctx->sic = (sw->sic[0] != '\0') ? (u16)atoi(sw->sic) : 0;

        int prefix_len = 4 + (ctx->pnic > 0 ? (int)strlen(sw->pnic) : 0) +
                         (int)strlen(sw->x121_ind) + ctx->sgclen + ctx->siclen;

        if (sw->sublen > 0) {
            ctx->subnumlen = (u8)sw->sublen;
        } else {
            int auto_sub = 14 - prefix_len;
            ctx->subnumlen = (u8)(auto_sub > 0 ? auto_sub : 4);
        }

        if (prefix_len + ctx->subnumlen > 14) {
            LOG_WARN("Config: X.121 Total prefix length (%d) + sublen (%u) = %d exceeds 14-digit standard",
                     prefix_len, ctx->subnumlen, prefix_len + ctx->subnumlen);
        }
    } else if (sw->nspf == NSPF_E164) {
        u16 cc_val = (sw->cc[0] != '\0') ? (u16)atoi(sw->cc) : 62;
        ctx->dcc = cc_val;

        ctx->sgclen = (sw->sgclen >= 0) ? (u8)sw->sgclen : 0;
        ctx->sgc = (sw->sgc[0] != '\0') ? (u16)atoi(sw->sgc) : 0;

        ctx->siclen = (sw->siclen >= 0) ? (u8)sw->siclen : 0;
        ctx->sic = (sw->sic[0] != '\0') ? (u16)atoi(sw->sic) : 0;

        int prefix_len = (int)strlen(sw->cc) + (int)strlen(sw->ndc) +
                         (int)strlen(sw->ic) + (int)strlen(sw->area) +
                         (int)strlen(sw->e164_ind) + ctx->sgclen + ctx->siclen;

        if (sw->sublen > 0) {
            ctx->subnumlen = (u8)sw->sublen;
        } else {
            int auto_sub = 15 - prefix_len;
            ctx->subnumlen = (u8)(auto_sub > 0 ? auto_sub : 4);
        }

        if (prefix_len + ctx->subnumlen > 15) {
            LOG_WARN("Config: E.164 Total prefix length (%d) + sublen (%u) = %d exceeds 15-digit standard",
                     prefix_len, ctx->subnumlen, prefix_len + ctx->subnumlen);
        }
    }

    return 0;
}

/* Compile log settings */
static int compile_log(vfrs_ctx_t *ctx, const ast_log_t *log)
{
    (void)ctx;
    if (log->con_level[0] != '\0') {
        g_log_console = parse_log_level(log->con_level);
    }
    if (log->txt_level[0] != '\0') {
        g_log_file = parse_log_level(log->txt_level);
    }
    if (log->file_path[0] != '\0') {
        logger_reopen(log->file_path);
    }
    if (log->rot_size_mb > 0 || log->rot_files > 0) {
        logger_set_rotation(log->rot_size_mb * 1024 * 1024, log->rot_files);
    }
    return 0;
}

/* Compile port definitions */
static int compile_ports(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_ports; i++) {
        const ast_port_t *ap = &ast->ports[i];

        int port_type = (strncmp(ap->port_name, "nni", 3) == 0) ? PORT_TYPE_NNI : PORT_TYPE_UNI;
        int grp = 0, idx = 0;
        port_parse_name(ap->port_name, &port_type, &grp, &idx);

        vfr_port_t *p = NULL;
        if (strcasecmp(ap->transport, "pipe-client") == 0) {
            p = port_pipe_create(ap->port_name, port_type, ap->pipe_name, 0);
        } else if (strcasecmp(ap->transport, "pipe-server") == 0) {
            p = port_pipe_create(ap->port_name, port_type, ap->pipe_name, 1);
        } else if (strcasecmp(ap->transport, "serial") == 0) {
            p = port_serial_create(ap->port_name, port_type, ap->serial_dev, ap->baud);
        } else if (strcasecmp(ap->transport, "tcp") == 0) {
            const char *lh = ap->local_host[0] ? ap->local_host : "0.0.0.0";
            p = port_tcp_create(ap->port_name, port_type, lh, ap->local_port, ap->remote_host, ap->remote_port);
        } else if (strcasecmp(ap->transport, "tcp-client") == 0) {
            p = port_tcp_cli_create(ap->port_name, port_type, ap->remote_host, ap->remote_port);
        } else if (strcasecmp(ap->transport, "tcp-server") == 0) {
            const char *lh = ap->local_host[0] ? ap->local_host : "0.0.0.0";
            p = port_tcp_ser_create(ap->port_name, port_type, lh, ap->local_port);
        } else if (strcasecmp(ap->transport, "udp") == 0) {
            const char *lh = ap->local_host[0] ? ap->local_host : "0.0.0.0";
            p = port_udp_create(ap->port_name, port_type, lh, ap->local_port, ap->remote_host, ap->remote_port);
        } else if (strcasecmp(ap->transport, "udp-client") == 0) {
            p = port_udp_cli_create(ap->port_name, port_type, ap->remote_host, ap->remote_port);
        } else if (strcasecmp(ap->transport, "udp-server") == 0) {
            const char *lh = ap->local_host[0] ? ap->local_host : "0.0.0.0";
            p = port_udp_ser_create(ap->port_name, port_type, lh, ap->local_port);
        } else if (strcasecmp(ap->transport, "l2tpv3-fr") == 0 || strcasecmp(ap->transport, "l2tpv3fr") == 0) {
            const char *lh = ap->local_host[0] ? ap->local_host : "0.0.0.0";
            p = port_l2tpv3_create(ap->port_name, port_type, PORT_TRANS_L2TPV3_FR, lh, ap->remote_host, ap->vcid);
        } else if (strcasecmp(ap->transport, "l2tpv3-hdlc") == 0 || strcasecmp(ap->transport, "l2tpv3hdlc") == 0) {
            const char *lh = ap->local_host[0] ? ap->local_host : "0.0.0.0";
            p = port_l2tpv3_create(ap->port_name, port_type, PORT_TRANS_L2TPV3_HDLC, lh, ap->remote_host, ap->vcid);
        }

        if (!p) {
            LOG_ERROR("Config: Failed to create port '%s' on line %d", ap->port_name, ap->line);
            return -1;
        }

        p->dlcibit = ap->dlcibit;
        if (vfrs_add_port(ctx, p) < 0) {
            LOG_ERROR("Config: Failed to add port '%s' to switch context", ap->port_name);
            port_free(p);
            return -1;
        }
    }
    return 0;
}

/* Compile LMI settings */
static int compile_lmis(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_lmis; i++) {
        const ast_lmi_t *al = &ast->lmis[i];
        vfr_port_t *p = find_port(ctx, al->port);
        if (!p) {
            LOG_ERROR("Config: LMI references non-existent port '%s' on line %d", al->port, al->line);
            return -1;
        }

        if (strcasecmp(al->role, "dte") == 0) {
            lmi_dte_enable(p, al->n391, al->t391_s, al->n392, al->n393);
        } else {
            if (strcasecmp(al->standard, "ansi") == 0) {
                lmi_ansi_init(p, al->t392_s, al->n392, al->n393);
            } else if (strcasecmp(al->standard, "cisco") == 0) {
                lmi_gof_init(p, al->t392_s, al->n392, al->n393);
            } else if (strcasecmp(al->standard, "none") == 0) {
                /* None */
            } else {
                lmi_q933a_init(p, al->t392_s, al->n392, al->n393);
            }
        }
    }
    return 0;
}

/* Compile LAPF settings */
static int compile_lapfs(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_lapfs; i++) {
        const ast_lapf_t *al = &ast->lapfs[i];
        vfr_port_t *p = find_port(ctx, al->port);
        if (!p) {
            LOG_ERROR("Config: LAPF references non-existent port '%s' on line %d", al->port, al->line);
            return -1;
        }

        lapf_port_init(p, 0, al->k, al->n200, al->n201, al->t200_ms, al->t203_ms);
    }
    return 0;
}

/* Compile PVC connections */
static int compile_pvcs(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_pvcs; i++) {
        const ast_pvc_t *ap = &ast->pvcs[i];
        vfr_port_t *p1 = find_port(ctx, ap->port1);
        vfr_port_t *p2 = find_port(ctx, ap->port2);

        if (!p1 || !p2) {
            LOG_ERROR("Config: PVC on line %d references unknown port (%s or %s)",
                      ap->line, ap->port1, ap->port2);
            return -1;
        }

        u32 max_dlci1 = (p1->dlcibit == 23) ? 8388607 : 1023;
        u32 max_dlci2 = (p2->dlcibit == 23) ? 8388607 : 1023;

        if (ap->dlci1 < 16 || ap->dlci1 > max_dlci1 || ap->dlci2 < 16 || ap->dlci2 > max_dlci2) {
            LOG_ERROR("Config: PVC on line %d has invalid DLCI values (%u or %u)",
                      ap->line, ap->dlci1, ap->dlci2);
            return -1;
        }

        if (vfrs_add_pvc(ctx, ap->port1, ap->dlci1, ap->port2, ap->dlci2,
                         ap->cir, ap->bc, ap->be, ap->ftp, ap->fdp, ap->srv_class) < 0) {
            LOG_ERROR("Config: Failed to create PVC %s:%u <-> %s:%u",
                      ap->port1, ap->dlci1, ap->port2, ap->dlci2);
            return -1;
        }
    }

    return 0;
}

/* Compile SVC interfaces and addresses */
static int compile_svc(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    /* 1. Compile SVC Interfaces */
    for (int i = 0; i < ast->num_svc_ints; i++) {
        const ast_svc_int_t *si = &ast->svc_ints[i];
        vfr_port_t *p = find_port(ctx, si->port);
        if (!p) {
            LOG_ERROR("Config: svc int on line %d references unknown port '%s'", si->line, si->port);
            return -1;
        }

        u8 is_nni = (strncmp(si->port, "nni", 3) == 0);
        svc_init_port_ex(p, si->dlci_low, si->dlci_high, 0, is_nni, SVC_DLCI_ALLOC_ASCENDING);

        if (p->svc_ctx) {
            vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)p->svc_ctx;
            sctx->default_cir = si->cirdef;
            sctx->default_bc = si->bcdef;
            sctx->default_be = si->bedef;
            sctx->default_fmif = si->fmifdef;
            sctx->default_ftp = si->ftpdef;
            sctx->default_fdp = si->fdpdef;
            sctx->default_svc_class = si->clsdef;
            sctx->reverse_charging_acceptance = si->revchg_allow;
            sctx->reverse_charging_prevention = (si->revchg_allow == 0) ? 1 : 0;
        }
    }

    /* 2. Compile SVC Addresses */
    for (int i = 0; i < ast->num_svc_addrs; i++) {
        const ast_svc_addr_t *sa = &ast->svc_addrs[i];
        vfr_port_t *p = find_port(ctx, sa->port);
        if (!p) {
            LOG_ERROR("Config: svc addr on line %d references unknown port '%s'", sa->line, sa->port);
            return -1;
        }

        int revchg_acc = 1;
        int revchg_prev = 0;
        if (p->svc_ctx) {
            vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)p->svc_ctx;
            revchg_acc = sctx->reverse_charging_acceptance;
            revchg_prev = sctx->reverse_charging_prevention;
        }

        if (strcasecmp(sa->mode, "autonumber") == 0) {
            /* Autonumber sequential generator */
            int port_idx = 1;
            for (int pi = 0; pi < ctx->port_count; pi++) {
                if (ctx->ports[pi] == p) {
                    port_idx = pi + 1;
                    break;
                }
            }

            char sub_buf[32];
            int sublen = (ctx->subnumlen > 0 && ctx->subnumlen <= 15) ? (int)ctx->subnumlen : 4;
            if (sublen > 15) sublen = 15;
            if (sublen < 1) sublen = 1;
            snprintf(sub_buf, sizeof(sub_buf), "%0*u", sublen, (unsigned int)(port_idx % 1000000000u));

            svc_numbering_add_mode(ctx, sa->port, "autoprefix", sa->plan, sub_buf,
                                   NULL, NULL, revchg_acc, revchg_prev);
        } else {
            svc_numbering_add_mode(ctx, sa->port, sa->mode, sa->plan, sa->number_primary,
                                   sa->num_aliases > 0 ? sa->aliases[0].plan : NULL,
                                   sa->num_aliases > 0 ? sa->aliases[0].number : NULL,
                                   revchg_acc, revchg_prev);
        }
    }

    /* 3. Compile SVC Routes & Digit Trie */
    for (int i = 0; i < ast->num_svc_routes; i++) {
        const ast_svc_route_t *sr = &ast->svc_routes[i];
        if (strcasecmp(sr->mode, "prefix") == 0) {
            svc_route_add_ex(ctx, sr->prefix_regex, sr->port, NULL, (u8)sr->metric);
        } else if (strcasecmp(sr->mode, "x121") == 0) {
            char dnic_buf[16] = {0};
            if (sr->dnic[0] != '\0') strncpy(dnic_buf, sr->dnic, sizeof(dnic_buf) - 1);
            else if (sr->dcc[0] != '\0') snprintf(dnic_buf, sizeof(dnic_buf), "%s%s", sr->dcc, sr->nd[0] ? sr->nd : "0");
            else snprintf(dnic_buf, sizeof(dnic_buf), "%04u", ctx->dnic);

            svc_route_add_structural_ex(ctx, sr->port, dnic_buf,
                                        sr->sgc[0] ? sr->sgc : NULL,
                                        sr->sic[0] ? sr->sic : NULL,
                                        NULL, (u8)sr->metric);
        } else if (strcasecmp(sr->mode, "e164") == 0) {
            char cc_builder[32] = {0};
            snprintf(cc_builder, sizeof(cc_builder), "%s%s%s",
                     sr->cc[0] ? sr->cc : "",
                     sr->ndc[0] ? sr->ndc : (sr->ic[0] ? sr->ic : ""),
                     sr->area[0] ? sr->area : "");
            svc_route_add_ex(ctx, cc_builder, sr->port, NULL, (u8)sr->metric);
        }
    }

    return 0;
}

/* Compile capture definitions */
static int compile_captures(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_captures; i++) {
        const ast_capture_t *ac = &ast->captures[i];
        if (strcasecmp(ac->target, "all") == 0 || strcasecmp(ac->target, "global") == 0) {
            if (ctx->global_capture) {
                pcap_writer_close(ctx->global_capture);
                free(ctx->global_capture);
                ctx->global_capture = NULL;
            }
            ctx->global_capture = calloc(1, sizeof(pcap_writer_t));
            if (ctx->global_capture) {
                if (pcap_writer_init(ctx->global_capture, ac->pcap_file, 65535) < 0) {
                    LOG_ERROR("Config: Failed to initialize global PCAP capture file '%s'", ac->pcap_file);
                    free(ctx->global_capture);
                    ctx->global_capture = NULL;
                }
            }
        } else {
            vfr_port_t *p = find_port(ctx, ac->target);
            if (!p) {
                LOG_ERROR("Config: capture on line %d references unknown port '%s'", ac->line, ac->target);
                return -1;
            }
            if (p->capture) {
                pcap_writer_close(p->capture);
                free(p->capture);
                p->capture = NULL;
            }
            p->capture = calloc(1, sizeof(pcap_writer_t));
            if (p->capture) {
                if (pcap_writer_init(p->capture, ac->pcap_file, 65535) < 0) {
                    LOG_ERROR("Config: Failed to initialize PCAP capture file '%s' on port %s", ac->pcap_file, p->name);
                    free(p->capture);
                    p->capture = NULL;
                }
            }
        }
    }
    return 0;
}

/* Compile congestion settings */
static int compile_cgsts(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_cgsts; i++) {
        const ast_cgst_t *cg = &ast->cgsts[i];
        vfr_port_t *p = find_port(ctx, cg->port);
        if (!p) {
            LOG_ERROR("Config: cgst on line %d references unknown port '%s'", cg->line, cg->port);
            return -1;
        }
        if (p->cgst_ctx) {
            cgst_free(p);
        }
        u32 rate = cg->rate_fps ? cg->rate_fps : (ctx->access_rate ? ctx->access_rate : 1000);
        u32 thresh = cg->threshold ? cg->threshold : 1000;
        cgst_init(p, rate, thresh);
        if (cg->cllm_enabled) {
            cgst_set_cllm_enabled(p, 1);
            if (cg->cllm_txint_ms > 0) {
                cgst_set_cllm_tx_interval(p, cg->cllm_txint_ms);
            }
        }
    }
    return 0;
}

/* Copy global scoped defaults into switch context struct */
static void compile_defaults_to_ctx(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    if (!ctx || !ast) return;

    ctx->default_n391 = ast->defaults.lmi_dte_def.n391;
    ctx->default_t391 = ast->defaults.lmi_dte_def.t391_s;
    ctx->default_n392_dte = ast->defaults.lmi_dte_def.n392;
    ctx->default_n393_dte = ast->defaults.lmi_dte_def.n393;

    ctx->default_n392 = ast->defaults.lmi_dce_def.n392;
    ctx->default_n393 = ast->defaults.lmi_dce_def.n393;
    ctx->default_t392 = ast->defaults.lmi_dce_def.t392_s;

    ctx->default_lapf_k = ast->defaults.lapf_def.k;
    ctx->default_lapf_n200 = ast->defaults.lapf_def.n200;
    ctx->default_lapf_n201 = ast->defaults.lapf_def.n201;
    ctx->default_lapf_t200 = ast->defaults.lapf_def.t200_ms;
    ctx->default_lapf_t203 = ast->defaults.lapf_def.t203_ms;

    ctx->default_cir = ast->defaults.pvc_def.cir;
    ctx->default_bc = ast->defaults.pvc_def.bc;
    ctx->default_be = ast->defaults.pvc_def.be;
    ctx->access_rate = ast->defaults.interface_def.ar;

    ctx->default_svc_fmif = ast->defaults.svc_def.fmif;
    ctx->default_svc_ftp = ast->defaults.svc_def.ftp;
    ctx->default_svc_fdp = ast->defaults.svc_def.fdp;
    ctx->default_svc_class = ast->defaults.svc_def.srv_class;

    ctx->default_svc_t301 = ast->defaults.svc_uni_def.t301_ms;
    ctx->default_svc_t303 = ast->defaults.svc_uni_def.t303_ms;
    ctx->default_svc_t305 = ast->defaults.svc_uni_def.t305_ms;
    ctx->default_svc_t308 = ast->defaults.svc_uni_def.t308_ms;
    ctx->default_svc_t310 = ast->defaults.svc_uni_def.t310_ms;
    ctx->default_svc_t316 = ast->defaults.svc_uni_def.t316_ms;
    ctx->default_svc_t317 = ast->defaults.svc_uni_def.t317_ms;
    ctx->default_svc_t322 = ast->defaults.svc_uni_def.t322_ms;
}

/* Compile PVC multicast groups */
static int compile_pvc_mcasts(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_pvc_mcasts; i++) {
        const ast_pvc_mcast_t *mc = &ast->pvc_mcasts[i];
        if (mc->src_port[0] != '\0') {
            const char *mode = mc->mode[0] ? mc->mode : "oneway";
            if (vfrs_add_mcast_group(ctx, mc->name, mc->src_port, mc->src_dlci, mode, mc->cir, mc->bc, mc->be) < 0) {
                LOG_ERROR("Config: Failed to add multicast group '%s' on line %d", mc->name, mc->line);
                return -1;
            }
        }
        for (int m = 0; m < mc->num_members; m++) {
            if (vfrs_add_mcast_member(ctx, mc->name, mc->members[m].port, mc->members[m].dlci) < 0) {
                LOG_ERROR("Config: Failed to add member %s:%u to multicast group '%s'",
                          mc->members[m].port, mc->members[m].dlci, mc->name);
                return -1;
            }
        }
    }
    return 0;
}

/* Compile SPVC definitions */
static int compile_spvcs(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    for (int i = 0; i < ast->num_spvcs; i++) {
        const ast_spvc_t *sp = &ast->spvcs[i];
        if (strcasecmp(sp->target_type, "svc-to-pvc") == 0) {
            spvc_add_svc_to_pvc(ctx, sp->port1, sp->local_dlci, sp->dest_vfrs_number);
        } else if (strcasecmp(sp->target_type, "pvc-to-svc") == 0) {
            spvc_add_pvc_to_svc(ctx, sp->port1, sp->local_dlci, sp->dest_vfrs_number, sp->cir, sp->bc, sp->be);
        } else {
            spvc_add_inter_pvc(ctx, sp->port1, sp->local_dlci, sp->dest_vfrs_number, sp->tdlci, sp->cir, sp->bc, sp->be);
        }
    }
    return 0;
}

/* Unified Compilation Entry Point */
int cfg_compile_ast(vfrs_ctx_t *ctx, const cfg_ast_t *ast)
{
    if (!ctx || !ast) return -1;

    if (compile_swconfig(ctx, &ast->swconfig) < 0) return -1;
    if (compile_log(ctx, &ast->log) < 0) return -1;
    compile_defaults_to_ctx(ctx, ast);
    if (compile_ports(ctx, ast) < 0) return -1;
    if (compile_lmis(ctx, ast) < 0) return -1;
    if (compile_lapfs(ctx, ast) < 0) return -1;
    if (compile_pvcs(ctx, ast) < 0) return -1;
    if (compile_pvc_mcasts(ctx, ast) < 0) return -1;
    if (compile_svc(ctx, ast) < 0) return -1;
    if (compile_spvcs(ctx, ast) < 0) return -1;
    if (compile_cgsts(ctx, ast) < 0) return -1;
    if (compile_captures(ctx, ast) < 0) return -1;

    return 0;
}

int cfg_compile_file(vfrs_ctx_t *ctx, const char *filename)
{
    if (!ctx || !filename) return -1;

    cfg_lexer_t *lex = cfg_lexer_create_file(filename);
    if (!lex) {
        LOG_ERROR("Config: Cannot open configuration file '%s'", filename);
        return -1;
    }

    cfg_ast_t *ast = cfg_ast_create();
    if (!ast) {
        cfg_lexer_destroy(lex);
        return -1;
    }

    if (cfg_parse_tokens(lex, ast) < 0 || ast->has_error) {
        LOG_ERROR("Config Syntax Error: %s", ast->err_msg);
        cfg_ast_destroy(ast);
        cfg_lexer_destroy(lex);
        return -1;
    }

    int rc = cfg_compile_ast(ctx, ast);

    cfg_ast_destroy(ast);
    cfg_lexer_destroy(lex);
    return rc;
}

int cfg_compile_string(vfrs_ctx_t *ctx, const char *source, const char *sourcename)
{
    if (!ctx || !source) return -1;

    cfg_lexer_t *lex = cfg_lexer_create_string(source, sourcename);
    if (!lex) return -1;

    cfg_ast_t *ast = cfg_ast_create();
    if (!ast) {
        cfg_lexer_destroy(lex);
        return -1;
    }

    if (cfg_parse_tokens(lex, ast) < 0 || ast->has_error) {
        LOG_ERROR("Config Syntax Error: %s", ast->err_msg);
        cfg_ast_destroy(ast);
        cfg_lexer_destroy(lex);
        return -1;
    }

    int rc = cfg_compile_ast(ctx, ast);

    cfg_ast_destroy(ast);
    cfg_lexer_destroy(lex);
    return rc;
}

int cfg_validate_file(const char *filename, char *err_msg, size_t err_len)
{
    if (!filename) return -1;

    cfg_lexer_t *lex = cfg_lexer_create_file(filename);
    if (!lex) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "Cannot open configuration file '%s'", filename);
        }
        return -1;
    }

    cfg_ast_t *ast = cfg_ast_create();
    if (!ast) {
        cfg_lexer_destroy(lex);
        return -1;
    }

    if (cfg_parse_tokens(lex, ast) < 0 || ast->has_error) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "%s", ast->err_msg);
        }
        cfg_ast_destroy(ast);
        cfg_lexer_destroy(lex);
        return -1;
    }

    /* Dry validation: compile into dummy context */
    vfrs_ctx_t *dummy_ctx = calloc(1, sizeof(vfrs_ctx_t));
    if (!dummy_ctx) {
        cfg_ast_destroy(ast);
        cfg_lexer_destroy(lex);
        return -1;
    }

    svc_numbering_init(dummy_ctx);
    svc_route_init(dummy_ctx);

    int rc = cfg_compile_ast(dummy_ctx, ast);
    if (rc < 0 && err_msg && err_len > 0) {
        snprintf(err_msg, err_len, "Semantic validation failed");
    }

    /* Clean up dummy context */
    for (int i = 0; i < dummy_ctx->port_count; i++) {
        if (dummy_ctx->ports[i]) {
            if (dummy_ctx->ports[i]->svc_ctx) free(dummy_ctx->ports[i]->svc_ctx);
            free(dummy_ctx->ports[i]);
        }
    }
    free(dummy_ctx);

    cfg_ast_destroy(ast);
    cfg_lexer_destroy(lex);
    return rc;
}
