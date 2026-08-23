/*
 * vfrs_main.c - VFRS Main Entry Point
 * Virtual Frame Relay Switch
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vfr.h"
#include "config.h"
#include "svc_signalling/svc_sig_common.h"
#include "svc_signalling/svc_sig_uni.h"
#include "svc_signalling/svc_sig_nni.h"
#include "svc_signalling/svc_sig_iel.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "fr_switching/svc_routing_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* Global context */
_Atomic int g_running = 1;   /* defined here; declared extern _Atomic int in vfr.h */

#ifndef NDEBUG
#  ifdef _MSC_VER
     __declspec(thread) uint32_t vfr_tls_lock_bitmap = 0;
#  else
     __thread uint32_t vfr_tls_lock_bitmap = 0;
#  endif
#endif

/* Signal handler */
static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("Received signal %d, shutting down...", sig);
        g_running = 0;
    }
}

/* Print usage */
static void print_usage(const char *prog)
{
    printf("VFRS - Virtual Frame Relay Switch\n");
    printf("Usage: %s [options] [config_file]\n", prog);
    printf("\nOptions:\n");
    printf("  -h, --help           Show this help\n");
    printf("  -t, --check-config   Test and validate configuration file and exit (dry-run)\n");
    printf("  -d, --debug          Enable debug logging\n");
    printf("  -c, --console        Log to console only (no file)\n");
    printf("  -s, --log-size <mb>  Set max log file size in MB (default: 10)\n");
    printf("  -n, --log-files <n>  Set max log files for rotation (default: 5)\n");
    printf("\nConfig file format:\n");
    printf("  port <name> udp <lhost> <lport> <rhost> <rport>\n");
    printf("  pvc <port1> <dlci1> <port2> <dlci2>\n");
    printf("  lmi <port> [ansi|q933a|cisco] [t392=<s>] [n392=<n>] [n393=<n>]\n");
    printf("  capture <port|all> <filename>\n");
}

/* Tokenize a config or console line into tokens, clearing the destination array first (Helper #4.1) */
static int parse_config_line(char *line, char tokens[64][256])
{
    char *p = line;
    int tok_count = 0;
    memset(tokens, 0, 64 * 256);
    while (tok_count < 64) {
        char tok[256];
        if (!get_token(&p, tok, sizeof(tok)) || !tok[0]) break;
        snprintf(tokens[tok_count], sizeof(tokens[tok_count]), "%s", tok);
        tok_count++;
    }
    return tok_count;
}

/* Perform post-parse validation for configuration consistency (F17) */
static int vfrs_validate_config(vfrs_ctx_t *ctx)
{
    if (!ctx) return -1;
    int errors = 0;

    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
    mutex_lock(&ctx->port_mutex);

    /* 1. Check PVCs (port_mutex -> pvc_mutex) */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
    mutex_lock(&ctx->pvc_mutex);
    for (int i = 0; i < PVC_HASH_SIZE; i++) {
        vfr_pvc_detail_t *pvc = ctx->pvc_table[i];
        while (pvc) {
            vfr_port_t *p_in = vfrs_find_port_unlocked(ctx, pvc->port_in);
            vfr_port_t *p_out = vfrs_find_port_unlocked(ctx, pvc->port_out);
            if (!p_in) {
                LOG_WARN("Config Inconsistency: PVC %s:%u -> %s:%u references non-existent ingress port '%s'",
                         pvc->port_in, pvc->dlci_in, pvc->port_out, pvc->dlci_out, pvc->port_in);
                errors++;
            }
            if (!p_out) {
                LOG_WARN("Config Inconsistency: PVC %s:%u -> %s:%u references non-existent egress port '%s'",
                         pvc->port_in, pvc->dlci_in, pvc->port_out, pvc->dlci_out, pvc->port_out);
                errors++;
            }
            pvc = pvc->next;
        }
    }
    mutex_unlock(&ctx->pvc_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);

    /* 2. Check multicast groups and members (port_mutex -> mcast_mutex) */
    ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
    mutex_lock(&ctx->mcast_mutex);
    vfr_mcast_group_t *g = ctx->mcast_groups;
    while (g) {
        vfr_port_t *src_port = vfrs_find_port_unlocked(ctx, g->source_port);
        if (!src_port) {
            LOG_WARN("Config Inconsistency: Multicast group '%s' references non-existent source port '%s'",
                     g->name, g->source_port);
            errors++;
        }
        vfr_mcast_member_t *m = g->members;
        while (m) {
            vfr_port_t *m_port = vfrs_find_port_unlocked(ctx, m->port_name);
            if (!m_port) {
                LOG_WARN("Config Inconsistency: Multicast group '%s' member references non-existent port '%s'",
                         g->name, m->port_name);
                errors++;
            }
            m = m->next;
        }
        g = g->next;
    }
    mutex_unlock(&ctx->mcast_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

    /* 3. Check LMI types on ports */
    for (int i = 0; i < ctx->port_count; i++) {
        vfr_port_t *port = ctx->ports[i];
        if (port && port->lmi_ctx) {
            vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
            if (lmi->type != LMI_TYPE_ANSI && lmi->type != LMI_TYPE_Q933A && lmi->type != LMI_TYPE_CISCO && lmi->type != LMI_TYPE_NONE) {
                LOG_WARN("Config Inconsistency: Port '%s' has invalid LMI type %d",
                         port->name, lmi->type);
                errors++;
            }
        }
    }

    mutex_unlock(&ctx->port_mutex);
    RELEASE_LOCK_LEVEL(LOCK_LEVEL_PORT);

    return errors;
}

/* Parse configuration file */
static int parse_config(vfrs_ctx_t *ctx, const char *filename)
{
    config_t *cfg;
    char line[1024];
    char tokens[64][256];
    int tok_count;
    cfg_result_t result;
    int line_num = 0;

    /* PASS 1: Parse global defaults, swconfig, and logging configurations first */
    LOG_INFO("Parsing configuration (Pass 1 - Defaults & Global Settings): %s", filename);
    cfg = config_create(filename);
    if (!cfg) {
        return -1;
    }

    while ((result = config_next_line(cfg, line, sizeof(line))) == CFG_OK) {
        line_num = config_error_line(cfg);

        /* Tokenize line */
        tok_count = parse_config_line(line, tokens);

        if (tok_count == 0) continue;

        /* Process global / logging commands in Pass 1 */
        if (strcmp(tokens[0], "swconfig") == 0) {
            /* Switch configuration */
            for (int i = 1; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "swid") == 0) {
                        snprintf(ctx->swid, sizeof(ctx->swid), "%s", val);
                    } else if (strcmp(key, "dnic") == 0) {
                        ctx->dnic = atoi(val);
                    } else if (strcmp(key, "dcc") == 0) {
                        ctx->dcc = atoi(val);
                    } else if (strcmp(key, "nd") == 0) {
                        ctx->nd = (strcmp(val, "none") == 0) ? 0 : atoi(val);
                    } else if (strcmp(key, "pnic") == 0) {
                        ctx->pnic = (strcmp(val, "none") == 0) ? 0 : (u32)atoi(val);
                    } else if (strcmp(key, "sgclen") == 0) {
                        ctx->sgclen = atoi(val);
                    } else if (strcmp(key, "sgc") == 0) {
                        ctx->sgc = atoi(val);
                    } else if (strcmp(key, "siclen") == 0) {
                        ctx->siclen = atoi(val);
                    } else if (strcmp(key, "sic") == 0) {
                        ctx->sic = atoi(val);
                    } else if (strcmp(key, "subnumlen") == 0) {
                        ctx->subnumlen = atoi(val);
                    }
                }
            }
            if (ctx->sgclen < 1 || ctx->sgclen > 4) {
                LOG_WARN("Line %d: sgclen (%u) out of range (1-4), defaulting to 1", line_num, ctx->sgclen);
                ctx->sgclen = (ctx->sgclen < 1) ? 1 : 4;
            }
            if (ctx->siclen < 1 || ctx->siclen > 4) {
                LOG_WARN("Line %d: siclen (%u) out of range (1-4), defaulting to 1", line_num, ctx->siclen);
                ctx->siclen = (ctx->siclen < 1) ? 1 : 4;
            }
            if (ctx->subnumlen < 1 || ctx->subnumlen > 6) {
                LOG_WARN("Line %d: subnumlen (%u) out of range (1-6), defaulting to 4", line_num, ctx->subnumlen);
                ctx->subnumlen = (ctx->subnumlen < 1) ? 1 : 6;
            }
            LOG_INFO("Switch configured: %s (DNIC=%04u, PNIC=%u, SGC=%u, SIC=%u, subnumlen=%u)",
                     ctx->swid, ctx->dnic, ctx->pnic, ctx->sgc, ctx->sic, ctx->subnumlen);

        } else if (strcmp(tokens[0], "log_level") == 0) {
            /* Log level configuration */
            if (tok_count < 2) {
                LOG_WARN("Line %d: log_level requires at least one parameter (con= or txt=)", line_num);
                continue;
            }
            for (int i = 1; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "con") == 0) {
                        if (strcmp(val, "trace") == 0) g_log_console = LOG_TRACE;
                        else if (strcmp(val, "debug") == 0) g_log_console = LOG_DEBUG;
                        else if (strcmp(val, "info") == 0) g_log_console = LOG_INFO;
                        else if (strcmp(val, "warn") == 0) g_log_console = LOG_WARN;
                        else if (strcmp(val, "error") == 0) g_log_console = LOG_ERROR;
                        else LOG_WARN("Line %d: Unknown log level '%s' for '%s'", line_num, val, key);
                    } else if (strcmp(key, "txt") == 0) {
                        if (strcmp(val, "trace") == 0) g_log_file = LOG_TRACE;
                        else if (strcmp(val, "debug") == 0) g_log_file = LOG_DEBUG;
                        else if (strcmp(val, "info") == 0) g_log_file = LOG_INFO;
                        else if (strcmp(val, "warn") == 0) g_log_file = LOG_WARN;
                        else if (strcmp(val, "error") == 0) g_log_file = LOG_ERROR;
                        else LOG_WARN("Line %d: Unknown log level '%s' for '%s'", line_num, val, key);
                    } else {
                        LOG_WARN("Line %d: Unknown log_level parameter '%s'", line_num, key);
                    }
                }
            }

        } else if (strcmp(tokens[0], "log_file") == 0) {
            /* Log file configuration */
            if (tok_count < 2 || tokens[1][0] == '\0') {
                LOG_WARN("Line %d: log_file requires a file path", line_num);
                continue;
            }
            if (logger_reopen(tokens[1]) < 0) {
                LOG_ERROR("Line %d: Failed to open log file: %s", line_num, tokens[1]);
            } else {
                LOG_INFO("Log file reconfigured: %s", tokens[1]);
            }

        } else if (strcmp(tokens[0], "log_rotation") == 0) {
            /* Log rotation configuration */
            size_t max_size_mb = 10;
            int max_files = 5;
            for (int i = 1; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "size") == 0) {
                        max_size_mb = (size_t)atoi(val);
                    } else if (strcmp(key, "files") == 0) {
                        max_files = atoi(val);
                    } else {
                        LOG_WARN("Line %d: Unknown log_rotation parameter '%s'", line_num, key);
                    }
                }
            }
            logger_set_rotation(max_size_mb * 1024 * 1024, max_files);
            LOG_INFO("Log rotation configured: max_size=%zu MB, max_files=%d", max_size_mb, max_files);

        } else if (strcmp(tokens[0], "defaults") == 0) {
            /* Global default parameters */
            for (int i = 1; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "lmi_t392") == 0) {
                        int v = atoi(val);
                        if (v >= 5 && v <= 30) ctx->default_t392 = v;
                        else LOG_WARN("Line %d: default lmi_t392 (%d) out of protocol range (5-30)", line_num, v);
                    }
                    else if (strcmp(key, "lmi_n392") == 0) {
                        int v = atoi(val);
                        if (v >= 1 && v <= 10) ctx->default_n392 = v;
                        else LOG_WARN("Line %d: default lmi_n392 (%d) out of protocol range (1-10)", line_num, v);
                    }
                    else if (strcmp(key, "lmi_n393") == 0) {
                        int v = atoi(val);
                        if (v >= 1 && v <= 10) ctx->default_n393 = v;
                        else LOG_WARN("Line %d: default lmi_n393 (%d) out of protocol range (1-10)", line_num, v);
                    }
                    else if (strcmp(key, "lmi_dte_n391") == 0) {
                        int v = atoi(val);
                        if (v >= 1 && v <= 255) ctx->default_n391 = v;
                        else LOG_WARN("Line %d: default lmi_dte_n391 (%d) out of protocol range (1-255)", line_num, v);
                    }
                    else if (strcmp(key, "lmi_dte_t391") == 0) {
                        int v = atoi(val);
                        if (v >= 5 && v <= 30) ctx->default_t391 = v;
                        else LOG_WARN("Line %d: default lmi_dte_t391 (%d) out of protocol range (5-30)", line_num, v);
                    }
                    else if (strcmp(key, "lmi_dte_n392") == 0) {
                        int v = atoi(val);
                        if (v >= 1 && v <= 10) ctx->default_n392_dte = v;
                        else LOG_WARN("Line %d: default lmi_dte_n392 (%d) out of protocol range (1-10)", line_num, v);
                    }
                    else if (strcmp(key, "lmi_dte_n393") == 0) {
                        int v = atoi(val);
                        if (v >= 1 && v <= 10) ctx->default_n393_dte = v;
                        else LOG_WARN("Line %d: default lmi_dte_n393 (%d) out of protocol range (1-10)", line_num, v);
                    }
                    else if (strcmp(key, "lapf_k") == 0) {
                        int v = atoi(val);
                        if (v >= 1 && v <= 127) ctx->default_lapf_k = v;
                        else LOG_WARN("Line %d: default lapf_k (%d) out of range (1-127)", line_num, v);
                    }
                    else if (strcmp(key, "lapf_n200") == 0) ctx->default_lapf_n200 = atoi(val);
                    else if (strcmp(key, "lapf_n201") == 0) ctx->default_lapf_n201 = atoi(val);
                    else if (strcmp(key, "lapf_t200") == 0) ctx->default_lapf_t200 = (u16)(atof(val) * 1000);
                    else if (strcmp(key, "lapf_t203") == 0) ctx->default_lapf_t203 = (u16)(atof(val) * 1000);
                    else if (strcmp(key, "access_rate") == 0 || strcmp(key, "ar") == 0) ctx->access_rate = atoi(val);
                    else if (strcmp(key, "svc_default_cir") == 0) ctx->default_cir = atoi(val);
                    else if (strcmp(key, "svc_default_bc") == 0) ctx->default_bc = atoi(val);
                    else if (strcmp(key, "svc_default_be") == 0) ctx->default_be = atoi(val);
                    else if (strcmp(key, "svc_default_fmif") == 0) ctx->default_svc_fmif = (u16)atoi(val);
                    else if (strcmp(key, "svc_default_ftp") == 0) ctx->default_svc_ftp = (u8)atoi(val);
                    else if (strcmp(key, "svc_default_fdp") == 0) ctx->default_svc_fdp = (u8)atoi(val);
                    else if (strcmp(key, "svc_default_svc_class") == 0 || strcmp(key, "svc_default_srvcls") == 0) ctx->default_svc_class = (u8)atoi(val);
                    else if (strcmp(key, "svc_t301") == 0 || strcmp(key, "svc_uni_t301") == 0 || strcmp(key, "svc_nni_t301") == 0) ctx->default_svc_t301 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "svc_t303") == 0 || strcmp(key, "svc_uni_t303") == 0 || strcmp(key, "svc_nni_t303") == 0) ctx->default_svc_t303 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "svc_t305") == 0 || strcmp(key, "svc_uni_t305") == 0 || strcmp(key, "svc_nni_t305") == 0) ctx->default_svc_t305 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "svc_t308") == 0 || strcmp(key, "svc_uni_t308") == 0 || strcmp(key, "svc_nni_t308") == 0) ctx->default_svc_t308 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "svc_t310") == 0 || strcmp(key, "svc_uni_t310") == 0 || strcmp(key, "svc_nni_t310") == 0) ctx->default_svc_t310 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "svc_t316") == 0 || strcmp(key, "svc_uni_t316") == 0 || strcmp(key, "svc_nni_t316") == 0) ctx->default_svc_t316 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "svc_t317") == 0 || strcmp(key, "svc_uni_t317") == 0 || strcmp(key, "svc_nni_t317") == 0) ctx->default_svc_t317 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "svc_t322") == 0 || strcmp(key, "svc_uni_t322") == 0 || strcmp(key, "svc_nni_t322") == 0) ctx->default_svc_t322 = (u32)(atof(val) * 1000);
                }
            }
            LOG_DEBUG("Default parameters updated");
        }
    }

    if (result != CFG_EOF) {
        LOG_ERROR("Configuration error at line %d (Pass 1)", line_num);
        config_destroy(cfg);
        return -1;
    }
    config_destroy(cfg);

    /* PASS 2: Parse interface-level and network-level settings */
    LOG_INFO("Parsing configuration (Pass 2 - Interfaces & PVCs): %s", filename);
    cfg = config_create(filename);
    if (!cfg) {
        return -1;
    }

    while ((result = config_next_line(cfg, line, sizeof(line))) == CFG_OK) {
        line_num = config_error_line(cfg);

        /* Tokenize line */
        tok_count = parse_config_line(line, tokens);

        if (tok_count == 0) continue;

        /* Skip global / logging commands and cllm in Pass 2 */
        if (strcmp(tokens[0], "swconfig") == 0 ||
            strcmp(tokens[0], "log_level") == 0 ||
            strcmp(tokens[0], "log_file") == 0 ||
            strcmp(tokens[0], "log_rotation") == 0 ||
            strcmp(tokens[0], "defaults") == 0 ||
            strcmp(tokens[0], "cllm") == 0) {
            continue;
        }

        /* Process Pass 2 commands */
        if (strcmp(tokens[0], "port") == 0) {
            /* Port definition */
            char name[32], transport[32], args[256];
            vfr_port_t *port = NULL;

            if (config_parse_port(cfg, tokens[0], tokens, tok_count,
                                  name, sizeof(name), transport, sizeof(transport),
                                  args, sizeof(args)) < 0) {
                continue;
            }

            int port_type = PORT_TYPE_UNI;
            if (port_parse_name(name, &port_type, NULL, NULL) < 0) {
                LOG_ERROR("Line %d: Invalid port name '%s'", line_num, name);
                continue;
            }

            if (vfrs_find_port(ctx, name) != NULL) {
                LOG_WARN("Line %d: Port '%s' already defined, skipping redefinition", line_num, name);
                continue;
            }

            /* Parse transport type */
            if (strcmp(transport, "udp") == 0) {
                char lhost[MAX_ADDR_STR], rhost[MAX_ADDR_STR];
                u16 lport, rport;
                /* Parse: port <name> udp <lhost> <lport> <rhost> <rport> */
                /* tokens[0]=port, tokens[1]=name, tokens[2]=udp, tokens[3]=lhost, tokens[4]=lport, tokens[5]=rhost, tokens[6]=rport */
                if (tok_count >= 7) {
                    snprintf(lhost, sizeof(lhost), "%.*s", (int)(sizeof(lhost) - 1), tokens[3]);
                    lport = atoi(tokens[4]);
                    snprintf(rhost, sizeof(rhost), "%.*s", (int)(sizeof(rhost) - 1), tokens[5]);
                    rport = atoi(tokens[6]);
                    port = port_udp_create(name, port_type, lhost, lport, rhost, rport);
                }
            } else if (strcmp(transport, "udp-client") == 0) {
                char rhost[MAX_ADDR_STR];
                u16 rport;
                /* tokens[0]=port, tokens[1]=name, tokens[2]=udp-client, tokens[3]=rhost, tokens[4]=rport */
                if (tok_count >= 5) {
                    snprintf(rhost, sizeof(rhost), "%.*s", (int)(sizeof(rhost) - 1), tokens[3]);
                    rport = atoi(tokens[4]);
                    port = port_udp_cli_create(name, port_type, rhost, rport);
                }
            } else if (strcmp(transport, "udp-server") == 0) {
                char lhost[MAX_ADDR_STR] = "0.0.0.0";
                u16 lport = 0;
                /* tokens[0]=port, tokens[1]=name, tokens[2]=udp-server, [tokens[3]=lhost], tokens[last]=lport */
                if (tok_count >= 5) {
                    snprintf(lhost, sizeof(lhost), "%.*s", (int)(sizeof(lhost) - 1), tokens[3]);
                    lport = atoi(tokens[4]);
                } else if (tok_count == 4) {
                    lport = atoi(tokens[3]);
                }
                if (lport > 0) {
                    port = port_udp_ser_create(name, port_type, lhost, lport);
                }
            } else if (strcmp(transport, "tcp") == 0) {
                char lhost[MAX_ADDR_STR], rhost[MAX_ADDR_STR];
                u16 lport, rport;
                if (tok_count >= 7) {
                    snprintf(lhost, sizeof(lhost), "%.*s", (int)(sizeof(lhost) - 1), tokens[3]);
                    lport = atoi(tokens[4]);
                    snprintf(rhost, sizeof(rhost), "%.*s", (int)(sizeof(rhost) - 1), tokens[5]);
                    rport = atoi(tokens[6]);
                    port = port_tcp_create(name, port_type, lhost, lport, rhost, rport);
                }
            } else if (strcmp(transport, "tcp-client") == 0) {
                char rhost[MAX_ADDR_STR];
                u16 rport;
                /* tokens[0]=port, tokens[1]=name, tokens[2]=tcp-client, tokens[3]=rhost, tokens[4]=rport */
                if (tok_count >= 5) {
                    snprintf(rhost, sizeof(rhost), "%.*s", (int)(sizeof(rhost) - 1), tokens[3]);
                    rport = atoi(tokens[4]);
                    port = port_tcp_cli_create(name, port_type, rhost, rport);
                }
            } else if (strcmp(transport, "tcp-server") == 0) {
                char lhost[MAX_ADDR_STR] = "0.0.0.0";
                u16 lport = 0;
                /* tokens[0]=port, tokens[1]=name, tokens[2]=tcp-server, [tokens[3]=lhost], tokens[last]=lport */
                if (tok_count >= 5) {
                    snprintf(lhost, sizeof(lhost), "%.*s", (int)(sizeof(lhost) - 1), tokens[3]);
                    lport = atoi(tokens[4]);
                } else if (tok_count == 4) {
                    lport = atoi(tokens[3]);
                }
                if (lport > 0) {
                    port = port_tcp_ser_create(name, port_type, lhost, lport);
                }
            } else if (strcmp(transport, "serial") == 0) {
                /* Serial port: port <name> serial <device> <baudrate> */
                /* tokens[0]=port, tokens[1]=name, tokens[2]=serial, tokens[3]=device, tokens[4]=baudrate */
                if (tok_count >= 5) {
                    port = port_serial_create(name, port_type, tokens[3], atoi(tokens[4]));
                }
            } else if (strcmp(transport, "pipe-server") == 0 ||
                       strcmp(transport, "pipe-client") == 0) {
                /* Named pipe: port <name> pipe-server <pipename> */
                /* tokens[0]=port, tokens[1]=name, tokens[2]=pipe-server/pipe-client, tokens[3]=pipename */
                int server = (strcmp(transport, "pipe-server") == 0);
                if (tok_count >= 4) {
                    port = port_pipe_create(name, port_type, tokens[3], server);
                }
            } else if (strcmp(transport, "l2tpv3-fr") == 0 ||
                       strcmp(transport, "l2tpv3-hdlc") == 0) {
                int trans_type = (strcmp(transport, "l2tpv3-fr") == 0) ? PORT_TRANS_L2TPV3_FR : PORT_TRANS_L2TPV3_HDLC;
                char lhost[MAX_ADDR_STR] = "0.0.0.0";
                char rhost[MAX_ADDR_STR] = {0};
                u32 vcid = 1;
                if (tok_count >= 6) {
                    snprintf(lhost, sizeof(lhost), "%.*s", (int)(sizeof(lhost) - 1), tokens[3]);
                    snprintf(rhost, sizeof(rhost), "%.*s", (int)(sizeof(rhost) - 1), tokens[4]);
                    vcid = (u32)atoi(tokens[5]);
                } else if (tok_count == 5) {
                    /* port <name> l2tpv3-* <rhost> <vcid> */
                    snprintf(rhost, sizeof(rhost), "%.*s", (int)(sizeof(rhost) - 1), tokens[3]);
                    vcid = (u32)atoi(tokens[4]);
                } else if (tok_count == 4) {
                    snprintf(rhost, sizeof(rhost), "%.*s", (int)(sizeof(rhost) - 1), tokens[3]);
                }
                if (rhost[0]) {
                    port = port_l2tpv3_create(name, port_type, trans_type, lhost, rhost, vcid);
                }
            }

            if (port) {
                for (int i = 3; i < tok_count; i++) {
                    char key[64], val[64];
                    if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                        if (strcmp(key, "dlcibit") == 0) {
                            port->dlcibit = atoi(val);
                            LOG_INFO("Port %s configured with dlcibit=%d", port->name, port->dlcibit);
                        } else if (strcmp(key, "pcap") == 0) {
                            char pcap_filename[256];
                            if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0) {
                                char clean_port[64];
                                snprintf(clean_port, sizeof(clean_port), "%s", port->name);
                                for (char *cp = clean_port; *cp; cp++) if (*cp == '/') *cp = '_';
                                snprintf(pcap_filename, sizeof(pcap_filename), "%s.pcap", clean_port);
                            } else {
                                snprintf(pcap_filename, sizeof(pcap_filename), "%s", val);
                            }
                            pcap_writer_t *pw = calloc(1, sizeof(pcap_writer_t));
                            if (pw && pcap_writer_init(pw, pcap_filename, 65535) == 0) {
                                port->capture = pw;
                                LOG_INFO("Port %s enabled PCAP packet capture to '%s'", port->name, pcap_filename);
                            } else {
                                if (pw) free(pw);
                                LOG_ERROR("Failed to initialize PCAP capture '%s' on port %s", pcap_filename, port->name);
                            }
                        }
                    }
                }
                  /* (D7) Do NOT auto-initialize LAPF DLCI 0 unconditionally here; do it only on svc_int configuration.
                   * Auto-initialize LAPF DLCI 1015 for NNI ports only. */
                  if (port->type == PORT_TYPE_NNI) {
                      if (!port_get_lapf_ctx(port, 1015)) {
                          lapf_port_init(port, 1015, ctx->default_lapf_k, ctx->default_lapf_n200,
                                         ctx->default_lapf_n201, ctx->default_lapf_t200, ctx->default_lapf_t203);
                      }
                  }
                  vfrs_add_port(ctx, port);
            } else {
                LOG_ERROR("Failed to create port: %s", name);
            }

        } else if (strcmp(tokens[0], "pvc") == 0) {
            /* PVC definition */
            char p1[VFR_MAX_NAME_LEN], p2[VFR_MAX_NAME_LEN];
            u32 d1, d2;
            u32 cir = 0, bc = 0, be = 0;
            u8 ftp = 0, fdp = 0, srvcls = 0;

            if (config_parse_pvc(cfg, tokens[0], tokens, tok_count,
                                  p1, sizeof(p1), &d1, p2, sizeof(p2), &d2,
                                  &cir, &bc, &be, &ftp, &fdp, &srvcls) < 0) {
                continue;
            }

            if (vfrs_add_pvc(ctx, p1, d1, p2, d2, cir, bc, be, ftp, fdp, srvcls) < 0) {
                LOG_ERROR("Failed to add PVC: %s:%u -> %s:%u", p1, d1, p2, d2);
            }

        } else if (strcmp(tokens[0], "lmi") == 0) {
            /* LMI configuration */
            char port_name[VFR_MAX_NAME_LEN];
            int lmi_type = LMI_TYPE_Q933A;
            u8 n392 = (u8)ctx->default_n392;
            u8 n393 = (u8)ctx->default_n393;
            u16 t392 = ctx->default_t392;

            if (config_parse_lmi(cfg, tokens[0], tokens, tok_count,
                                  port_name, sizeof(port_name),
                                  &lmi_type, &n392, &n393, &t392) < 0) {
                continue;
            }

            int async_val = -1; // -1 = unset
            for (int i = 2; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "async") == 0) {
                        async_val = (strcmp(val, "true") == 0 || strcmp(val, "on") == 0);
                    }
                }
            }

            vfr_port_t *port = vfrs_find_port(ctx, port_name);
            if (!port) {
                LOG_WARN("Port '%s' not found for LMI configuration", port_name);
                continue;
            }

            /* NNI ports can only use Q.933A LMI (or none). Force to Q.933A if not none. */
            if (port->type == PORT_TYPE_NNI && lmi_type != LMI_TYPE_NONE) {
                lmi_type = LMI_TYPE_Q933A;
            }

            switch (lmi_type) {
            case LMI_TYPE_ANSI:
                lmi_ansi_init(port, t392, n392, n393);
                break;
            case LMI_TYPE_Q933A:
                lmi_q933a_init(port, t392, n392, n393);
                /* Automatically enable DTE polling on NNI ports when Q.933A is active */
                if (port->type == PORT_TYPE_NNI) {
                    lmi_dte_enable(port, (u8)ctx->default_n391, ctx->default_t391, (u8)ctx->default_n392_dte, (u8)ctx->default_n393_dte);
                }
                break;
            case LMI_TYPE_CISCO:
                lmi_gof_init(port, t392, n392, n393);
                break;
            default:
                LOG_DEBUG("LMI disabled on %s", port_name);
                break;
            }

            if (async_val != -1 && port->lmi_ctx) {
                vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
                lmi->async_enabled = async_val;
                LOG_INFO("LMI on %s configured async_enabled=%d", port->name, async_val);
            }

        } else if (strcmp(tokens[0], "lmi_dte") == 0) {
            /* LMI DTE configuration */
            char port_name[VFR_MAX_NAME_LEN];
            u8 n391 = (u8)ctx->default_n391;
            u8 n392 = (u8)ctx->default_n392_dte;
            u8 n393 = (u8)ctx->default_n393_dte;
            u16 t391 = ctx->default_t391;

            if (config_parse_lmi_dte(cfg, tokens[0], tokens, tok_count,
                                      port_name, sizeof(port_name),
                                      &n391, &n392, &n393, &t391) < 0) {
                continue;
            }

            vfr_port_t *dte_port = vfrs_find_port(ctx, port_name);
            if (dte_port) {
                if (dte_port->type == PORT_TYPE_UNI && !dte_port->lmi_ctx) {
                    LOG_WARN("Line %d: DTE LMI cannot be enabled on UNI port '%s' without LMI DCE configuration", line_num, port_name);
                    continue;
                }
                /* If NNI port and LMI is not initialized, implicitly enable Q.933A DCE LMI with defaults first */
                if (dte_port->type == PORT_TYPE_NNI && !dte_port->lmi_ctx) {
                    lmi_q933a_init(dte_port, ctx->default_t392, ctx->default_n392, ctx->default_n393);
                }

                if (lmi_dte_enable(dte_port, n391, t391, n392, n393) < 0) {
                    LOG_WARN("lmi_dte_enable failed for %s", port_name);
                }
            } else {
                LOG_WARN("Port '%s' not found for lmi_dte", port_name);
            }

        } else if (strcmp(tokens[0], "capture") == 0) {
            /* Packet capture configuration */
            char cap_port[VFR_MAX_NAME_LEN], filename[VFR_MAX_PATH];
            pcap_writer_t *pw;

            if (config_parse_capture(cfg, tokens[0], tokens, tok_count,
                                     cap_port, sizeof(cap_port),
                                     filename, sizeof(filename)) < 0) {
                continue;
            }

            char svc_val[64] = "";
            for (int i = 3; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "svc") == 0) {
                        snprintf(svc_val, sizeof(svc_val), "%s", val);
                    }
                }
            }

            pw = calloc(1, sizeof(pcap_writer_t));
            if (!pw) {
                LOG_ERROR("Failed to allocate capture context memory: %s", filename);
                continue;
            }
            if (pcap_writer_init(pw, filename, 65535) < 0) {
                free(pw);
                LOG_ERROR("Failed to initialize capture: %s", filename);
                continue;
            }

            if (strcmp(cap_port, "all") == 0) {
                ctx->global_capture = pw;
                if (svc_val[0] != '\0') {
                    LOG_INFO("Global capture enabled: %s (svc mode: %s - SVC not active)", filename, svc_val);
                } else {
                    LOG_INFO("Global capture enabled: %s", filename);
                }
            } else {
                vfr_port_t *port = vfrs_find_port(ctx, cap_port);
                if (port) {
                    port->capture = pw;
                    if (svc_val[0] != '\0') {
                        LOG_INFO("Port capture enabled: %s -> %s (svc mode: %s - SVC not active)", cap_port, filename, svc_val);
                    } else {
                        LOG_INFO("Port capture enabled: %s -> %s", cap_port, filename);
                    }
                } else {
                    pcap_writer_close(pw);
                    free(pw);
                    LOG_WARN("Port '%s' not found for capture", cap_port);
                }
            }

        } else if (strcmp(tokens[0], "congestion") == 0) {
            /* congestion <port_name> rate=<fps> [clear=<fps>] [threshold=<n>] [cllm=on|off] [access_rate=<bps>] */
            if (tok_count < 3) {
                LOG_ERROR("Line %d: congestion requires port name and rate", line_num);
                continue;
            }
            char *port_name = tokens[1];
            vfr_port_t *port = vfrs_find_port(ctx, port_name);
            if (!port) {
                LOG_WARN("Port '%s' not found for congestion", port_name);
                continue;
            }

            u32 rate = 0;
            u32 clear = 0;
            u32 access_rate = (ctx->access_rate > 0) ? ctx->access_rate : cgst_get_access_rate(port);
            int cllm_val = 0;
            u32 threshold = 0;

            for (int i = 2; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "rate") == 0) rate = atoi(val);
                    else if (strcmp(key, "clear") == 0) clear = atoi(val);
                    else if (strcmp(key, "access_rate") == 0) access_rate = atoi(val);
                    else if (strcmp(key, "threshold") == 0) threshold = atoi(val);
                    else if (strcmp(key, "cllm") == 0) {
                        cllm_val = (strcmp(val, "on") == 0);
                    }
                }
            }

            if (rate > 0) {
                cgst_init(port, access_rate, rate);
                if (clear > 0) {
                    cgst_set_clear_threshold(port, clear);
                }
                if (threshold > 0) {
                    cgst_set_write_failure_threshold(port, threshold);
                }
                cgst_set_cllm_enabled(port, cllm_val);
            } else {
                LOG_ERROR("Line %d: rate parameter is required and must be > 0", line_num);
            }

        } else if (strcmp(tokens[0], "lapf") == 0) {
            /* lapf <port_name> [k=<n>] [n200=<n>] [n201=<n>] [t200=<s>] [t203=<s>] */
            if (tok_count < 2) {
                LOG_ERROR("Line %d: lapf command requires port name", line_num);
                continue;
            }
            char *port_name = tokens[1];
            vfr_port_t *port = vfrs_find_port(ctx, port_name);
            if (!port) {
                LOG_WARN("Port '%s' not found for lapf", port_name);
                continue;
            }

            u8 k = ctx->default_lapf_k;
            u8 n200 = ctx->default_lapf_n200;
            u16 n201 = ctx->default_lapf_n201;
            u32 t200 = ctx->default_lapf_t200;
            u32 t203 = ctx->default_lapf_t203;
            u32 dlci = 0;
            int role_active = 0;

            for (int i = 2; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "k") == 0) k = atoi(val);
                    else if (strcmp(key, "n200") == 0) n200 = atoi(val);
                    else if (strcmp(key, "n201") == 0) n201 = atoi(val);
                    else if (strcmp(key, "t200") == 0) t200 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "t203") == 0) t203 = (u32)(atof(val) * 1000);
                    else if (strcmp(key, "dlci") == 0) dlci = (u32)atoi(val);
                    else if (strcmp(key, "role") == 0) {
                        if (strcmp(val, "active") == 0 || strcmp(val, "on") == 0) {
                            role_active = 1;
                        }
                    }
                }
            }

            lapf_port_init(port, dlci, k, n200, n201, t200, t203);
            if (role_active) {
                lapf_establish_link(port, dlci);
            }


        } else if (strcmp(tokens[0], "mcast") == 0 || strcmp(tokens[0], "mcast_group") == 0) {
            char name[VFR_MAX_NAME_LEN];
            char src_port[VFR_MAX_NAME_LEN];
            u32 src_dlci = 0;
            char mode[VFR_MAX_NAME_LEN];
            u32 cir = 0, bc = 0, be = 0;

            if (config_parse_mcast(cfg, tokens[0], tokens, tok_count,
                                   name, sizeof(name),
                                   src_port, sizeof(src_port),
                                   &src_dlci,
                                   mode, sizeof(mode),
                                   &cir, &bc, &be) == 0) {
                if (vfrs_add_mcast_group(ctx, name, src_port, src_dlci, mode, cir, bc, be) < 0) {
                    LOG_ERROR("Line %d: Failed to add multicast group '%s'", line_num, name);
                }
            }

        } else if (strcmp(tokens[0], "mcast_member") == 0) {
            char group_name[VFR_MAX_NAME_LEN];
            char mem_port[VFR_MAX_NAME_LEN];
            u32 mem_dlci = 0;

            if (config_parse_mcast_member(cfg, tokens[0], tokens, tok_count,
                                          group_name, sizeof(group_name),
                                          mem_port, sizeof(mem_port),
                                          &mem_dlci) == 0) {
                if (vfrs_add_mcast_member(ctx, group_name, mem_port, mem_dlci) < 0) {
                    LOG_ERROR("Line %d: Failed to add multicast member %s:%u to group '%s'",
                              line_num, mem_port, mem_dlci, group_name);
                }
            }

        } else if (strcmp(tokens[0], "svc_int") == 0) {
            if (tok_count < 2) {
                LOG_WARN("Line %d: svc_int requires a port name", line_num);
                continue;
            }
            char *port_name = tokens[1];
            vfr_port_t *port = vfrs_find_port(ctx, port_name);
            if (!port) {
                LOG_WARN("Line %d: Port '%s' not found for svc_int", line_num, port_name);
                continue;
            }

            u32 dlci_low = 512, dlci_high = 991;
            u8 crv_len_cfg = 0;
            u8 is_nni = (port->type == PORT_TYPE_NNI) ? 1 : 0;
            vfr_dlci_alloc_dir_t alloc_dir = is_nni ? SVC_DLCI_ALLOC_DESCENDING : SVC_DLCI_ALLOC_ASCENDING;
            char net_id[16] = {0};
            char rem_net_id[16] = {0};

            for (int i = 2; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "dlci_low") == 0) dlci_low = atoi(val);
                    else if (strcmp(key, "dlci_high") == 0) dlci_high = atoi(val);
                    else if (strcmp(key, "crv_len") == 0) {
                        if (strcmp(val, "1") == 0) crv_len_cfg = 1;
                        else if (strcmp(val, "2") == 0) crv_len_cfg = 2;
                        else crv_len_cfg = 0;
                    }
                    else if (strcmp(key, "is_nni") == 0 || strcmp(key, "type") == 0) {
                        if (strcmp(val, "1") == 0 || strcasecmp(val, "nni") == 0 || strcasecmp(val, "true") == 0) {
                            is_nni = 1;
                        } else {
                            is_nni = 0;
                        }
                    }
                    else if (strcmp(key, "dlci_side") == 0 || strcmp(key, "alloc_dir") == 0 || strcmp(key, "side") == 0) {
                        if (strcasecmp(val, "high") == 0 || strcasecmp(val, "desc") == 0 || strcasecmp(val, "descending") == 0) {
                            alloc_dir = SVC_DLCI_ALLOC_DESCENDING;
                        } else {
                            alloc_dir = SVC_DLCI_ALLOC_ASCENDING;
                        }
                    }
                    else if (strcmp(key, "network_id") == 0 || strcmp(key, "net_id") == 0) {
                        snprintf(net_id, sizeof(net_id), "%.*s", (int)(sizeof(net_id) - 1), val);
                    }
                    else if (strcmp(key, "remote_network_id") == 0 || strcmp(key, "rem_net_id") == 0) {
                        snprintf(rem_net_id, sizeof(rem_net_id), "%.*s", (int)(sizeof(rem_net_id) - 1), val);
                    }
                }
            }

            if (svc_init_port_ex(port, dlci_low, dlci_high, crv_len_cfg, is_nni, alloc_dir) < 0) {
                LOG_ERROR("Line %d: Failed to initialize SVC on port %s", line_num, port_name);
            } else {
                /* Static per-port override of SVC parameters */
                vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
                if (net_id[0]) snprintf(sctx->network_id, sizeof(sctx->network_id), "%s", net_id);
                if (rem_net_id[0]) snprintf(sctx->remote_network_id, sizeof(sctx->remote_network_id), "%s", rem_net_id);

                for (int i = 2; i < tok_count; i++) {
                    char key[64], val[64];
                    if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                        if (strcmp(key, "t301") == 0) sctx->t301_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "t303") == 0) sctx->t303_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "t305") == 0) sctx->t305_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "t308") == 0) sctx->t308_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "t310") == 0) sctx->t310_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "t316") == 0) sctx->t316_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "t317") == 0) sctx->t317_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "t322") == 0) sctx->t322_ms = (u32)(atof(val) * 1000);
                        else if (strcmp(key, "default_cir") == 0) sctx->default_cir = (u32)atol(val);
                        else if (strcmp(key, "default_bc") == 0) sctx->default_bc = (u32)atol(val);
                        else if (strcmp(key, "default_be") == 0) sctx->default_be = (u32)atol(val);
                        else if (strcmp(key, "default_fmif") == 0) sctx->default_fmif = (u16)atoi(val);
                        else if (strcmp(key, "default_ftp") == 0) sctx->default_ftp = (u8)atoi(val);
                        else if (strcmp(key, "default_fdp") == 0) sctx->default_fdp = (u8)atoi(val);
                        else if (strcmp(key, "default_svc_class") == 0) sctx->default_svc_class = (u8)atoi(val);
                    }
                }
                /* (D7) Conditional LAPF initialization only when SVC signaling (svc_int) is configured on this port */
                if (!port_get_lapf_ctx(port, 0)) {
                    lapf_port_init(port, 0, ctx->default_lapf_k, ctx->default_lapf_n200,
                                   ctx->default_lapf_n201, ctx->default_lapf_t200, ctx->default_lapf_t203);
                }
            }

        } else if (strcmp(tokens[0], "svc_addr") == 0) {
            if (tok_count < 3) {
                LOG_WARN("Line %d: svc_addr requires port name and number", line_num);
                continue;
            }
            char *port_name = tokens[1];
            const char *mode = "manual";
            const char *num_type = "x121";
            const char *primary_num = "";
            int arg_idx = 2;

            if (arg_idx < tok_count && (strcasecmp(tokens[arg_idx], "manual") == 0 || strcasecmp(tokens[arg_idx], "autoprefix") == 0)) {
                mode = tokens[arg_idx];
                arg_idx++;
            }

            if (arg_idx < tok_count && (strcasecmp(tokens[arg_idx], "x121") == 0 || strcasecmp(tokens[arg_idx], "e164") == 0)) {
                num_type = tokens[arg_idx];
                arg_idx++;
            }

            if (arg_idx < tok_count) {
                primary_num = tokens[arg_idx];
                arg_idx++;
            }

            const char *alias_type = "x121";
            char alias_num[SVC_MAX_ADDR_LEN + 1] = {0};
            int rev_charge_acc = 1; /* Default is accepted */
            int rev_charge_prev = 0; /* Default is not prevented */

            for (int i = arg_idx; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "alias") == 0) {
                        char *comma = strchr(val, ',');
                        if (comma) {
                            *comma = '\0';
                            alias_type = val;
                            snprintf(alias_num, sizeof(alias_num), "%.*s", (int)(sizeof(alias_num) - 1), comma + 1);
                        } else {
                            snprintf(alias_num, sizeof(alias_num), "%.*s", (int)(sizeof(alias_num) - 1), val);
                        }
                    } else if (strcmp(key, "rev_charge_acc") == 0) {
                        rev_charge_acc = atoi(val);
                    } else if (strcmp(key, "rev_charge_prev") == 0) {
                        rev_charge_prev = atoi(val);
                    }
                }
            }

            if (svc_numbering_add_mode(ctx, port_name, mode, num_type, primary_num, alias_type, alias_num, rev_charge_acc, rev_charge_prev) < 0) {
                LOG_ERROR("Line %d: Failed to register subscriber number for port %s", line_num, port_name);
            }

        } else if (strcmp(tokens[0], "svc_route") == 0) {
            if (tok_count < 3) {
                LOG_WARN("Line %d: svc_route requires parameters", line_num);
                continue;
            }

            char prefix[SVC_MAX_ADDR_LEN + 1] = {0};
            char egress_port[VFR_MAX_NAME_LEN] = {0};
            char dnic[16] = {0}, sgc[16] = {0}, sic[16] = {0};
            char tns[16] = {0};
            u8 metric = 10;
            int is_structural = 0;

            /* Check syntax variants:
             * 1. svc_route prefix=<pfx> port=<port> [tns=<tns>] [metric=<m>]
             * 2. svc_route <port> [x121|e164] prefix=<pfx> [tns=<tns>] [metric=<m>]
             * 3. svc_route <port> x121 dnic=<dnic> [sgc=<sgc>] [sic=<sic>] [tns=<tns>] [metric=<m>]
             * 4. svc_route <prefix> <port> [tns=<tns>] [metric=<m>]
             */
            if (strncmp(tokens[1], "prefix=", 7) == 0) {
                for (int i = 1; i < tok_count; i++) {
                    char key[64], val[64];
                    if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                        if (strcmp(key, "prefix") == 0) snprintf(prefix, sizeof(prefix), "%.*s", (int)(sizeof(prefix) - 1), val);
                        else if (strcmp(key, "port") == 0) snprintf(egress_port, sizeof(egress_port), "%.*s", (int)(sizeof(egress_port) - 1), val);
                        else if (strcmp(key, "tns") == 0 || strcmp(key, "transit_net_id") == 0) snprintf(tns, sizeof(tns), "%.*s", (int)(sizeof(tns) - 1), val);
                        else if (strcmp(key, "metric") == 0 || strcmp(key, "cost") == 0) metric = (u8)atoi(val);
                    }
                }
            } else if (vfrs_find_port(ctx, tokens[1]) != NULL) {
                /* tokens[1] is egress port */
                snprintf(egress_port, sizeof(egress_port), "%.*s", (int)(sizeof(egress_port) - 1), tokens[1]);
                int start_idx = 2;
                if (start_idx < tok_count && (strcasecmp(tokens[start_idx], "x121") == 0 || strcasecmp(tokens[start_idx], "e164") == 0)) {
                    start_idx++;
                }
                for (int i = start_idx; i < tok_count; i++) {
                    char key[64], val[64];
                    if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                        if (strcmp(key, "prefix") == 0) snprintf(prefix, sizeof(prefix), "%.*s", (int)(sizeof(prefix) - 1), val);
                        else if (strcmp(key, "dnic") == 0) { snprintf(dnic, sizeof(dnic), "%.*s", (int)(sizeof(dnic) - 1), val); is_structural = 1; }
                        else if (strcmp(key, "sgc") == 0) { snprintf(sgc, sizeof(sgc), "%.*s", (int)(sizeof(sgc) - 1), val); is_structural = 1; }
                        else if (strcmp(key, "sic") == 0) { snprintf(sic, sizeof(sic), "%.*s", (int)(sizeof(sic) - 1), val); is_structural = 1; }
                        else if (strcmp(key, "tns") == 0 || strcmp(key, "transit_net_id") == 0) snprintf(tns, sizeof(tns), "%.*s", (int)(sizeof(tns) - 1), val);
                        else if (strcmp(key, "metric") == 0 || strcmp(key, "cost") == 0) metric = (u8)atoi(val);
                    } else if (i == start_idx && !is_structural) {
                        /* Positional prefix after port */
                        snprintf(prefix, sizeof(prefix), "%.*s", (int)(sizeof(prefix) - 1), tokens[i]);
                    }
                }
            } else {
                /* Positional: svc_route <prefix> <port> [tns=<tns>] [metric=<m>] */
                snprintf(prefix, sizeof(prefix), "%.*s", (int)(sizeof(prefix) - 1), tokens[1]);
                snprintf(egress_port, sizeof(egress_port), "%.*s", (int)(sizeof(egress_port) - 1), tokens[2]);
                for (int i = 3; i < tok_count; i++) {
                    char key[64], val[64];
                    if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                        if (strcmp(key, "tns") == 0 || strcmp(key, "transit_net_id") == 0) snprintf(tns, sizeof(tns), "%.*s", (int)(sizeof(tns) - 1), val);
                        else if (strcmp(key, "metric") == 0 || strcmp(key, "cost") == 0) metric = (u8)atoi(val);
                    }
                }
            }

            int rc = 0;
            if (is_structural) {
                rc = svc_route_add_structural_ex(ctx, egress_port, dnic, sgc, sic, tns[0] ? tns : NULL, metric);
            } else {
                rc = svc_route_add_ex(ctx, prefix, egress_port, tns[0] ? tns : NULL, metric);
            }
            if (rc < 0) {
                LOG_ERROR("Line %d: Failed to add SVC route egress_port=%s", line_num, egress_port);
            }

        } else {
            LOG_WARN("Unknown command at line %d: %s", line_num, tokens[0]);
        }
    }

    if (result != CFG_EOF) {
        LOG_ERROR("Configuration error at line %d (Pass 2)", line_num);
        config_destroy(cfg);
        return -1;
    }
    config_destroy(cfg);

    /* PASS 3: Parse CLLM settings last (ensures explicit cllm commands override congestion settings) */
    LOG_INFO("Parsing configuration (Pass 3 - CLLM Settings): %s", filename);
    cfg = config_create(filename);
    if (!cfg) {
        return -1;
    }

    while ((result = config_next_line(cfg, line, sizeof(line))) == CFG_OK) {
        line_num = config_error_line(cfg);

        /* Tokenize line */
        tok_count = parse_config_line(line, tokens);

        if (tok_count == 0) continue;

        /* Process ONLY cllm commands in Pass 3 */
        if (strcmp(tokens[0], "cllm") == 0) {
            if (tok_count < 2) {
                LOG_ERROR("Line %d: cllm requires port name", line_num);
                continue;
            }
            char *port_name = tokens[1];
            vfr_port_t *port = vfrs_find_port(ctx, port_name);
            if (!port) {
                LOG_WARN("Port '%s' not found for cllm", port_name);
                continue;
            }

            u32 tx = 10; // Default 10s
            for (int i = 2; i < tok_count; i++) {
                char key[64], val[64];
                if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                    if (strcmp(key, "tx") == 0) {
                        int parsed_tx = atoi(val);
                        if (parsed_tx < 5 || parsed_tx > 30) {
                            LOG_WARN("Line %d: CLLM tx interval (%d) is out of range (5-30s). Using default 10s.", line_num, parsed_tx);
                            tx = 10;
                        } else {
                            tx = (u32)parsed_tx;
                        }
                    }
                }
            }

            cgst_set_cllm_enabled(port, 1);
            cgst_set_cllm_tx_interval(port, tx * 1000);
            LOG_INFO("CLLM explicitly enabled on %s: tx=%us", port->name, tx);
        }
    }

    if (result != CFG_EOF) {
        LOG_ERROR("Configuration error at line %d (Pass 3)", line_num);
        config_destroy(cfg);
        return -1;
    }

    config_destroy(cfg);

    /* (F17) Validate config for inconsistencies after all passes have completed */
    vfrs_validate_config(ctx);

    return 0;
}

/* Main event loop */
/* Slow Path Thread Function (Thread 2) */
thread_ret_t THREAD_CALL run_slow_path_thread(void *arg)
{
    vfrs_ctx_t *ctx = (vfrs_ctx_t *)arg;
    vfr_ctrl_frame_t ctrl_frame;
    vfr_port_t *ports_copy[MAX_PORTS];
    int i;

    LOG_INFO("Starting slow-path control plane thread...");

    while (g_running) {
        /* 1. Process signaling queue frames from Thread 1 */
        while (spsc_queue_pop(&ctx->sig_queue, &ctrl_frame) == 0) {
            vfr_port_t *port = NULL;
            mutex_lock(&ctx->port_mutex);
            if (ctrl_frame.src_port_idx < ctx->port_count) {
                port = ctx->ports[ctrl_frame.src_port_idx];
            }
            mutex_unlock(&ctx->port_mutex);

            if (port) {
                /* Decode frame address to route to LAPF/LMI */
                fr_addr_t addr;
                fr_decode_addr(ctrl_frame.data, &addr);
                /* Execute full fr_switch_input_processed on Thread 2 (is_slow_path=1) */
                fr_switch_input_processed(ctx, port, &addr, ctrl_frame.data, ctrl_frame.len);
            }
        }

        /* 2. Run timer ticks for all ports */
        mutex_lock(&ctx->port_mutex);
        int count = ctx->port_count;
        for (i = 0; i < count; i++) {
            ports_copy[i] = ctx->ports[i];
        }
        mutex_unlock(&ctx->port_mutex);

        for (i = 0; i < count; i++) {
            vfr_port_t *port = ports_copy[i];
            if (port) {
                if (port->lmi_ctx) {
                    lmi_poll_timer(port);
                }
                if (port->cgst_ctx) {
                    cgst_poll_timer(port);
                }
                if (port->lapf_ctx_count > 0) {
                    lapf_poll_timer(port);
                }
                if (port->svc_ctx) {
                    svc_poll_timers(port);
                }
            }
        }

        /* Tick interval of 100ms */
        msleep(100);
    }

    LOG_INFO("Slow-path control plane thread exited");
    return THREAD_RET;
}

/* Main event loop */
static int run_main_loop(vfrs_ctx_t *ctx)
{
    vfr_pollfd_t fds[MAX_PORTS];
    vfr_port_t *poll_ports[MAX_PORTS];
    int nfds = 0;
    int ret;
    u8 buf[2048];
    int i;

    LOG_INFO("Starting fast-path main loop (poll/WSAPoll)...");

    while (g_running) {
        /* Build pollfd array under port_mutex */
        nfds = 0;
        mutex_lock(&ctx->port_mutex);
        for (i = 0; i < ctx->port_count && nfds < MAX_PORTS; i++) {
            vfr_port_t *port = ctx->ports[i];
            if (port && port->fd != INVALID_SOCKET && port->status != PORT_STATUS_DOWN && port->ops->poll != NULL) {
                fds[nfds].fd = port->fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                poll_ports[nfds] = port;
                nfds++;
            }
        }
        mutex_unlock(&ctx->port_mutex);

        if (nfds == 0) {
            /* No ports to poll, sleep briefly to avoid busy wait */
            msleep(100);
            continue;
        }

        /* Wait for events with 100ms timeout */
        ret = VFR_POLL(fds, nfds, 100);

        if (ret < 0) {
            if (!g_running) break;
            msleep(10);
            continue;
        }

        if (ret > 0) {
            for (i = 0; i < nfds; i++) {
                if (fds[i].revents & POLLIN) {
                    vfr_port_t *port = poll_ports[i];
                    /* Receive frame */
                    int len = port->ops->recv(port, buf, sizeof(buf));
                    if (len > 0) {
                        /* Process frame */
                        fr_switch_input(ctx, port, buf, len);
                    } else if (len < 0 && port->transport == PORT_TRANS_TCP_CLI) {
                        /* TCP client reconnection */
                        tcp_priv_t *priv = (tcp_priv_t *)port->priv;
                        if (priv && !priv->connected) {
                            msleep(priv->reconnect_delay);
                            tcp_connect(port);
                        }
                    }
                }
            }
        }
    }

    LOG_INFO("Main loop exited");
    return 0;
}

/* Console command processing */
static thread_ret_t THREAD_CALL process_console_command(void *arg)
{
    vfrs_ctx_t *ctx = (vfrs_ctx_t *)arg;
    char line[256];

    while (g_running) {
        printf("vfrs> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        /* Trim newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            g_running = 0;
        } else if (strcmp(line, "show config") == 0 || strcmp(line, "show running-config") == 0) {
            vfrs_show_config(ctx, stdout);
        } else if (strcmp(line, "show defaults") == 0) {
            vfrs_show_defaults(ctx, stdout);
        } else if (strcmp(line, "show swconfig") == 0) {
            vfrs_show_swconfig(ctx, stdout);
        } else if (strcmp(line, "show ports") == 0) {
            vfrs_show_ports(ctx, stdout);
        } else if (strcmp(line, "show pvc") == 0) {
            vfrs_show_pvcs(ctx, stdout);
        } else if (strcmp(line, "show stats") == 0) {
            vfrs_show_stats(ctx, NULL, stdout);
        } else if (strncmp(line, "show stats ", 11) == 0) {
            vfrs_show_stats(ctx, line + 11, stdout);
        } else if (strcmp(line, "show svc calls") == 0) {
            vfrs_show_svc_calls(ctx, stdout);
        } else if (strncmp(line, "show svc call ", 14) == 0) {
            vfrs_show_svc_call_detail(ctx, line + 14, stdout);
        } else if (strcmp(line, "show svc stats") == 0 || strcmp(line, "show svc statistics") == 0) {
            vfrs_show_svc_stats(ctx, stdout);
        } else if (strcmp(line, "show svc subscribers") == 0) {
            vfrs_show_svc_subscribers(ctx, stdout);
        } else if (strcmp(line, "show svc routes") == 0) {
            vfrs_show_svc_routes(ctx, stdout);
        } else if (strcmp(line, "help") == 0) {
            printf("Commands:\n");
            printf("  show config            - Show parsed running configuration\n");
            printf("  show defaults          - Show global default parameters\n");
            printf("  show swconfig          - Show switch identity & numbering plan\n");
            printf("  show ports             - Show port status\n");
            printf("  show pvc               - Show PVC table\n");
            printf("  show stats             - Show all statistics\n");
            printf("  show stats <port>       - Show port statistics\n");
            printf("  show svc calls         - Show active SVC call references and states\n");
            printf("  show svc call <crv>    - Show detailed parameters of specific SVC call\n");
            printf("  show svc stats         - Show SVC signaling statistics summary\n");
            printf("  show svc subscribers   - Show registered SVC subscribers per port\n");
            printf("  show svc routes        - Show hierarchical SVC NNI routing table\n");
            printf("  svc restart <port>     - Initiate ITU-T X.76 interface RESTART procedure\n");
            printf("  svc clear <port> <crv> - Force clear an active SVC call\n");
            printf("  pvc add <p_in> <d_in> <p_out> <d_out> [cir <n>] [bc <n>] [be <n>] - Dynamically add PVC\n");
            printf("  pvc del <p_in> <d_in>  - Dynamically delete PVC\n");
            printf("  lmi set <port> type <ansi|q933a|cisco> - Dynamically set LMI type\n");
            printf("  congestion set <port> cir <n> bc <n> be <n> - Update congestion settings on port\n");
            printf("  reload <config_file>   - Reload configuration file\n");
            printf("  clear stats [<port>]   - Clear statistics on port(s)\n");
            printf("  help                   - Show this help\n");
            printf("  quit                   - Exit program\n");
        } else if (line[0] != '\0') {
            /* Parse console command using tokens (D11/F1) */
            char tokens[64][256];
            char line_copy[256];
            strncpy(line_copy, line, sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';
            int tok_count = parse_config_line(line_copy, tokens);

            if (tok_count >= 6 && strcmp(tokens[0], "pvc") == 0 && strcmp(tokens[1], "add") == 0) {
                char *port_in = tokens[2];
                u32 dlci_in = atoi(tokens[3]);
                char *port_out = tokens[4];
                u32 dlci_out = atoi(tokens[5]);
                u32 cir = 0, bc = 0, be = 0;
                for (int i = 6; i < tok_count; i++) {
                    char key[64], val[64];
                    if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                        if (strcmp(key, "cir") == 0) cir = atoi(val);
                        else if (strcmp(key, "bc") == 0) bc = atoi(val);
                        else if (strcmp(key, "be") == 0) be = atoi(val);
                    }
                }
                if (vfrs_add_pvc(ctx, port_in, dlci_in, port_out, dlci_out, cir, bc, be, 0, 0, 0) == 0) {
                    printf("PVC added successfully: %s:%u -> %s:%u\n", port_in, dlci_in, port_out, dlci_out);
                } else {
                    printf("Failed to add PVC\n");
                }
            } else if (tok_count >= 4 && strcmp(tokens[0], "pvc") == 0 && strcmp(tokens[1], "del") == 0) {
                char *port_in = tokens[2];
                u32 dlci_in = atoi(tokens[3]);
                if (vfrs_del_pvc(ctx, port_in, dlci_in) == 0) {
                    printf("PVC deleted successfully: %s:%u\n", port_in, dlci_in);
                } else {
                    printf("Failed to delete PVC\n");
                }
            } else if (tok_count >= 5 && strcmp(tokens[0], "lmi") == 0 && strcmp(tokens[1], "set") == 0 && strcmp(tokens[3], "type") == 0) {
                char *port_name = tokens[2];
                vfr_port_t *port = vfrs_find_port(ctx, port_name);
                if (!port) {
                    printf("Port '%s' not found\n", port_name);
                } else {
                    int type = LMI_TYPE_Q933A;
                    if (strcmp(tokens[4], "ansi") == 0) type = LMI_TYPE_ANSI;
                    else if (strcmp(tokens[4], "cisco") == 0) type = LMI_TYPE_CISCO;
                    else if (strcmp(tokens[4], "q933a") == 0) type = LMI_TYPE_Q933A;
                    
                    if (port->lmi_ctx) {
                        vfr_lmi_state_t *lmi = (vfr_lmi_state_t *)port->lmi_ctx;
                        lmi->type = type;
                        printf("Port %s LMI type set to %s\n", port_name, tokens[4]);
                    } else {
                        printf("LMI is not initialized on port %s\n", port_name);
                    }
                }
            } else if (tok_count >= 4 && strcmp(tokens[0], "congestion") == 0 && strcmp(tokens[1], "set") == 0) {
                char *port_name = tokens[2];
                vfr_port_t *port = vfrs_find_port(ctx, port_name);
                if (!port) {
                    printf("Port '%s' not found\n", port_name);
                } else {
                    u32 cir = 0, bc = 0, be = 0;
                    for (int i = 3; i < tok_count; i++) {
                        char key[64], val[64];
                        if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                            if (strcmp(key, "cir") == 0) cir = atoi(val);
                            else if (strcmp(key, "bc") == 0) bc = atoi(val);
                            else if (strcmp(key, "be") == 0) be = atoi(val);
                        }
                    }
                    (void)bc;
                    (void)be;
                    u32 current_ar = cgst_get_access_rate(port);
                    u32 target_cir = cir ? cir : (current_ar ? current_ar : ctx->access_rate);
                    cgst_init(port, target_cir, 1000);
                    if (port->cgst_ctx) {
                        /* To update the internal token bucket bounds we re-trigger it */
                        cgst_free(port);
                        cgst_init(port, target_cir, 1000);
                        printf("Congestion/Policer updated on %s (CIR/AR=%u)\n", port_name, target_cir);
                    }
                }
            } else if (tok_count >= 2 && strcmp(tokens[0], "reload") == 0) {
                char *config_file = tokens[1];
                printf("Reloading configuration from: %s...\n", config_file);

                /* Safe dynamic table cleanup before reloading */
                svc_route_init(ctx);
                svc_numbering_init(ctx);

                for (int i = 0; i < ctx->port_count; i++) {
                    vfr_port_t *p = ctx->ports[i];
                    if (p) {
                        if (p->lmi_ctx) {
                            lmi_free(p);
                            p->lmi_ctx = NULL;
                        }
                        if (p->capture) {
                            pcap_writer_close(p->capture);
                            free(p->capture);
                            p->capture = NULL;
                        }
                    }
                }
                if (ctx->global_capture) {
                    pcap_writer_close(ctx->global_capture);
                    free(ctx->global_capture);
                    ctx->global_capture = NULL;
                }

                if (parse_config(ctx, config_file) == 0) {
                    vfrs_validate_config(ctx);
                    printf("Configuration reloaded successfully\n");
                } else {
                    printf("Failed to reload configuration\n");
                }
            } else if (tok_count >= 2 && strcmp(tokens[0], "clear") == 0 && strcmp(tokens[1], "stats") == 0) {
                char *port_name = (tok_count >= 3) ? tokens[2] : NULL;
                if (port_name) {
                    vfr_port_t *port = vfrs_find_port(ctx, port_name);
                    if (port) {
                        port_stats_init(port);
                        printf("Statistics cleared for port %s\n", port_name);
                    } else {
                        printf("Port '%s' not found\n", port_name);
                    }
                } else {
                    ASSERT_LOCK_ORDER(LOCK_LEVEL_PORT);
                    mutex_lock(&ctx->port_mutex);
                    for (int i = 0; i < ctx->port_count; i++) {
                        if (ctx->ports[i]) {
                            port_stats_init(ctx->ports[i]);
                        }
                    }
                    mutex_unlock(&ctx->port_mutex);
                    printf("All port statistics cleared\n");
                }
            } else if (tok_count >= 3 && strcmp(tokens[0], "svc") == 0 && strcmp(tokens[1], "restart") == 0) {
                char *port_name = tokens[2];
                vfr_port_t *port = vfrs_find_port(ctx, port_name);
                if (!port || !port->svc_ctx) {
                    printf("Port '%s' not found or SVC not enabled\n", port_name);
                } else {
                    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
                    if (!sctx->is_nni) {
                        printf("Port '%s' is UNI. Interface restart is only applicable to NNI interfaces.\n", port_name);
                    } else {
                        svc_nni_initiate_restart(port);
                        printf("Initiated ITU-T X.76 interface RESTART procedure on %s\n", port_name);
                    }
                }
            } else if (tok_count >= 4 && strcmp(tokens[0], "svc") == 0 && strcmp(tokens[1], "clear") == 0) {
                char *port_name = tokens[2];
                u16 target_crv = (u16)strtoul(tokens[3], NULL, 0);
                vfr_port_t *port = vfrs_find_port(ctx, port_name);
                if (!port || !port->svc_ctx) {
                    printf("Port '%s' not found or SVC not enabled\n", port_name);
                } else {
                    vfr_svc_ctx_t *sctx = (vfr_svc_ctx_t *)port->svc_ctx;
                    vfr_call_t *call = svc_find_call(sctx, target_crv, 0);
                    if (!call) call = svc_find_call(sctx, target_crv, 1);
                    if (!call) {
                        printf("Call with CRV 0x%04X not found on port %s\n", target_crv, port_name);
                    } else {
                        if (sctx->is_nni) {
                            svc_nni_initiate_clearing(port, sctx, call, Q850_CAUSE_NORMAL_CLEARING);
                        } else {
                            svc_initiate_clearing_before_active(port, sctx, call, Q850_CAUSE_NORMAL_CLEARING);
                        }
                        printf("Cleared SVC call CRV 0x%04X on port %s\n", target_crv, port_name);
                    }
                }
            } else {
                printf("Unknown command: %s\n", line);
            }
        }
    }

    return THREAD_RET;
}

/* Main function */
int main(int argc, char *argv[])
{
    vfrs_ctx_t *ctx;
    const char *config_file = NULL;
    const char *log_file = NULL;
    int console_only = 0;
    size_t cmd_log_size = 0;
    int cmd_log_files = 0;
    int check_only = 0;
    int i;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--check-config") == 0 || strcmp(argv[i], "--test-config") == 0) {
            check_only = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            g_log_console = LOG_DEBUG;
            g_log_file = LOG_DEBUG;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--console") == 0) {
            console_only = 1;
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--log-size") == 0) && i + 1 < argc) {
            cmd_log_size = (size_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--log-files") == 0) && i + 1 < argc) {
            cmd_log_files = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            config_file = argv[i];
        }
    }

    /* Initialize winsock */
    if (winsock_init() < 0) {
        fprintf(stderr, "Failed to initialize Winsock\n");
        return 1;
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Create switch context */
    ctx = vfrs_create("vfrs0");
    if (!ctx) {
        fprintf(stderr, "Failed to create VFRS context\n");
        return 1;
    }

    if (check_only) {
        if (!config_file) {
            fprintf(stderr, "Error: Option -t / --check-config requires a configuration file argument\n");
            vfrs_destroy(ctx);
            winsock_shutdown();
            return 1;
        }
        logger_init(NULL, LOG_INFO, LOG_DEBUG);
        printf("\n======================================================================\n");
        printf("VFRS Configuration Check & Dry-Run: %s\n", config_file);
        printf("======================================================================\n\n");
        int parse_rc = parse_config(ctx, config_file);
        int val_rc = (parse_rc == 0) ? vfrs_validate_config(ctx) : -1;

        int total_pvcs = 0;
        for (int p = 0; p < PVC_HASH_SIZE; p++) {
            for (vfr_pvc_detail_t *pv = ctx->pvc_table[p]; pv; pv = pv->next) total_pvcs++;
        }
        int total_mcast = 0;
        for (vfr_mcast_group_t *mg = ctx->mcast_groups; mg; mg = mg->next) total_mcast++;

        printf("\n=== Parsed Configuration Summary ===\n");
        printf("Switch ID:        %s (DNIC=%04u, PNIC=%u, SGC=%u, SIC=%u)\n",
               ctx->swid[0] ? ctx->swid : "(default)", ctx->dnic, ctx->pnic, ctx->sgc, ctx->sic);
        printf("Configured Ports: %d\n", ctx->port_count);
        printf("Configured PVCs:  %d\n", total_pvcs / 2);
        printf("Multicast Groups: %d\n", total_mcast);
        printf("SVC Subscribers:  %d\n", ctx->svc_subscriber_count);
        printf("SVC Routes:       %d\n", ctx->svc_route_count);
        printf("Default Access Rate: %u bps\n", ctx->access_rate);
        printf("Default SVC CIR:  %u bps (Bc=%u, Be=%u)\n", ctx->default_cir, ctx->default_bc, ctx->default_be);
        printf("Syntax Parsing:   %s\n", parse_rc == 0 ? "PASSED (0 errors)" : "FAILED");
        printf("Cross Validation: %s\n", val_rc == 0 ? "PASSED (0 errors)" : "FAILED");

        int valid = (parse_rc == 0 && val_rc == 0);
        printf("\nOverall Result:   >>> CONFIGURATION %s <<<\n\n", valid ? "VALID" : "INVALID");
        fflush(stdout);
        fflush(stderr);

        vfrs_destroy(ctx);
        logger_shutdown();
        winsock_shutdown();
        return valid ? 0 : 1;
    }

    /* Open log file */
    if (!console_only) {
        log_file = "vfrs.log";
    }
    if (logger_init(log_file, g_log_console, LOG_DEBUG) < 0) {
        fprintf(stderr, "Failed to initialize logger\n");
        vfrs_destroy(ctx);
        return 1;
    }

    /* Set command-line log rotation parameters if provided */
    if (cmd_log_size > 0 || cmd_log_files > 0) {
        logger_set_rotation(cmd_log_size * 1024 * 1024, cmd_log_files);
    }

    LOG_INFO("VFRS - Virtual Frame Relay Switch");
    LOG_INFO("Build: " __DATE__ " " __TIME__);

    /* Parse configuration file */
    if (config_file) {
        if (parse_config(ctx, config_file) < 0) {
            LOG_ERROR("Failed to parse configuration file");
            vfrs_destroy(ctx);
            return 1;
        }
    } else {
        LOG_INFO("No configuration file specified, running in interactive mode");
    }

    /* Start slow path thread */
    if (thread_create(&ctx->slow_path_thread, run_slow_path_thread, ctx) != 0) {
        LOG_ERROR("Failed to create slow path thread");
        vfrs_destroy(ctx);
        return 1;
    }

    if (console_only) {
        thread_t console_thread;
        if (thread_create(&console_thread, process_console_command, ctx) == 0) {
            thread_detach(console_thread);
        }
    }

    /* Run main loop or interactive console */
    run_main_loop(ctx);

    /* Cleanup */
    LOG_INFO("Shutting down VFRS...");
    g_running = 0;
    vfrs_shutdown_svc(ctx);  /* Tear down active SVC calls gracefully (D8) */
    thread_join(ctx->slow_path_thread, NULL);
    vfrs_destroy(ctx);
    logger_shutdown();
    winsock_shutdown();

    LOG_INFO("VFRS terminated");
    return 0;
}