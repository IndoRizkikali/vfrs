/*
 * test_phase1d.c - Phase 1D Verification Unit Test
 * Tests:
 * 1. vfrs_show_svc_calls
 * 2. vfrs_show_svc_stats
 * 3. vfrs_show_svc_subscribers
 * 4. vfrs_show_svc_routes
 * 5. PCAP Packet Capture Writer
 */

#include "vfr.h"
#include "svc_signalling/svc_sig_common.h"
#include "svc_signalling/svc_sig_uni.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern vfrs_ctx_t *g_vfrs;

int main(void) {
    winsock_init();
    logger_init(NULL, LOG_INFO, LOG_INFO);

    printf("============================================================\n");
    printf(" VFRS PHASE 1D VERIFICATION UNIT TEST\n");
    printf("============================================================\n");

    /* Create Switch Context */
    g_vfrs = vfrs_create("SW_TEST_PHASE1D");
    assert(g_vfrs != NULL);
    g_vfrs->dnic = 5104;
    g_vfrs->sgc = 1;
    g_vfrs->sic = 1;

    /* Create Ports */
    vfr_port_t *port0 = port_udp_create("uni0/0", PORT_TYPE_UNI, "127.0.0.1", 10010, "127.0.0.1", 20010);
    vfr_port_t *port1 = port_udp_create("uni0/1", PORT_TYPE_UNI, "127.0.0.1", 10011, "127.0.0.1", 20011);
    assert(port0 != NULL && port1 != NULL);

    vfrs_add_port(g_vfrs, port0);
    vfrs_add_port(g_vfrs, port1);

    /* Initialize SVC on Ports */
    svc_init_port(port0, 512, 991, 2);
    svc_init_port(port1, 512, 991, 2);

    vfr_svc_ctx_t *sctx0 = (vfr_svc_ctx_t *)port0->svc_ctx;
    vfr_svc_ctx_t *sctx1 = (vfr_svc_ctx_t *)port1->svc_ctx;
    assert(sctx0 != NULL && sctx1 != NULL);

    /* Set Subscribers */
    snprintf(sctx0->subscriber_number, sizeof(sctx0->subscriber_number), "510401011001");
    sctx0->subscriber_type = 1; // X.121
    snprintf(sctx0->alias_number, sizeof(sctx0->alias_number), "1001");

    snprintf(sctx1->subscriber_number, sizeof(sctx1->subscriber_number), "510401011002");
    sctx1->subscriber_type = 1; // X.121
    snprintf(sctx1->alias_number, sizeof(sctx1->alias_number), "1002");

    /* Add NNI Routes */
    snprintf(g_vfrs->svc_routes[0].prefix, sizeof(g_vfrs->svc_routes[0].prefix), "51040101");
    snprintf(g_vfrs->svc_routes[0].egress_port, sizeof(g_vfrs->svc_routes[0].egress_port), "uni0/1");
    g_vfrs->svc_route_count = 1;

    /* Create an Active Call Reference for testing show_svc_calls */
    vfr_call_t *call = svc_alloc_call_ref(sctx0, 0x0042, 0);
    assert(call != NULL);
    call->state = SVC_STATE_ACTIVE;
    snprintf(call->calling_number, sizeof(call->calling_number), "510401011001");
    snprintf(call->called_number, sizeof(call->called_number), "510401011002");
    snprintf(call->ingress_port, sizeof(call->ingress_port), "uni0/0");
    snprintf(call->egress_port, sizeof(call->egress_port), "uni0/1");
    call->ingress_dlci = 512;
    call->egress_dlci = 512;

    /* 1. Test CLI Inspection Commands */
    printf("\n-> Testing 'show svc calls'...\n");
    vfrs_show_svc_calls(g_vfrs, stdout);

    printf("\n-> Testing 'show svc subscribers'...\n");
    vfrs_show_svc_subscribers(g_vfrs, stdout);

    printf("\n-> Testing 'show svc routes'...\n");
    vfrs_show_svc_routes(g_vfrs, stdout);

    printf("\n-> Testing 'show svc stats'...\n");
    vfrs_show_svc_stats(g_vfrs, stdout);

    /* 2. Test PCAP Packet Capture */
    printf("\n-> Testing PCAP Packet Capture Writer...\n");
    pcap_writer_t pw;
    int ret = pcap_writer_init(&pw, "scratch/test_phase1d.pcap", 65535);
    assert(ret == 0);

    /* Write sample Frame Relay frame to PCAP (DLCI 512, UI Control 0x03, Protocol Disc 0x08 Q.933 SETUP) */
    u8 sample_fr_frame[] = {
        0x20, 0x01,       /* DLCI 512 (2-octet Q.922 address) */
        0x03,             /* UI Control */
        0x08,             /* Q.933 Protocol Discriminator */
        0x02, 0x00, 0x42, /* Call Ref Length=2, Value=0x0042 */
        0x05              /* SETUP message type */
    };
    ret = pcap_writer_write(&pw, sample_fr_frame, sizeof(sample_fr_frame));
    assert(ret == 0);

    ret = pcap_writer_close(&pw);
    assert(ret == 0);
    printf("   [PASS] PCAP Packet Capture generated successfully ('scratch/test_phase1d.pcap')\n");

    /* Cleanup */
    vfrs_destroy(g_vfrs);
    logger_shutdown();
    winsock_shutdown();

    printf("\n============================================================\n");
    printf(" SUCCESS: ALL PHASE 1D VERIFICATION TESTS PASSED!\n");
    printf("============================================================\n");
    return 0;
}
