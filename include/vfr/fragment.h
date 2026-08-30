/*
 * fragment.h - FRF.12 / ITU-T X.36 §9.6 Frame Relay Fragmentation Sublayer
 * Virtual Frame Relay Switch
 */

#ifndef VFR_FRAGMENT_H
#define VFR_FRAGMENT_H

#include "types.h"
#include "platform.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRF12_HDR_LEN           2
#define FRF12_REASM_TIMEOUT_MS  1000
#define FRF12_MAX_FRAGMENTS     16

#define FRF12_FLAG_B            0x80   /* Beginning fragment bit */
#define FRF12_FLAG_E            0x40   /* Ending fragment bit */
#define FRF12_FLAG_C            0x20   /* Control / drop indicator */

/* Reassembly context for a single Virtual Circuit */
typedef struct fr_reasm_ctx_s {
    u8          buf[FR_MAX_FRAMESZ + 128];
    size_t      len;
    u16         expected_seq;
    int         in_progress;
    u64         last_frag_ms;
    u32         frag_count;
} fr_reasm_ctx_t;

/* Initialize reassembly context */
VFR_API void frf12_reasm_init(fr_reasm_ctx_t *ctx);

/*
 * Fragment an outgoing Frame Relay frame according to FRF.12 Format 1.
 *
 * in_frame         - full unfragmented Frame Relay frame (starts with Address field)
 * in_len           - length of in_frame
 * fragment_size    - maximum payload size per fragment (excluding Q.922 address and FRF.12 header)
 * out_frags        - array of buffers to receive generated fragment frames
 * out_lens         - array to receive the lengths of each fragment frame
 * max_frags        - maximum number of entries in out_frags
 * seq_counter      - pointer to 12-bit sequence counter for this VC (0..4095)
 *
 * Returns number of fragments generated (>0) on success, or -1 on error.
 */
VFR_API int frf12_fragment_frame(const u8 *in_frame, size_t in_len,
                                 size_t fragment_size,
                                 u8 out_frags[][FR_MAX_FRAMESZ + 32],
                                 size_t out_lens[],
                                 int max_frags,
                                 u16 *seq_counter);

/*
 * Process an incoming fragment frame and reassemble into a complete frame.
 *
 * ctx              - reassembly context for this VC
 * frag_frame       - incoming fragment frame (starts with Q.922 Address field)
 * frag_len         - length of frag_frame
 * out_frame        - buffer to receive complete reassembled frame (starts with Q.922 Address)
 * out_len          - out: length of reassembled frame
 * is_complete      - out: set to 1 if a complete frame was reassembled, 0 if fragment buffered
 *
 * Returns 0 on success, -1 on corrupt / dropped fragment.
 */
VFR_API int frf12_reassemble_frame(fr_reasm_ctx_t *ctx,
                                   const u8 *frag_frame, size_t frag_len,
                                   u8 *out_frame, size_t *out_len,
                                   int *is_complete);

#ifdef __cplusplus
}
#endif

#endif /* VFR_FRAGMENT_H */
