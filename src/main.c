/*
 * vfrs_main.c - VFRS Main Entry Point
 * Virtual Frame Relay Switch
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vfr.h"
#include "vfr/config.h"
#include "svc/svc_sig_common.h"
#include "svc/svc_sig_uni.h"
#include "svc/svc_sig_nni.h"
#include "svc/svc_sig_iel.h"
#include "svc/svc_spvc.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "switching/svc_routing_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* Global context */
_Atomic int g_running = 1;   /* defined here; declared extern _Atomic int in vfr.h */

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
    LOG_INFO("Parsing configuration: %s", filename);
    int rc = cfg_compile_file(ctx, filename);
    if (rc < 0) {
        LOG_ERROR("Configuration compilation failed for '%s'", filename);
        return -1;
    }
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
                if (port->lapf_count > 0) {
                    lapf_poll_timer(port);
                }
                if (port->svc_ctx) {
                    svc_poll_timers(port);
                }
            }
        }

        /* 3. Run SPVC periodic state machine & retry timers */
        spvc_poll_timers(ctx);

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
        } else if (strcmp(line, "show spvc") == 0 || strcmp(line, "show spvcs") == 0) {
            vfrs_show_spvcs(ctx, stdout);
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
            printf("  capture <port|all> <filename>          - Start live PCAP packet capture\n");
            printf("  capture stop <port|all>                - Stop live PCAP packet capture\n");
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
            } else if (tok_count >= 3 && strcmp(tokens[0], "capture") == 0 && strcmp(tokens[1], "stop") == 0) {
                char *target = tokens[2];
                if (strcasecmp(target, "all") == 0 || strcasecmp(target, "global") == 0) {
                    if (ctx->global_capture) {
                        pcap_writer_close(ctx->global_capture);
                        free(ctx->global_capture);
                        ctx->global_capture = NULL;
                        printf("Global packet capture stopped\n");
                    } else {
                        printf("Global packet capture is not active\n");
                    }
                } else {
                    vfr_port_t *port = vfrs_find_port(ctx, target);
                    if (!port) {
                        printf("Port '%s' not found\n", target);
                    } else if (port->capture) {
                        pcap_writer_close(port->capture);
                        free(port->capture);
                        port->capture = NULL;
                        printf("Packet capture stopped on %s\n", target);
                    } else {
                        printf("Packet capture is not active on %s\n", target);
                    }
                }
            } else if (tok_count >= 3 && strcmp(tokens[0], "capture") == 0) {
                char *target = tokens[1];
                char *pcap_file = tokens[2];
                if (strcasecmp(target, "all") == 0 || strcasecmp(target, "global") == 0) {
                    if (ctx->global_capture) {
                        pcap_writer_close(ctx->global_capture);
                        free(ctx->global_capture);
                        ctx->global_capture = NULL;
                    }
                    ctx->global_capture = calloc(1, sizeof(pcap_writer_t));
                    if (ctx->global_capture && pcap_writer_init(ctx->global_capture, pcap_file, 65535) == 0) {
                        printf("Global packet capture started: %s\n", pcap_file);
                    } else {
                        printf("Failed to start global packet capture: %s\n", pcap_file);
                    }
                } else {
                    vfr_port_t *port = vfrs_find_port(ctx, target);
                    if (!port) {
                        printf("Port '%s' not found\n", target);
                    } else {
                        if (port->capture) {
                            pcap_writer_close(port->capture);
                            free(port->capture);
                            port->capture = NULL;
                        }
                        port->capture = calloc(1, sizeof(pcap_writer_t));
                        if (port->capture && pcap_writer_init(port->capture, pcap_file, 65535) == 0) {
                            printf("Packet capture started on %s: %s\n", target, pcap_file);
                        } else {
                            printf("Failed to start packet capture on %s: %s\n", target, pcap_file);
                        }
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
    spvc_init(ctx);

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