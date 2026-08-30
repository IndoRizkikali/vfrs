/*
 * test_q933_parser.c - Unit Test for Q.933 Message Builder & Parser
 * Virtual Frame Relay Switch - Phase 1B
 */

#include "vfr.h"
#include "svc/svc_sig_common.h"
#include "svc/svc_sig_iel.h"
#include "svc/svc_sig_iep.h"
#include <assert.h>

int main(void) {
    printf("=== Running Q.933 Parser & Builder Unit Test ===\n");

    u8 buf[512];

    /* 1. Test Header Building and Parsing (2-byte CRV) */
    int len = q933_build_header(buf, sizeof(buf), 0x0123, 0, 2, Q933_MSG_SETUP);
    assert(len == 5);
    assert(buf[0] == 0x08); /* Protocol Discriminator */
    assert(buf[1] == 0x02); /* CRV len = 2 */
    assert(buf[4] == Q933_MSG_SETUP);

    q933_msg_header_t hdr;
    int parsed_len = q933_parse_header(buf, len, &hdr);
    assert(parsed_len == 5);
    assert(hdr.protocol_disc == 0x08);
    assert(hdr.call_ref_len == 2);
    assert(hdr.call_ref_value == 0x0123);
    assert(hdr.call_ref_flag == 0);
    assert(hdr.message_type == Q933_MSG_SETUP);
    printf("[PASS] Header 2-byte CRV build & parse verified (CRV=0x0123, type=SETUP)\n");

    /* 2. Test 1-byte CRV translation & parsing (Postel's principle compliance) */
    len = q933_build_header(buf, sizeof(buf), 0x0005, 1, 1, Q933_MSG_CONNECT);
    assert(len == 4);
    assert(buf[1] == 0x01); /* CRV len = 1 */

    parsed_len = q933_parse_header(buf, len, &hdr);
    assert(parsed_len == 4);
    assert(hdr.call_ref_len == 1);
    assert(hdr.call_ref_value == 0x0005);
    assert(hdr.call_ref_flag == 1);
    assert(hdr.message_type == Q933_MSG_CONNECT);
    printf("[PASS] Header 1-byte CRV build & parse translated cleanly (CRV=0x0005, flag=1)\n");

    /* 3. Test Called Party Number IE */
    len = q933_build_called_number(buf, sizeof(buf), "510401011002", 1, 3);
    assert(len > 3);
    assert(buf[0] == Q933_IE_CALLED_NUMBER);

    char num_buf[32];
    u8 type = 0, plan = 0, pres = 0, scr = 0;
    int res = q933_parse_number_ie(&buf[2], buf[1], num_buf, sizeof(num_buf), &type, &plan, &pres, &scr);
    assert(res == 0);
    assert(strcmp(num_buf, "510401011002") == 0);
    assert(type == 1);
    assert(plan == 3);
    printf("[PASS] Called Party Number IE build & parse verified (Num=%s)\n", num_buf);

    /* 4. Test Calling Party Number IE with Presentation & Screening */
    len = q933_build_calling_number(buf, sizeof(buf), "510401011001", 1, 3, 0, 3);
    assert(buf[0] == Q933_IE_CALLING_NUMBER);
    res = q933_parse_number_ie(&buf[2], buf[1], num_buf, sizeof(num_buf), &type, &plan, &pres, &scr);
    assert(res == 0);
    assert(strcmp(num_buf, "510401011001") == 0);
    assert(pres == 0); /* Presentation allowed */
    assert(scr == 3);  /* Network provided */
    printf("[PASS] Calling Party Number IE build & parse verified (Pres=Allowed, Scr=Network)\n");

    /* 5. Test DLCI IE */
    len = q933_build_dlci_ie(buf, sizeof(buf), 512, 2);
    assert(buf[0] == Q933_IE_DLCI);
    u32 parsed_dlci = 0;
    u8 dlci_len = 0;
    res = q933_parse_dlci_ie(&buf[2], buf[1], &parsed_dlci, &dlci_len);
    printf("  [DEBUG DLCI] buf[2]=0x%02X, buf[3]=0x%02X, parsed_dlci=%u\n", buf[2], buf[3], parsed_dlci);
    assert(res == 0);
    assert(parsed_dlci == 512);
    printf("[PASS] DLCI IE build & parse verified (DLCI=%u)\n", parsed_dlci);

    /* 6. Test Cause IE & Q.850 cause string lookup */
    len = q933_build_cause(buf, sizeof(buf), Q850_LOC_PUBLIC_LOCAL_NET, Q850_CAUSE_UNALLOCATED_NUMBER);
    assert(buf[0] == Q933_IE_CAUSE);
    u8 loc = 0, cause = 0;
    res = q933_parse_cause(&buf[2], buf[1], &loc, &cause);
    assert(res == 0);
    assert(cause == Q850_CAUSE_UNALLOCATED_NUMBER);
    const char *str = q850_get_cause_str(cause);
    assert(str != NULL);
    printf("[PASS] Cause IE & Q.850 lookup verified (Cause #%u: '%s')\n", cause, str);

    /* 7. Test High-Level RELEASE COMPLETE Message Builder */
    len = q933_build_release_complete(buf, sizeof(buf), 0x00A1, 1, 2, Q850_CAUSE_MANDATORY_IE_MISSING);
    assert(len > 5);
    res = q933_parse_header(buf, len, &hdr);
    assert(res == 5);
    assert(hdr.message_type == Q933_MSG_RELEASE_COMPLETE);
    assert(hdr.call_ref_value == 0x00A1);
    /* 8. Test LLCORE IE Builder and Parser */
    q933_llcore_params_t ll_in, ll_out;
    memset(&ll_in, 0, sizeof(ll_in));
    ll_in.fwd_cir = 64000;
    ll_in.fwd_bc = 64000;
    ll_in.fwd_be = 0;
    ll_in.fwd_fmif = 1600;

    len = q933_build_llcore_params(buf, sizeof(buf), &ll_in);
    assert(buf[0] == Q933_IE_LLCORE_PARAMS);
    res = q933_parse_llcore_params(&buf[2], buf[1], &ll_out);
    assert(res == 0);
    assert(ll_out.fwd_cir == 64000);
    assert(ll_out.fwd_bc == 64000);
    printf("[PASS] LLCORE IE build & parse verified (CIR=%u, Bc=%u)\n", ll_out.fwd_cir, ll_out.fwd_bc);

    printf("=== All Q.933 Parser & Builder Tests Passed Successfully! ===\n");
    return 0;
}
