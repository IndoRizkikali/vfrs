/*
 * cgst_mgnt.c - Congestion Management
 * VFRS - Virtual Frame Relay Switch
 * X.36 Clause 12 - Congestion Detection and Control
 */

#include "vfr.h"

#include <string.h>

/* cgst_pvc_tb_init is declared in vfr.h */

/* Congestion state per port */
typedef struct {
    u32         access_rate;     /* Physical access rate (bps) */
    u32         rate_threshold;   /* Frames/sec to trigger congestion */
    u32         clear_threshold;  /* Frames/sec to clear congestion */
    int         congested;        /* Congestion state */
    u64         last_rate_check;  /* For rate calculation */
    u32         frames_since_check;
    token_bucket_t tb;            /* Token bucket for traffic shaping */
    int         cllm_enabled;     /* Enable CLLM */
    u32         cllm_tx_interval; /* CLLM tx interval in ms */
    vfr_timer_t cllm_timer;       /* CLLM timer */
    u64         congested_start_time; /* Timestamp when congestion was entered */
    u32         write_failure_threshold;
    u32         write_failure_count;
} cgst_state_t;

/* Initialize congestion state */
int cgst_init(vfr_port_t *port, u32 access_rate, u32 rate_threshold)
{
    cgst_state_t *cgst;

    if (!port) return -1;

    cgst = (cgst_state_t *)port->cgst_ctx;
    if (!cgst) {
        cgst = calloc(1, sizeof(cgst_state_t));
        if (!cgst) return -1;
        port->cgst_ctx = cgst;
    }

    cgst->access_rate = access_rate;
    cgst->rate_threshold = rate_threshold;
    cgst->clear_threshold = rate_threshold / 2;
    cgst->congested = 0;
    cgst->last_rate_check = get_tick_count();
    cgst->frames_since_check = 0;

    /* Initialize token bucket */
    cgst->tb.cir = access_rate;
    cgst->tb.bc = access_rate;
    cgst->tb.be = access_rate / 2;
    cgst->tb.tokens = cgst->tb.bc + cgst->tb.be;  /* Start full */
    cgst->tb.last_update = get_tick_count();

    /* Initialize CLLM properties */
    cgst->cllm_enabled = 0;
    cgst->cllm_tx_interval = 10000; /* Default 10s */
    timer_cancel(&cgst->cllm_timer);

    /* Initialize write failure threshold */
    cgst->write_failure_threshold = 0;
    cgst->write_failure_count = 0;

    LOG_INFO("Congestion management initialized on %s (AR=%u bps, threshold=%u fps)",
             port->name, access_rate, rate_threshold);

    return 0;
}

/* Initialize token bucket for individual DLCI entry */
void cgst_dlci_tb_init(vfr_dlci_entry_t *entry, vfr_port_t *port, u32 cir, u32 bc, u32 be)
{
    if (!entry) return;

    if (cir > 0) {
        entry->tb.cir = cir;
        entry->tb.bc = bc;
        entry->tb.be = be;
        entry->tb.tokens = bc + be;
    } else {
        /* Best effort (CIR = 0) per I.370:
         * Replenish at physical access rate, max tokens = Be, all frames marked DE */
        u32 ar = port ? cgst_get_access_rate(port) : 0;
        entry->tb.cir = ar;
        entry->tb.bc = 0;
        entry->tb.be = be;
        entry->tb.tokens = entry->tb.be;
    }
    entry->tb.last_update = get_tick_count();
}

/* Update token bucket */
static void tb_update(token_bucket_t *tb, u64 now)
{
    u64 elapsed;
    u64 tokens_added;

    if (!tb || tb->cir == 0) return;

    elapsed = now - tb->last_update;
    if (elapsed > 10000) elapsed = 10000;  /* Cap at 10 seconds */

    /* Add tokens based on CIR: tokens += (elapsed_ms * cir_bps) / 1000 */
    tokens_added = (elapsed * (u64)tb->cir) / 1000;
    tb->tokens += tokens_added;

    /* Cap at max bucket size (Bc + Be) */
    u64 max_tokens = (u64)tb->bc + (u64)tb->be;
    if (tb->tokens > max_tokens) {
        tb->tokens = max_tokens;
    }

    tb->last_update = now;
}

/* Process frame for congestion management using a specific token bucket */
int cgst_process_frame_tb(vfr_port_t *port, token_bucket_t *tb, u8 *frame, size_t len, fr_addr_t *addr)
{
    cgst_state_t *cgst;
    u32 frame_bits = (u32)len * 8;
    int action = 0;  /* 0=forward, 1=set_de, 2=discard */
    int port_congested = 0;

    (void)frame;

    if (!port) return 0;

    cgst = (cgst_state_t *)port->cgst_ctx;
    if (!cgst) return 0;

    /* Increment port-level frame count for rate checking */
    if (cgst->rate_threshold > 0) {
        cgst->frames_since_check++;
    }

    port_congested = cgst->congested;

    if (tb) {
        if (tb->bc == 0 && (tb->be == 0 || tb->cir == 0)) {
            /* Pure best-effort bypass: forward all frames with DE=1.
             * If port is congested, discard them. */
            if (port_congested) {
                action = 2; /* Discard best effort under congestion */
            } else {
                action = 1; /* Forward with DE=1 */
            }
        } else if (tb->cir > 0) {
            u64 now = get_tick_count();
            tb_update(tb, now);

            if (addr && addr->de) {
                /* Incoming DE=1: treat as excess. Discard if port congested or bucket empty. */
                if (port_congested) {
                    action = 2; /* Discard DE under congestion */
                } else if (tb->tokens >= frame_bits) {
                    tb->tokens -= frame_bits;
                    action = 1; /* Forward with DE=1 */
                } else {
                    action = 2; /* Discard (rate limit exceeded) */
                }
            } else {
                /* Incoming DE=0: check bucket thresholds */
                if (tb->tokens >= frame_bits) {
                    tb->tokens -= frame_bits;
                    if (tb->bc > 0 && tb->tokens >= tb->be) {
                        action = 0; /* Forward as DE=0 (within Bc) */
                    } else {
                        /* Within Be (excess): discard if congested, else mark DE=1 */
                        if (port_congested) {
                            action = 2; /* Discard excess under congestion */
                        } else {
                            action = 1; /* Forward as DE=1 */
                        }
                    }
                } else {
                    action = 2; /* Discard (rate limit exceeded) */
                }
            }

            /* NNI compliance (D5): if port->type is NNI, never clear incoming DE — only set/preserve it */
            if (port->type == PORT_TYPE_NNI && action == 0 && addr && addr->de) {
                action = 1;
            }
        }
    }

    return action;
}

/* Process frame for congestion management */
int cgst_process_frame(vfr_port_t *port, vfr_dlci_entry_t *entry, u8 *frame, size_t len, fr_addr_t *addr)
{
    if (!port) return 0;
    cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
    if (!cgst) return 0;

    /* Select token bucket: DLCI-level if available, fallback to port-level */
    token_bucket_t *tb = (entry) ? &entry->tb : &cgst->tb;
    return cgst_process_frame_tb(port, tb, frame, len, addr);
}

/* Check if DE should be set based on congestion */
int cgst_should_set_de(vfr_port_t *port)
{
    cgst_state_t *cgst;

    if (!port || !port->cgst_ctx) return 0;

    cgst = (cgst_state_t *)port->cgst_ctx;
    return cgst->congested;
}

/* Mark port as congested */
void cgst_set_congested(vfr_port_t *port, int congested)
{
    cgst_state_t *cgst;

    if (!port || !port->cgst_ctx) return;

    cgst = (cgst_state_t *)port->cgst_ctx;

    if (cgst->congested != congested) {
        cgst->congested = congested;
        if (congested) {
            cgst->congested_start_time = get_tick_count();
        } else {
            cgst->congested_start_time = 0;
        }
        LOG_INFO("Port %s congestion state: %s",
                 port->name, congested ? "CONGESTED" : "CLEAR");
    }
}

/* Free congestion state */
void cgst_free(vfr_port_t *port)
{
    if (port && port->cgst_ctx) {
        free(port->cgst_ctx);
        port->cgst_ctx = NULL;
    }
}

/* Periodic congestion / CLLM timer */
void cgst_poll_timer(vfr_port_t *port)
{
    cgst_state_t *cgst;
    u64 now = get_tick_count();

    if (!port || !port->cgst_ctx) return;

    cgst = (cgst_state_t *)port->cgst_ctx;

    /* Active Ty timer check/expiry for PVCs and multicast on this port */
    if (g_vfrs) {
        mutex_lock(&g_vfrs->pvc_mutex);
        for (int i = 0; i < PVC_HASH_SIZE; i++) {
            vfr_pvc_detail_t *pvc = g_vfrs->pvc_table[i];
            while (pvc) {
                if (strcmp(pvc->port_in, port->name) == 0) {
                    vfr_dlci_entry_t *entry = port_lookup_dlci(port, pvc->dlci_in);
                    if (entry && entry->peer_congested && (now - entry->last_cllm_time > 11000)) {
                        entry->peer_congested = 0;
                    }
                    if (entry && entry->reverse_entry && entry->reverse_entry->peer_congested && (now - entry->reverse_entry->last_cllm_time > 11000)) {
                        entry->reverse_entry->peer_congested = 0;
                    }
                }
                pvc = pvc->next;
            }
        }
        mutex_unlock(&g_vfrs->pvc_mutex);

        mutex_lock(&g_vfrs->mcast_mutex);
        vfr_mcast_group_t *mcast_g = g_vfrs->mcast_groups;
        while (mcast_g) {
            if (strcmp(mcast_g->source_port, port->name) == 0) {
                if (mcast_g->peer_congested && (now - mcast_g->last_cllm_time > 11000)) {
                    mcast_g->peer_congested = 0;
                }
            }
            vfr_mcast_member_t *mcast_m = mcast_g->members;
            while (mcast_m) {
                if (strcmp(mcast_m->port_name, port->name) == 0) {
                    if (mcast_m->peer_congested && (now - mcast_m->last_cllm_time > 11000)) {
                        mcast_m->peer_congested = 0;
                    }
                }
                mcast_m = mcast_m->next;
            }
            mcast_g = mcast_g->next;
        }
        mutex_unlock(&g_vfrs->mcast_mutex);
    }

    /* Perform periodic port-level rate check */
    if (cgst->rate_threshold > 0) {
        u64 elapsed = now - cgst->last_rate_check;
        if (elapsed >= 1000) {  /* Check every second */
            u32 fps = (cgst->frames_since_check * 1000) / elapsed;

            if (cgst->congested) {
                if (fps < cgst->clear_threshold) {
                    cgst_set_congested(port, 0);
                }
            } else {
                if (fps > cgst->rate_threshold) {
                    cgst_set_congested(port, 1);
                }
            }

            cgst->frames_since_check = 0;
            cgst->last_rate_check = now;
        }
    }

    if (cgst->cllm_enabled && cgst->congested) {
        if (cgst->cllm_timer.interval == 0) {
            cgst->cllm_timer.interval = cgst->cllm_tx_interval;
            cgst->cllm_timer.expire = 0; /* Expire immediately to trigger first tx */
        }
        if (timer_is_expired(&cgst->cllm_timer)) {
            u32 dlci_list[128];
            int dlci_count = 0;

            if (g_vfrs) {
                /* Collect all active virtual circuits on this port (PVCs, SVCs, SPVCs) */
                mutex_lock(&g_vfrs->dlci_mutex);
                for (int i = 0; i < PVC_HASH_SIZE; i++) {
                    vfr_dlci_entry_t *entry = g_vfrs->dlci_table[i];
                    while (entry) {
                        if (strcmp(entry->port_in, port->name) == 0 && entry->active) {
                            if (cllm_should_include_dlci(entry->dlci_in, CLLM_LEVEL_PERSISTENT)) {
                                int exists = 0;
                                for (int k = 0; k < dlci_count; k++) {
                                    if (dlci_list[k] == entry->dlci_in) { exists = 1; break; }
                                }
                                if (!exists && dlci_count < 128) {
                                    dlci_list[dlci_count++] = entry->dlci_in;
                                }
                            }
                        }
                        entry = entry->next;
                    }
                }
                mutex_unlock(&g_vfrs->dlci_mutex);

                mutex_lock(&g_vfrs->mcast_mutex);
                vfr_mcast_group_t *mcast_g = g_vfrs->mcast_groups;
                while (mcast_g) {
                    if (strcmp(mcast_g->source_port, port->name) == 0) {
                        if (cllm_should_include_dlci(mcast_g->source_dlci, CLLM_LEVEL_PERSISTENT)) {
                            if (dlci_count < 128) {
                                dlci_list[dlci_count++] = mcast_g->source_dlci;
                            }
                        }
                    }
                    vfr_mcast_member_t *mcast_m = mcast_g->members;
                    while (mcast_m) {
                        if (strcmp(mcast_m->port_name, port->name) == 0) {
                            if (cllm_should_include_dlci(mcast_m->dlci, CLLM_LEVEL_PERSISTENT)) {
                                if (dlci_count < 128) {
                                    dlci_list[dlci_count++] = mcast_m->dlci;
                                }
                            }
                        }
                        mcast_m = mcast_m->next;
                    }
                    mcast_g = mcast_g->next;
                }
                mutex_unlock(&g_vfrs->mcast_mutex);
            }

            if (dlci_count > 0) {
                u8 cause = 0x02; /* Short term excessive traffic per X.36 Table C.1 / Q.922 §A.7.3.4.3 */
                if (cgst->congested_start_time > 0 && (now - cgst->congested_start_time >= 10000)) {
                    cause = 0x03; /* Long term persistent excessive traffic (>= 10 seconds) */
                }
                cllm_send_notification(port, cause, dlci_list, dlci_count);
                timer_set(&cgst->cllm_timer, cgst->cllm_tx_interval);
            } else {
                timer_cancel(&cgst->cllm_timer);
            }
        }
    } else {
        timer_cancel(&cgst->cllm_timer);
    }
}

/* Set clear threshold */
void cgst_set_clear_threshold(vfr_port_t *port, u32 clear_threshold)
{
    if (port && port->cgst_ctx) {
        cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
        cgst->clear_threshold = clear_threshold;
    }
}

/* Set CLLM enabled state */
void cgst_set_cllm_enabled(vfr_port_t *port, int enabled)
{
    if (port && port->cgst_ctx) {
        cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
        cgst->cllm_enabled = enabled;
        if (enabled) {
            timer_set(&cgst->cllm_timer, cgst->cllm_tx_interval);
        } else {
            timer_cancel(&cgst->cllm_timer);
        }
    }
}

/* Set CLLM tx interval */
void cgst_set_cllm_tx_interval(vfr_port_t *port, u32 interval_ms)
{
    if (port && port->cgst_ctx) {
        cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
        cgst->cllm_tx_interval = interval_ms;
        if (cgst->cllm_enabled) {
            timer_set(&cgst->cllm_timer, interval_ms);
        }
    }
}

/* Set write failure threshold */
void cgst_set_write_failure_threshold(vfr_port_t *port, u32 threshold)
{
    if (port && port->cgst_ctx) {
        cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
        cgst->write_failure_threshold = threshold;
    }
}

/* Handle write failure */
void cgst_handle_write_failure(vfr_port_t *port)
{
    if (port && port->cgst_ctx) {
        cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
        if (cgst->write_failure_threshold > 0) {
            cgst->write_failure_count++;
            if (cgst->write_failure_count >= cgst->write_failure_threshold && !cgst->congested) {
                cgst_set_congested(port, 1);
                LOG_INFO("Congestion triggered on %s: write failure threshold reached (%u/%u)",
                         port->name, cgst->write_failure_count, cgst->write_failure_threshold);
            }
        }
    }
}

/* Handle write success */
void cgst_handle_write_success(vfr_port_t *port)
{
    if (port && port->cgst_ctx) {
        cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
        if (cgst->write_failure_threshold > 0) {
            if (cgst->write_failure_count > 0) {
                cgst->write_failure_count = 0;
                if (cgst->congested) {
                    cgst_set_congested(port, 0);
                    LOG_INFO("Congestion cleared on %s: successful write", port->name);
                }
            }
        }
    }
}

u32 cgst_get_access_rate(vfr_port_t *port)
{
    if (port && port->cgst_ctx) {
        cgst_state_t *cgst = (cgst_state_t *)port->cgst_ctx;
        return cgst->access_rate;
    }
    return 0;
}