/*
 * cgst_cllm.c - Consolidated Link Layer Management (CLLM)
 * VFRS - Virtual Frame Relay Switch
 * X.36 Annex C - CLLM Encoding
 */

#include "vfr.h"

#include <string.h>

/* CLLM constants */
#define CLLM_DLCI              1007    /* CLLM DLCI (X.36 §9.3.1) */

/* Q.922 Table 3 — XID U-frame control byte: 101 P/F 1111.
 * For unsolicited response: F=0 → 10101111 = 0xAF.                      */
#define CLLM_XID_CTRL          0xAF   /* XID control byte (F=0) */
#define CLLM_XID_CTRL_MASK     0xEF   /* Mask out P/F bit (bit 4) */

/* ISO 8885 / Q.922 §A.7.3 — XID information-field header */
#define CLLM_FI                0x82   /* Format Identifier: general purpose */
#define CLLM_GI                0x0F   /* Group Identifier: private params */

/* Q.922 §A.7.3 — Parameter Identifiers inside group value */
#define CLLM_PI_PARAM_SET_ID   0x00   /* PI=0: Parameter Set Identifier */
#define CLLM_PI_CAUSE          0x02   /* PI=2: Cause Identifier */
#define CLLM_PI_DLCI_LIST      0x03   /* PI=3: DLCI List */

/* Congestion levels are defined globally in vfr.h */

/* CLLM message structure per Q.922 Annex A Figure A-5:
 * [Address DLCI=1007, C/R=1] [Control XID=0xAF] [FI] [GI] [GL] [PI=0 PL PV] [PI=2 PL PV] [PI=3 PL PV] [FCS]
 */

/* Build CLLM XID frame per Q.922 Annex A Figure A-5 / §A.7 */
size_t cllm_build_xid(u8 *buf, size_t max_len, u8 congestion_level,
                      const u32 *dlci_list, int dlci_count, int dlci_len)
{
    size_t pos = 0;
    int i;

    /* --- Q.922 Address field (DLCI 1007, C/R=1 for response) --- */
    if (dlci_len == 4) {
        if (pos + 4 > max_len) return 0;
        buf[pos++] = (u8)(((CLLM_DLCI >> 17) & 0x3F) << 2) | 0x02;  /* DLCI[22:17], C/R=1, EA=0 */
        buf[pos++] = (u8)(((CLLM_DLCI >> 13) & 0x0F) << 4);         /* DLCI[16:13], EA=0 */
        buf[pos++] = (u8)(((CLLM_DLCI >> 6)  & 0x7F) << 1);         /* DLCI[12:6],  EA=0 */
        buf[pos++] = (u8)(((CLLM_DLCI & 0x3F) << 2) | 0x01);        /* DLCI[5:0],   EA=1 */
    } else {
        /* 2-octet address per Q.922 §3.3 / Figure A-5:
         * Octet 1: DLCI[9:4] (6 bits) | C/R | EA=0
         * Octet 2: DLCI[3:0] (4 bits) | FECN=0 | BECN=0 | DE=0 | EA=1 */
        if (pos + 2 > max_len) return 0;
        buf[pos++] = (u8)(((CLLM_DLCI >> 4) & 0x3F) << 2) | 0x02;  /* C/R=1, EA=0 */
        buf[pos++] = (u8)((CLLM_DLCI & 0x0F) << 4) | 0x01;         /* FECN=0, BECN=0, DE=0, EA=1 */
    }

    /* --- Control field: XID (U-frame, 1 octet) --- */
    if (pos + 1 > max_len) return 0;
    buf[pos++] = CLLM_XID_CTRL;  /* 0xAF: XID, F=0 (unsolicited) */

    /* --- XID Information Field per ISO 8885 / Q.922 §A.7.3 --- */

    /* Format Identifier (1 octet) */
    if (pos + 1 > max_len) return 0;
    buf[pos++] = CLLM_FI;  /* 0x82 */

    /* Group Identifier (1 octet) */
    if (pos + 1 > max_len) return 0;
    buf[pos++] = CLLM_GI;  /* 0x0F */

    /* Group Length (2 octets, big-endian) — filled after building group value.
     * GL = PI=0 hdr(2) + PV(4) + PI=2 hdr(2) + PV(1) + PI=3 hdr(2) + PV(2*dlci_count)
     *    = 13 + 2*dlci_count                                                               */
    if (pos + 2 > max_len) return 0;
    size_t gl_pos = pos;          /* remember position for back-patching */
    pos += 2;                     /* reserve 2 bytes for GL */

    /* --- Group Value: Parameters in order per §A.7.3 --- */

    /* PI=0: Parameter Set Identifier → IA5 "I122" */
    if (pos + 6 > max_len) return 0;
    buf[pos++] = CLLM_PI_PARAM_SET_ID;  /* 0x00 */
    buf[pos++] = 0x04;                   /* PL = 4 octets */
    buf[pos++] = 0x49;                   /* 'I' */
    buf[pos++] = 0x31;                   /* '1' */
    buf[pos++] = 0x32;                   /* '2' */
    buf[pos++] = 0x32;                   /* '2' */

    /* PI=2: Cause Identifier */
    if (pos + 3 > max_len) return 0;
    buf[pos++] = CLLM_PI_CAUSE;          /* 0x02 */
    buf[pos++] = 0x01;                    /* PL = 1 octet */
    buf[pos++] = congestion_level;

    /* PI=3: DLCI List — each 10-bit DLCI encoded as 2 octets per Q.922 §A.7.3.5.3 / X.36 §C.3.5.3:
     *   Octet 1: DLCI[9:4] in bits 8..3, bits 2..1 reserved (0)
     *   Octet 2: DLCI[3:0] in bits 8..5, bits 4..1 reserved (0) */
    if (pos + 2 > max_len) return 0;
    buf[pos++] = CLLM_PI_DLCI_LIST;      /* 0x03 */
    buf[pos++] = (u8)(dlci_len * dlci_count);    /* PL = dlci_len bytes per DLCI */

    for (i = 0; i < dlci_count; i++) {
        u32 dlci = dlci_list[i];
        if (pos + dlci_len > max_len) break;
        if (dlci_len == 4) {
            buf[pos++] = (u8)(((dlci >> 17) & 0x3F) << 2);   /* DLCI[22:17] */
            buf[pos++] = (u8)(((dlci >> 13) & 0x0F) << 4);   /* DLCI[16:13] */
            buf[pos++] = (u8)(((dlci >> 6)  & 0x7F) << 1);   /* DLCI[12:6]  */
            buf[pos++] = (u8)(((dlci & 0x3F) << 2));          /* DLCI[5:0]   */
        } else {
            buf[pos++] = (u8)(((dlci >> 4) & 0x3F) << 2);    /* DLCI[9:4] in bits 8..3, bits 2..1 reserved (0) */
            buf[pos++] = (u8)((dlci & 0x0F) << 4);           /* DLCI[3:0] in bits 8..5, bits 4..1 reserved (0) */
        }
    }

    /* Back-patch Group Length (big-endian) */
    {
        u16 gl = (u16)(pos - gl_pos - 2);
        buf[gl_pos]     = (u8)(gl >> 8);
        buf[gl_pos + 1] = (u8)(gl & 0xFF);
    }

    return pos;
}

/* Parse CLLM XID frame */
int cllm_parse_xid(const u8 *buf, size_t len, u8 *congestion_level,
                   u32 *dlci_list, int *dlci_count, int max_dlcis)
{
    size_t pos = 0;
    int i = 0;

    if (!buf || len < 8) return -1;

    /* Skip address field */
    size_t addr_len = fr_get_addr_len(buf, len);
    if (addr_len < 2 || addr_len > 4) return -1;
    if (len < addr_len + 5) return -1; /* Address + Control + FI + GI + GL(2) */
    pos += addr_len;

    /* Control field check: XID (0xAF, mask out P/F bit 4: ctrl & 0xEF == 0xAF) */
    u8 ctrl = buf[pos++];
    if ((ctrl & CLLM_XID_CTRL_MASK) != CLLM_XID_CTRL) return -1;

    /* FI (Format Identifier) check: must be 0x82 */
    if (buf[pos++] != CLLM_FI) return -1;

    /* GI (Group Identifier) check: must be 0x0F */
    if (buf[pos++] != CLLM_GI) return -1;

    /* GL (Group Length) */
    u16 gl = ((u16)buf[pos] << 8) | buf[pos + 1];
    pos += 2;

    if (pos + gl > len) return -1; /* Out of bounds */
    size_t group_end = pos + gl;

    /* Parse parameters inside the group value */
    while (pos < group_end) {
        if (pos + 2 > group_end) return -1; /* Malformed parameter header */
        u8 pi = buf[pos++];
        u8 pl = buf[pos++];

        if (pos + pl > group_end) return -1; /* Parameter value out of bounds */

        switch (pi) {
        case CLLM_PI_PARAM_SET_ID:
            if (pl != 4 || memcmp(&buf[pos], "I122", 4) != 0) {
                /* Not the expected parameter set */
                return -1;
            }
            pos += pl;
            break;

        case CLLM_PI_CAUSE:
            if (pl >= 1) {
                *congestion_level = buf[pos];
            }
            pos += pl;
            break;

        case CLLM_PI_DLCI_LIST:
            {
                size_t param_end = pos + pl;
                /* Derive DLCI octet width from the CLLM frame's address field length:
                 * 4-octet address → 4-octet DLCIs (23-bit); otherwise → 2-octet DLCIs (10-bit) */
                int dlci_octet_width = (addr_len == 4) ? 4 : 2;

                while (pos + dlci_octet_width <= param_end && i < max_dlcis) {
                    u32 dlci;
                    if (dlci_octet_width == 4) {
                        dlci = (((u32)(buf[pos] & 0xFC) >> 2) << 17) |
                               (((u32)(buf[pos + 1] & 0xF0) >> 4) << 13) |
                               (((u32)(buf[pos + 2] & 0xFE) >> 1) << 6) |
                               (((u32)(buf[pos + 3] & 0xFC) >> 2));
                    } else {
                        dlci = (((u32)(buf[pos] & 0xFC) >> 2) << 4) |
                               (((u32)(buf[pos + 1] & 0xF0) >> 4));
                    }
                    dlci_list[i++] = dlci;
                    pos += dlci_octet_width;
                }
                pos = param_end;
            }
            break;

        default:
            /* Unknown parameter, skip */
            pos += pl;
            break;
        }
    }

    *dlci_count = i;
    return 0;
}

/* Send CLLM notification */
int cllm_send_notification(vfr_port_t *port, u8 congestion_level,
                           const u32 *dlci_list, int dlci_count)
{
    u8 buf[256];
    size_t len;

    if (!port) return -1;

    int dlci_len = lmi_port_dlci_len(port);
    len = cllm_build_xid(buf, sizeof(buf), congestion_level, dlci_list, dlci_count, dlci_len);
    if (len > 0) {
        /* Compute and append FCS-16 per X.36 §9.2.4 only for serial and named pipe transports. */
        if (port->transport == PORT_TRANS_SERIAL || port->transport == PORT_TRANS_PIPE) {
            u16 fcs = crc16_fcs(buf, len);
            if (len + 2 > sizeof(buf)) {
                LOG_WARN("CLLM message too long for FCS on %s", port->name);
                return -1;
            }
            buf[len++] = fcs & 0xFF;
            buf[len++] = (fcs >> 8) & 0xFF;
        }

        if (port->ops && port->ops->send) {
            LOG_DEBUG("Sending CLLM notification (level=%u, %d DLCIs) on %s", congestion_level, dlci_count, port->name);
            int ret = port->ops->send(port, buf, len);
            if (ret >= 0) {
                port->stats.tx_frames++;
                port->stats.tx_bytes += (u64)ret;
                return 0;
            }
        }
    }

    return -1;
}

/* Handle incoming CLLM */
int cllm_handle_frame(vfr_port_t *port, const u8 *frame, size_t len)
{
    u8 congestion_level = CLLM_LEVEL_NONE;
    u32 dlci_list[64];
    int dlci_count = 0;
    int i;

    if (!port) return -1;

    if (cllm_parse_xid(frame, len, &congestion_level, dlci_list, &dlci_count, 64) < 0) {
        LOG_DEBUG("Invalid CLLM frame");
        return -1;
    }

    LOG_INFO("CLLM received on %s: level=%u, %d DLCIs",
             port->name, congestion_level, dlci_count);

    /* Update port congestion state and PVC peer congestion flags */
    u64 now = get_tick_count();
    if (congestion_level >= CLLM_LEVEL_INDICATED) {
        if (g_vfrs) {
            /* Update peer congestion on all virtual circuits (PVCs, SVCs, SPVCs) */
            mutex_lock(&g_vfrs->dlci_mutex);
            for (int h = 0; h < PVC_HASH_SIZE; h++) {
                vfr_dlci_entry_t *entry = g_vfrs->dlci_table[h];
                while (entry) {
                    if (strcmp(entry->port_in, port->name) == 0) {
                        if (entry->reverse_entry) {
                            int in_list = 0;
                            for (i = 0; i < dlci_count; i++) {
                                if (dlci_list[i] == entry->dlci_in) {
                                    in_list = 1;
                                    break;
                                }
                            }
                            if (in_list) {
                                entry->reverse_entry->peer_congested = 1;
                                entry->reverse_entry->last_cllm_time = now;
                                LOG_DEBUG("  Affected DLCI: %u (%s - setting peer congestion)",
                                          entry->dlci_in, entry->vc_type == VC_TYPE_SVC ? "SVC" : "PVC");
                            } else {
                                /* Standard compliance: clear peer_congested if missing from the list */
                                entry->reverse_entry->peer_congested = 0;
                            }
                        }
                    }
                    entry = entry->next;
                }
            }
            mutex_unlock(&g_vfrs->dlci_mutex);

            mutex_lock(&g_vfrs->mcast_mutex);
            vfr_mcast_group_t *g = g_vfrs->mcast_groups;
            while (g) {
                if (strcmp(g->source_port, port->name) == 0) {
                    int in_list = 0;
                    for (i = 0; i < dlci_count; i++) {
                        if (dlci_list[i] == g->source_dlci) {
                            in_list = 1;
                            break;
                        }
                    }
                    if (in_list) {
                        g->peer_congested = 1;
                        g->last_cllm_time = now;
                    } else {
                        g->peer_congested = 0;
                    }
                }
                vfr_mcast_member_t *m = g->members;
                while (m) {
                    if (strcmp(m->port_name, port->name) == 0) {
                        int in_list = 0;
                        for (i = 0; i < dlci_count; i++) {
                            if (dlci_list[i] == m->dlci) {
                                in_list = 1;
                                break;
                            }
                        }
                        if (in_list) {
                            m->peer_congested = 1;
                            m->last_cllm_time = now;
                        } else {
                            m->peer_congested = 0;
                        }
                    }
                    m = m->next;
                }
                g = g->next;
            }
            mutex_unlock(&g_vfrs->mcast_mutex);
        }
    }

    return 0;
}

/* Check if DLCI should be included in CLLM */
int cllm_should_include_dlci(u32 dlci, u8 congestion_level)
{
    (void)dlci;
    /* Based on X.36 §9.3.2, CLLM includes:
     * - All PVCs when congestion level >= 2
     * - Only PVCs with recent congestion activity at level 1
     */
    if (congestion_level >= CLLM_LEVEL_PERSISTENT) {
        return 1;  /* Include all */
    }
    if (congestion_level >= CLLM_LEVEL_INDICATED) {
        /* Could filter based on recent activity */
        return 1;
    }
    return 0;
}