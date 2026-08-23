/*
 * fr_frame.h - Frame Relay Frame Encoding/Decoding
 * VFRS - Virtual Frame Relay Switch
 * Q.922 Frame Format
 */

#ifndef FR_FRAME_H
#define FR_FRAME_H

#include "../vfr.h"

/* ============================================================
 * Frame Relay Constants
 * ============================================================ */

#define FR_FLAG            0x7E
#define FR_ADDR_LEN        2
#define FR_CTRL_UI         0x03
#define FR_MAX_FRAMESZ     1600

/* DLCI Ranges */
#define FR_DLCI_RESERVED_MIN    0
#define FR_DLCI_RESERVED_MAX    15
#define FR_DLCI_USER_MIN        16
#define FR_DLCI_USER_MAX        991
#define FR_DLCI_MGMT_MIN        992
#define FR_DLCI_MGMT_MAX        1007
#define FR_DLCI_LMI             0
#define FR_DLCI_CISCO_LMI       1023

/* ============================================================
 * CRC-16 (FCS) Functions
 * ============================================================ */

/* Calculate CRC-16 FCS for frame data */
u16 crc16_fcs(const u8 *data, size_t len);

/* Verify CRC-16 FCS (last 2 bytes are FCS) */
int crc16_check(const u8 *data, size_t len);

/* ============================================================
 * Frame Relay Address Encoding (Q.922)
 * ============================================================ */

/* Decode DLCI from Q.922 address */
u32 fr_decode_dlci(const u8 *addr);

/* Encode DLCI into Q.922 address */
void fr_encode_dlci(u8 *addr, u32 dlci, int dlci_len);

/* Encode DLCI with congestion flags */
void fr_encode_dlci_with_flags(u8 *addr, u32 dlci, int fecn, int becn, int de, int cr, int dlci_len);

/* Decode full address structure */
void fr_decode_addr(const u8 *addr, fr_addr_t *result);

/* Check if DLCI is reserved */
int fr_dlci_reserved(u32 dlci);

/* Check if DLCI is valid for user traffic across any format */
int fr_dlci_valid(u32 dlci);

/* Check if DLCI is valid for user traffic on a specific address width (2, 3, or 4 octets)
 * Reconciled against ITU-T Q.922 Table 1 and ITU-T X.36 Tables 9-1 / 9-2 */
int fr_dlci_valid_for_width(u32 dlci, int addr_len);

/* Rewrite DLCI in frame header while preserving C/R, FECN, BECN, and DE bits */
void fr_rewrite_dlci(u8 *frame, u32 old_dlci, u32 new_dlci);

/* ============================================================
 * Frame Type Detection
 * ============================================================ */

#define FR_CTRL_I      0x00   /* Information */
#define FR_CTRL_RR     0x01   /* Receive Ready */
#define FR_CTRL_RNR    0x05   /* Receive Not Ready */
#define FR_CTRL_REJ    0x09   /* Reject */
#define FR_CTRL_UI     0x03   /* Unnumbered Information */
#define FR_CTRL_SABME  0x6F   /* Set Asynchronous Balanced Mode Extended */
#define FR_CTRL_UA     0x63   /* Unnumbered Acknowledgment */
#define FR_CTRL_DM     0x0F   /* Disconnected Mode */
#define FR_CTRL_DISC   0x43   /* Disconnect */

static inline int fr_is_u_frame(u8 ctrl) { return (ctrl & 0x03) == 0x03; }
static inline int fr_is_s_frame(u8 ctrl) { return (ctrl & 0x03) == 0x01; }
static inline int fr_is_i_frame(u8 ctrl) { return (ctrl & 0x03) == 0x00; }

/* ============================================================
 * HDLC Bit Stuffing
 * ============================================================ */

#define HDLC_FLAG   0x7E
#define HDLC_ESCAPE 0x7D
#define HDLC_ESC_MASK 0x20

/* Bit stuff data for HDLC transmission */
size_t hdlc_stuff(const u8 *in, size_t in_len, u8 *out, size_t out_max);

/* Unstuff HDLC data */
size_t hdlc_unstuff(const u8 *in, size_t in_len, u8 *out, size_t out_max);

/* ============================================================
 * Frame Parsing
 * ============================================================ */

/* Parse Frame Relay frame header (skip flags, destuff HDLC, return offset to data).
 * Sets *frame_len to the total destuffed frame body length (address+ctrl+data,
 * excluding the 2-byte FCS) so callers can validate the FCS with crc16_check. */
int fr_parse_frame(u8 *frame, size_t len, fr_addr_t *addr, size_t *frame_len, int is_serial);

/* Build a Frame Relay UI frame with HDLC flags and zero-bit stuffing.
 * Pass flags!=NULL to preserve C/R, FECN, BECN, DE from the original address. */
size_t fr_build_ui_frame(u8 *buf, size_t max_len, u32 dlci,
                          const u8 *data, size_t data_len,
                          const fr_addr_t *flags);

/* Frame size calculations */
size_t fr_max_info_size(size_t frame_size);
size_t fr_calc_frame_size(size_t info_size);

#endif /* FR_FRAME_H */