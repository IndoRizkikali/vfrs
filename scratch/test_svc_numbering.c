/*
 * test_svc_numbering.c - Unit Test for SVC Numbering Register & Macro Expansion
 * Virtual Frame Relay Switch - Phase 1A Core Infrastructure
 */

#include "vfr.h"
#include "svc/svc_sig_common.h"
#include "ports/svc_numbering/svc_numbering.h"
#include <assert.h>

int main(void) {
    printf("=== Running SVC Numbering Engine Unit Test ===\n");

    vfrs_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.dnic = 5104;
    ctx.sgclen = 2;
    ctx.sgc = 1;
    ctx.siclen = 2;
    ctx.sic = 1;
    ctx.subnumlen = 4;

    svc_numbering_init(&ctx);

    char buf[64];
    /* Test DGE expansion */
    svc_numbering_expand(&ctx, "DGE1001", buf, sizeof(buf));
    assert(strcmp(buf, "510401011001") == 0);
    printf("[PASS] DGE Macro Expansion: DGE1001 -> %s\n", buf);

    /* Test dnic,sgrc,sysc expansion */
    svc_numbering_expand(&ctx, "dnic,sgrc,sysc,1001", buf, sizeof(buf));
    assert(strcmp(buf, "510401011001") == 0);
    printf("[PASS] Individual Macro Expansion: dnic,sgrc,sysc,1001 -> %s\n", buf);

    /* Test all-zeros self number detection */
    assert(svc_numbering_is_all_zeros(&ctx, "DGE0000") == 1);
    assert(svc_numbering_is_all_zeros(&ctx, "510401010000") == 1);
    assert(svc_numbering_is_all_zeros(&ctx, "DGE1001") == 0);
    printf("[PASS] All-zeros reserved switch number detection verified\n");

    /* Test subscriber registration */
    int res1 = svc_numbering_add(&ctx, "uni0/0", "x121", "DGE1001", "x121", "");
    assert(res1 == 0);

    /* Prohibit all-zeros subscriber assignment on user port */
    int res_zeros = svc_numbering_add(&ctx, "uni0/1", "x121", "DGE0000", "x121", "");
    assert(res_zeros == -1);
    printf("[PASS] All-zeros assignment to user port rejected as expected\n");

    printf("=== All SVC Numbering Tests Passed Successfully! ===\n");
    return 0;
}
