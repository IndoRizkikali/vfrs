/*
 * fr_frame.c - Frame Relay Frame Encoding/Decoding
 * VFRS - Virtual Frame Relay Switch
 * Q.922 Frame Format
 */

#include "vfr.h"

/* ============================================================
 * CRC-16 (FCS) - Q.922 Frame Check Sequence
 * ============================================================
 *
 * Implements CRC-16-CCITT per X.36 §9.2.4 / Q.922 Annex A.
 * Polynomial: x^16 + x^12 + x^5 + 1  (0x1021)
 * Bit order: LSB-first on the wire (reflected algorithm, poly 0x8408)
 * Init: 0xFFFF   Final: ones-complement of remainder
 * FCS transmission: remainder LSB first (X.36 §9.2.4).
 */

/* CRC-16-CCITT lookup table (reflected, poly 0x8408). */
static const u16 crc16_table[256] = {
    0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
    0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
    0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
    0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
    0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
    0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
    0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
    0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
    0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
    0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
    0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
    0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
    0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
    0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
    0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
    0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
    0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
    0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
    0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
    0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
    0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
    0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
    0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
    0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
    0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
    0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
    0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
    0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
    0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
    0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
    0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78
};

/* Precomputed CRC16_GOOD for validation: ones-complement of
 * crc16_bitbybit(0xFFFF, all-zeroes[2], 2) */
#define CRC16_INIT   0xFFFF  /* FCS initial value per X.36 §9.2.4 */
#define CRC16_GOOD   0xF0B8  /* Standard FCS good value for {0,0} */

static u16 crc16_table_slice8[8][256];

static void crc16_slice8_init(void)
{
    int n, k;
    
    /* Initialize table 0 with the existing reflected crc16_table */
    for (n = 0; n < 256; n++) {
        crc16_table_slice8[0][n] = crc16_table[n];
    }

    /* Generate tables 1 to 7 */
    for (n = 0; n < 256; n++) {
        u16 crc = crc16_table_slice8[0][n];
        for (k = 1; k < 8; k++) {
            crc = crc16_table_slice8[0][crc & 0xFF] ^ (crc >> 8);
            crc16_table_slice8[k][n] = crc;
        }
    }
}

/*
 * One-time init guard for the slice-8 CRC16 table (#4.10 / D10).
 * Guarantees that crc16_slice8_init() is called exactly once regardless
 * of how many threads call crc16_fcs() concurrently at startup.
 */
#ifdef _WIN32
static INIT_ONCE  crc16_init_once   = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK crc16_init_once_fn(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    crc16_slice8_init();
    return TRUE;
}
static void crc16_ensure_init(void)
{
    InitOnceExecuteOnce(&crc16_init_once, crc16_init_once_fn, NULL, NULL);
}
#else
static pthread_once_t crc16_once = PTHREAD_ONCE_INIT;
static void crc16_ensure_init(void)
{
    pthread_once(&crc16_once, crc16_slice8_init);
}
#endif

static u16 crc16_fcs_speed(u16 fcs, const void *buf, size_t len)
{
    const u8 *next = (const u8 *)buf;

    /* Process individual bytes until we reach an 8-byte aligned pointer */
    while (len && ((uintptr_t)next & 7) != 0) {
        fcs = (fcs >> 8) ^ crc16_table_slice8[0][(fcs ^ *next++) & 0xFF];
        len--;
    }

    /* Fast middle processing, 8 bytes per loop */
    while (len >= 8) {
        uint64_t n;
        memcpy(&n, next, sizeof(n));
        uint32_t x = (uint32_t)(n ^ fcs);
        fcs = crc16_table_slice8[7][x & 0xFF] ^
              crc16_table_slice8[6][(x >> 8) & 0xFF] ^
              crc16_table_slice8[5][(n >> 16) & 0xFF] ^
              crc16_table_slice8[4][(n >> 24) & 0xFF] ^
              crc16_table_slice8[3][(n >> 32) & 0xFF] ^
              crc16_table_slice8[2][(n >> 40) & 0xFF] ^
              crc16_table_slice8[1][(n >> 48) & 0xFF] ^
              crc16_table_slice8[0][n >> 56];
        next += 8;
        len -= 8;
    }

    /* Process remaining bytes */
    while (len) {
        fcs = (fcs >> 8) ^ crc16_table_slice8[0][(fcs ^ *next++) & 0xFF];
        len--;
    }

    return fcs;
}

u16 crc16_fcs(const u8 *data, size_t len)
{
    crc16_ensure_init();
    u16 fcs = CRC16_INIT;
    fcs = crc16_fcs_speed(fcs, data, len);
    return fcs ^ 0xFFFF;
}

int crc16_check(const u8 *data, size_t len)
{
    /* LSB-first FCS transmission (X.36 §9.2.4): data[len-2]=FCS_LSB,
     * data[len-1]=FCS_MSB. Computed FCS uses same init/XOR as crc16_fcs. */
    if (len < 2) return 0;
    u16 recv = data[len - 2] | ((u16)data[len - 1] << 8);
    u16 calc = crc16_fcs(data, len - 2);
    return (calc == recv);
}

/* ============================================================
 * Frame Relay Address Encoding (Q.922)
 * ============================================================ */

/* Note: fr_decode_dlci, fr_encode_dlci, and fr_encode_dlci_with_flags
 * are already defined as inline functions in vfr.h */

void fr_decode_addr(const u8 *addr, fr_addr_t *result)
{
    /* Determine address length internally using EA bits */
    size_t addr_len = 2;
    if ((addr[0] & 0x01) == 0) {
        if ((addr[1] & 0x01) == 0) {
            if ((addr[2] & 0x01) == 0) {
                addr_len = 4;
            } else {
                addr_len = 3;
            }
        }
    }

    if (addr_len == 4) {
        /* 4-octet Q.922 address — X.36 Figure 9-2, frame_format.md §3.3
         *   addr[0]: DLCI[22:17] in bits 7..2, C/R in bit 1, EA=0 in bit 0
         *   addr[1]: DLCI[16:13] in bits 7..4, FECN in bit 3, BECN in bit 2,
         *            DE in bit 1, EA=0 in bit 0
         *   addr[2]: DLCI[12:6] in bits 7..1, EA=0 in bit 0
         *   addr[3]: DLCI[5:0] in bits 7..2, D/C in bit 1, EA=1 in bit 0
         */
        result->dlci = (((u32)(addr[0] & 0xFC) >> 2) << 17) |  /* DLCI[22:17] */
                       (((u32)(addr[1] & 0xF0) >> 4) << 13) |  /* DLCI[16:13] */
                       (((u32)(addr[2] & 0xFE) >> 1) << 6)  |  /* DLCI[12:6]  */
                       (((u32)(addr[3] & 0xFC) >> 2));          /* DLCI[5:0]   */
        result->cr   = (addr[0] & 0x02) >> 1;
        result->fecn = (addr[1] & 0x08) >> 3;  /* bit 3 of octet 2 */
        result->becn = (addr[1] & 0x04) >> 2;  /* bit 2 of octet 2 */
        result->de   = (addr[1] & 0x02) >> 1;  /* bit 1 of octet 2 */
        result->ea   = (addr[3] & 0x01);
    } else if (addr_len == 3) {
        /* 3-octet Q.922 address — Figure 1/Q.922
         *   addr[0]: DLCI[15:10] in bits 7..2, C/R in bit 1, EA=0 in bit 0
         *   addr[1]: DLCI[9:6] in bits 7..4, FECN in bit 3, BECN in bit 2,
         *            DE in bit 1, EA=0 in bit 0
         *   addr[2]: DLCI[5:0] in bits 7..2, D/C in bit 1, EA=1 in bit 0
         */
        result->dlci = (((u32)(addr[0] & 0xFC) >> 2) << 10) |  /* DLCI[15:10] */
                       (((u32)(addr[1] & 0xF0) >> 4) << 6)  |  /* DLCI[9:6]   */
                       (((u32)(addr[2] & 0xFC) >> 2));          /* DLCI[5:0]   */
        result->cr   = (addr[0] & 0x02) >> 1;
        result->fecn = (addr[1] & 0x08) >> 3;
        result->becn = (addr[1] & 0x04) >> 2;
        result->de   = (addr[1] & 0x02) >> 1;
        result->ea   = (addr[2] & 0x01);
    } else {
        /* Default 2-byte Q.922 address */
        result->dlci = fr_decode_dlci(addr);
        result->cr   = (addr[0] & 0x02) >> 1;
        result->fecn = (addr[1] & 0x08) >> 3;
        result->becn = (addr[1] & 0x04) >> 2;
        result->de   = (addr[1] & 0x02) >> 1;
        result->ea   = (addr[1] & 0x01);
    }
}

/* Check if DLCI is reserved/management (not for user traffic) — X.36 Table 9-1.
 * Ranges:
 *   0      : signalling (LMI)
 *   1-15   : reserved (ITU)
 *   992-1007: Layer 2 management (CLLM) — FR_DLCI_MGMT_MIN..FR_DLCI_MGMT_MAX
 *   1008-1022: reserved
 *   1023   : in-channel L2 management / Cisco LMI
 */
int fr_dlci_reserved(u32 dlci)
{
    if (dlci == FR_DLCI_LMI) return 1;                                   /* 0: signalling */
    if (dlci >= 1 && dlci <= 15) return 1;                               /* 1-15: reserved */
    if (dlci >= FR_DLCI_MGMT_MIN && dlci <= FR_DLCI_MGMT_MAX) return 1; /* 992-1007: CLLM */
    if (dlci >= 1008 && dlci <= 1022) return 1;                          /* 1008-1022: reserved */
    if (dlci == FR_DLCI_CISCO) return 1;                                  /* 1023: Cisco LMI */
    return 0;
}

/* Check if DLCI is valid for user traffic on a specific address width —
 * Reconciled per ITU-T Q.922 Table 1 and ITU-T X.36 Tables 9-1 & 9-2 / X.76.
 */
int fr_dlci_valid_for_width(u32 dlci, int addr_len)
{
    if (addr_len == 2) {
        /* 2-octet address (10-bit DLCI): Table 1/Q.922 & Table 9-1/X.36 */
        return (dlci >= FR_DLCI_MIN && dlci <= FR_DLCI_MAX); /* 16-991 */
    } else if (addr_len == 3) {
        /* 3-octet address (16-bit DLCI, D/C=0): Table 1/Q.922 */
        return (dlci >= 1024 && dlci <= 63487);
    } else if (addr_len == 4) {
        /* 4-octet address (23-bit DLCI): Table 9-2/X.36 allows 16-991 & 1024-8388607 */
        return ((dlci >= FR_DLCI_MIN && dlci <= FR_DLCI_MAX) ||
                (dlci >= 1024 && dlci <= 8388607));
    }
    return fr_dlci_valid(dlci);
}

/* Check if DLCI is valid for user traffic across any address width.
 * Valid user ranges: 16-991 (2-octet & 4-octet) and 1024-8388607 (3-octet & 4-octet).
 * Management/reserved DLCIs (992-1023) are excluded.
 */
int fr_dlci_valid(u32 dlci)
{
    if (dlci < FR_DLCI_MIN) return 0;             /* < 16: signalling/reserved */
    if (dlci <= FR_DLCI_MAX) return 1;            /* 16-991: user VCs (2-octet & 4-octet) */
    if (dlci <= 1022) return 0;                   /* 992-1022: management/reserved */
    if (dlci == FR_DLCI_CISCO) return 0;          /* 1023: Cisco LMI */
    return (dlci <= 8388607);                     /* 1024-8388607: user VCs (3-octet & 4-octet) */
}

/* Encode DLCI with congestion flags into Q.922 address field.
 *
 * 2-octet (DLCI <= 1023) — X.36 Figure 9-1 / Q.922 Figure A-1:
 *   addr[0]: DLCI[9:4] in bits 7..2, C/R in bit 1, EA=0 in bit 0
 *   addr[1]: DLCI[3:0] in bits 7..4, FECN in bit 3, BECN in bit 2,
 *            DE in bit 1, EA=1 in bit 0
 *
 * 3-octet (1024 <= DLCI <= 63487) — Q.922 Figure A-2:
 *   addr[0]: DLCI[15:10] in bits 7..2, C/R in bit 1, EA=0 in bit 0
 *   addr[1]: DLCI[9:6] in bits 7..4, FECN in bit 3, BECN in bit 2,
 *            DE in bit 1, EA=0 in bit 0
 *   addr[2]: DLCI[5:0] in bits 7..2, D/C=0 in bit 1, EA=1 in bit 0
 *
 * 4-octet (DLCI > 63487 or dlci_len==4) — X.36 Figure 9-2 / Q.922 Figure A-2:
 *   addr[0]: DLCI[22:17] in bits 7..2, C/R in bit 1, EA=0 in bit 0
 *   addr[1]: DLCI[16:13] in bits 7..4, FECN in bit 3, BECN in bit 2,
 *            DE in bit 1, EA=0 in bit 0
 *   addr[2]: DLCI[12:6] in bits 7..1, EA=0 in bit 0
 *   addr[3]: DLCI[5:0]  in bits 7..2, D/C=0 in bit 1, EA=1 in bit 0
 */
void fr_encode_dlci_with_flags(u8 *addr, u32 dlci, int fecn, int becn, int de, int cr, int dlci_len)
{
    if (dlci_len == 4 || dlci > 63487) {
        addr[0] = (u8)(((dlci >> 17) & 0x3F) << 2) | ((cr & 1) << 1);  /* EA=0; DLCI[22:17] */
        addr[1] = (u8)(((dlci >> 13) & 0x0F) << 4)                      /* EA=0; DLCI[16:13] */
                      | (fecn ? 0x08 : 0)                                /*       FECN=bit3   */
                      | (becn ? 0x04 : 0)                                /*       BECN=bit2   */
                      | (de   ? 0x02 : 0);                               /*       DE=bit1     */
        addr[2] = (u8)(((dlci >> 6)  & 0x7F) << 1);                     /* EA=0; DLCI[12:6]  */
        addr[3] = (u8)(((dlci & 0x3F) << 2) | 0x01);                    /* EA=1; DLCI[5:0]; D/C=0 */
    } else if (dlci_len == 3 || (dlci > 1023 && dlci <= 63487)) {
        addr[0] = (u8)(((dlci >> 10) & 0x3F) << 2) | ((cr & 1) << 1);  /* EA=0; DLCI[15:10] */
        addr[1] = (u8)(((dlci >> 6)  & 0x0F) << 4)                      /* EA=0; DLCI[9:6]   */
                      | (fecn ? 0x08 : 0)                                /*       FECN=bit3   */
                      | (becn ? 0x04 : 0)                                /*       BECN=bit2   */
                      | (de   ? 0x02 : 0);                               /*       DE=bit1     */
        addr[2] = (u8)(((dlci & 0x3F) << 2) | 0x01);                    /* EA=1; DLCI[5:0]; D/C=0 */
    } else {
        addr[0] = (u8)(((dlci >> 4) & 0x3F) << 2) | ((cr & 1) << 1);  /* EA=0; DLCI[9:4] */
        addr[1] = (u8)(((dlci & 0x0F) << 4) |
                   (fecn ? 0x08 : 0) |
                   (becn ? 0x04 : 0) |
                   (de   ? 0x02 : 0) |
                   0x01);                                               /* EA=1; DLCI[3:0] */
    }
}

/* ============================================================
 * Frame Type Detection
 * ============================================================ */

/* Frame Relay control field values */
#define FR_CTRL_UI     0x03   /* Unnumbered Information */
#define FR_CTRL_SABME  0x6F   /* Set Asynchronous Balanced Mode Extended */
#define FR_CTRL_UA     0x63   /* Unnumbered Acknowledgment */
#define FR_CTRL_DM     0x0F   /* Disconnected Mode */
#define FR_CTRL_DISC   0x43   /* Disconnect */
#define FR_CTRL_XID    0xAF   /* Exchange Identification */
#define FR_CTRL_TEST   0xE3   /* Test */

#define FR_CTRL_I      0x00   /* Information (N(R) in lower bits) */
#define FR_CTRL_RR     0x01   /* Receive Ready (RR) */
#define FR_CTRL_RNR    0x05   /* Receive Not Ready (RNR) */
#define FR_CTRL_REJ    0x09   /* Reject (REJ) */

static inline int is_u_frame(int ctrl)
{
    return ((ctrl & 0x03) == 0x03);
}

static inline int is_s_frame(int ctrl)
{
    return ((ctrl & 0x03) == 0x01);
}

static inline int is_i_frame(int ctrl)
{
    return ((ctrl & 0x03) == 0x00);
}

/* ============================================================
 * HDLC Byte Stuffing
 * ============================================================ */

#define HDLC_FLAG      0x7E
#define HDLC_ESCAPE    0x7D
#define HDLC_ESC_MASK  0x20

/* Async HDLC byte-oriented stuffing (ISO 13239 §4.3 / RFC 1549):
 *   0x7E (flag byte) → {0x7D, 0x5E}
 *   0x7D (escape byte) → {0x7D, 0x5D}
 *
 * NOTE: X.36 §9.4.3 specifies zero-bit insertion (insert a 0-bit after every
 * 5 consecutive 1-bits at the bit level). This implementation uses byte-level
 * escape-code stuffing instead, which is incompatible with hardware HDLC
 * controllers that perform bit-level stuffing. It is chosen deliberately for
 * virtual/software serial transports (virtual COM, named pipe) where both
 * endpoints run this same code. UDP/TCP transports do not use stuffing at all.
 */
size_t hdlc_stuff(const u8 *in, size_t in_len, u8 *out, size_t out_max)
{
    size_t j = 0;

    for (size_t i = 0; i < in_len; i++) {
        u8 byte = in[i];

        if (byte == HDLC_FLAG || byte == HDLC_ESCAPE) {
            if (j + 2 > out_max) break;
            out[j++] = HDLC_ESCAPE;
            out[j++] = byte ^ HDLC_ESC_MASK;
        } else {
            if (j + 1 > out_max) break;
            out[j++] = byte;
        }
    }

    return j;
}

/* Async HDLC byte unstuffing — reverse of hdlc_stuff. */
size_t hdlc_unstuff(const u8 *in, size_t in_len, u8 *out, size_t out_max)
{
    size_t j = 0;

    for (size_t i = 0; i < in_len && j < out_max; i++) {
        u8 byte = in[i];

        if (byte == HDLC_ESCAPE) {
            if (i + 1 >= in_len) break;
            byte = in[++i] ^ HDLC_ESC_MASK;
        }

        out[j++] = byte;
    }

    return j;
}

/* ============================================================
 * Frame Parsing
 * ============================================================ */

/* Frame Relay frame structure:
 * [Flag] Address(2) Control [PID] [Data] FCS [Flag]
 *
 * Address format (Q.922 2-byte):
 *   byte 0: DLCI[9:4] | CR | EA=0
 *   byte 1: DLCI[3:0] | FECN | BECN | DE | EA=1
 *
 * Control:
 *   UI (0x03): Unnumbered Information (LMI, user data)
 *   I-frame:  N(S)|P|0|N(R) (LAPF for SVC)
 *   S-frame:  0|0|0|P|S|0|0|1
 *
 * NLPID (Network Layer Protocol Identifier):
 *   0x09 - Cisco LMI
 *   0xCC - ISO/IEC TR 9577 (IP)
 *   0x8E - CLNS (ISO 8473)
 *   0x00 - Null encapsulated
 */

/* Parse a Frame Relay frame from the wire.
 *
 * For serial/pipe HDLC transport (is_serial=1):
 *   Strips leading/trailing 0x7E flag bytes, performs async HDLC byte-oriented
 *   destuffing, and decodes the Q.922 address. The frame must include FCS.
 *
 * For UDP/TCP transport (is_serial=0):
 *   No flags or stuffing — raw octets passed through. No FCS present.
 *
 * Additionally performs the following invalid-frame checks (X.36 §9.4.5):
 *   (e) Rejects single-octet address fields (EA=1 in first byte)
 *   (h) Rejects frames whose information field exceeds N203 (FR_MAX_FRAMESZ)
 *
 * Parameters:
 *   frame     — input buffer (modified in-place during destuffing for serial)
 *   len       — total input buffer length
 *   addr      — out: decoded Q.922 address fields
 *   frame_len — out: destuffed frame length (addr+ctrl+data, excluding FCS)
 *
 * Returns 0 on success, -1 if the frame is invalid.
 *
 * For serial transport, *frame_len excludes the 2 FCS bytes, so callers can
 * validate FCS via crc16_check(frame, *frame_len + 2).
 */
int fr_parse_frame(u8 *frame, size_t len, fr_addr_t *addr, size_t *frame_len, int is_serial)
{
    if (!frame || len < 3) return -1;

    size_t start = 0;
    size_t end = len;

    if (is_serial) {
        /* Check for HDLC flag sequence and skip if present.
         * For serial HDLC transport the frame arrives wrapped in 0x7E flags.
         */
        if (frame[0] == HDLC_FLAG) {
            start = 1;
            if (len > 1 && frame[len - 1] == HDLC_FLAG) {
                end = len - 1;
            }
        }

        if (end <= start) return -1;

        u8 work[FR_MAX_FRAMESZ + 16];
        size_t stuffed_len = end - start;
        size_t unstuffed_len;

        if (stuffed_len > sizeof(work)) return -1;
        memcpy(work, frame + start, stuffed_len);

        /*
         * In-place unstuffing safety invariant: the destuffed output is always
         * shorter than or equal to the stuffed input (each escape sequence
         * 0x7D xx expands to one byte), so writing into work[] from index 0
         * never overwrites bytes that have not yet been read.  This is safe
         * as long as input and output point to the same buffer. (#2.12)
         */
        unstuffed_len = hdlc_unstuff(work, stuffed_len, work, sizeof(work));
        /* Allow minimum frame containing only Address + FCS (4 bytes) */
        if (unstuffed_len < 4) return -1;

        /* Move destuffed frame (including FCS) to the start of the buffer. */
        memmove(frame, work, unstuffed_len);

        /* frame_len excludes the 2-byte FCS; FCS lives at frame[frame_len]..[frame_len+1]. */
        *frame_len = unstuffed_len - 2;
    } else {
        /* UDP/TCP are already packetized and destuffed; no FCS is present. */
        *frame_len = len;
    }

    /* Check (e): single-octet address field is invalid — X.36 §9.4.5(e).
     * EA=1 in the first byte means the address ends after one octet, which
     * is not valid for Frame Relay (minimum address is 2 octets). */
    if (frame[0] & 0x01) {
        LOG_DEBUG("Invalid frame: single-octet address (EA=1 in byte 0)");
        return -1;
    }

    /* Determine address field length dynamically using the bounded helper.
     * Passing *frame_len ensures we cannot read past the end of the buffer. */
    size_t addr_len = fr_get_addr_len(frame, *frame_len);
    if (addr_len < 2 || addr_len > 4 || !(frame[addr_len - 1] & 0x01)) {
        LOG_DEBUG("Invalid frame: address field format error (no EA=1 within 2-4 octets)");
        return -1;
    }

    /* Ensure the frame is long enough to contain the address field */
    if (*frame_len < addr_len) {
        LOG_DEBUG("Invalid frame: frame too short for address length %zu", addr_len);
        return -1;
    }

    /* Check D/C bit in the last octet of multi-octet addresses.
     * If D/C bit (bit 2) is 1, the frame uses an unsupported control format. */
    if (addr_len == 4 && (frame[3] & 0x02)) {
        LOG_DEBUG("Invalid frame: D/C bit set to 1 in 4-octet address (unsupported)");
        return -1;
    }
    if (addr_len == 3 && (frame[2] & 0x02)) {
        LOG_DEBUG("Invalid frame: D/C bit set to 1 in 3-octet address (unsupported)");
        return -1;
    }

    /* Check (h): information field must not exceed N203 — X.36 §9.4.5(h), §8.2.6.
     * frame_len = addr + ctrl + data; info_len = frame_len - addr_len.
     * In VFRS LMI frames, there is a 1-byte Control field which is treated
     * as part of the Information field by the core relayer. */
    if (*frame_len > addr_len) {
        size_t info_len = *frame_len - addr_len;
        if (info_len > FR_MAX_FRAMESZ) {
            LOG_DEBUG("Invalid frame: info field %zu bytes exceeds N203 (%d)",
                      info_len, FR_MAX_FRAMESZ);
            return -1;
        }
    }

    /* Decode Q.922 address fields */
    fr_decode_addr(frame, addr);

    return 0;
}

/* Get control field from frame */
int fr_get_control(const u8 *frame, size_t len, int *pf, int *nr, int *ns)
{
    if (!frame || len < 3) return -1;

    u8 ctrl = frame[2];

    if (is_i_frame(ctrl)) {
        /* I-frame: N(S) in bits 1-3, P/F in bit 4, N(R) in bits 9-11 */
        *ns = (ctrl >> 1) & 0x07;
        *pf = (ctrl >> 4) & 0x01;
        /* Need third byte for N(R) in basic format */
        if (len < 4) return -1;
        *nr = (frame[3] >> 1) & 0x07;
        return 0;  /* I-frame */
    } else if (is_s_frame(ctrl)) {
        /* S-frame: RR/RNR/REJ */
        *nr = (frame[3] >> 1) & 0x07;
        *pf = (ctrl >> 4) & 0x01;
        return 1;  /* S-frame */
    } else if (is_u_frame(ctrl)) {
        /* U-frame */
        *pf = (ctrl >> 4) & 0x01;
        return 2;  /* U-frame */
    }

    return -1;
}

/* Rewrite DLCI in frame header while preserving C/R, FECN, BECN, and DE flags per Q.922 §A.3.3 */
void fr_rewrite_dlci(u8 *frame, u32 old_dlci, u32 new_dlci)
{
    (void)old_dlci;
    if (!frame) return;
    fr_addr_t old_addr;
    memset(&old_addr, 0, sizeof(old_addr));
    fr_decode_addr(frame, &old_addr);
    int dlci_len = (new_dlci > 63487) ? 4 : ((new_dlci > 1023) ? 3 : 2);
    fr_encode_dlci_with_flags(frame, new_dlci, old_addr.fecn, old_addr.becn, old_addr.de, old_addr.cr, dlci_len);
}

/* ============================================================
 * Frame Building
 * ============================================================
 * Build a Frame Relay UI frame ready for HDLC serial transmission.
 * Applies Q.922 2-byte address encoding (with optional congestion flags),
 * computes the FCS-16 over [address..data], appends it, applies HDLC
 * byte-oriented stuffing (Async HDLC), and wraps the result in 0x7E flag sequences.
 *
 * Callers that need congestion flags (FECN, BECN, DE, CR) should pass
 * a non-NULL flags pointer; otherwise congestion bits are zeroed.
 *
 * Returns the total number of bytes written (stuffed + 2 flag bytes),
 * or 0 if the output buffer is too small.
 */
size_t fr_build_ui_frame(u8 *buf, size_t max_len, u32 dlci,
                          const u8 *data, size_t data_len,
                          const fr_addr_t *flags)
{
    u8 raw[FR_MAX_FRAMESZ + 16];  /* temp buffer for raw (pre-stuff) frame */
    u8 stuffed[FR_MAX_FRAMESZ * 2 + 16]; /* temp for stuffed output */
    size_t pos = 0;
    size_t stuffed_len;
    size_t addr_sz = (dlci > 1023) ? 4 : 2;

    /* --- Build raw frame (no flags, no stuffing) --------------------- */

    /* Address: use congestion-flag variant */
    if (pos + addr_sz > sizeof(raw)) return 0;
    if (flags) {
        fr_encode_dlci_with_flags(raw + pos, dlci,
                                  flags->fecn, flags->becn, flags->de, flags->cr, addr_sz);
    } else {
        fr_encode_dlci(raw + pos, dlci, addr_sz);
    }
    pos += addr_sz;

    /* Control (UI frame) */
    if (pos + 1 > sizeof(raw)) return 0;
    raw[pos++] = FR_CTRL_UI;

    /* Data */
    if (data && data_len > 0) {
        if (pos + data_len > sizeof(raw)) return 0;
        memcpy(raw + pos, data, data_len);
        pos += data_len;
    }

    /* FCS — computed over [address..data] = raw[0..pos-1] per X.36 §9.2.4.
     * Appends 2 bytes: remainder LSB first (X.36 wire order). */
    if (pos + 2 > sizeof(raw)) return 0;
    {
        u16 fcs = crc16_fcs(raw, pos);           /* init=0xFFFF, final XOR */
        raw[pos++] = (u8)(fcs & 0xFF);           /* FCS LSB first */
        raw[pos++] = (u8)(fcs >> 8);             /* FCS MSB */
    }

    /* --- HDLC bit stuffing ------------------------------------------ */
    stuffed_len = hdlc_stuff(raw, pos, stuffed, sizeof(stuffed));

    /* --- Copy stuffed frame + flags into caller's buffer ------------ */
    if (stuffed_len + 2 > max_len) return 0;  /* flags + stuffed */
    size_t out = 0;
    buf[out++] = HDLC_FLAG;                    /* opening flag */
    memcpy(buf + out, stuffed, stuffed_len);
    out += stuffed_len;
    buf[out++] = HDLC_FLAG;                    /* closing flag */

    return out;
}

/* ============================================================
 * Frame Size Calculations
 * ============================================================ */

/* Calculate maximum info field size given frame relay overhead */
size_t fr_max_info_size(size_t frame_size)
{
    /* Frame Relay overhead:
     * - Flag: 1 byte
     * - Address: 2 bytes
     * - Control: 1 byte
     * - PID: 1 byte (optional)
     * - FCS: 2 bytes
     * - Flag: 1 byte
     * Total overhead: 8 bytes (without PID and data)
     */
    if (frame_size <= 8) return 0;
    return frame_size - 8;
}

/* Calculate total frame size for given info field */
size_t fr_calc_frame_size(size_t info_size)
{
    return info_size + 8;  /* 2 address + 1 control + 1 PID + 2 FCS + 2 flags */
}