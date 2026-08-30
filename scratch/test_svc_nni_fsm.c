/*
 * test_svc_nni_fsm.c - Unit Tests for ITU-T X.76 Clause 10 NNI State Machine & Call Control
 * Virtual Frame Relay Switch
 */

#include "vfr.h"
#include "svc/svc_sig_common.h"
#include "svc/svc_sig_iel.h"
#include "svc/svc_sig_iep.h"
#include "svc/svc_sig_nni.h"
#include "svc/svc_sig_uni.h"
#include "switching/svc_routing_common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mock logging & globals */
vfrs_ctx_t *g_vfrs = NULL;
__thread uint32_t vfr_tls_lock_bitmap = 0;
static u8 g_last_tx_buf[1024];
static size_t g_last_tx_len = 0;
static char g_last_tx_port[32];

void vfr_log(enum log_level level, const char *file, int line, const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)fmt;
}

int mutex_init(mutex_t *m) { (void)m; return 0; }
void mutex_lock(mutex_t *m) { (void)m; }
void mutex_unlock(mutex_t *m) { (void)m; }
void mutex_destroy(mutex_t *m) { (void)m; }

u64 get_tick_count(void) {
    return GetTickCount64();
}

void timer_set(vfr_timer_t *t, u32 interval_ms) {
    if (t) {
        t->interval = interval_ms;
        t->expire = get_tick_count() + interval_ms;
    }
}

void timer_cancel(vfr_timer_t *t) {
    if (t) {
        t->expire = 0;
        t->interval = 0;
    }
}

int timer_is_expired(vfr_timer_t *t) {
    return (t && t->expire > 0 && get_tick_count() >= t->expire);
}

int lmi_port_dlci_len(vfr_port_t *port) {
    (void)port;
    return 2;
}

vfr_port_t *vfrs_find_port(vfrs_ctx_t *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    for (int i = 0; i < ctx->port_count; i++) {
        if (ctx->ports[i] && strcmp(ctx->ports[i]->name, name) == 0) return ctx->ports[i];
    }
    return NULL;
}

void cgst_dlci_tb_init(vfr_dlci_entry_t *entry, struct vfr_port_s *port, u32 cir, u32 bc, u32 be) {
    (void)entry; (void)port; (void)cir; (void)bc; (void)be;
}
int dlci_table_add(vfrs_ctx_t *ctx, vfr_dlci_entry_t *entry) {
    (void)ctx; (void)entry; return 0;
}
int port_add_dlci_entry(vfr_port_t *port, vfr_dlci_entry_t *entry) {
    (void)port; (void)entry; return 0;
}
int port_del_dlci_entry(vfr_port_t *port, u32 dlci) {
    (void)port; (void)dlci; return 0;
}
int dlci_table_del(vfrs_ctx_t *ctx, const char *port, u32 dlci) {
    (void)ctx; (void)port; (void)dlci; return 0;
}
int lapf_send_l3(vfr_port_t *port, u32 dlci, const u8 *data, size_t len) {
    (void)port; (void)dlci; (void)data; (void)len; return 0;
}
int pcap_writer_write(pcap_writer_t *pw, const u8 *data, size_t len) {
    (void)pw; (void)data; (void)len; return 0;
}

static int mock_send(vfr_port_t *port, const u8 *buf, size_t len) {
    if (!port || !buf) return -1;
    snprintf(g_last_tx_port, sizeof(g_last_tx_port), "%s", port->name);
    memcpy(g_last_tx_buf, buf, len);
    g_last_tx_len = len;
    return (int)len;
}

static port_ops_t mock_ops = {
    .send = mock_send,
};

static void reset_tx(void) {
    memset(g_last_tx_buf, 0, sizeof(g_last_tx_buf));
    g_last_tx_len = 0;
    g_last_tx_port[0] = '\0';
}

int main(void) {
    printf("=====================================================\n");
    printf("=== Starting ITU-T X.76 NNI State Machine Unit Tests ===\n");
    printf("=====================================================\n");

    /* ------------------------------------------------------------
     * Test 1: Bidirectional DLCI Range Allocator (Ascending vs Descending)
     * ------------------------------------------------------------ */
    printf("\n[Test 1] Testing Bidirectional DLCI Range Allocator...\n");
    vfr_dlci_allocator_t alloc_low, alloc_high;
    svc_dlci_alloc_init_ex(&alloc_low, 512, 991, SVC_DLCI_ALLOC_ASCENDING);
    svc_dlci_alloc_init_ex(&alloc_high, 512, 991, SVC_DLCI_ALLOC_DESCENDING);

    u32 d_low1 = svc_dlci_alloc(&alloc_low);
    u32 d_low2 = svc_dlci_alloc(&alloc_low);
    assert(d_low1 == 512);
    assert(d_low2 == 513);
    printf("  [PASS] Ascending allocator allocates lowest DLCIs: 512, 513\n");

    u32 d_high1 = svc_dlci_alloc(&alloc_high);
    u32 d_high2 = svc_dlci_alloc(&alloc_high);
    assert(d_high1 == 991);
    assert(d_high2 == 990);
    printf("  [PASS] Descending allocator allocates highest DLCIs: 991, 990\n");

    svc_dlci_free(&alloc_low, 512);
    u32 d_low_reuse = svc_dlci_alloc(&alloc_low);
    assert(d_low_reuse == 512);
    printf("  [PASS] Freed DLCI 512 reused correctly\n");

    /* ------------------------------------------------------------
     * Test 2: Global Monotonic Call Identification Counter
     * ------------------------------------------------------------ */
    printf("\n[Test 2] Testing Call Identification Monotonic Generator...\n");
    u32 cid1 = svc_generate_call_ident();
    u32 cid2 = svc_generate_call_ident();
    u32 cid3 = svc_generate_call_ident();
    assert(cid2 == cid1 + 1);
    assert(cid3 == cid2 + 1);
    printf("  [PASS] Call Ident generator produces monotonic values: %u, %u, %u\n", cid1, cid2, cid3);

    /* ------------------------------------------------------------
     * Setup Mock Environment: Global Switch Context and Ports
     * ------------------------------------------------------------ */
    vfrs_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.dnic = 5101; /* Indonesian Telkom / Indosat test DNIC */
    ctx.default_svc_t303 = 4000;
    ctx.default_svc_t308 = 4000;
    ctx.default_svc_t310 = 40000;
    g_vfrs = &ctx;

    vfr_port_t port_nni1, port_nni2;
    memset(&port_nni1, 0, sizeof(port_nni1));
    memset(&port_nni2, 0, sizeof(port_nni2));
    snprintf(port_nni1.name, sizeof(port_nni1.name), "nni1");
    snprintf(port_nni2.name, sizeof(port_nni2.name), "nni2");
    port_nni1.ops = &mock_ops;
    port_nni2.ops = &mock_ops;

    ctx.ports[0] = &port_nni1;
    ctx.ports[1] = &port_nni2;
    ctx.port_count = 2;

    svc_init_port_ex(&port_nni1, 512, 991, 2, 1, SVC_DLCI_ALLOC_ASCENDING);
    svc_init_port_ex(&port_nni2, 512, 991, 2, 1, SVC_DLCI_ALLOC_DESCENDING);

    vfr_svc_ctx_t *sctx1 = (vfr_svc_ctx_t *)port_nni1.svc_ctx;
    vfr_svc_ctx_t *sctx2 = (vfr_svc_ctx_t *)port_nni2.svc_ctx;
    assert(sctx1->is_nni == 1);
    assert(sctx2->is_nni == 1);
    assert(strcmp(sctx1->network_id, "5101") == 0);
    assert(strcmp(sctx2->network_id, "5101") == 0);
    printf("  [PASS] Ports initialized: nni1 (low-side, network_id=5101), nni2 (high-side, network_id=5101)\n");

    /* Configure routing: route prefix "5102" -> nni2 */
    svc_route_init(&ctx);
    svc_route_add(&ctx, "5102", "nni2");

    /* ------------------------------------------------------------
     * Test 3: NNI Ingress SETUP -> CALL PROCEEDING -> Forward SETUP
     * ------------------------------------------------------------ */
    printf("\n[Test 3] Testing NNI SETUP Processing & Call Forwarding...\n");
    reset_tx();

    vfr_call_t test_call;
    memset(&test_call, 0, sizeof(test_call));
    test_call.call_ref = 0x0123;
    test_call.call_ref_flag = 0;
    test_call.call_ref_len = 2;
    test_call.ingress_dlci = 520;
    test_call.call_ident = 9999;
    snprintf(test_call.calling_number, sizeof(test_call.calling_number), "5101123456");
    snprintf(test_call.called_number, sizeof(test_call.called_number), "5102789001");
    test_call.called_number_plan = 0x01;
    test_call.called_number_type = 0x01;
    test_call.llcore.fwd_cir = 64000;
    test_call.llcore.bwd_cir = 64000;
    test_call.tni_list.count = 1;
    test_call.tni_list.entries[0].type_plan = 0x33;
    snprintf(test_call.tni_list.entries[0].net_id, sizeof(test_call.tni_list.entries[0].net_id), "5100");

    u8 setup_msg[512];
    int setup_len = q933_build_nni_setup(setup_msg, sizeof(setup_msg), &test_call, sctx1);
    assert(setup_len > 0);

    /* Direct L3 Message Processing */
    svc_handle_l3_message(&port_nni1, setup_msg, setup_len);

    /* Verify call was created on nni1 */
    vfr_call_t *c1 = svc_find_call(sctx1, 0x0123, 1);
    assert(c1 != NULL);
    assert(c1->state == SVC_NNI_STATE_NN3_CALL_PROC_SENT);
    assert(c1->ingress_dlci == 520);
    assert(c1->call_ident == 9999);
    assert(c1->peer_port == &port_nni2);
    printf("  [PASS] nni1 state transition -> NN3 (Call Proceeding Sent)\n");

    /* Verify peer call created on nni2 */
    vfr_call_t *c2 = svc_find_call(sctx2, c1->peer_call_ref, c1->peer_call_ref_flag);
    assert(c2 != NULL);
    assert(c2->state == SVC_NNI_STATE_NN6_CALL_PRESENT);
    assert(c2->ingress_dlci == 520);
    assert(c2->egress_dlci == 991); /* High side allocator */
    assert(c2->call_ident == 9999);
    /* Verify TNI chain: 5100 -> 5101 */
    assert(c2->tni_list.count == 2);
    assert(strcmp(c2->tni_list.entries[0].net_id, "5100") == 0);
    assert(strcmp(c2->tni_list.entries[1].net_id, "5101") == 0);
    printf("  [PASS] nni2 peer call allocated -> NN6 (Call Present, DLCI=991, TNI chain=[5100, 5101])\n");

    /* ------------------------------------------------------------
     * Test 4: Egress CALL PROCEEDING -> Transition to NN9
     * ------------------------------------------------------------ */
    printf("\n[Test 4] Testing CALL PROCEEDING Reception on Egress...\n");
    u8 cp_msg[128];
    vfr_call_t c2_resp = *c2;
    c2_resp.call_ref_flag = c2->call_ref_flag ^ 1; /* Peer response flag (1) */
    int cp_len = q933_build_nni_call_proceeding(cp_msg, sizeof(cp_msg), &c2_resp);
    assert(cp_len > 0);

    svc_handle_l3_message(&port_nni2, cp_msg, cp_len);

    assert(c2->state == SVC_NNI_STATE_NN9_CALL_PROC_RCVD);
    printf("  [PASS] nni2 state transition -> NN9 (Call Proceeding Received)\n");

    /* ------------------------------------------------------------
     * Test 5: Egress CONNECT -> Both legs enter NN10 (Active) & Reflect TNIs
     * ------------------------------------------------------------ */
    printf("\n[Test 5] Testing CONNECT Reception & Propagation...\n");
    u8 conn_msg[256];
    int conn_len = q933_build_nni_connect(conn_msg, sizeof(conn_msg), &c2_resp);
    assert(conn_len > 0);

    svc_handle_l3_message(&port_nni2, conn_msg, conn_len);

    assert(c2->state == SVC_NNI_STATE_NN10_ACTIVE);
    assert(c1->state == SVC_NNI_STATE_NN10_ACTIVE);
    printf("  [PASS] Both legs transitioned to NN10 (Active)\n");

    /* ------------------------------------------------------------
     * Test 6: Ingress RELEASE -> Release Complete + Peer Release Request (NN11)
     * ------------------------------------------------------------ */
    printf("\n[Test 6] Testing Call Clearing & Release Request (NN11)...\n");
    u8 rel_msg[128];
    vfr_call_t c1_caller = *c1;
    c1_caller.call_ref_flag = 0; /* Caller sends with originator flag 0 */
    int rel_len = q933_build_nni_release(rel_msg, sizeof(rel_msg), &c1_caller, Q850_CAUSE_NORMAL_CLEARING, "5100", 0x33);
    assert(rel_len > 0);

    svc_handle_l3_message(&port_nni1, rel_msg, rel_len);

    /* Ingress call is cleared */
    assert(svc_find_call(sctx1, 0x0123, 1) == NULL);
    /* Egress peer call entered NN11 (Release Request) */
    assert(c2->state == SVC_NNI_STATE_NN11_REL_REQ);
    printf("  [PASS] nni1 call cleared (NN0), nni2 entered NN11 (Release Request)\n");

    /* Egress receives RELEASE COMPLETE -> Call fully cleared */
    u8 rel_comp_msg[128];
    int rel_comp_len = q933_build_nni_release_complete(rel_comp_msg, sizeof(rel_comp_msg), c2->call_ref, c2->call_ref_flag ^ 1, c2->call_ref_len, 0, "5102", 0x33);
    assert(rel_comp_len > 0);

    svc_handle_l3_message(&port_nni2, rel_comp_msg, rel_comp_len);

    assert(svc_find_call(sctx2, c2->call_ref, c2->call_ref_flag) == NULL);
    printf("  [PASS] nni2 call cleared on RELEASE COMPLETE (NN0)\n");

    /* ------------------------------------------------------------
     * Test 7: Loop Detection (Own TNI in SETUP -> Rejected with Cause 100)
     * ------------------------------------------------------------ */
    printf("\n[Test 7] Testing Loop Detection (Own TNI in Received SETUP)...\n");
    reset_tx();
    memset(&test_call, 0, sizeof(test_call));
    test_call.call_ref = 0x0456;
    test_call.call_ref_flag = 0;
    test_call.call_ref_len = 2;
    test_call.ingress_dlci = 521;
    test_call.call_ident = 10001;
    snprintf(test_call.calling_number, sizeof(test_call.calling_number), "5101123456");
    snprintf(test_call.called_number, sizeof(test_call.called_number), "5102789001");
    test_call.called_number_plan = 0x01;
    test_call.called_number_type = 0x01;
    test_call.tni_list.count = 2;
    test_call.tni_list.entries[0].type_plan = 0x33;
    snprintf(test_call.tni_list.entries[0].net_id, sizeof(test_call.tni_list.entries[0].net_id), "5100");
    test_call.tni_list.entries[1].type_plan = 0x33;
    snprintf(test_call.tni_list.entries[1].net_id, sizeof(test_call.tni_list.entries[1].net_id), "5101"); /* OWN NETWORK ID! */

    setup_len = q933_build_nni_setup(setup_msg, sizeof(setup_msg), &test_call, sctx1);
    svc_handle_l3_message(&port_nni1, setup_msg, setup_len);

    /* Call must be rejected immediately, no CCB lingering */
    assert(svc_find_call(sctx1, 0x0456, 1) == NULL);
    printf("  [PASS] Loop detected and call rejected immediately (no call allocated)\n");

    /* ------------------------------------------------------------
     * Test 8: Missing Call Ident -> Rejected with Cause 96
     * ------------------------------------------------------------ */
    printf("\n[Test 8] Testing Missing Mandatory Call Ident IE...\n");
    memset(&test_call, 0, sizeof(test_call));
    test_call.call_ref = 0x0789;
    test_call.call_ref_flag = 0;
    test_call.call_ref_len = 2;
    test_call.ingress_dlci = 522;
    test_call.call_ident = 0; /* Missing */
    snprintf(test_call.calling_number, sizeof(test_call.calling_number), "5101123456");
    snprintf(test_call.called_number, sizeof(test_call.called_number), "5102789001");
    test_call.called_number_plan = 0x01;
    test_call.called_number_type = 0x01;

    setup_len = q933_build_nni_setup(setup_msg, sizeof(setup_msg), &test_call, sctx1);
    svc_handle_l3_message(&port_nni1, setup_msg, setup_len);

    assert(svc_find_call(sctx1, 0x0789, 1) == NULL);
    printf("  [PASS] Missing mandatory Call Ident rejected immediately\n");

    /* ------------------------------------------------------------
     * Test 9: Excess TNIs (> 6) -> Rejected with Cause 104
     * ------------------------------------------------------------ */
    printf("\n[Test 9] Testing Excess TNIs (> 6) Reject Procedure...\n");
    memset(&test_call, 0, sizeof(test_call));
    test_call.call_ref = 0x0AAA;
    test_call.call_ref_flag = 0;
    test_call.call_ref_len = 2;
    test_call.ingress_dlci = 523;
    test_call.call_ident = 20002;
    snprintf(test_call.calling_number, sizeof(test_call.calling_number), "5101123456");
    snprintf(test_call.called_number, sizeof(test_call.called_number), "5102789001");
    test_call.called_number_plan = 0x01;
    test_call.called_number_type = 0x01;
    test_call.tni_list.count = 6;
    for (int i = 0; i < 6; i++) {
        test_call.tni_list.entries[i].type_plan = 0x33;
        snprintf(test_call.tni_list.entries[i].net_id, sizeof(test_call.tni_list.entries[i].net_id), "51%02d", i + 10);
    }

    setup_len = q933_build_nni_setup(setup_msg, sizeof(setup_msg), &test_call, sctx1);
    /* Manually append a 7th TNI IE to trigger Cause 104 */
    u8 tni7[8];
    int tni7_len = q933_build_transit_net_id(tni7, sizeof(tni7), "5199", 0x33);
    memcpy(&setup_msg[setup_len], tni7, tni7_len);
    setup_len += tni7_len;

    svc_handle_l3_message(&port_nni1, setup_msg, setup_len);

    assert(svc_find_call(sctx1, 0x0AAA, 1) == NULL);
    printf("  [PASS] Excess TNIs (> 6) rejected immediately\n");

    /* ------------------------------------------------------------
     * Test 10: Global CRV RESTART Handling
     * ------------------------------------------------------------ */
    printf("\n[Test 10] Testing Global CRV RESTART Message...\n");
    reset_tx();
    u8 rest_msg[64];
    int rest_len = q933_build_restart(rest_msg, sizeof(rest_msg), 0);
    assert(rest_len > 0);

    svc_handle_l3_message(&port_nni1, rest_msg, rest_len);

    assert(g_last_tx_len > 0);
    assert(strcmp(g_last_tx_port, "nni1") == 0);
    printf("  [PASS] Sent RESTART ACKNOWLEDGE on Global CRV RESTART reception\n");

    printf("\n=====================================================\n");
    printf("=== ALL ITU-T X.76 NNI State Machine Tests PASSED! ===\n");
    printf("=====================================================\n");
    return 0;
}

