/*
 * test_q933_nni_parser.c - Unit Test for X.76 NNI Q.933 IE Parsers & Builders
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../vfr.h"
#include "../svc_signalling/svc_sig_common.h"
#include "../svc_signalling/svc_sig_iel.h"
#include "../svc_signalling/svc_sig_iep.h"

/* Global and logger stubs for standalone testing */
vfrs_ctx_t *g_vfrs = NULL;
void vfr_log(enum log_level level, const char *file, int line, const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)fmt;
}
int mutex_init(mutex_t *m) { (void)m; return 0; }
void mutex_destroy(mutex_t *m) { (void)m; }
void mutex_lock(mutex_t *m) { (void)m; }
void mutex_unlock(mutex_t *m) { (void)m; }

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_tests_passed++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        g_tests_failed++; \
    } \
} while (0)

void test_call_ident_ie(void) {
    printf("Testing Call Identification IE (0x69)...\n");
    u8 buf[32];
    u32 call_id_in = 0x12345678;
    int len = q933_build_call_ident(buf, sizeof(buf), call_id_in);
    TEST_ASSERT(len == 6, "Call Ident IE length is 6");
    TEST_ASSERT(buf[0] == Q933_IE_CALL_IDENT, "IE identifier is 0x69");
    TEST_ASSERT(buf[1] == 4, "IE content length is 4");
    TEST_ASSERT(buf[2] == 0x12 && buf[3] == 0x34 && buf[4] == 0x56 && buf[5] == 0x78, "Binary encoding is big-endian");

    u32 call_id_out = 0;
    int parse_rc = q933_parse_call_ident(&buf[2], buf[1], &call_id_out);
    TEST_ASSERT(parse_rc == 0, "q933_parse_call_ident success");
    TEST_ASSERT(call_id_out == call_id_in, "Parsed Call ID matches original value");
}

void test_transit_net_id_ie(void) {
    printf("Testing Transit Network Identification IE (0x67)...\n");
    u8 buf[32];
    const char *net_id_in = "5104";
    u8 type_plan_in = 0x33; /* International, X.121 DNIC */
    int len = q933_build_transit_net_id(buf, sizeof(buf), net_id_in, type_plan_in);
    TEST_ASSERT(len == 7, "TNI IE length is 7 (1 ID + 1 len + 1 type_plan + 4 digits)");
    TEST_ASSERT(buf[0] == Q933_IE_TRANSIT_NET_ID, "IE identifier is 0x67");
    TEST_ASSERT(buf[1] == 5, "Content length is 5 (1 type_plan + 4 digits)");
    TEST_ASSERT((buf[2] & 0x80) != 0, "Ext bit is set in octet 3");

    char net_id_out[16];
    u8 type_plan_out = 0;
    int parse_rc = q933_parse_transit_net_id(&buf[2], buf[1], net_id_out, sizeof(net_id_out), &type_plan_out);
    TEST_ASSERT(parse_rc == 0, "q933_parse_transit_net_id success");
    TEST_ASSERT(strcmp(net_id_out, net_id_in) == 0, "Decoded net_id matches '5104'");
    TEST_ASSERT((type_plan_out & 0x7F) == (type_plan_in & 0x7F), "Decoded type_plan matches 0x33");
}

void test_clearing_net_id_ie(void) {
    printf("Testing Clearing Network Identification IE (0x6B)...\n");
    u8 buf[32];
    const char *net_id_in = "5105";
    u8 type_plan_in = 0x33;
    int len = q933_build_clearing_net_id(buf, sizeof(buf), net_id_in, type_plan_in);
    TEST_ASSERT(len == 7, "CNI IE length is 7");
    TEST_ASSERT(buf[0] == Q933_IE_CLEARING_NET_ID, "IE identifier is 0x6B");
    TEST_ASSERT(buf[1] == 5, "Content length is 5");

    char net_id_out[16];
    u8 type_plan_out = 0;
    int parse_rc = q933_parse_clearing_net_id(&buf[2], buf[1], net_id_out, sizeof(net_id_out), &type_plan_out);
    TEST_ASSERT(parse_rc == 0, "q933_parse_clearing_net_id success");
    TEST_ASSERT(strcmp(net_id_out, net_id_in) == 0, "Decoded net_id matches '5105'");
}

void test_gat_ie(void) {
    printf("Testing Generic Application Transport IE (0x6E)...\n");
    u8 buf[64];
    u8 gat_in[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    int len = q933_build_gat(buf, sizeof(buf), gat_in, sizeof(gat_in));
    TEST_ASSERT(len == 10, "GAT IE length is 10 (1 ID + 1 len + 8 data)");
    TEST_ASSERT(buf[0] == Q933_IE_GAT, "IE identifier is 0x6E");
    TEST_ASSERT(buf[1] == 8, "Content length is 8");

    u8 gat_out[16];
    size_t out_len = 0;
    int parse_rc = q933_parse_gat(&buf[2], buf[1], gat_out, sizeof(gat_out), &out_len);
    TEST_ASSERT(parse_rc == 0, "q933_parse_gat success");
    TEST_ASSERT(out_len == 8, "Parsed GAT length is 8");
    TEST_ASSERT(memcmp(gat_in, gat_out, 8) == 0, "Parsed GAT payload matches");
}

void test_nni_messages(void) {
    printf("Testing High-Level NNI Message Construction...\n");
    u8 msg_buf[512];
    vfr_svc_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.default_cir = 128000;
    sctx.default_bc  = 128000;
    sctx.default_be  = 32000;
    sctx.default_fmif = 1600;

    vfr_call_t call;
    memset(&call, 0, sizeof(call));
    call.call_ref = 0x002A; /* 42 */
    call.call_ref_flag = 0;
    call.call_ref_len = 2;
    call.is_nni = 1;
    call.egress_dlci = 600;
    call.ingress_dlci = 550;
    call.call_ident = 0xAABBCCDD;
    strcpy(call.calling_number, "510401011001");
    strcpy(call.called_number, "510401021002");
    strcpy(call.calling_subaddr, "99");
    strcpy(call.called_subaddr, "88");
    call.tni_list.count = 2;
    call.tni_list.entries[0].type_plan = 0x33;
    strcpy(call.tni_list.entries[0].net_id, "5104");
    call.tni_list.entries[1].type_plan = 0x33;
    strcpy(call.tni_list.entries[1].net_id, "5105");

    /* 1. NNI SETUP */
    int setup_len = q933_build_nni_setup(msg_buf, sizeof(msg_buf), &call, &sctx);
    TEST_ASSERT(setup_len > 0, "q933_build_nni_setup produced frame");

    q933_msg_header_t hdr;
    int hlen = q933_parse_header(msg_buf, setup_len, &hdr);
    TEST_ASSERT(hlen == 5, "Header parse successful");
    TEST_ASSERT(hdr.protocol_disc == Q933_PROTOCOL_DISC, "Protocol discriminator is 0x08");
    TEST_ASSERT(hdr.call_ref_value == 0x002A, "Call Reference is 42");
    TEST_ASSERT(hdr.message_type == Q933_MSG_SETUP, "Message type is SETUP");

    /* 2. NNI CALL PROCEEDING */
    int cp_len = q933_build_nni_call_proceeding(msg_buf, sizeof(msg_buf), &call);
    TEST_ASSERT(cp_len > 0, "q933_build_nni_call_proceeding produced frame");
    hlen = q933_parse_header(msg_buf, cp_len, &hdr);
    TEST_ASSERT(hdr.message_type == Q933_MSG_CALL_PROCEEDING, "Message type is CALL PROCEEDING");

    /* 3. NNI CONNECT */
    strcpy(call.connected_number, "510401021002");
    int conn_len = q933_build_nni_connect(msg_buf, sizeof(msg_buf), &call);
    TEST_ASSERT(conn_len > 0, "q933_build_nni_connect produced frame");
    hlen = q933_parse_header(msg_buf, conn_len, &hdr);
    TEST_ASSERT(hdr.message_type == Q933_MSG_CONNECT, "Message type is CONNECT");

    /* 4. NNI RELEASE with CNI */
    int rel_len = q933_build_nni_release(msg_buf, sizeof(msg_buf), &call, Q850_CAUSE_NORMAL_CLEARING, "5104", 0x33);
    TEST_ASSERT(rel_len > 0, "q933_build_nni_release produced frame");
    hlen = q933_parse_header(msg_buf, rel_len, &hdr);
    TEST_ASSERT(hdr.message_type == Q933_MSG_RELEASE, "Message type is RELEASE");

    /* 5. NNI RELEASE COMPLETE with CNI */
    int rc_len = q933_build_nni_release_complete(msg_buf, sizeof(msg_buf), 0x002A, 1, 2, Q850_CAUSE_CIRCUIT_NOT_AVAILABLE, "5104", 0x33);
    TEST_ASSERT(rc_len > 0, "q933_build_nni_release_complete produced frame");
    hlen = q933_parse_header(msg_buf, rc_len, &hdr);
    TEST_ASSERT(hdr.message_type == Q933_MSG_RELEASE_COMPLETE, "Message type is RELEASE COMPLETE");
}

void test_q850_cause_lookup(void) {
    printf("Testing Q.850 Cause Descriptions...\n");
    TEST_ASSERT(strcmp(q850_get_cause_str(Q850_CAUSE_CIRCUIT_NOT_AVAILABLE), "Requested circuit/channel not available") == 0,
                "Cause 44 description is correct");
    TEST_ASSERT(strcmp(q850_get_cause_str(Q850_CAUSE_EXCESS_REPETITIONS_OF_IE), "Excess repetitions of information element") == 0,
                "Cause 104 description is correct");
    TEST_ASSERT(strcmp(q850_get_cause_str(Q850_CAUSE_PREEMPTION), "Preemption") == 0,
                "Cause 8 description is correct");
    TEST_ASSERT(strcmp(q850_get_cause_str(Q850_CAUSE_FACILITY_NOT_IMPLEMENTED), "Requested facility not implemented") == 0,
                "Cause 69 description is correct");
}

int main(void) {
    printf("============================================================\n");
    printf("  RUNNING X.76 NNI Q.933 IE PARSER & BUILDER UNIT TESTS     \n");
    printf("============================================================\n");

    test_call_ident_ie();
    test_transit_net_id_ie();
    test_clearing_net_id_ie();
    test_gat_ie();
    test_nni_messages();
    test_q850_cause_lookup();

    printf("\n------------------------------------------------------------\n");
    printf("Tests Passed: %d, Failed: %d\n", g_tests_passed, g_tests_failed);
    printf("============================================================\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
