/*
 * switching.h - VFRS Frame Relay Switching Core & Frame Processing
 * Virtual Frame Relay Switch
 */

#ifndef VFR_SWITCHING_H
#define VFR_SWITCHING_H

#include "platform.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct vfr_port_s vfr_port_t;
typedef struct vfrs_ctx_s vfrs_ctx_t;

/* ============================================================
 * Frame Relay Address Encoding (Q.922)
 * ============================================================ */

/* Frame relay address structure */
typedef struct {
    u32  dlci;
    u8   cr:1;
    u8   fecn:1;
    u8   becn:1;
    u8   de:1;
    u8   ea:1;
} fr_addr_t;

/* Return the number of octets in the Q.922 address field (1–4) by walking
 * the EA (Extension Address) bits.  max_len prevents out-of-bounds reads
 * on short or partial buffers.  A return value of 1 is always invalid for
 * Frame Relay (minimum address is 2 octets); callers must check for this.
 * Spec ref: X.36 §9.3.3 / Q.922 §3.6. */
static inline size_t fr_get_addr_len(const u8 *addr, size_t max_len) {
    if (!addr || max_len < 1) return 0;
    if ((addr[0] & 0x01) != 0) return 1; /* Single octet address (invalid for FR) */
    if (max_len < 2) return 1;           /* Incomplete address */
    if ((addr[1] & 0x01) != 0) return 2; /* 2-octet address */
    if (max_len < 3) return 2;           /* Incomplete address */
    if ((addr[2] & 0x01) != 0) return 3; /* 3-octet address */
    if (max_len < 4) return 3;           /* Incomplete address (missing 4th octet) */
    return 4;                            /* 4-octet address */
}

static inline u32 fr_decode_dlci(const u8 *addr) {
    if ((addr[0] & 0x01) == 0 && (addr[1] & 0x01) == 0) {
        if ((addr[2] & 0x01) == 0) {
            /* 4-octet: X.36 Figure 9-2
             *   addr[0] bits 7..2 = DLCI[22:17],  addr[1] bits 7..4 = DLCI[16:13]
             *   addr[2] bits 7..1 = DLCI[12:6],   addr[3] bits 7..2 = DLCI[5:0]
             */
            return (((u32)(addr[0] & 0xFC) >> 2) << 17) |
                   (((u32)(addr[1] & 0xF0) >> 4) << 13) |
                   (((u32)(addr[2] & 0xFE) >> 1) << 6)  |
                   (((u32)(addr[3] & 0xFC) >> 2));
        } else {
            /* 3-octet: addr[0] EA=0, addr[1] EA=0, addr[2] EA=1
             * Check D/C bit (bit 1 of addr[2]) to determine DLCI width */
            u8 dc = (addr[2] >> 1) & 0x01;
            if (dc == 0) {
                /* D/C=0: lower DLCI bits present → 16-bit DLCI */
                return (((u32)(addr[0] & 0xFC) >> 2) << 10) |
                       (((u32)(addr[1] & 0xF0) >> 4) << 6) |
                       (((u32)(addr[2] & 0xFC) >> 2));
            } else {
                /* D/C=1: DL-CORE control → treat as 10-bit DLCI (same as 2-octet) */
                return ((((u32)addr[0] & 0xFC) >> 2) << 4) | ((addr[1] & 0xF0) >> 4);
            }
        }
    }
    /* Default 2-octet DLCI: bits 8..3 of addr[0] & 7..4 of addr[1] */
    return ((((u32)addr[0] & 0xFC) >> 2) << 4) | ((addr[1] & 0xF0) >> 4);
}

static inline void fr_encode_dlci(u8 *addr, u32 dlci, int dlci_len) {
    if (dlci_len == 4 || dlci > 1023) {
        /* 4-octet — X.36 Figure 9-2 */
        addr[0] = (u8)(((dlci >> 17) & 0x3F) << 2);   /* EA=0; DLCI[22:17] in bits 7..2 */
        addr[1] = (u8)(((dlci >> 13) & 0x0F) << 4);   /* EA=0; DLCI[16:13] in bits 7..4 */
        addr[2] = (u8)(((dlci >> 6)  & 0x7F) << 1);   /* EA=0; DLCI[12:6]  in bits 7..1 */
        addr[3] = (u8)(((dlci & 0x3F) << 2) | 0x01);  /* EA=1; DLCI[5:0]   in bits 7..2 */
    } else if (dlci_len == 3) {
        /* 3-octet — X.36 Figure 9-1b, D/C=0 (16-bit DLCI) */
        addr[0] = (u8)(((dlci >> 10) & 0x3F) << 2);          /* EA=0; DLCI[15:10] in bits 7..2 */
        addr[1] = (u8)(((dlci >> 6) & 0x0F) << 4);           /* EA=0; DLCI[9:6]   in bits 7..4 */
        addr[2] = (u8)(((dlci & 0x3F) << 2) | 0x01);         /* EA=1; D/C=0; DLCI[5:0] in bits 7..2 */
    } else {
        /* 2-octet — X.36 Figure 9-1 */
        addr[0] = (u8)(((dlci >> 4) & 0x3F) << 2);    /* EA=0; DLCI[9:4]   in bits 7..2 */
        addr[1] = (u8)((dlci & 0x0F) << 4) | 0x01;    /* EA=1; DLCI[3:0]   in bits 7..4 */
    }
}

/* ============================================================
 * CRC-16 (FCS) - Q.922 Frame Check Sequence
 * ============================================================ */

VFR_API u16 crc16_fcs(const u8 *data, size_t len);
VFR_API int crc16_check(const u8 *data, size_t len);

/* ============================================================
 * HDLC Bit Stuffing
 * ============================================================ */

VFR_API size_t hdlc_stuff(const u8 *in, size_t in_len, u8 *out, size_t out_max);
VFR_API size_t hdlc_unstuff(const u8 *in, size_t in_len, u8 *out, size_t out_max);

/* HDLC Flag byte (0x7E) — ISO 13239 §4.3.2 / Q.922 §3.8 */
#define HDLC_FLAG    0x7E

/* ============================================================
 * Switch Core Functions
 * ============================================================ */

/* Frame processing */
VFR_API int fr_switch_input(vfrs_ctx_t *ctx, vfr_port_t *port, u8 *frame, size_t len);
VFR_API int fr_switch_input_processed(vfrs_ctx_t *ctx, vfr_port_t *port, const fr_addr_t *addr, u8 *frame, size_t frame_len);
VFR_API thread_ret_t THREAD_CALL run_slow_path_thread(void *arg);

/* Frame parsing — frame_len is set to the destuffed frame body length
 * (address + ctrl + data, excluding the 2-byte FCS). */
VFR_API int fr_parse_frame(u8 *frame, size_t len, fr_addr_t *addr, size_t *frame_len, int is_serial);
VFR_API void fr_decode_addr(const u8 *addr, fr_addr_t *result);
VFR_API void fr_encode_dlci_with_flags(u8 *addr, u32 dlci, int fecn, int becn, int de, int cr, int dlci_len);
VFR_API int fr_dlci_reserved(u32 dlci);
VFR_API int fr_dlci_valid(u32 dlci);

/* DLCI address rewriting */
VFR_API void fr_rewrite_dlci(u8 *frame, u32 old_dlci, u32 new_dlci);

/* Frame building */
VFR_API size_t fr_build_ui_frame(u8 *buf, size_t max_len, u32 dlci,
                                 const u8 *data, size_t data_len,
                                 const fr_addr_t *flags);

/* Global switch frame function */
VFR_API int vfrs_switch_frame(vfrs_ctx_t *ctx, vfr_port_t *src_port,
                              u32 dlci, const u8 *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VFR_SWITCHING_H */
