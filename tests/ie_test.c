/*
 * tests/ie_test.c - Unit tests for Q.933 Information Elements
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "vfr.h"
#include "svc_signalling/svc_sig_iep.h"
#include "fr_switching/fr_frame.h"

/* Global context stub */
vfrs_ctx_t *g_vfrs = NULL;

/* Stubs for required external symbols in dependencies */
void lmi_free(vfr_port_t *port) { (void)port; }
void svc_free_port(vfr_port_t *port) { (void)port; }
int svc_numbering_is_all_zeros(vfrs_ctx_t *ctx, const char *number) { (void)ctx; (void)number; return 0; }
int svc_uni_handle_virtual_switch_call(vfr_port_t *port, vfr_call_t *call) { (void)port; (void)call; return 0; }
vfr_port_t *svc_find_subscriber(vfrs_ctx_t *ctx, const char *number) { (void)ctx; (void)number; return NULL; }
int svc_get_primary_number(vfrs_ctx_t *ctx, const char *alias, char *primary, size_t max_len) { (void)ctx; (void)alias; (void)primary; (void)max_len; return 0; }
vfr_port_t *svc_route_lookup(vfrs_ctx_t *ctx, const char *number) { (void)ctx; (void)number; return NULL; }
vfr_call_t *svc_alloc_call_ref(vfr_svc_ctx_t *sctx, u16 call_ref, u8 flag) { (void)sctx; (void)call_ref; (void)flag; return NULL; }
u32 svc_dlci_alloc(vfr_dlci_allocator_t *alloc) { (void)alloc; return 0; }
vfr_call_t *svc_find_call(vfr_svc_ctx_t *sctx, u16 call_ref, u8 flag) { (void)sctx; (void)call_ref; (void)flag; return NULL; }
void svc_destroy_data_pvcs(vfrs_ctx_t *ctx, vfr_call_t *call) { (void)ctx; (void)call; }
void send_sig_frame(vfr_port_t *port, const u8 *data, size_t len) { (void)port; (void)data; (void)len; }
void svc_free_call(vfr_svc_ctx_t *sctx, vfr_call_t *call) { (void)sctx; (void)call; }
void svc_initiate_clearing_before_active(vfr_port_t *port, vfr_svc_ctx_t *sctx, vfr_call_t *call, u8 cause) { (void)port; (void)sctx; (void)call; (void)cause; }
int lmi_port_dlci_len(vfr_port_t *port) { (void)port; return 2; }
u64 get_tick_count(void) { return 0; }
vfr_dlci_entry_t *port_lookup_dlci(vfr_port_t *port, u32 dlci) { (void)port; (void)dlci; return NULL; }

int mutex_init(mutex_t *m) { (void)m; return 0; }
void mutex_destroy(mutex_t *m) { (void)m; }
void mutex_lock(mutex_t *m) { (void)m; }
void mutex_unlock(mutex_t *m) { (void)m; }
void vfr_log(enum log_level level, const char *file, int line, const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)fmt;
}

int main() {
    printf("Starting Q.933 Information Element Unit Tests...\n");

    /* ----------------------------------------------------------
     * Test Case 1: Bearer Capability IE
     * ---------------------------------------------------------- */
    {
        u8 buf[128];
        memset(buf, 0, sizeof(buf));
        int len = q933_build_bearer_capability(buf, sizeof(buf));
        assert(len == 5);
        assert(buf[0] == 0x04); /* Bearer Capability ID */
        assert(buf[1] == 0x03); /* Length = 3 */
        assert(buf[2] == 0x88);
        assert(buf[3] == 0xA0); /* Ext=1, Transfer Mode=01 (Frame mode) */
        assert(buf[4] == 0xCF); /* Standard L2 Identifier = 10 */

        /* Parse and check */
        int parse_res = q933_parse_bearer_capability(&buf[2], 3);
        assert(parse_res == 0);
        printf("Test Case 1 (Bearer Capability IE): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 2: 2-Octet DLCI IE
     * ---------------------------------------------------------- */
    {
        u8 buf[128];
        memset(buf, 0, sizeof(buf));
        u32 dlci = 512;
        int len = q933_build_dlci_ie(buf, sizeof(buf), dlci, 2);
        assert(len == 4);
        assert(buf[0] == 0x19); /* DLCI IE ID */
        assert(buf[1] == 0x02); /* Length = 2 */
        assert(buf[2] == 0x60); /* Ext=0, Pref/Excl=1, DLCI MSB 6 bits = 0x20 */
        assert(buf[3] == 0x80); /* Ext=1, DLCI LSB 4 bits = 0 */

        u32 parsed_dlci = 0;
        u8 parsed_len = 0;
        int parse_res = q933_parse_dlci_ie(&buf[2], 2, &parsed_dlci, &parsed_len);
        assert(parse_res == 0);
        assert(parsed_dlci == dlci);
        assert(parsed_len == 2);
        printf("Test Case 2 (2-Octet DLCI IE): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 3: 4-Octet DLCI IE
     * ---------------------------------------------------------- */
    {
        u8 buf[128];
        memset(buf, 0, sizeof(buf));
        u32 dlci = 500000; /* Requires 4 octets */
        int len = q933_build_dlci_ie(buf, sizeof(buf), dlci, 4);
        assert(len == 6);
        assert(buf[0] == 0x19); /* DLCI IE ID */
        assert(buf[1] == 0x04); /* Length = 4 */

        /* Let's verify standard bit positions:
         * dlci = 500000 = 0x07A120 = 000 0111 1010 0001 0010 0000 binary.
         * DLCI bits split:
         * DLCI 22-17 (6 bits): (500000 >> 17) & 0x3F = 3 = 000011 binary.
         *   buf[2] = 0x40 | 3 = 0x43 (ext=0, Pref/Excl=1)
         * DLCI 16-13 (4 bits): (500000 >> 13) & 0x0F = 13 = 1101 binary.
         *   buf[3] = 13 << 3 = 0x68 = 0110 1000 binary (ext=0, Reserved=000)
         * DLCI 12-6 (7 bits): (500000 >> 6) & 0x7F = 4 = 000 0100 binary.
         *   buf[4] = 4 = 0x04 (ext=0)
         * DLCI 5-0 (6 bits): 500000 & 0x3F = 32 = 100000 binary.
         *   buf[5] = 0x80 | (32 << 1) = 0x80 | 64 = 0xC0 (ext=1, Reserved=0)
         */
        assert(buf[2] == 0x43);
        assert(buf[3] == 0x68);
        assert(buf[4] == 0x04);
        assert(buf[5] == 0xC0);

        u32 parsed_dlci = 0;
        u8 parsed_len = 0;
        int parse_res = q933_parse_dlci_ie(&buf[2], 4, &parsed_dlci, &parsed_len);
        assert(parse_res == 0);
        assert(parsed_dlci == dlci);
        assert(parsed_len == 4);
        printf("Test Case 3 (4-Octet DLCI IE): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 4: LLCORE CIR Magnitude/Multiplier IE
     * ---------------------------------------------------------- */
    {
        u8 buf[128];
        memset(buf, 0, sizeof(buf));
        q933_llcore_params_t params;
        memset(&params, 0, sizeof(params));
        params.fwd_cir = 64000;
        
        int len = q933_build_llcore_params(buf, sizeof(buf), &params);
        assert(len > 0);

        /* Inspect Group 4 CIR parameter bytes in buf:
         * Sub-IE ID = 0x0A (Throughput)
         * mag = 3 (10^3), mult = 64.
         * buf[i+1] = ((mag & 0x07) << 4) | ((mult >> 7) & 0x0F) = (3 << 4) | 0 = 0x30.
         * buf[i+2] = 0x80 | (mult & 0x7F) = 0x80 | 64 = 0xC0.
         */
        u8 found_cir_sub_ie = 0;
        for (int i = 2; i < len - 2; i++) {
            if (buf[i] == 0x0A) {
                found_cir_sub_ie = 1;
                assert(buf[i+1] == 0x30);
                assert(buf[i+2] == 0xC0);
                break;
            }
        }
        assert(found_cir_sub_ie);

        q933_llcore_params_t parsed_params;
        int parse_res = q933_parse_llcore_params(&buf[2], buf[1], &parsed_params);
        assert(parse_res == 0);
        assert(parsed_params.fwd_cir == 64000);
        assert(parsed_params.bwd_cir == 64000);
        printf("Test Case 4 (LLCORE CIR Magnitude/Multiplier IE): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 5: LLCORE CIR with Larger Value (2.048 Mbps)
     * ---------------------------------------------------------- */
    {
        u8 buf[128];
        memset(buf, 0, sizeof(buf));
        q933_llcore_params_t params;
        memset(&params, 0, sizeof(params));
        params.fwd_cir = 2048000; /* mag = 4 (10^4), mult = 205 (best approximation) */
        
        int len = q933_build_llcore_params(buf, sizeof(buf), &params);
        assert(len > 0);

        q933_llcore_params_t parsed_params;
        int parse_res = q933_parse_llcore_params(&buf[2], buf[1], &parsed_params);
        assert(parse_res == 0);
        assert(parsed_params.fwd_cir == 2050000);
        printf("Test Case 5 (LLCORE CIR Larger Value IE): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 6: 0-Octet and 2-Octet Global Call Reference Parsing
     * ---------------------------------------------------------- */
    {
        /* 0-octet Global CRV SETUP (not standard, but robustly accepted) */
        u8 buf_0octet[] = { 0x08, 0x00, 0x46 }; /* Prot=0x08, CRV len=0, MsgType=RESTART */
        q933_msg_header_t hdr;
        int parsed_len = q933_parse_header(buf_0octet, sizeof(buf_0octet), &hdr);
        assert(parsed_len == 3);
        assert(hdr.protocol_disc == 0x08);
        assert(hdr.call_ref_len == 0);
        assert(hdr.call_ref_value == 0);
        assert(hdr.message_type == 0x46);

        /* 2-octet Global CRV RESTART (ITU-T X.36 compliant) */
        u8 buf_2octet[] = { 0x08, 0x02, 0x00, 0x00, 0x46 }; /* Prot=0x08, CRV len=2, CRV=0x0000, MsgType=RESTART */
        parsed_len = q933_parse_header(buf_2octet, sizeof(buf_2octet), &hdr);
        assert(parsed_len == 5);
        assert(hdr.protocol_disc == 0x08);
        assert(hdr.call_ref_len == 2);
        assert(hdr.call_ref_value == 0);
        assert(hdr.message_type == 0x46);
        printf("Test Case 6 (0-Octet & 2-Octet Global CRV Parsing): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 7: Bearer Capability content mismatch returns -2
     * ---------------------------------------------------------- */
    {
        u8 bad_bc[] = { 0x88, 0xC0, 0x8F }; /* Octet 6 L2 Ident = 00 (invalid) */
        int parse_res = q933_parse_bearer_capability(bad_bc, sizeof(bad_bc));
        assert(parse_res == -2);
        printf("Test Case 7 (Bearer Capability Invalid Content returns -2): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 8: SETUP message building with LLC & User-User IEs
     * ---------------------------------------------------------- */
    {
        u8 buf[512];
        memset(buf, 0, sizeof(buf));
        vfr_call_t call;
        memset(&call, 0, sizeof(call));
        vfr_svc_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));

        call.call_ref = 0x1234;
        call.call_ref_flag = 0;
        call.call_ref_len = 2;
        call.ingress_dlci = 512;
        call.rev_charge_requested = 1;
        call.priority_present = 1;
        call.ftp_out = 4;
        call.ftp_in = 4;
        call.fdp_out = 2;
        call.fdp_in = 2;
        call.srv_class = 1;
        strcpy(call.calling_number, "12345");
        strcpy(call.called_number, "67890");

        /* Set LLC and User-User data */
        call.llc_len = 4;
        memcpy(call.llc_data, "\x01\x02\x03\x04", 4);
        call.uu_len = 5;
        memcpy(call.uu_data, "HELLO", 5);

        int len = q933_build_setup(buf, sizeof(buf), &call, &sctx);
        assert(len > 0);

        /* Verify Q.933 Header: Prot=0x08, Len=2, CRV, MsgType=SETUP(0x05) */
        assert(buf[0] == 0x08);
        assert(buf[1] == 0x02);
        assert(buf[4] == 0x05);

        /* Verify Information Element IDs appear in monotonic ascending order */
        u8 last_ie = 0;
        size_t off = 5;
        int found_llc = 0;
        int found_uu = 0;

        while (off + 2 <= (size_t)len) {
            u8 ie_id = buf[off];
            u8 ie_len = buf[off + 1];
            assert(ie_id > last_ie); /* Must be strictly ascending */
            last_ie = ie_id;

            if (ie_id == 0x7C) found_llc = 1;
            if (ie_id == 0x7E) found_uu = 1;

            off += 2 + ie_len;
        }

        assert(found_llc == 1);
        assert(found_uu == 1);
        assert(off == (size_t)len);
        printf("Test Case 8 (SETUP with LLC & User-User IEs in ascending order): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 9: 2, 3, and 4-Octet Address Encoding/Decoding with Flags
     * ---------------------------------------------------------- */
    {
        /* 2-octet test */
        u8 addr2[2];
        memset(addr2, 0, sizeof(addr2));
        fr_encode_dlci_with_flags(addr2, 100, 1, 0, 1, 1, 2);
        fr_addr_t dec2;
        memset(&dec2, 0, sizeof(dec2));
        fr_decode_addr(addr2, &dec2);
        assert(dec2.dlci == 100);
        assert(dec2.fecn == 1);
        assert(dec2.becn == 0);
        assert(dec2.de == 1);
        assert(dec2.cr == 1);
        assert(dec2.ea == 1);

        /* 3-octet test */
        u8 addr3[3];
        memset(addr3, 0, sizeof(addr3));
        fr_encode_dlci_with_flags(addr3, 2048, 0, 1, 1, 0, 3);
        assert((addr3[0] & 0x01) == 0); /* EA=0 */
        assert((addr3[1] & 0x01) == 0); /* EA=0 */
        assert((addr3[2] & 0x01) == 1); /* EA=1 */
        assert((addr3[2] & 0x02) == 0); /* D/C=0 */
        fr_addr_t dec3;
        memset(&dec3, 0, sizeof(dec3));
        fr_decode_addr(addr3, &dec3);
        assert(dec3.dlci == 2048);
        assert(dec3.fecn == 0);
        assert(dec3.becn == 1);
        assert(dec3.de == 1);
        assert(dec3.cr == 0);
        assert(dec3.ea == 1);

        /* 4-octet test */
        u8 addr4[4];
        memset(addr4, 0, sizeof(addr4));
        fr_encode_dlci_with_flags(addr4, 500000, 1, 1, 0, 1, 4);
        assert((addr4[0] & 0x01) == 0); /* EA=0 */
        assert((addr4[1] & 0x01) == 0); /* EA=0 */
        assert((addr4[2] & 0x01) == 0); /* EA=0 */
        assert((addr4[3] & 0x01) == 1); /* EA=1 */
        fr_addr_t dec4;
        memset(&dec4, 0, sizeof(dec4));
        fr_decode_addr(addr4, &dec4);
        assert(dec4.dlci == 500000);
        assert(dec4.fecn == 1);
        assert(dec4.becn == 1);
        assert(dec4.de == 0);
        assert(dec4.cr == 1);
        assert(dec4.ea == 1);

        printf("Test Case 9 (2/3/4-Octet DLCI Encode & Decode with Flags): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 10: Flag Preservation during DLCI Rewriting (Q.922 §A.3.3)
     * ---------------------------------------------------------- */
    {
        u8 frame[8];
        memset(frame, 0, sizeof(frame));
        fr_encode_dlci_with_flags(frame, 100, 1, 1, 1, 1, 2);
        fr_rewrite_dlci(frame, 100, 200);

        fr_addr_t rewritten;
        memset(&rewritten, 0, sizeof(rewritten));
        fr_decode_addr(frame, &rewritten);
        assert(rewritten.dlci == 200);
        assert(rewritten.fecn == 1);
        assert(rewritten.becn == 1);
        assert(rewritten.de == 1);
        assert(rewritten.cr == 1);
        printf("Test Case 10 (Flag Preservation in DLCI Rewriting): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 11: Reconciled DLCI Range Validation (Q.922 & X.36)
     * ---------------------------------------------------------- */
    {
        /* 2-octet interface */
        assert(fr_dlci_valid_for_width(0, 2) == 0);
        assert(fr_dlci_valid_for_width(15, 2) == 0);
        assert(fr_dlci_valid_for_width(16, 2) == 1);
        assert(fr_dlci_valid_for_width(991, 2) == 1);
        assert(fr_dlci_valid_for_width(992, 2) == 0);
        assert(fr_dlci_valid_for_width(1023, 2) == 0);
        assert(fr_dlci_valid_for_width(1024, 2) == 0);

        /* 3-octet interface */
        assert(fr_dlci_valid_for_width(100, 3) == 0);
        assert(fr_dlci_valid_for_width(1024, 3) == 1);
        assert(fr_dlci_valid_for_width(63487, 3) == 1);
        assert(fr_dlci_valid_for_width(63488, 3) == 0);

        /* 4-octet interface (ITU-T X.36 Table 9-2) */
        assert(fr_dlci_valid_for_width(16, 4) == 1);
        assert(fr_dlci_valid_for_width(991, 4) == 1);
        assert(fr_dlci_valid_for_width(992, 4) == 0);
        assert(fr_dlci_valid_for_width(1023, 4) == 0);
        assert(fr_dlci_valid_for_width(1024, 4) == 1);
        assert(fr_dlci_valid_for_width(8388607, 4) == 1);
        assert(fr_dlci_valid_for_width(8388608, 4) == 0);

        printf("Test Case 11 (Reconciled DLCI Range Validation): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 12: Bounded fr_get_addr_len Buffer Check
     * ---------------------------------------------------------- */
    {
        u8 partial[3] = { 0x00, 0x00, 0x00 }; /* EA bits 0, 0, 0 */
        size_t len = fr_get_addr_len(partial, 3);
        assert(len == 3); /* Must not return 4 if max_len is 3 */
        printf("Test Case 12 (fr_get_addr_len Boundary Safety): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 13: CLLM XID Construction & DLCI Parameter 3 Parsing (X.36 Annex C)
     * ---------------------------------------------------------- */
    {
        u8 xid_buf[256];
        memset(xid_buf, 0, sizeof(xid_buf));
        u32 dlcis[2] = { 100, 200 };
        size_t build_len = cllm_build_xid(xid_buf, sizeof(xid_buf), 0x02, dlcis, 2, 2);
        assert(build_len > 0);

        /* Find PI=3 inside parameter block and verify octet 2 bits 4..1 are 0000 */
        size_t p3_pos = 0;
        for (size_t k = 0; k < build_len - 4; k++) {
            if (xid_buf[k] == CLLM_PI_DLCI_LIST) {
                p3_pos = k;
                break;
            }
        }
        assert(p3_pos > 0);
        u8 p3_len = xid_buf[p3_pos + 1];
        assert(p3_len == 4); /* 2 DLCIs * 2 octets = 4 */
        /* Check DLCI 100: octet 2 (xid_buf[p3_pos + 3]) lower 4 bits must be 0 */
        assert((xid_buf[p3_pos + 3] & 0x0F) == 0x00);
        /* Check DLCI 200: octet 2 (xid_buf[p3_pos + 5]) lower 4 bits must be 0 */
        assert((xid_buf[p3_pos + 5] & 0x0F) == 0x00);

        /* Parse XID and verify decoded DLCIs */
        u8 parsed_cause = 0;
        u32 parsed_dlcis[16];
        int parsed_count = 0;
        int parse_res = cllm_parse_xid(xid_buf, build_len, &parsed_cause, parsed_dlcis, &parsed_count, 16);
        assert(parse_res == 0);
        assert(parsed_cause == 0x02);
        assert(parsed_count == 2);
        assert(parsed_dlcis[0] == 100);
        assert(parsed_dlcis[1] == 200);

        printf("Test Case 13 (CLLM XID DLCI Parameter 3 Bit Alignment & Parsing): PASSED\n");
    }

    /* ----------------------------------------------------------
     * Test Case 14: CLLM Congestion Handling Across Both PVCs & SVCs
     * ---------------------------------------------------------- */
    {
        vfrs_ctx_t dummy_ctx;
        memset(&dummy_ctx, 0, sizeof(dummy_ctx));
        g_vfrs = &dummy_ctx;

        /* Create a forward/reverse PVC pair (DLCI 100 on port1 -> port2) */
        vfr_dlci_entry_t pvc_fwd, pvc_rev;
        memset(&pvc_fwd, 0, sizeof(pvc_fwd));
        memset(&pvc_rev, 0, sizeof(pvc_rev));
        strcpy(pvc_fwd.port_in, "port1");
        strcpy(pvc_fwd.port_out, "port2");
        pvc_fwd.dlci_in = 100;
        pvc_fwd.dlci_out = 100;
        pvc_fwd.vc_type = VC_TYPE_PVC;
        pvc_fwd.active = 1;
        pvc_fwd.reverse_entry = &pvc_rev;
        pvc_rev.reverse_entry = &pvc_fwd;

        /* Create a forward/reverse SVC pair (DLCI 550 on port1 -> port2) */
        vfr_dlci_entry_t svc_fwd, svc_rev;
        memset(&svc_fwd, 0, sizeof(svc_fwd));
        memset(&svc_rev, 0, sizeof(svc_rev));
        strcpy(svc_fwd.port_in, "port1");
        strcpy(svc_fwd.port_out, "port2");
        svc_fwd.dlci_in = 550;
        svc_fwd.dlci_out = 550;
        svc_fwd.vc_type = VC_TYPE_SVC;
        svc_fwd.active = 1;
        svc_fwd.reverse_entry = &svc_rev;
        svc_rev.reverse_entry = &svc_fwd;

        /* Link them into g_vfrs->dlci_table */
        dummy_ctx.dlci_table[0] = &pvc_fwd;
        pvc_fwd.next = &svc_fwd;
        svc_fwd.next = NULL;

        /* Simulate CLLM frame containing DLCI 100 (PVC) and DLCI 550 (SVC) received on port1 */
        u8 xid_buf[256];
        u32 congested_dlcis[2] = { 100, 550 };
        size_t xid_len = cllm_build_xid(xid_buf, sizeof(xid_buf), 0x02, congested_dlcis, 2, 2);
        assert(xid_len > 0);

        vfr_port_t port1;
        memset(&port1, 0, sizeof(port1));
        strcpy(port1.name, "port1");

        int handle_rc = cllm_handle_frame(&port1, xid_buf, xid_len);
        assert(handle_rc == 0);

        /* Verify BOTH the PVC reverse entry AND the SVC reverse entry have peer_congested = 1 */
        assert(pvc_rev.peer_congested == 1);
        assert(svc_rev.peer_congested == 1);

        g_vfrs = NULL;
        printf("Test Case 14 (CLLM Congestion Notification on PVCs and SVCs): PASSED\n");
    }

    printf("ALL INFORMATION ELEMENT & CONFORMANCE UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
