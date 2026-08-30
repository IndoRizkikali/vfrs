/*
 * fr_fragment.c - FRF.12 / ITU-T X.36 §9.6 Frame Relay Fragmentation Sublayer
 * Virtual Frame Relay Switch
 */

#include "vfr.h"
#include "switching/fr_frame.h"

void frf12_reasm_init(fr_reasm_ctx_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

int frf12_fragment_frame(const u8 *in_frame, size_t in_len,
                         size_t fragment_size,
                         u8 out_frags[][FR_MAX_FRAMESZ + 32],
                         size_t out_lens[],
                         int max_frags,
                         u16 *seq_counter)
{
    if (!in_frame || in_len < 2 || !out_frags || !out_lens || max_frags <= 0 || !seq_counter) {
        return -1;
    }

    size_t addr_len = fr_get_addr_len(in_frame, in_len);
    if (addr_len < 2 || in_len < addr_len) {
        return -1;
    }

    size_t payload_len = in_len - addr_len;
    if (payload_len == 0) {
        /* No payload to fragment, return frame as single fragment */
        memcpy(out_frags[0], in_frame, in_len);
        out_lens[0] = in_len;
        return 1;
    }

    if (fragment_size == 0 || payload_len <= fragment_size) {
        /* Frame fits in single fragment; prepend B=1, E=1 FRF.12 header if fragment_size was set */
        if (fragment_size > 0) {
            u16 seq = (*seq_counter) & 0x0FFF;
            *seq_counter = (*seq_counter + 1) & 0x0FFF;

            memcpy(out_frags[0], in_frame, addr_len);
            out_frags[0][addr_len + 0] = (u8)(FRF12_FLAG_B | FRF12_FLAG_E | ((seq >> 8) & 0x0F));
            out_frags[0][addr_len + 1] = (u8)(seq & 0xFF);
            memcpy(out_frags[0] + addr_len + FRF12_HDR_LEN, in_frame + addr_len, payload_len);
            out_lens[0] = addr_len + FRF12_HDR_LEN + payload_len;
            return 1;
        } else {
            memcpy(out_frags[0], in_frame, in_len);
            out_lens[0] = in_len;
            return 1;
        }
    }

    size_t offset = 0;
    int frag_idx = 0;

    while (offset < payload_len && frag_idx < max_frags) {
        size_t chunk = payload_len - offset;
        if (chunk > fragment_size) {
            chunk = fragment_size;
        }

        int is_first = (offset == 0);
        int is_last = (offset + chunk >= payload_len);

        u16 seq = (*seq_counter) & 0x0FFF;
        *seq_counter = (*seq_counter + 1) & 0x0FFF;

        u8 flags = 0;
        if (is_first) flags |= FRF12_FLAG_B;
        if (is_last)  flags |= FRF12_FLAG_E;

        /* Copy Address header */
        memcpy(out_frags[frag_idx], in_frame, addr_len);

        /* Write FRF.12 2-byte header */
        out_frags[frag_idx][addr_len + 0] = (u8)(flags | ((seq >> 8) & 0x0F));
        out_frags[frag_idx][addr_len + 1] = (u8)(seq & 0xFF);

        /* Copy fragment payload */
        memcpy(out_frags[frag_idx] + addr_len + FRF12_HDR_LEN, in_frame + addr_len + offset, chunk);
        out_lens[frag_idx] = addr_len + FRF12_HDR_LEN + chunk;

        offset += chunk;
        frag_idx++;
    }

    return frag_idx;
}

int frf12_reassemble_frame(fr_reasm_ctx_t *ctx,
                           const u8 *frag_frame, size_t frag_len,
                           u8 *out_frame, size_t *out_len,
                           int *is_complete)
{
    if (!ctx || !frag_frame || !out_frame || !out_len || !is_complete) {
        return -1;
    }

    *is_complete = 0;
    *out_len = 0;

    size_t addr_len = fr_get_addr_len(frag_frame, frag_len);
    if (addr_len < 2 || frag_len < addr_len + FRF12_HDR_LEN) {
        /* Not enough bytes for FRF.12 header, treat as regular unfragmented frame */
        memcpy(out_frame, frag_frame, frag_len);
        *out_len = frag_len;
        *is_complete = 1;
        return 0;
    }

    u8 hdr0 = frag_frame[addr_len + 0];
    u8 hdr1 = frag_frame[addr_len + 1];

    int flag_b = (hdr0 & FRF12_FLAG_B) != 0;
    int flag_e = (hdr0 & FRF12_FLAG_E) != 0;
    u16 seq = (u16)(((hdr0 & 0x0F) << 8) | hdr1);

    size_t frag_payload_len = frag_len - addr_len - FRF12_HDR_LEN;
    const u8 *frag_payload = frag_frame + addr_len + FRF12_HDR_LEN;

    u64 now_ms = (u64)(time(NULL) * 1000);

    /* Check reassembly timeout */
    if (ctx->in_progress && (now_ms - ctx->last_frag_ms > FRF12_REASM_TIMEOUT_MS)) {
        LOG_DEBUG("FRF.12 reassembly timeout expired, discarding %zu partial bytes", ctx->len);
        ctx->in_progress = 0;
        ctx->len = 0;
    }

    /* Single complete unfragmented frame (B=1, E=1) */
    if (flag_b && flag_e) {
        ctx->in_progress = 0;
        ctx->len = 0;

        memcpy(out_frame, frag_frame, addr_len);
        memcpy(out_frame + addr_len, frag_payload, frag_payload_len);
        *out_len = addr_len + frag_payload_len;
        *is_complete = 1;
        return 0;
    }

    /* First fragment of multi-fragment frame (B=1, E=0) */
    if (flag_b && !flag_e) {
        memcpy(ctx->buf, frag_frame, addr_len);
        memcpy(ctx->buf + addr_len, frag_payload, frag_payload_len);
        ctx->len = addr_len + frag_payload_len;
        ctx->expected_seq = (seq + 1) & 0x0FFF;
        ctx->in_progress = 1;
        ctx->last_frag_ms = now_ms;
        ctx->frag_count = 1;
        *is_complete = 0;
        return 0;
    }

    /* Middle or Last fragment */
    if (!ctx->in_progress) {
        LOG_DEBUG("FRF.12 received fragment (seq=%u) without B=1 start, dropping", seq);
        return -1;
    }

    if (seq != ctx->expected_seq) {
        LOG_WARN("FRF.12 sequence mismatch: expected %u, got %u — aborting reassembly", ctx->expected_seq, seq);
        ctx->in_progress = 0;
        ctx->len = 0;
        return -1;
    }

    if (ctx->len + frag_payload_len > FR_MAX_FRAMESZ) {
        LOG_WARN("FRF.12 reassembled frame exceeds maximum frame size (%zu bytes) — aborting", ctx->len + frag_payload_len);
        ctx->in_progress = 0;
        ctx->len = 0;
        return -1;
    }

    memcpy(ctx->buf + ctx->len, frag_payload, frag_payload_len);
    ctx->len += frag_payload_len;
    ctx->expected_seq = (seq + 1) & 0x0FFF;
    ctx->last_frag_ms = now_ms;
    ctx->frag_count++;

    if (flag_e) {
        /* Last fragment complete! */
        memcpy(out_frame, ctx->buf, ctx->len);
        *out_len = ctx->len;
        *is_complete = 1;

        ctx->in_progress = 0;
        ctx->len = 0;
        return 0;
    }

    /* Middle fragment buffered */
    *is_complete = 0;
    return 0;
}
