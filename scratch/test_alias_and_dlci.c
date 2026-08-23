/*
 * test_alias_and_dlci.c - Unit test for Alias Translation & Per-Port DLCI Isolation
 * VFRS - Virtual Frame Relay Switch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "vfr.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "fr_switching/svc_routing_common.h"
#include "svc_signalling/svc_sig_common.h"

int main(void) {
    printf("============================================================\n");
    printf(" TESTING: ALIAS TRANSLATION & PER-PORT DLCI ISOLATION\n");
    printf("============================================================\n");

    /* Create switch context */
    vfrs_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.dnic = 5104;
    ctx.sgc = 1;
    ctx.sic = 1;
    svc_numbering_init(&ctx);

    /* 1. Register Port 1 with primary E.164 (510401011000) and alias (1000) */
    vfr_port_t port1;
    memset(&port1, 0, sizeof(port1));
    snprintf(port1.name, sizeof(port1.name), "uni0/0");
    vfr_svc_ctx_t sctx1;
    memset(&sctx1, 0, sizeof(sctx1));
    svc_dlci_alloc_init(&sctx1.dlci_alloc, 512, 991);
    port1.svc_ctx = &sctx1;

    ctx.ports[0] = &port1;
    ctx.port_count = 1;

    int rc = svc_numbering_add(&ctx, "uni0/0", "e164", "510401011000", "e164", "1000");
    assert(rc == 0);

    /* 2. Register Port 2 with primary E.164 (510401011001) and alias (1001) */
    vfr_port_t port2;
    memset(&port2, 0, sizeof(port2));
    snprintf(port2.name, sizeof(port2.name), "uni0/1");
    vfr_svc_ctx_t sctx2;
    memset(&sctx2, 0, sizeof(sctx2));
    svc_dlci_alloc_init(&sctx2.dlci_alloc, 512, 991);
    port2.svc_ctx = &sctx2;

    ctx.ports[1] = &port2;
    ctx.port_count = 2;

    rc = svc_numbering_add(&ctx, "uni0/1", "e164", "510401011001", "e164", "1001");
    assert(rc == 0);

    /* Test 1: Alias Translation */
    printf("-> Test 1: Alias Translation (1001 -> 510401011001)...\n");
    char primary_buf[SVC_MAX_ADDR_LEN + 1];
    int translated = svc_get_primary_number(&ctx, "1001", primary_buf, sizeof(primary_buf));
    assert(translated == 1);
    assert(strcmp(primary_buf, "510401011001") == 0);
    printf("   [PASS] Alias '1001' successfully translated to primary number '%s'\n", primary_buf);

    /* Test 2: Primary number passed as-is */
    printf("-> Test 2: Primary Number Pass-Through (510401011000)...\n");
    translated = svc_get_primary_number(&ctx, "510401011000", primary_buf, sizeof(primary_buf));
    assert(translated == 0);
    assert(strcmp(primary_buf, "510401011000") == 0);
    printf("   [PASS] Primary number '510401011000' passed through without translation\n");

    /* Test 3: Per-Port DLCI Pool Independence */
    printf("-> Test 3: Per-Port DLCI Pool Independence...\n");
    u32 dlci_p1 = svc_dlci_alloc(&sctx1.dlci_alloc);
    u32 dlci_p2 = svc_dlci_alloc(&sctx2.dlci_alloc);
    assert(dlci_p1 == 512);
    assert(dlci_p2 == 512);
    printf("   [PASS] Both Port uni0/0 and Port uni0/1 allocated DLCI 512 independently\n");

    printf("------------------------------------------------------------\n");
    printf(" SUCCESS: ALL ALIAS & DLCI ISOLATION TESTS PASSED!\n");
    printf("============================================================\n");
    return 0;
}
