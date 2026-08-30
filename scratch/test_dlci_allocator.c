/*
 * test_dlci_allocator.c - Unit Test for DLCI Range Allocator
 * Virtual Frame Relay Switch - Phase 1A Core Infrastructure
 */

#include "vfr.h"
#include "svc/svc_sig_common.h"
#include "switching/svc_routing_common.h"
#include <assert.h>

int main(void) {
    printf("=== Running DLCI Allocator Unit Test ===\n");

    vfr_dlci_allocator_t alloc;
    int res = svc_dlci_alloc_init(&alloc, 512, 991);
    assert(res == 0);
    assert(alloc.count == 1);
    assert(alloc.ranges[0].start == 512);
    assert(alloc.ranges[0].end == 991);
    printf("[PASS] DLCI Allocator Initialized (512-991, total 480 DLCIs)\n");

    u32 allocated[480];
    for (int i = 0; i < 480; i++) {
        allocated[i] = svc_dlci_alloc(&alloc);
        assert(allocated[i] == (u32)(512 + i));
    }
    printf("[PASS] Allocated 480 DLCIs sequentially (512 to 991)\n");

    /* Verify exhaustion */
    u32 exhausted = svc_dlci_alloc(&alloc);
    assert(exhausted == 0);
    printf("[PASS] Allocator correctly returns 0 when exhausted\n");

    /* Free DLCIs in non-contiguous chunks */
    svc_dlci_free(&alloc, 512);
    svc_dlci_free(&alloc, 513);
    svc_dlci_free(&alloc, 514);
    assert(alloc.count == 1);
    assert(alloc.ranges[0].start == 512);
    assert(alloc.ranges[0].end == 514);
    printf("[PASS] Freed contiguous DLCIs 512..514, merged into single range\n");

    svc_dlci_free(&alloc, 600);
    svc_dlci_free(&alloc, 601);
    assert(alloc.count == 2);
    printf("[PASS] Freed non-contiguous DLCI 600..601, creating second range\n");

    /* Free middle gap 515..599 */
    for (u32 d = 515; d < 600; d++) {
        svc_dlci_free(&alloc, d);
    }
    assert(alloc.count == 1);
    assert(alloc.ranges[0].start == 512);
    assert(alloc.ranges[0].end == 601);
    printf("[PASS] Merged gap 515..599 with adjacent ranges into unified range [512, 601]\n");

    printf("=== All DLCI Allocator Tests Passed Successfully! ===\n");
    return 0;
}
