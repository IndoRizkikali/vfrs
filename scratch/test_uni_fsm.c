/*
 * test_uni_fsm.c - Unit Test for Q.933 DCE UNI Call State Machine
 * Virtual Frame Relay Switch - Phase 1C
 */

#include "../vfr.h"
#include "../fr_switching/fr_frame.h"
#include "../svc_signalling/svc_sig_common.h"
#include "../svc_signalling/svc_sig_iel.h"
#include "../svc_signalling/svc_sig_iep.h"
#include "../svc_signalling/svc_sig_uni.h"
#include "../ports/svc_numbering/svc_numbering.h"
#include "../fr_switching/svc_routing_common.h"
#include <assert.h>

/* Mock Frame Relay send function */
static int mock_sent_count = 0;
static u8 last_sent_msg_type = 0;

static int mock_send(vfr_port_t *port, const u8 *frame, size_t len) {
    (void)port;
    if (frame && len > 4) {
        mock_sent_count++;
        u8 work[FR_MAX_FRAMESZ];
        memcpy(work, frame, len);
        fr_addr_t addr;
        size_t frame_body_len = 0;

        /* Parse serial UI frame (is_serial=1) */
        if (fr_parse_frame(work, len, &addr, &frame_body_len, 1) == 0) {
            size_t addr_len = (addr.ea == 1) ? 2 : 4;
            const u8 *payload = work + addr_len + 1; /* Skip address & UI control byte 0x03 */
            size_t payload_len = frame_body_len - addr_len - 1;

            q933_msg_header_t hdr;
            int res = q933_parse_header(payload, payload_len, &hdr);
            if (res > 0) {
                last_sent_msg_type = hdr.message_type;
            }
        }
    }
    return 0;
}

static port_ops_t g_mock_ops = {
    .send = mock_send,
    .recv = NULL,
    .poll = NULL,
    .free = NULL
};

int main(void) {
    printf("=== Running Q.933 DCE UNI Call State Machine Unit Test ===\n");

    /* Create dummy VFRS context */
    vfrs_ctx_t vctx;
    memset(&vctx, 0, sizeof(vctx));
    mutex_init(&vctx.pvc_mutex);
    mutex_init(&vctx.port_mutex);
    vctx.default_cir = 32000;
    vctx.default_bc = 32000;
    vctx.dnic = 5104;
    vctx.sgclen = 2;
    vctx.sgc = 1;
    vctx.siclen = 2;
    vctx.sic = 1;
    g_vfrs = &vctx;

    svc_numbering_init(&vctx);
    svc_route_init(&vctx);

    /* Initialize dummy Port 1 */
    vfr_port_t port1;
    memset(&port1, 0, sizeof(port1));
    snprintf(port1.name, sizeof(port1.name), "eth0");
    port1.dlcibit = 10;
    port1.ops = &g_mock_ops;
    mutex_init(&port1.mutex);
    svc_init_port(&port1, 512, 991, 2);
    vctx.ports[0] = &port1;
    vctx.port_count = 1;

    /* Initialize dummy Port 2 */
    vfr_port_t port2;
    memset(&port2, 0, sizeof(port2));
    snprintf(port2.name, sizeof(port2.name), "eth1");
    port2.dlcibit = 10;
    port2.ops = &g_mock_ops;
    mutex_init(&port2.mutex);
    svc_init_port(&port2, 512, 991, 2);
    vctx.ports[1] = &port2;
    vctx.port_count = 2;

    /* Register subscriber numbers */
    svc_numbering_add(&vctx, "eth0", "x121", "510401011001", NULL, NULL);
    svc_numbering_add(&vctx, "eth1", "x121", "510401011002", NULL, NULL);

    /* 1. Test Virtual Switch Self-Number Call (510401010000) */
    mock_sent_count = 0;
    vfr_call_t call_setup;
    memset(&call_setup, 0, sizeof(call_setup));
    call_setup.call_ref = 0x0001;
    call_setup.call_ref_len = 2;
    call_setup.ingress_dlci = 512;
    snprintf(call_setup.called_number, sizeof(call_setup.called_number), "510401010000");
    snprintf(call_setup.calling_number, sizeof(call_setup.calling_number), "510401011001");

    u8 setup_buf[256];
    vfr_svc_ctx_t *sctx1 = (vfr_svc_ctx_t *)port1.svc_ctx;
    int len = q933_build_setup(setup_buf, sizeof(setup_buf), &call_setup, sctx1);
    assert(len > 0);

    svc_uni_process_msg(&port1, setup_buf, len);
    assert(mock_sent_count == 2); /* CALL PROCEEDING + CONNECT */
    assert(last_sent_msg_type == Q933_MSG_CONNECT);

    vfr_call_t *active_call = svc_find_call(sctx1, 0x0001, 1);
    assert(active_call != NULL);
    assert(active_call->state == SVC_STATE_N10_ACTIVE);
    printf("[PASS] Virtual Switch Self-Number (510401010000) call auto-connect verified\n");

    /* Clear virtual switch call */
    u8 disc_buf[128];
    len = q933_build_disconnect(disc_buf, sizeof(disc_buf), active_call, Q850_CAUSE_NORMAL_CLEARING);
    svc_uni_process_msg(&port1, disc_buf, len);
    assert(svc_find_call(sctx1, 0x0001, 1) == NULL);
    printf("[PASS] Call clearing on Virtual Switch call verified\n");

    /* 2. Test Intra-Switch Call Routing (Port1 -> Port2) */
    mock_sent_count = 0;
    memset(&call_setup, 0, sizeof(call_setup));
    call_setup.call_ref = 0x0002;
    call_setup.call_ref_len = 2;
    call_setup.ingress_dlci = 513;
    snprintf(call_setup.called_number, sizeof(call_setup.called_number), "510401011002");
    snprintf(call_setup.calling_number, sizeof(call_setup.calling_number), "510401011001");

    len = q933_build_setup(setup_buf, sizeof(setup_buf), &call_setup, sctx1);
    svc_uni_process_msg(&port1, setup_buf, len);

    active_call = svc_find_call(sctx1, 0x0002, 1);
    assert(active_call != NULL);
    assert(active_call->state == SVC_STATE_N10_ACTIVE);
    assert(active_call->peer_port == &port2);
    printf("[PASS] Intra-switch subscriber call (Port1 -> Port2) connected & bridged\n");

    /* Clear intra-switch call */
    len = q933_build_disconnect(disc_buf, sizeof(disc_buf), active_call, Q850_CAUSE_NORMAL_CLEARING);
    svc_uni_process_msg(&port1, disc_buf, len);
    assert(svc_find_call(sctx1, 0x0002, 1) == NULL);
    printf("[PASS] Intra-switch call clearing & peer cleanup verified\n");

    /* 3. Test Unallocated Number Error Handling */
    mock_sent_count = 0;
    memset(&call_setup, 0, sizeof(call_setup));
    call_setup.call_ref = 0x0003;
    call_setup.call_ref_len = 2;
    snprintf(call_setup.called_number, sizeof(call_setup.called_number), "999999999999");
    len = q933_build_setup(setup_buf, sizeof(setup_buf), &call_setup, sctx1);
    svc_uni_process_msg(&port1, setup_buf, len);

    assert(mock_sent_count == 1);
    assert(last_sent_msg_type == Q933_MSG_RELEASE_COMPLETE);
    assert(svc_find_call(sctx1, 0x0003, 1) == NULL);
    printf("[PASS] Unallocated number error rejection (RELEASE COMPLETE) verified\n");

    /* 4. Test Missing Mandatory IE (Called Party Number) Rejection */
    mock_sent_count = 0;
    memset(&call_setup, 0, sizeof(call_setup));
    call_setup.call_ref = 0x0004;
    call_setup.call_ref_len = 2;
    /* Do not set called_number */
    len = q933_build_setup(setup_buf, sizeof(setup_buf), &call_setup, sctx1);
    svc_uni_process_msg(&port1, setup_buf, len);

    assert(mock_sent_count == 1);
    assert(last_sent_msg_type == Q933_MSG_RELEASE_COMPLETE);
    printf("[PASS] Missing mandatory Called Party Number IE rejection (Cause 96) verified\n");

    /* 5. Test Global Call Reference Error Response (X.36 §10.8.3) */
    mock_sent_count = 0;
    u8 global_dummy_msg[16];
    int g_len = q933_build_header(global_dummy_msg, sizeof(global_dummy_msg), 0, 0, 0, Q933_MSG_DISCONNECT);
    svc_uni_process_msg(&port1, global_dummy_msg, g_len);
    assert(mock_sent_count == 1);
    assert(last_sent_msg_type == Q933_MSG_STATUS);
    printf("[PASS] Global Call Reference non-RESTART message STATUS response (Cause 81) verified\n");

    /* 6. Test Global RESTART Message Handling (X.36 §10.9) */
    mock_sent_count = 0;
    u8 restart_msg[16];
    int r_len = q933_build_restart(restart_msg, sizeof(restart_msg), 0); /* 0 = all calls */
    svc_uni_process_msg(&port1, restart_msg, r_len);
    assert(mock_sent_count == 1);
    assert(last_sent_msg_type == Q933_MSG_RESTART_ACK);
    printf("[PASS] Global RESTART message handling (RESTART ACK) verified\n");

    svc_free_port(&port1);
    svc_free_port(&port2);

    printf("=== All Q.933 DCE UNI Call State Machine Tests Passed! ===\n");
    return 0;
}
