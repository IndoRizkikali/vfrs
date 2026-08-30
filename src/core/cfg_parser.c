/*
 * cfg_parser.c - VFRS Recursive Descent AST Parser
 * Virtual Frame Relay Switch
 */

#include "vfr/cfg_ast.h"
#include "vfr/cfg_lexer.h"
#include "vfr/cfg_schema.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cfg_ast_t *cfg_ast_create(void)
{
    cfg_ast_t *ast = calloc(1, sizeof(cfg_ast_t));
    if (!ast) return NULL;
    cfg_scoped_defaults_init(&ast->defaults);
    return ast;
}

void cfg_ast_destroy(cfg_ast_t *ast)
{
    if (ast) free(ast);
}

static void set_ast_error(cfg_ast_t *ast, int line, const char *msg)
{
    if (!ast || ast->has_error) return;
    ast->has_error = 1;
    ast->err_line = line;
    snprintf(ast->err_msg, sizeof(ast->err_msg), "Line %d: %s", line, msg);
}


/* Helper to parse swconfig statement */
static int parse_stmt_swconfig(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    ast_swconfig_t *sw = &ast->swconfig;
    sw->configured = 1;
    sw->sgclen = -1;
    sw->siclen = -1;
    sw->sublen = -1;

    for (int i = 1; i < n; i++) {
        if (toks[i].type != TOK_KEYVAL) continue;
        const char *k = toks[i].key;
        const char *v = toks[i].val;

        if (strcasecmp(k, "swid") == 0) {
            strncpy(sw->swid, v, sizeof(sw->swid) - 1);
        } else if (strcasecmp(k, "nspf") == 0) {
            if (strcasecmp(v, "x121") == 0) sw->nspf = NSPF_X121;
            else if (strcasecmp(v, "e164") == 0) sw->nspf = NSPF_E164;
            else {
                set_ast_error(ast, toks[i].line, "Invalid nspf (must be x121 or e164)");
                return -1;
            }
        } else if (strcasecmp(k, "dcc") == 0) {
            strncpy(sw->dcc, v, sizeof(sw->dcc) - 1);
        } else if (strcasecmp(k, "nd") == 0) {
            if (strcasecmp(v, "none") != 0) {
                strncpy(sw->nd, v, sizeof(sw->nd) - 1);
            }
        } else if (strcasecmp(k, "dnic") == 0) {
            strncpy(sw->dnic, v, sizeof(sw->dnic) - 1);
        } else if (strcasecmp(k, "pnic") == 0) {
            if (strcasecmp(v, "none") != 0) {
                strncpy(sw->pnic, v, sizeof(sw->pnic) - 1);
            }
        } else if (strcasecmp(k, "ind") == 0) {
            strncpy(sw->x121_ind, v, sizeof(sw->x121_ind) - 1);
            strncpy(sw->e164_ind, v, sizeof(sw->e164_ind) - 1);
        } else if (strcasecmp(k, "sgclen") == 0) {
            if (strcasecmp(v, "none") == 0) sw->sgclen = -1;
            else sw->sgclen = atoi(v);
        } else if (strcasecmp(k, "sgc") == 0) {
            strncpy(sw->sgc, v, sizeof(sw->sgc) - 1);
        } else if (strcasecmp(k, "siclen") == 0) {
            if (strcasecmp(v, "none") == 0) sw->siclen = -1;
            else sw->siclen = atoi(v);
        } else if (strcasecmp(k, "sic") == 0) {
            strncpy(sw->sic, v, sizeof(sw->sic) - 1);
        } else if (strcasecmp(k, "sublen") == 0 || strcasecmp(k, "subnumlen") == 0) {
            if (strcasecmp(v, "auto") == 0 || strcasecmp(v, "none") == 0) sw->sublen = -1;
            else sw->sublen = atoi(v);
        } else if (strcasecmp(k, "cc") == 0) {
            strncpy(sw->cc, v, sizeof(sw->cc) - 1);
        } else if (strcasecmp(k, "ic") == 0) {
            strncpy(sw->ic, v, sizeof(sw->ic) - 1);
        } else if (strcasecmp(k, "ndc") == 0) {
            strncpy(sw->ndc, v, sizeof(sw->ndc) - 1);
        } else if (strcasecmp(k, "area") == 0) {
            strncpy(sw->area, v, sizeof(sw->area) - 1);
        }
    }
    return 0;
}

/* Parse log subcommands: log level, log file, log rotation */
static int parse_stmt_log(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 2) return 0;
    const char *sub = toks[1].text;

    if (strcasecmp(sub, "level") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type == TOK_KEYVAL) {
                if (strcasecmp(toks[i].key, "con") == 0) {
                    strncpy(ast->log.con_level, toks[i].val, sizeof(ast->log.con_level) - 1);
                } else if (strcasecmp(toks[i].key, "txt") == 0) {
                    strncpy(ast->log.txt_level, toks[i].val, sizeof(ast->log.txt_level) - 1);
                }
            }
        }
    } else if (strcasecmp(sub, "file") == 0 && n >= 3) {
        strncpy(ast->log.file_path, toks[2].text, sizeof(ast->log.file_path) - 1);
    } else if (strcasecmp(sub, "rotation") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type == TOK_KEYVAL) {
                if (strcasecmp(toks[i].key, "size") == 0) {
                    cfg_parse_u32(toks[i].val, 1, 1024, &ast->log.rot_size_mb);
                } else if (strcasecmp(toks[i].key, "files") == 0) {
                    cfg_parse_u32(toks[i].val, 1, 100, &ast->log.rot_files);
                }
            }
        }
    }
    return 0;
}

/* Parse default <scope> statements */
static int parse_stmt_default(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 2) return 0;
    const char *scope = toks[1].text;
    vfr_scoped_defaults_t *d = &ast->defaults;

    if (strcasecmp(scope, "interface") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "dlcibit") == 0) {
                cfg_parse_u8(toks[i].val, 10, 23, &d->interface_def.dlcibit);
                d->interface_def.set_dlcibit = 1;
            } else if (strcasecmp(toks[i].key, "ar") == 0) {
                cfg_parse_rate(toks[i].val, &d->interface_def.ar);
                d->interface_def.set_ar = 1;
            }
        }
    } else if (strcasecmp(scope, "lapf") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "k") == 0) {
                cfg_parse_u8(toks[i].val, 1, 127, &d->lapf_def.k);
                d->lapf_def.set_k = 1;
            } else if (strcasecmp(toks[i].key, "n200") == 0) {
                cfg_parse_u8(toks[i].val, 1, 10, &d->lapf_def.n200);
                d->lapf_def.set_n200 = 1;
            } else if (strcasecmp(toks[i].key, "n201") == 0) {
                cfg_parse_u16(toks[i].val, 128, 8192, &d->lapf_def.n201);
                d->lapf_def.set_n201 = 1;
            } else if (strcasecmp(toks[i].key, "t200") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->lapf_def.t200_ms);
                d->lapf_def.set_t200 = 1;
            } else if (strcasecmp(toks[i].key, "t203") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->lapf_def.t203_ms);
                d->lapf_def.set_t203 = 1;
            }
        }
    } else if (strcasecmp(scope, "lmi-dce") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "n392") == 0) {
                cfg_parse_u8(toks[i].val, 1, 10, &d->lmi_dce_def.n392);
                d->lmi_dce_def.set_n392 = 1;
            } else if (strcasecmp(toks[i].key, "n393") == 0) {
                cfg_parse_u8(toks[i].val, 1, 10, &d->lmi_dce_def.n393);
                d->lmi_dce_def.set_n393 = 1;
            } else if (strcasecmp(toks[i].key, "t392") == 0) {
                cfg_parse_time_s(toks[i].val, &d->lmi_dce_def.t392_s);
                d->lmi_dce_def.set_t392 = 1;
            }
        }
    } else if (strcasecmp(scope, "lmi-dte") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "n391") == 0) {
                cfg_parse_u8(toks[i].val, 1, 255, &d->lmi_dte_def.n391);
                d->lmi_dte_def.set_n391 = 1;
            } else if (strcasecmp(toks[i].key, "n392") == 0) {
                cfg_parse_u8(toks[i].val, 1, 10, &d->lmi_dte_def.n392);
                d->lmi_dte_def.set_n392 = 1;
            } else if (strcasecmp(toks[i].key, "n393") == 0) {
                cfg_parse_u8(toks[i].val, 1, 10, &d->lmi_dte_def.n393);
                d->lmi_dte_def.set_n393 = 1;
            } else if (strcasecmp(toks[i].key, "t391") == 0) {
                cfg_parse_time_s(toks[i].val, &d->lmi_dte_def.t391_s);
                d->lmi_dte_def.set_t391 = 1;
            }
        }
    } else if (strcasecmp(scope, "pvc") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "cir") == 0) {
                cfg_parse_rate(toks[i].val, &d->pvc_def.cir);
                d->pvc_def.set_cir = 1;
            } else if (strcasecmp(toks[i].key, "bc") == 0) {
                cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &d->pvc_def.bc);
                d->pvc_def.set_bc = 1;
            } else if (strcasecmp(toks[i].key, "be") == 0) {
                cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &d->pvc_def.be);
                d->pvc_def.set_be = 1;
            } else if (strcasecmp(toks[i].key, "ftp") == 0) {
                cfg_parse_u8(toks[i].val, 0, 15, &d->pvc_def.ftp);
                d->pvc_def.set_ftp = 1;
            } else if (strcasecmp(toks[i].key, "fdp") == 0) {
                cfg_parse_u8(toks[i].val, 0, 7, &d->pvc_def.fdp);
                d->pvc_def.set_fdp = 1;
            } else if (strcasecmp(toks[i].key, "class") == 0) {
                cfg_parse_u8(toks[i].val, 0, 3, &d->pvc_def.srv_class);
                d->pvc_def.set_srv_class = 1;
            }
        }
    } else if (strcasecmp(scope, "svc") == 0) {
        for (int i = 2; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "cir") == 0) {
                cfg_parse_rate(toks[i].val, &d->svc_def.cir);
                d->svc_def.set_cir = 1;
            } else if (strcasecmp(toks[i].key, "bc") == 0) {
                cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &d->svc_def.bc);
                d->svc_def.set_bc = 1;
            } else if (strcasecmp(toks[i].key, "be") == 0) {
                cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &d->svc_def.be);
                d->svc_def.set_be = 1;
            } else if (strcasecmp(toks[i].key, "fmif") == 0) {
                cfg_parse_u16(toks[i].val, 128, 8192, &d->svc_def.fmif);
                d->svc_def.set_fmif = 1;
            } else if (strcasecmp(toks[i].key, "ftp") == 0) {
                cfg_parse_u8(toks[i].val, 0, 15, &d->svc_def.ftp);
                d->svc_def.set_ftp = 1;
            } else if (strcasecmp(toks[i].key, "fdp") == 0) {
                cfg_parse_u8(toks[i].val, 0, 7, &d->svc_def.fdp);
                d->svc_def.set_fdp = 1;
            } else if (strcasecmp(toks[i].key, "class") == 0) {
                cfg_parse_u8(toks[i].val, 0, 3, &d->svc_def.srv_class);
                d->svc_def.set_srv_class = 1;
            } else if (strcasecmp(toks[i].key, "revchg") == 0) {
                int b = 0;
                cfg_parse_bool(toks[i].val, &b);
                d->svc_def.revchg_allow = (u8)b;
                d->svc_def.set_revchg = 1;
            } else if (strcasecmp(toks[i].key, "ogrevchg") == 0) {
                int b = 0;
                cfg_parse_bool(toks[i].val, &b);
                d->svc_def.ogrevchg_allow = (u8)b;
                d->svc_def.set_ogrevchg = 1;
            } else if (strcasecmp(toks[i].key, "icrevchg") == 0) {
                int b = 0;
                cfg_parse_bool(toks[i].val, &b);
                d->svc_def.icrevchg_allow = (u8)b;
                d->svc_def.set_icrevchg = 1;
            }
        }
    } else if (strcasecmp(scope, "svc-uni") == 0 || (strcasecmp(scope, "svc") == 0 && n >= 3 && strcasecmp(toks[2].text, "uni") == 0)) {
        int st = (strcasecmp(scope, "svc-uni") == 0) ? 2 : 3;
        for (int i = st; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "t301") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t301_ms);
                d->svc_uni_def.set_t301 = 1;
            } else if (strcasecmp(toks[i].key, "t303") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t303_ms);
                d->svc_uni_def.set_t303 = 1;
            } else if (strcasecmp(toks[i].key, "t305") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t305_ms);
                d->svc_uni_def.set_t305 = 1;
            } else if (strcasecmp(toks[i].key, "t308") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t308_ms);
                d->svc_uni_def.set_t308 = 1;
            } else if (strcasecmp(toks[i].key, "t310") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t310_ms);
                d->svc_uni_def.set_t310 = 1;
            } else if (strcasecmp(toks[i].key, "t316") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t316_ms);
                d->svc_uni_def.set_t316 = 1;
            } else if (strcasecmp(toks[i].key, "t317") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t317_ms);
                d->svc_uni_def.set_t317 = 1;
            } else if (strcasecmp(toks[i].key, "t322") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_uni_def.t322_ms);
                d->svc_uni_def.set_t322 = 1;
            }
        }
    } else if (strcasecmp(scope, "svc-nni") == 0 || (strcasecmp(scope, "svc") == 0 && n >= 3 && strcasecmp(toks[2].text, "nni") == 0)) {
        int st = (strcasecmp(scope, "svc-nni") == 0) ? 2 : 3;
        for (int i = st; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "t301") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t301_ms);
                d->svc_nni_def.set_t301 = 1;
            } else if (strcasecmp(toks[i].key, "t303") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t303_ms);
                d->svc_nni_def.set_t303 = 1;
            } else if (strcasecmp(toks[i].key, "t305") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t305_ms);
                d->svc_nni_def.set_t305 = 1;
            } else if (strcasecmp(toks[i].key, "t308") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t308_ms);
                d->svc_nni_def.set_t308 = 1;
            } else if (strcasecmp(toks[i].key, "t310") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t310_ms);
                d->svc_nni_def.set_t310 = 1;
            } else if (strcasecmp(toks[i].key, "t316") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t316_ms);
                d->svc_nni_def.set_t316 = 1;
            } else if (strcasecmp(toks[i].key, "t317") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t317_ms);
                d->svc_nni_def.set_t317 = 1;
            } else if (strcasecmp(toks[i].key, "t322") == 0) {
                cfg_parse_time_ms(toks[i].val, &d->svc_nni_def.t322_ms);
                d->svc_nni_def.set_t322 = 1;
            }
        }
    }
    return 0;
}

/* Parse port statement */
static int parse_stmt_port(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 3) {
        set_ast_error(ast, toks[0].line, "Incomplete port definition");
        return -1;
    }
    if (ast->num_ports >= CFG_MAX_PORTS) {
        set_ast_error(ast, toks[0].line, "Exceeded maximum ports limit");
        return -1;
    }

    ast_port_t *p = &ast->ports[ast->num_ports++];
    p->line = toks[0].line;
    strncpy(p->port_name, toks[1].text, sizeof(p->port_name) - 1);
    strncpy(p->transport, toks[2].text, sizeof(p->transport) - 1);
    p->dlcibit = ast->defaults.interface_def.dlcibit;
    p->ar = ast->defaults.interface_def.ar;

    int idx = 3;
    if (strcasecmp(p->transport, "pipe-client") == 0 || strcasecmp(p->transport, "pipe-server") == 0) {
        if (idx < n && toks[idx].type == TOK_WORD) {
            strncpy(p->pipe_name, toks[idx++].text, sizeof(p->pipe_name) - 1);
        }
    } else if (strcasecmp(p->transport, "serial") == 0) {
        if (idx < n && toks[idx].type == TOK_WORD) {
            strncpy(p->serial_dev, toks[idx++].text, sizeof(p->serial_dev) - 1);
        }
    } else if (strcasecmp(p->transport, "tcp") == 0 || strcasecmp(p->transport, "udp") == 0) {
        if (idx + 3 < n && toks[idx].type == TOK_WORD && toks[idx+1].type == TOK_WORD &&
            toks[idx+2].type == TOK_WORD && toks[idx+3].type == TOK_WORD) {
            strncpy(p->local_host, toks[idx++].text, sizeof(p->local_host) - 1);
            p->local_port = (u16)atoi(toks[idx++].text);
            strncpy(p->remote_host, toks[idx++].text, sizeof(p->remote_host) - 1);
            p->remote_port = (u16)atoi(toks[idx++].text);
        }
    } else if (strcasecmp(p->transport, "tcp-client") == 0 || strcasecmp(p->transport, "udp-client") == 0) {
        if (idx + 1 < n && toks[idx].type == TOK_WORD && toks[idx+1].type == TOK_WORD) {
            strncpy(p->remote_host, toks[idx++].text, sizeof(p->remote_host) - 1);
            p->remote_port = (u16)atoi(toks[idx++].text);
        }
    } else if (strcasecmp(p->transport, "tcp-server") == 0 || strcasecmp(p->transport, "udp-server") == 0) {
        if (idx < n && toks[idx].type == TOK_WORD) {
            if (idx + 1 < n && toks[idx+1].type == TOK_WORD) {
                strncpy(p->local_host, toks[idx++].text, sizeof(p->local_host) - 1);
                p->local_port = (u16)atoi(toks[idx++].text);
            } else {
                p->local_port = (u16)atoi(toks[idx++].text);
            }
        }
    }

    /* Key-value options */
    for (int i = idx; i < n; i++) {
        if (toks[i].type != TOK_KEYVAL) continue;
        if (strcasecmp(toks[i].key, "dlcibit") == 0) {
            cfg_parse_u8(toks[i].val, 10, 23, &p->dlcibit);
        } else if (strcasecmp(toks[i].key, "ar") == 0) {
            cfg_parse_rate(toks[i].val, &p->ar);
        } else if (strcasecmp(toks[i].key, "baud") == 0) {
            cfg_parse_u32(toks[i].val, 300, 10000000, &p->baud);
        } else if (strcasecmp(toks[i].key, "vcid") == 0) {
            cfg_parse_u32(toks[i].val, 1, 0xFFFFFFFF, &p->vcid);
        }
    }

    return 0;
}

/* Parse pvc statement */
static int parse_stmt_pvc(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    /* Check for pvc mcast subcommands */
    if (n >= 2 && strcasecmp(toks[1].text, "mcast") == 0) {
        if (n >= 3 && strcasecmp(toks[2].text, "group") == 0) {
            /* pvc mcast group <name> <port> <dlci> [mode] */
            if (n < 6) return -1;
            const char *grp_name = toks[3].text;
            ast_pvc_mcast_t *mc = NULL;
            for (int i = 0; i < ast->num_pvc_mcasts; i++) {
                if (strcasecmp(ast->pvc_mcasts[i].name, grp_name) == 0) {
                    mc = &ast->pvc_mcasts[i];
                    break;
                }
            }
            if (!mc && ast->num_pvc_mcasts < CFG_MAX_MCAST_GROUPS) {
                mc = &ast->pvc_mcasts[ast->num_pvc_mcasts++];
                strncpy(mc->name, grp_name, sizeof(mc->name) - 1);
            }
            if (mc) {
                strncpy(mc->src_port, toks[4].text, sizeof(mc->src_port) - 1);
                mc->src_dlci = (u32)atoi(toks[5].text);
                if (n >= 7) strncpy(mc->mode, toks[6].text, sizeof(mc->mode) - 1);
            }
            return 0;
        } else if (n >= 3 && strcasecmp(toks[2].text, "member") == 0) {
            /* pvc mcast member <name> <port> <dlci> [options] */
            if (n < 6) return -1;
            const char *grp_name = toks[3].text;
            ast_pvc_mcast_t *mc = NULL;
            for (int i = 0; i < ast->num_pvc_mcasts; i++) {
                if (strcasecmp(ast->pvc_mcasts[i].name, grp_name) == 0) {
                    mc = &ast->pvc_mcasts[i];
                    break;
                }
            }
            if (!mc && ast->num_pvc_mcasts < CFG_MAX_MCAST_GROUPS) {
                mc = &ast->pvc_mcasts[ast->num_pvc_mcasts++];
                strncpy(mc->name, grp_name, sizeof(mc->name) - 1);
            }
            if (mc && mc->num_members < CFG_MAX_MCAST_MEMBERS) {
                int mi = mc->num_members++;
                strncpy(mc->members[mi].port, toks[4].text, sizeof(mc->members[mi].port) - 1);
                mc->members[mi].dlci = (u32)atoi(toks[5].text);
                mc->members[mi].cir = ast->defaults.pvc_def.cir;
                mc->members[mi].bc = ast->defaults.pvc_def.bc;
                mc->members[mi].be = ast->defaults.pvc_def.be;
                mc->members[mi].ftp = ast->defaults.pvc_def.ftp;
                mc->members[mi].fdp = ast->defaults.pvc_def.fdp;
                mc->members[mi].srv_class = ast->defaults.pvc_def.srv_class;

                for (int i = 6; i < n; i++) {
                    if (toks[i].type != TOK_KEYVAL) continue;
                    if (strcasecmp(toks[i].key, "cir") == 0) cfg_parse_rate(toks[i].val, &mc->members[mi].cir);
                    else if (strcasecmp(toks[i].key, "bc") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &mc->members[mi].bc);
                    else if (strcasecmp(toks[i].key, "be") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &mc->members[mi].be);
                    else if (strcasecmp(toks[i].key, "ftp") == 0) cfg_parse_u8(toks[i].val, 0, 15, &mc->members[mi].ftp);
                    else if (strcasecmp(toks[i].key, "fdp") == 0) cfg_parse_u8(toks[i].val, 0, 7, &mc->members[mi].fdp);
                    else if (strcasecmp(toks[i].key, "class") == 0) cfg_parse_u8(toks[i].val, 0, 3, &mc->members[mi].srv_class);
                }
            }
            return 0;
        } else {
            /* pvc mcast <name> [options] */
            if (n < 3) return -1;
            const char *grp_name = toks[2].text;
            ast_pvc_mcast_t *mc = NULL;
            for (int i = 0; i < ast->num_pvc_mcasts; i++) {
                if (strcasecmp(ast->pvc_mcasts[i].name, grp_name) == 0) {
                    mc = &ast->pvc_mcasts[i];
                    break;
                }
            }
            if (!mc && ast->num_pvc_mcasts < CFG_MAX_MCAST_GROUPS) {
                mc = &ast->pvc_mcasts[ast->num_pvc_mcasts++];
                strncpy(mc->name, grp_name, sizeof(mc->name) - 1);
            }
            if (mc) {
                mc->cir = ast->defaults.pvc_def.cir;
                mc->bc = ast->defaults.pvc_def.bc;
                mc->be = ast->defaults.pvc_def.be;
                mc->ftp = ast->defaults.pvc_def.ftp;
                mc->fdp = ast->defaults.pvc_def.fdp;
                mc->srv_class = ast->defaults.pvc_def.srv_class;

                for (int i = 3; i < n; i++) {
                    if (toks[i].type != TOK_KEYVAL) continue;
                    if (strcasecmp(toks[i].key, "cir") == 0) cfg_parse_rate(toks[i].val, &mc->cir);
                    else if (strcasecmp(toks[i].key, "bc") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &mc->bc);
                    else if (strcasecmp(toks[i].key, "be") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &mc->be);
                    else if (strcasecmp(toks[i].key, "ftp") == 0) cfg_parse_u8(toks[i].val, 0, 15, &mc->ftp);
                    else if (strcasecmp(toks[i].key, "fdp") == 0) cfg_parse_u8(toks[i].val, 0, 7, &mc->fdp);
                    else if (strcasecmp(toks[i].key, "class") == 0) cfg_parse_u8(toks[i].val, 0, 3, &mc->srv_class);
                }
            }
            return 0;
        }
    }

    /* Standard Point-to-Point PVC: pvc <p1> <d1> <p2> <d2> [options] */
    if (n < 5) {
        set_ast_error(ast, toks[0].line, "Incomplete pvc definition (expected pvc <p1> <d1> <p2> <d2>)");
        return -1;
    }
    if (ast->num_pvcs >= CFG_MAX_PVCS) {
        set_ast_error(ast, toks[0].line, "Exceeded maximum PVCs limit");
        return -1;
    }

    ast_pvc_t *pvc = &ast->pvcs[ast->num_pvcs++];
    pvc->line = toks[0].line;
    strncpy(pvc->port1, toks[1].text, sizeof(pvc->port1) - 1);
    pvc->dlci1 = (u32)atoi(toks[2].text);
    strncpy(pvc->port2, toks[3].text, sizeof(pvc->port2) - 1);
    pvc->dlci2 = (u32)atoi(toks[4].text);

    /* Inherit scoped defaults */
    pvc->cir = ast->defaults.pvc_def.cir;
    pvc->bc = ast->defaults.pvc_def.bc;
    pvc->be = ast->defaults.pvc_def.be;
    pvc->ftp = ast->defaults.pvc_def.ftp;
    pvc->fdp = ast->defaults.pvc_def.fdp;
    pvc->srv_class = ast->defaults.pvc_def.srv_class;

    for (int i = 5; i < n; i++) {
        if (toks[i].type != TOK_KEYVAL) continue;
        if (strcasecmp(toks[i].key, "cir") == 0) cfg_parse_rate(toks[i].val, &pvc->cir);
        else if (strcasecmp(toks[i].key, "bc") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &pvc->bc);
        else if (strcasecmp(toks[i].key, "be") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &pvc->be);
        else if (strcasecmp(toks[i].key, "ftp") == 0) cfg_parse_u8(toks[i].val, 0, 15, &pvc->ftp);
        else if (strcasecmp(toks[i].key, "fdp") == 0) cfg_parse_u8(toks[i].val, 0, 7, &pvc->fdp);
        else if (strcasecmp(toks[i].key, "class") == 0) cfg_parse_u8(toks[i].val, 0, 3, &pvc->srv_class);
    }

    return 0;
}

/* Parse lmi statement */
static int parse_stmt_lmi(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 2) return -1;
    if (ast->num_lmis >= CFG_MAX_PORTS) return -1;

    ast_lmi_t *lmi = &ast->lmis[ast->num_lmis++];
    lmi->line = toks[0].line;
    strncpy(lmi->port, toks[1].text, sizeof(lmi->port) - 1);
    strcpy(lmi->role, "dce");
    strcpy(lmi->standard, "q933a");
    lmi->t392_s = ast->defaults.lmi_dce_def.t392_s;
    lmi->n392 = ast->defaults.lmi_dce_def.n392;
    lmi->n393 = ast->defaults.lmi_dce_def.n393;

    for (int i = 2; i < n; i++) {
        if (toks[i].type == TOK_WORD) {
            if (strcasecmp(toks[i].text, "dte") == 0) {
                strcpy(lmi->role, "dte");
                lmi->t391_s = ast->defaults.lmi_dte_def.t391_s;
                lmi->n391 = ast->defaults.lmi_dte_def.n391;
                lmi->n392 = ast->defaults.lmi_dte_def.n392;
                lmi->n393 = ast->defaults.lmi_dte_def.n393;
            } else if (strcasecmp(toks[i].text, "dce") == 0) {
                strcpy(lmi->role, "dce");
            } else if (strcasecmp(toks[i].text, "ansi") == 0 ||
                       strcasecmp(toks[i].text, "cisco") == 0 ||
                       strcasecmp(toks[i].text, "q933a") == 0 ||
                       strcasecmp(toks[i].text, "none") == 0) {
                strncpy(lmi->standard, toks[i].text, sizeof(lmi->standard) - 1);
            }
        } else if (toks[i].type == TOK_KEYVAL) {
            if (strcasecmp(toks[i].key, "t391") == 0) cfg_parse_time_s(toks[i].val, &lmi->t391_s);
            else if (strcasecmp(toks[i].key, "t392") == 0) cfg_parse_time_s(toks[i].val, &lmi->t392_s);
            else if (strcasecmp(toks[i].key, "n391") == 0) cfg_parse_u8(toks[i].val, 1, 255, &lmi->n391);
            else if (strcasecmp(toks[i].key, "n392") == 0) cfg_parse_u8(toks[i].val, 1, 10, &lmi->n392);
            else if (strcasecmp(toks[i].key, "n393") == 0) cfg_parse_u8(toks[i].val, 1, 10, &lmi->n393);
            else if (strcasecmp(toks[i].key, "async") == 0) cfg_parse_bool(toks[i].val, &lmi->async_enabled);
        }
    }
    return 0;
}

/* Parse lapf statement */
static int parse_stmt_lapf(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 2) return -1;
    if (ast->num_lapfs >= CFG_MAX_PORTS) return -1;

    ast_lapf_t *lapf = &ast->lapfs[ast->num_lapfs++];
    lapf->line = toks[0].line;
    strncpy(lapf->port, toks[1].text, sizeof(lapf->port) - 1);
    strcpy(lapf->sabme, "bidirectional");
    strcpy(lapf->xid, "active");
    lapf->k = ast->defaults.lapf_def.k;
    lapf->n200 = ast->defaults.lapf_def.n200;
    lapf->n201 = ast->defaults.lapf_def.n201;
    lapf->t200_ms = ast->defaults.lapf_def.t200_ms;
    lapf->t203_ms = ast->defaults.lapf_def.t203_ms;

    for (int i = 2; i < n; i++) {
        if (toks[i].type != TOK_KEYVAL) continue;
        if (strcasecmp(toks[i].key, "sabme") == 0) {
            strncpy(lapf->sabme, toks[i].val, sizeof(lapf->sabme) - 1);
        } else if (strcasecmp(toks[i].key, "xid") == 0) {
            strncpy(lapf->xid, toks[i].val, sizeof(lapf->xid) - 1);
        } else if (strcasecmp(toks[i].key, "k") == 0) {
            cfg_parse_u8(toks[i].val, 1, 127, &lapf->k);
        } else if (strcasecmp(toks[i].key, "n200") == 0) {
            cfg_parse_u8(toks[i].val, 1, 10, &lapf->n200);
        } else if (strcasecmp(toks[i].key, "n201") == 0) {
            cfg_parse_u16(toks[i].val, 128, 8192, &lapf->n201);
        } else if (strcasecmp(toks[i].key, "t200") == 0) {
            cfg_parse_time_ms(toks[i].val, &lapf->t200_ms);
        } else if (strcasecmp(toks[i].key, "t203") == 0) {
            cfg_parse_time_ms(toks[i].val, &lapf->t203_ms);
        }
    }
    return 0;
}

/* Parse svc subcommands: svc int, svc addr, svc route, svc mcast */
static int parse_stmt_svc(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 2) return -1;
    const char *sub = toks[1].text;

    if (strcasecmp(sub, "int") == 0) {
        if (n < 3) return -1;
        if (ast->num_svc_ints >= CFG_MAX_PORTS) return -1;
        ast_svc_int_t *si = &ast->svc_ints[ast->num_svc_ints++];
        si->line = toks[0].line;
        strncpy(si->port, toks[2].text, sizeof(si->port) - 1);
        si->dlci_low = 512;
        si->dlci_high = 991;
        si->cirdef = ast->defaults.svc_def.cir;
        si->bcdef = ast->defaults.svc_def.bc;
        si->bedef = ast->defaults.svc_def.be;
        si->fmifdef = ast->defaults.svc_def.fmif;
        si->ftpdef = ast->defaults.svc_def.ftp;
        si->fdpdef = ast->defaults.svc_def.fdp;
        si->clsdef = ast->defaults.svc_def.srv_class;
        si->revchg_allow = ast->defaults.svc_def.revchg_allow;
        si->ogrevchg_allow = ast->defaults.svc_def.ogrevchg_allow;
        si->icrevchg_allow = ast->defaults.svc_def.icrevchg_allow;

        for (int i = 3; i < n; i++) {
            if (toks[i].type != TOK_KEYVAL) continue;
            if (strcasecmp(toks[i].key, "dlci_low") == 0) cfg_parse_u32(toks[i].val, 16, 8388607, &si->dlci_low);
            else if (strcasecmp(toks[i].key, "dlci_high") == 0) cfg_parse_u32(toks[i].val, 16, 8388607, &si->dlci_high);
            else if (strcasecmp(toks[i].key, "cirdef") == 0) cfg_parse_rate(toks[i].val, &si->cirdef);
            else if (strcasecmp(toks[i].key, "bcdef") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &si->bcdef);
            else if (strcasecmp(toks[i].key, "bedef") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &si->bedef);
            else if (strcasecmp(toks[i].key, "fmifdef") == 0) cfg_parse_u16(toks[i].val, 128, 8192, &si->fmifdef);
            else if (strcasecmp(toks[i].key, "ftpdef") == 0) cfg_parse_u8(toks[i].val, 0, 15, &si->ftpdef);
            else if (strcasecmp(toks[i].key, "fdpdef") == 0) cfg_parse_u8(toks[i].val, 0, 7, &si->fdpdef);
            else if (strcasecmp(toks[i].key, "clsdef") == 0) cfg_parse_u8(toks[i].val, 0, 3, &si->clsdef);
            else if (strcasecmp(toks[i].key, "revchg") == 0) {
                int b = 0; cfg_parse_bool(toks[i].val, &b);
                si->revchg_allow = (u8)b;
            }
        }
        return 0;
    } else if (strcasecmp(sub, "addr") == 0) {
        /* svc addr <port> {manual|autoprefix|autonumber} ... */
        if (n < 4) return -1;
        if (ast->num_svc_addrs >= CFG_MAX_PORTS) return -1;
        ast_svc_addr_t *sa = &ast->svc_addrs[ast->num_svc_addrs++];
        sa->line = toks[0].line;
        strncpy(sa->port, toks[2].text, sizeof(sa->port) - 1);
        strncpy(sa->mode, toks[3].text, sizeof(sa->mode) - 1);
        strcpy(sa->plan, "x121");

        int idx = 4;
        if (idx < n && (strcasecmp(toks[idx].text, "x121") == 0 || strcasecmp(toks[idx].text, "e164") == 0)) {
            strncpy(sa->plan, toks[idx++].text, sizeof(sa->plan) - 1);
        }

        if (idx < n && toks[idx].type == TOK_WORD) {
            if (strcasecmp(sa->mode, "autonumber") == 0) {
                strncpy(sa->autonumber_scheme, toks[idx++].text, sizeof(sa->autonumber_scheme) - 1);
            } else {
                strncpy(sa->number_primary, toks[idx++].text, sizeof(sa->number_primary) - 1);
            }
        }

        /* Parse optional aliases or key-values */
        while (idx < n) {
            if (strcasecmp(toks[idx].text, "alias") == 0 && idx + 1 < n) {
                idx++;
                if (sa->num_aliases < CFG_MAX_ALIASES) {
                    int ai = sa->num_aliases++;
                    strcpy(sa->aliases[ai].plan, sa->plan);
                    if (strcasecmp(toks[idx].text, "x121") == 0 || strcasecmp(toks[idx].text, "e164") == 0) {
                        strncpy(sa->aliases[ai].plan, toks[idx++].text, sizeof(sa->aliases[ai].plan) - 1);
                    }
                    if (idx < n) {
                        strncpy(sa->aliases[ai].number, toks[idx++].text, sizeof(sa->aliases[ai].number) - 1);
                    }
                }
            } else {
                idx++;
            }
        }
        return 0;
    } else if (strcasecmp(sub, "route") == 0) {
        /* svc route <port> {x121|e164|prefix} ... */
        if (n < 4) return -1;
        if (ast->num_svc_routes >= CFG_MAX_ROUTES) return -1;
        ast_svc_route_t *sr = &ast->svc_routes[ast->num_svc_routes++];
        sr->line = toks[0].line;
        strncpy(sr->port, toks[2].text, sizeof(sr->port) - 1);
        strncpy(sr->mode, toks[3].text, sizeof(sr->mode) - 1);
        sr->sgclen = -1;
        sr->siclen = -1;
        sr->revchg_allow = 1;

        if (strcasecmp(sr->mode, "prefix") == 0) {
            int idx = 4;
            if (idx < n && (strcasecmp(toks[idx].text, "x121") == 0 || strcasecmp(toks[idx].text, "e164") == 0)) {
                strncpy(sr->plan, toks[idx++].text, sizeof(sr->plan) - 1);
            }
            if (idx < n && (toks[idx].type == TOK_WORD || toks[idx].type == TOK_STRING)) {
                strncpy(sr->prefix_regex, toks[idx++].text, sizeof(sr->prefix_regex) - 1);
            }
        } else {
            for (int i = 4; i < n; i++) {
                if (toks[i].type != TOK_KEYVAL) continue;
                const char *k = toks[i].key;
                const char *v = toks[i].val;
                if (strcasecmp(k, "dcc") == 0) strncpy(sr->dcc, v, sizeof(sr->dcc) - 1);
                else if (strcasecmp(k, "nd") == 0 && strcasecmp(v, "none") != 0) strncpy(sr->nd, v, sizeof(sr->nd) - 1);
                else if (strcasecmp(k, "dnic") == 0) strncpy(sr->dnic, v, sizeof(sr->dnic) - 1);
                else if (strcasecmp(k, "pnic") == 0 && strcasecmp(v, "none") != 0) strncpy(sr->pnic, v, sizeof(sr->pnic) - 1);
                else if (strcasecmp(k, "ind") == 0) {
                    strncpy(sr->x121_ind, v, sizeof(sr->x121_ind) - 1);
                    strncpy(sr->e164_ind, v, sizeof(sr->e164_ind) - 1);
                } else if (strcasecmp(k, "cc") == 0) strncpy(sr->cc, v, sizeof(sr->cc) - 1);
                else if (strcasecmp(k, "ic") == 0) strncpy(sr->ic, v, sizeof(sr->ic) - 1);
                else if (strcasecmp(k, "ndc") == 0) strncpy(sr->ndc, v, sizeof(sr->ndc) - 1);
                else if (strcasecmp(k, "area") == 0) strncpy(sr->area, v, sizeof(sr->area) - 1);
                else if (strcasecmp(k, "sgclen") == 0) sr->sgclen = (strcasecmp(v, "none") == 0) ? -1 : atoi(v);
                else if (strcasecmp(k, "sgc") == 0) strncpy(sr->sgc, v, sizeof(sr->sgc) - 1);
                else if (strcasecmp(k, "siclen") == 0) sr->siclen = (strcasecmp(v, "none") == 0) ? -1 : atoi(v);
                else if (strcasecmp(k, "sic") == 0) strncpy(sr->sic, v, sizeof(sr->sic) - 1);
                else if (strcasecmp(k, "metric") == 0) cfg_parse_u32(v, 1, 1000, &sr->metric);
                else if (strcasecmp(k, "revchg") == 0) {
                    int b = 0; cfg_parse_bool(v, &b);
                    sr->revchg_allow = (u8)b;
                }
            }
        }
        return 0;
    } else if (strcasecmp(sub, "mcast") == 0) {
        /* svc mcast <grp> conference/subaddress/confsubadd/source/member */
        if (n < 4) return -1;
        const char *grp_name = toks[2].text;
        const char *action = toks[3].text;

        ast_svc_mcast_t *sm = NULL;
        for (int i = 0; i < ast->num_svc_mcasts; i++) {
            if (strcasecmp(ast->svc_mcasts[i].group_name, grp_name) == 0) {
                sm = &ast->svc_mcasts[i];
                break;
            }
        }
        if (!sm && ast->num_svc_mcasts < CFG_MAX_MCAST_GROUPS) {
            sm = &ast->svc_mcasts[ast->num_svc_mcasts++];
            strncpy(sm->group_name, grp_name, sizeof(sm->group_name) - 1);
        }
        if (!sm) return -1;

        if (strcasecmp(action, "conference") == 0 && n >= 6) {
            strncpy(sm->conf_plan, toks[4].text, sizeof(sm->conf_plan) - 1);
            strncpy(sm->conf_number, toks[5].text, sizeof(sm->conf_number) - 1);
        } else if (strcasecmp(action, "subaddress") == 0 && n >= 5) {
            strncpy(sm->subaddr_number, toks[4].text, sizeof(sm->subaddr_number) - 1);
        } else if (strcasecmp(action, "confsubadd") == 0 && n >= 7) {
            strncpy(sm->conf_plan, toks[4].text, sizeof(sm->conf_plan) - 1);
            strncpy(sm->conf_number, toks[5].text, sizeof(sm->conf_number) - 1);
            strncpy(sm->subaddr_number, toks[6].text, sizeof(sm->subaddr_number) - 1);
        } else if (strcasecmp(action, "source") == 0 && n >= 6) {
            strncpy(sm->src_plan, toks[4].text, sizeof(sm->src_plan) - 1);
            strncpy(sm->src_number_or_port, toks[5].text, sizeof(sm->src_number_or_port) - 1);
        } else if (strcasecmp(action, "member") == 0 && n >= 6) {
            if (sm->num_members < CFG_MAX_MCAST_MEMBERS) {
                int mi = sm->num_members++;
                strncpy(sm->members[mi].plan, toks[4].text, sizeof(sm->members[mi].plan) - 1);
                strncpy(sm->members[mi].number_or_port, toks[5].text, sizeof(sm->members[mi].number_or_port) - 1);
            }
        }
        return 0;
    }
    return 0;
}

/* Parse spvc statement */
static int parse_stmt_spvc(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 6) return -1;
    if (ast->num_spvcs >= CFG_MAX_SPVCS) return -1;

    ast_spvc_t *sp = &ast->spvcs[ast->num_spvcs++];
    sp->line = toks[0].line;
    strncpy(sp->port1, toks[2].text, sizeof(sp->port1) - 1);
    sp->local_dlci = (u32)atoi(toks[3].text);

    int idx = 4;
    while (idx < n && toks[idx].type == TOK_KEYVAL) {
        if (strcasecmp(toks[idx].key, "lspvcid") == 0) {
            cfg_parse_u32(toks[idx].val, 1, 0xFFFFFFFF, &sp->lspvcid);
        }
        idx++;
    }

    if (idx < n && toks[idx].type == TOK_WORD) {
        strncpy(sp->dest_vfrs_number, toks[idx++].text, sizeof(sp->dest_vfrs_number) - 1);
    }

    for (int i = idx; i < n; i++) {
        if (toks[i].type != TOK_KEYVAL) continue;
        if (strcasecmp(toks[i].key, "tgt") == 0) strncpy(sp->target_type, toks[i].val, sizeof(sp->target_type) - 1);
        else if (strcasecmp(toks[i].key, "tdlci") == 0) cfg_parse_u32(toks[i].val, 16, 8388607, &sp->tdlci);
        else if (strcasecmp(toks[i].key, "tspvcid") == 0) cfg_parse_u32(toks[i].val, 1, 0xFFFFFFFF, &sp->tspvcid);
        else if (strcasecmp(toks[i].key, "cir") == 0) cfg_parse_rate(toks[i].val, &sp->cir);
        else if (strcasecmp(toks[i].key, "bc") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &sp->bc);
        else if (strcasecmp(toks[i].key, "be") == 0) cfg_parse_u32(toks[i].val, 0, 0xFFFFFFFF, &sp->be);
    }

    return 0;
}

/* Parse cgst statement */
static int parse_stmt_cgst(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 3) return -1;
    if (ast->num_cgsts >= CFG_MAX_PORTS) return -1;

    ast_cgst_t *cg = &ast->cgsts[ast->num_cgsts++];
    cg->line = toks[0].line;
    strncpy(cg->port, toks[1].text, sizeof(cg->port) - 1);

    for (int i = 2; i < n; i++) {
        if (toks[i].type != TOK_KEYVAL) continue;
        if (strcasecmp(toks[i].key, "rate") == 0) cfg_parse_u32(toks[i].val, 1, 1000000, &cg->rate_fps);
        else if (strcasecmp(toks[i].key, "clear") == 0) cfg_parse_u32(toks[i].val, 1, 1000000, &cg->clear_fps);
        else if (strcasecmp(toks[i].key, "threshold") == 0) cfg_parse_u32(toks[i].val, 1, 1000, &cg->threshold);
        else if (strcasecmp(toks[i].key, "cllm") == 0) {
            int b = 0; cfg_parse_bool(toks[i].val, &b);
            cg->cllm_enabled = b;
        } else if (strcasecmp(toks[i].key, "txint") == 0) {
            cfg_parse_time_ms(toks[i].val, &cg->cllm_txint_ms);
        }
    }
    return 0;
}

/* Parse capture statement */
static int parse_stmt_capture(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 3) return -1;
    if (ast->num_captures >= CFG_MAX_PORTS) return -1;

    ast_capture_t *cap = &ast->captures[ast->num_captures++];
    cap->line = toks[0].line;
    strncpy(cap->target, toks[1].text, sizeof(cap->target) - 1);
    strncpy(cap->pcap_file, toks[2].text, sizeof(cap->pcap_file) - 1);
    strcpy(cap->svccap, "iframe");

    for (int i = 3; i < n; i++) {
        if (toks[i].type == TOK_KEYVAL && strcasecmp(toks[i].key, "svccap") == 0) {
            strncpy(cap->svccap, toks[i].val, sizeof(cap->svccap) - 1);
        }
    }
    return 0;
}

/* Parse issmp statement */
static int parse_stmt_issmp(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n < 3) return -1;
    if (ast->num_issmps >= CFG_MAX_PORTS) return -1;

    ast_issmp_t *is = &ast->issmps[ast->num_issmps++];
    is->line = toks[0].line;
    strncpy(is->port, toks[1].text, sizeof(is->port) - 1);
    if (strcasecmp(toks[2].text, "enable") == 0) is->enabled = 1;

    for (int i = 3; i < n; i++) {
        if (toks[i].type != TOK_KEYVAL) continue;
        if (strcasecmp(toks[i].key, "as") == 0) cfg_parse_u32(toks[i].val, 1, 0xFFFFFFFF, &is->as);
        else if (strcasecmp(toks[i].key, "isic") == 0) cfg_parse_u32(toks[i].val, 1, 0xFFFFFFFF, &is->isic);
        else if (strcasecmp(toks[i].key, "cost") == 0) cfg_parse_u32(toks[i].val, 1, 0xFFFFFFFF, &is->cost);
        else if (strcasecmp(toks[i].key, "pw") == 0) strncpy(is->pw, toks[i].val, sizeof(is->pw) - 1);
    }
    return 0;
}

/* Main line parser */
static int parse_line_tokens(cfg_ast_t *ast, const cfg_token_t *toks, int n)
{
    if (n <= 0) return 0;
    const char *cmd = toks[0].text;

    if (strcasecmp(cmd, "swconfig") == 0) {
        return parse_stmt_swconfig(ast, toks, n);
    } else if (strcasecmp(cmd, "log") == 0) {
        return parse_stmt_log(ast, toks, n);
    } else if (strcasecmp(cmd, "default") == 0) {
        return parse_stmt_default(ast, toks, n);
    } else if (strcasecmp(cmd, "port") == 0) {
        return parse_stmt_port(ast, toks, n);
    } else if (strcasecmp(cmd, "pvc") == 0) {
        return parse_stmt_pvc(ast, toks, n);
    } else if (strcasecmp(cmd, "lmi") == 0) {
        return parse_stmt_lmi(ast, toks, n);
    } else if (strcasecmp(cmd, "lapf") == 0) {
        return parse_stmt_lapf(ast, toks, n);
    } else if (strcasecmp(cmd, "svc") == 0) {
        return parse_stmt_svc(ast, toks, n);
    } else if (strcasecmp(cmd, "spvc") == 0) {
        return parse_stmt_spvc(ast, toks, n);
    } else if (strcasecmp(cmd, "cgst") == 0) {
        return parse_stmt_cgst(ast, toks, n);
    } else if (strcasecmp(cmd, "capture") == 0) {
        return parse_stmt_capture(ast, toks, n);
    } else if (strcasecmp(cmd, "issmp") == 0) {
        return parse_stmt_issmp(ast, toks, n);
    }

    char err[256];
    snprintf(err, sizeof(err), "Unknown configuration statement '%.128s'", cmd);
    set_ast_error(ast, toks[0].line, err);
    return -1;
}

int cfg_parse_tokens(cfg_lexer_t *lex, cfg_ast_t *ast)
{
    if (!lex || !ast) return -1;

    cfg_token_t line_tokens[128];
    int token_count = 0;

    cfg_token_t tok;
    while (cfg_lexer_next_token(lex, &tok)) {
        if (tok.type == TOK_ERROR) {
            set_ast_error(ast, tok.line, cfg_lexer_error(lex));
            return -1;
        }

        if (tok.type == TOK_NEWLINE || tok.type == TOK_EOF) {
            if (token_count > 0) {
                if (parse_line_tokens(ast, line_tokens, token_count) < 0) {
                    return -1;
                }
                token_count = 0;
            }
            if (tok.type == TOK_EOF) break;
            continue;
        }

        if (token_count < 128) {
            line_tokens[token_count++] = tok;
        }
    }

    return 0;
}
