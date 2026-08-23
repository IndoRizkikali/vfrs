/*
 * svc_sig_iep.c - Q.933 Information Element Parser & Message Builder
 * Virtual Frame Relay Switch - ITU-T Q.933 / X.36 Clause 10
 */

#include "svc_sig_iep.h"

/* ============================================================
 * Q.933 Header Parsing & Building (X.36 §10.6.1 - §10.6.3)
 * ============================================================ */

int q933_parse_header(const u8 *data, size_t len, q933_msg_header_t *hdr) {
    if (!data || !hdr || len < 3) return -1;

    memset(hdr, 0, sizeof(*hdr));
    hdr->protocol_disc = data[0];

    if (hdr->protocol_disc != Q933_PROTOCOL_DISC) {
        LOG_WARN("Q.933 Parse Header: Invalid protocol discriminator 0x%02X (expected 0x08)", hdr->protocol_disc);
        return -1;
    }

    /* X.36 §10.10.3.1: Bits 5-8 of call reference octet 1 (data[1]) must be 0000 */
    if ((data[1] & 0xF0) != 0) {
        LOG_WARN("Q.933 Parse Header: Invalid CRV octet 1 upper bits 0x%02X (expected 0x00)", data[1] & 0xF0);
        return -1;
    }

    hdr->call_ref_len = data[1] & 0x0F;

    /* Support 0-octet Global Call Reference */
    if (hdr->call_ref_len == 0) {
        hdr->call_ref_flag = 0;
        hdr->call_ref_value = 0;
        hdr->message_type = data[2];
        return 3;
    }

    /* X.36 §10.10.3.1.2: Call Reference length MUST be 2 octets */
    if (hdr->call_ref_len != 2) {
        LOG_WARN("Q.933 Parse Header: Non-compliant CRV length %u (expected 2)", hdr->call_ref_len);
        return -1;
    }

    if (len < 5) return -1;
    hdr->call_ref_flag = (data[2] & 0x80) ? 1 : 0;
    hdr->call_ref_value = (((u16)(data[2] & 0x7F)) << 8) | data[3];
    hdr->message_type = data[4];
    return 5;
}

int q933_build_header(u8 *buf, size_t max_len, u16 call_ref, u8 call_ref_flag, u8 call_ref_len, u8 msg_type) {
    (void)call_ref_len; /* Standard: always 2-octet CRV */
    if (!buf || max_len < 5) return -1;
    buf[0] = Q933_PROTOCOL_DISC;
    buf[1] = 0x02; /* Length = 2 */
    buf[2] = (call_ref_flag ? 0x80 : 0x00) | ((call_ref >> 8) & 0x7F);
    buf[3] = (call_ref & 0xFF);
    buf[4] = msg_type;
    return 5;
}

/* ============================================================
 * IE Parsers (X.36 §10.6.4 - §10.6.21)
 * ============================================================ */

int q933_parse_bearer_capability(const u8 *ie_data, size_t ie_len) {
    if (!ie_data || ie_len < 3) return -1;

    /* Octet 3: Coding standard must be 00 (ITU), Transfer capability must be 01000 (Frame Relay) */
    if ((ie_data[0] & 0x7F) != 0x08) {
        LOG_WARN("Bearer Capability octet 3 mismatch: 0x%02X", ie_data[0]);
        return -2;
    }
    /* Octet 4: Transfer mode must be 01 (Frame mode) */
    if ((ie_data[1] & 0x60) != 0x20) {
        LOG_WARN("Bearer Capability octet 4 mismatch: 0x%02X (expected Transfer Mode 01)", ie_data[1]);
        return -2;
    }
    /* Octet 5/6: Layer 2 Ident must be 10, User information L2 protocol must be 01111 (Q.922/X.36 Core aspects) */
    if (ie_len >= 3) {
        if ((ie_data[2] & 0x60) != 0x40) {
            LOG_WARN("Bearer Capability octet 5/6 L2 identifier mismatch: 0x%02X (expected L2 ID 10)", ie_data[2] & 0x60);
            return -2;
        }
        if ((ie_data[2] & 0x1F) != 0x0F) {
            LOG_WARN("Bearer Capability octet 5/6 protocol mismatch: 0x%02X (expected Q.922 Core 01111)", ie_data[2] & 0x1F);
            return -2;
        }
    }
    return 0;
}

int q933_parse_cause(const u8 *ie_data, size_t ie_len, u8 *location, u8 *cause_value) {
    if (!ie_data || ie_len < 2) return -1;
    if (location) *location = ie_data[0] & 0x0F;
    if (cause_value) *cause_value = ie_data[1] & 0x7F;
    return 0;
}

int q933_parse_call_state(const u8 *ie_data, size_t ie_len, u8 *state) {
    if (!ie_data || ie_len < 1) return -1;
    if (state) *state = ie_data[0] & 0x3F;
    return 0;
}

int q933_parse_dlci_ie(const u8 *ie_data, size_t ie_len, u32 *dlci, u8 *dlci_len) {
    if (!ie_data || ie_len < 2) return -1;

    u32 d = 0;
    if (ie_len == 2) {
        d = (((u32)(ie_data[0] & 0x3F)) << 4) | (((u32)(ie_data[1] >> 3)) & 0x0F);
        if (dlci_len) *dlci_len = 2;
    } else if (ie_len >= 4) {
        d = (((u32)(ie_data[0] & 0x3F)) << 17) |
            ((((u32)(ie_data[1] >> 3)) & 0x0F) << 13) |
            (((u32)(ie_data[2] & 0x7F)) << 6)  |
            (((u32)(ie_data[3] >> 1)) & 0x3F);
        if (dlci_len) *dlci_len = 4;
    }

    if (dlci) *dlci = d;
    return 0;
}

int q933_parse_llcore_params(const u8 *ie_data, size_t ie_len, q933_llcore_params_t *params) {
    if (!ie_data || !params) return -1;
    memset(params, 0, sizeof(*params));
    params->present = 1;

    /* Defaults per X.36 §10.6.15 */
    params->fwd_cir = 64000;
    params->bwd_cir = 64000;
    params->fwd_bc  = 64000;
    params->bwd_bc  = 64000;
    params->fwd_be  = 0;
    params->bwd_be  = 0;
    params->fwd_fmif = FR_MAX_FRAMESZ;
    params->bwd_fmif = FR_MAX_FRAMESZ;

    size_t idx = 0;
    while (idx < ie_len) {
        u8 sub_id = ie_data[idx++] & 0x7F;

        if (sub_id == 0x09) { /* Max FRIF size */
            if (idx + 2 <= ie_len) {
                u16 fmif = (((u16)(ie_data[idx] & 0x7F)) << 7) | (ie_data[idx + 1] & 0x7F);
                params->fwd_fmif = fmif;
                params->bwd_fmif = fmif;
                idx += 2;
            }
        } else if (sub_id == 0x0A) { /* Throughput / CIR */
            if (idx + 2 <= ie_len) {
                u8 mag = (ie_data[idx] >> 4) & 0x07;
                u32 mult = ((u32)(ie_data[idx] & 0x0F) << 7) | (ie_data[idx + 1] & 0x7F);
                u32 p10 = 1;
                for (u8 m = 0; m < mag; m++) p10 *= 10;
                params->fwd_cir = mult * p10;
                params->bwd_cir = params->fwd_cir;
                idx += 2;
            }
        } else if (sub_id == 0x0D) { /* Bc */
            if (idx + 2 <= ie_len) {
                u32 bc_bytes = (((u32)(ie_data[idx] & 0x7F)) << 7) | (ie_data[idx + 1] & 0x7F);
                params->fwd_bc = bc_bytes * 8;
                params->bwd_bc = params->fwd_bc;
                idx += 2;
            }
        } else if (sub_id == 0x0E) { /* Be */
            if (idx + 2 <= ie_len) {
                u32 be_bytes = (((u32)(ie_data[idx] & 0x7F)) << 7) | (ie_data[idx + 1] & 0x7F);
                params->fwd_be = be_bytes * 8;
                params->bwd_be = params->fwd_be;
                idx += 2;
            }
        } else {
            /* Skip unknown sub-IE: consume value octets until one with ext=1 (MSB=1) is found */
            while (idx < ie_len) {
                u8 octet = ie_data[idx++];
                if (octet & 0x80) break;
            }
        }
    }

    return 0;
}

int q933_parse_number_ie(const u8 *ie_data, size_t ie_len, char *num_buf, size_t max_len,
                        u8 *type, u8 *plan, u8 *presentation, u8 *screening) {
    if (!ie_data || ie_len < 1 || !num_buf || max_len == 0) return -1;

    size_t idx = 0;
    u8 octet3 = ie_data[idx++];

    if (type) *type = (octet3 >> 4) & 0x07;
    if (plan) *plan = octet3 & 0x0F;

    if (presentation) *presentation = 0; /* Default: Allowed */
    if (screening) *screening = 3;       /* Default: Network provided */

    /* Check if octet 3a is present (bit 8 == 0) */
    if ((octet3 & 0x80) == 0 && idx < ie_len) {
        u8 octet3a = ie_data[idx++];
        if (presentation) *presentation = (octet3a >> 5) & 0x03;
        if (screening) *screening = octet3a & 0x03;
    }

    /* Copy remaining number digits */
    size_t digit_len = 0;
    while (idx < ie_len && digit_len < max_len - 1) {
        num_buf[digit_len++] = (char)(ie_data[idx++] & 0x7F);
    }
    num_buf[digit_len] = '\0';

    return 0;
}

int q933_parse_restart_indicator(const u8 *ie_data, size_t ie_len, u8 *restart_class) {
    if (!ie_data || ie_len < 1) return -1;
    if (restart_class) *restart_class = ie_data[0] & 0x07;
    return 0;
}

int q933_parse_reverse_charge_ind(const u8 *ie_data, size_t ie_len, u8 *rev_charge) {
    if (!ie_data || ie_len < 1) return -1;
    if (rev_charge) *rev_charge = ie_data[0] & 0x07;
    return 0;
}

int q933_parse_priority_params(const u8 *ie_data, size_t ie_len, u8 *ftp_out, u8 *ftp_in, u8 *fdp_out, u8 *fdp_in, u8 *srv_class) {
    if (!ie_data) return -1;
    size_t offset = 0;
    while (offset < ie_len) {
        if (offset + 1 >= ie_len) return -1; /* Malformed */
        u8 param_id = ie_data[offset];
        u8 param_val = ie_data[offset + 1];

        if (param_id == 0x01) { /* Frame transfer priority */
            if (ftp_out) *ftp_out = (param_val >> 4) & 0x0F;
            if (ftp_in)  *ftp_in  = param_val & 0x0F;
        } else if (param_id == 0x02) { /* Frame discard priority */
            if (fdp_out) *fdp_out = (param_val >> 4) & 0x07;
            if (fdp_in)  *fdp_in  = param_val & 0x07;
        } else if (param_id == 0x03) { /* Service class */
            if (srv_class) *srv_class = param_val & 0x03;
        }
        offset += 2;
    }
    return 0;
}

int q933_parse_subaddress(const u8 *ie_data, size_t ie_len, u8 *subaddr_buf, size_t max_buf, u8 *out_len) {
    if (!ie_data || !subaddr_buf || !out_len || ie_len == 0) return -1;
    if (ie_len > max_buf) {
        ie_len = max_buf;
    }
    memcpy(subaddr_buf, ie_data, ie_len);
    *out_len = (u8)ie_len;
    return 0;
}

int q933_build_subaddress(u8 *buf, size_t max_len, u8 ie_id, const u8 *subaddr_data, u8 subaddr_len) {
    if (!buf || !subaddr_data || subaddr_len == 0) return 0;
    if (max_len < (size_t)(2 + subaddr_len)) return 0;
    buf[0] = ie_id;
    buf[1] = subaddr_len;
    memcpy(&buf[2], subaddr_data, subaddr_len);
    return 2 + subaddr_len;
}

/* ============================================================
 * IE Builders (X.36 §10.6.4 - §10.6.21)
 * ============================================================ */

int q933_build_bearer_capability(u8 *buf, size_t max_len) {
    if (!buf || max_len < 5) return -1;
    buf[0] = Q933_IE_BEARER_CAPABILITY;
    buf[1] = 0x03; /* Length = 3 octets */
    buf[2] = 0x88; /* Ext=1, Coding Standard=00 (CCITT), Info Transfer Cap=01000 (Frame relay) */
    buf[3] = 0xA0; /* Ext=1, Transfer Mode=01 (Frame mode) */
    buf[4] = 0xCF; /* Ext=1, Layer 2 Ident=10 (User info L2 protocol), User info layer 2 protocol=01111 (Core aspects Q.922 / X.36) */
    return 5;
}

int q933_build_cause(u8 *buf, size_t max_len, u8 location, u8 cause_value) {
    if (!buf || max_len < 4) return -1;
    buf[0] = Q933_IE_CAUSE;
    buf[1] = 0x02; /* Length = 2 octets */
    buf[2] = 0x80 | (location & 0x0F);
    buf[3] = 0x80 | (cause_value & 0x7F);
    return 4;
}

int q933_build_call_state(u8 *buf, size_t max_len, u8 state) {
    if (!buf || max_len < 3) return -1;
    buf[0] = Q933_IE_CALL_STATE;
    buf[1] = 0x01; /* Length = 1 octet */
    buf[2] = 0x00 | (state & 0x3F); /* Coding Std=00 (bits 8-7) */
    return 3;
}

int q933_build_dlci_ie(u8 *buf, size_t max_len, u32 dlci, u8 dlci_len) {
    if (!buf) return -1;

    if (dlci_len == 4 || dlci > 1023) {
        if (max_len < 6) return -1;
        buf[0] = Q933_IE_DLCI;
        buf[1] = 0x04;
        buf[2] = 0x40 | (u8)((dlci >> 17) & 0x3F);   /* Ext=0, Pref/Excl=1 (bit 7), DLCI bits 22-17 */
        buf[3] = (u8)(((dlci >> 13) & 0x0F) << 3);   /* Ext=0, DLCI bits 16-13 in bits 7-4, Reserved 000 in bits 3-1 */
        buf[4] = (u8)((dlci >> 6) & 0x7F);           /* Ext=0, DLCI bits 12-6 in bits 7-1 */
        buf[5] = 0x80 | (u8)((dlci & 0x3F) << 1);    /* Ext=1, DLCI bits 5-0 in bits 7-2, Reserved 0 in bit 1 */
        return 6;
    } else {
        if (max_len < 4) return -1;
        buf[0] = Q933_IE_DLCI;
        buf[1] = 0x02;
        buf[2] = 0x40 | (u8)((dlci >> 4) & 0x3F);    /* Ext=0 (bit 8), Pref/Excl=1 (bit 7), DLCI bits 9-4 */
        buf[3] = 0x80 | (u8)((dlci & 0x0F) << 3);    /* Ext=1 (bit 8), DLCI bits 3-0 in bits 7-4, Reserved 000 in bits 3-1 */
        return 4;
    }
}

static void cir_to_mag_mult(u32 cir, u8 *mag_out, u32 *mult_out) {
    u8 best_mag = 3;
    u32 best_mult = 64;
    u32 min_diff = 0xFFFFFFFF;

    for (u8 mag = 0; mag <= 6; mag++) {
        u32 p10 = 1;
        for (u8 m = 0; m < mag; m++) p10 *= 10;
        
        u32 mult = (cir + p10 / 2) / p10;
        if (mult <= 2047) {
            u32 calc = mult * p10;
            u32 diff = calc > cir ? (calc - cir) : (cir - calc);
            if (diff < min_diff || (diff == min_diff && mult < best_mult)) {
                min_diff = diff;
                best_mag = mag;
                best_mult = mult;
            }
        }
    }
    *mag_out = best_mag;
    *mult_out = best_mult;
}

int q933_build_llcore_params(u8 *buf, size_t max_len, const q933_llcore_params_t *params) {
    if (!buf || max_len < 16 || !params) return -1;

    buf[0] = Q933_IE_LLCORE_PARAMS;
    buf[1] = 14; /* Length = 14 octets */
    size_t idx = 2;

    /* Group 3: Outgoing Max FRIF Size */
    buf[idx++] = 0x09; /* Sub-IE ID 0x09 */
    u16 fmif = params->fwd_fmif > 0 ? params->fwd_fmif : FR_MAX_FRAMESZ;
    buf[idx++] = (u8)((fmif >> 7) & 0x7F); /* Ext=0 */
    buf[idx++] = 0x80 | (u8)(fmif & 0x7F);  /* Ext=1 */

    /* Group 4: Throughput (CIR) */
    buf[idx++] = 0x0A; /* Sub-IE ID 0x0A */
    u32 cir = params->fwd_cir > 0 ? params->fwd_cir : 64000;
    u8 mag = 3;
    u32 mult = 64;
    cir_to_mag_mult(cir, &mag, &mult);
    buf[idx++] = (u8)(((mag & 0x07) << 4) | ((mult >> 7) & 0x0F)); /* Ext=0, mag in bits 7-5, mult in bits 4-1 */
    buf[idx++] = 0x80 | (u8)(mult & 0x7F);                         /* Ext=1, mult in bits 7-1 */

    /* Group 6: Committed Burst Size (Bc) */
    buf[idx++] = 0x0D; /* Sub-IE ID 0x0D */
    u32 bc_bytes = (params->fwd_bc > 0 ? params->fwd_bc : 64000) / 8;
    buf[idx++] = (u8)((bc_bytes >> 7) & 0x7F); /* Ext=0 */
    buf[idx++] = 0x80 | (u8)(bc_bytes & 0x7F);  /* Ext=1 */

    /* Group 7: Excess Burst Size (Be) */
    buf[idx++] = 0x0E; /* Sub-IE ID 0x0E */
    u32 be_bytes = params->fwd_be / 8;
    buf[idx++] = (u8)((be_bytes >> 7) & 0x7F); /* Ext=0 */
    buf[idx++] = 0x80 | (u8)(be_bytes & 0x7F);  /* Ext=1 */

    buf[1] = (u8)(idx - 2);
    return (int)idx;
}

int q933_build_called_number(u8 *buf, size_t max_len, const char *number, u8 type, u8 plan) {
    if (!buf || !number) return -1;
    size_t num_len = strlen(number);
    if (max_len < 3 + num_len) return -1;

    buf[0] = Q933_IE_CALLED_NUMBER;
    buf[1] = (u8)(1 + num_len);
    buf[2] = 0x80 | ((type & 0x07) << 4) | (plan & 0x0F); /* Ext bit = 1 */

    memcpy(&buf[3], number, num_len);
    return (int)(3 + num_len);
}

int q933_build_calling_number(u8 *buf, size_t max_len, const char *number, u8 type, u8 plan, u8 presentation, u8 screening) {
    if (!buf || !number) return -1;
    size_t num_len = strlen(number);
    if (max_len < 4 + num_len) return -1;

    buf[0] = Q933_IE_CALLING_NUMBER;
    buf[1] = (u8)(2 + num_len);
    buf[2] = 0x00 | ((type & 0x07) << 4) | (plan & 0x0F); /* Ext bit = 0 (octet 3a follows) */
    buf[3] = 0x80 | ((presentation & 0x03) << 5) | (screening & 0x03); /* Ext bit = 1 */

    memcpy(&buf[4], number, num_len);
    return (int)(4 + num_len);
}

int q933_build_connected_number(u8 *buf, size_t max_len, const char *number, u8 type, u8 plan, u8 presentation, u8 screening) {
    if (!buf || !number) return -1;
    size_t num_len = strlen(number);
    if (max_len < 4 + num_len) return -1;

    buf[0] = Q933_IE_CONNECTED_NUMBER;
    buf[1] = (u8)(2 + num_len);
    buf[2] = 0x00 | ((type & 0x07) << 4) | (plan & 0x0F);
    buf[3] = 0x80 | ((presentation & 0x03) << 5) | (screening & 0x03);

    memcpy(&buf[4], number, num_len);
    return (int)(4 + num_len);
}

int q933_build_reverse_charge_ind(u8 *buf, size_t max_len, u8 rev_charge) {
    if (!buf || max_len < 3) return -1;
    buf[0] = Q933_IE_REVERSE_CHARGE_IND;
    buf[1] = 0x01;
    buf[2] = 0x80 | (rev_charge & 0x07);
    return 3;
}

int q933_build_priority_params(u8 *buf, size_t max_len, u8 ftp_out, u8 ftp_in, u8 fdp_out, u8 fdp_in, u8 srv_class) {
    if (!buf || max_len < 8) return -1;
    buf[0] = Q933_IE_PRIORITY_SVC_CLASS;
    buf[1] = 0x06; /* Length of contents = 6 octets */
    buf[2] = 0x01; /* Frame transfer priority identifier */
    buf[3] = ((ftp_out & 0x0F) << 4) | (ftp_in & 0x0F);
    buf[4] = 0x02; /* Frame discard priority identifier */
    buf[5] = ((fdp_out & 0x07) << 4) | (fdp_in & 0x07);
    buf[6] = 0x03; /* Service class identifier */
    buf[7] = srv_class & 0x03;
    return 8;
}

int q933_build_restart_indicator(u8 *buf, size_t max_len, u8 restart_class) {
    if (!buf || max_len < 3) return -1;
    buf[0] = Q933_IE_RESTART_INDICATOR;
    buf[1] = 0x01;
    buf[2] = 0x80 | (restart_class & 0x07);
    return 3;
}

/* ============================================================
 * High-level Message Builders per X.36 Tables 10-2 to 10-11
 * ============================================================ */

int q933_build_setup(u8 *buf, size_t max_len, vfr_call_t *call, vfr_svc_ctx_t *sctx) {
    if (!buf || !call || !sctx) return -1;

    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_SETUP);
    if (offset < 0) return -1;

    /* 1. Bearer Capability (0x04) */
    offset += q933_build_bearer_capability(&buf[offset], max_len - offset);

    /* 2. DLCI (0x19) */
    if (call->ingress_dlci > 0) {
        offset += q933_build_dlci_ie(&buf[offset], max_len - offset, call->ingress_dlci, 2);
    }

    /* 3. Link Layer Core Parameters (0x48) */
    offset += q933_build_llcore_params(&buf[offset], max_len - offset, &call->llcore);

    /* 4. Reverse Charge Ind (0x4A) */
    if (call->rev_charge_requested) {
        offset += q933_build_reverse_charge_ind(&buf[offset], max_len - offset, 0x01); /* 0x01 = Reverse charging requested */
    }

    /* 5. Priority and Service Class Parameters (0x6A) - Optional per X.36 Table 10-9 */
    if (call->priority_present) {
        offset += q933_build_priority_params(&buf[offset], max_len - offset,
                                             call->ftp_out, call->ftp_in,
                                             call->fdp_out, call->fdp_in,
                                             call->srv_class);
    }

    /* 6. Calling Party Number (0x6C) */
    if (call->calling_number[0] != '\0') {
        u8 type = (call->calling_number_type != 0xFF) ? call->calling_number_type : 1;
        u8 plan = (call->calling_number_plan != 0xFF) ? call->calling_number_plan : 3;
        offset += q933_build_calling_number(&buf[offset], max_len - offset, call->calling_number,
                                            type, plan, call->calling_presentation, call->calling_screening);
    }

    /* 6a. Calling Party Subaddress (0x6D) */
    if (call->calling_subaddr_len > 0) {
        offset += q933_build_subaddress(&buf[offset], max_len - offset, Q933_IE_CALLING_SUBADDR, call->calling_subaddr, call->calling_subaddr_len);
    }

    /* 7. Called Party Number (0x70) */
    if (call->called_number[0] != '\0') {
        u8 type = (call->called_number_type != 0xFF) ? call->called_number_type : 1;
        u8 plan = (call->called_number_plan != 0xFF) ? call->called_number_plan : 3;
        offset += q933_build_called_number(&buf[offset], max_len - offset, call->called_number, type, plan);
    }

    /* 7a. Called Party Subaddress (0x71) */
    if (call->called_subaddr_len > 0) {
        offset += q933_build_subaddress(&buf[offset], max_len - offset, Q933_IE_CALLED_SUBADDR, call->called_subaddr, call->called_subaddr_len);
    }

    /* 8. Low Layer Compatibility (0x7C) */
    if (call->llc_len > 0) {
        if (max_len - offset >= (size_t)(call->llc_len + 2)) {
            buf[offset++] = Q933_IE_LOW_LAYER_COMPAT;
            buf[offset++] = call->llc_len;
            memcpy(&buf[offset], call->llc_data, call->llc_len);
            offset += call->llc_len;
        }
    }

    /* 9. User-User (0x7E) */
    if (call->uu_len > 0) {
        if (max_len - offset >= (size_t)(call->uu_len + 2)) {
            buf[offset++] = Q933_IE_USER_USER;
            buf[offset++] = call->uu_len;
            memcpy(&buf[offset], call->uu_data, call->uu_len);
            offset += call->uu_len;
        }
    }

    return offset;
}

int q933_build_call_proceeding(u8 *buf, size_t max_len, vfr_call_t *call) {
    if (!buf || !call) return -1;
    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_CALL_PROCEEDING);
    if (offset < 0) return -1;

    if (call->ingress_dlci > 0) {
        offset += q933_build_dlci_ie(&buf[offset], max_len - offset, call->ingress_dlci, 2);
    }

    return offset;
}

int q933_build_connect(u8 *buf, size_t max_len, vfr_call_t *call) {
    if (!buf || !call) return -1;
    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_CONNECT);
    if (offset < 0) return -1;

    if (call->ingress_dlci > 0) {
        offset += q933_build_dlci_ie(&buf[offset], max_len - offset, call->ingress_dlci, 2);
    }

    /* LLCORE is mandatory in CONNECT per X.36 Table 10-3 */
    offset += q933_build_llcore_params(&buf[offset], max_len - offset, &call->llcore);

    if (call->connected_number[0] != '\0') {
        u8 type = (call->connected_number_type != 0xFF) ? call->connected_number_type : 1;
        u8 plan = (call->connected_number_plan != 0xFF) ? call->connected_number_plan : 3;
        offset += q933_build_connected_number(&buf[offset], max_len - offset, call->connected_number,
                                              type, plan, call->connected_presentation, call->connected_screening);
    }

    /* Connected Subaddress (0x4D) */
    if (call->connected_subaddr_len > 0) {
        offset += q933_build_subaddress(&buf[offset], max_len - offset, Q933_IE_CONNECTED_SUBADDR, call->connected_subaddr, call->connected_subaddr_len);
    }

    /* Append LLC and UU optional IEs in correct ascending order if present (m1) */
    if (call->llc_len > 0) {
        if (max_len - offset >= (size_t)(call->llc_len + 2)) {
            buf[offset++] = Q933_IE_LOW_LAYER_COMPAT;
            buf[offset++] = call->llc_len;
            memcpy(&buf[offset], call->llc_data, call->llc_len);
            offset += call->llc_len;
        }
    }

    if (call->uu_len > 0) {
        if (max_len - offset >= (size_t)(call->uu_len + 2)) {
            buf[offset++] = Q933_IE_USER_USER;
            buf[offset++] = call->uu_len;
            memcpy(&buf[offset], call->uu_data, call->uu_len);
            offset += call->uu_len;
        }
    }

    return offset;
}

int q933_build_connect_ack(u8 *buf, size_t max_len, vfr_call_t *call) {
    if (!buf || !call) return -1;
    return q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_CONNECT_ACK);
}

int q933_build_disconnect(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause) {
    if (!buf || !call) return -1;
    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_DISCONNECT);
    if (offset < 0) return -1;

    offset += q933_build_cause(&buf[offset], max_len - offset, Q850_LOC_PUBLIC_LOCAL_NET, cause);
    return offset;
}

int q933_build_release(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause) {
    if (!buf || !call) return -1;
    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_RELEASE);
    if (offset < 0) return -1;

    offset += q933_build_cause(&buf[offset], max_len - offset, Q850_LOC_PUBLIC_LOCAL_NET, cause);
    return offset;
}

int q933_build_release_complete(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 cause) {
    if (!buf) return -1;
    int offset = q933_build_header(buf, max_len, crv, crv_flag, crv_len, Q933_MSG_RELEASE_COMPLETE);
    if (offset < 0) return -1;

    offset += q933_build_cause(&buf[offset], max_len - offset, Q850_LOC_PUBLIC_LOCAL_NET, cause);
    return offset;
}

int q933_build_status_raw(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 state, u8 cause) {
    if (!buf) return -1;
    int offset = q933_build_header(buf, max_len, crv, crv_flag, crv_len, Q933_MSG_STATUS);
    if (offset < 0) return -1;

    offset += q933_build_cause(&buf[offset], max_len - offset, Q850_LOC_PUBLIC_LOCAL_NET, cause);
    offset += q933_build_call_state(&buf[offset], max_len - offset, state);
    return offset;
}

int q933_build_status(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause) {
    u16 crv = call ? call->call_ref : 0;
    u8 flag = call ? call->call_ref_flag : 0;
    u8 crv_len = call ? call->call_ref_len : 2;
    u8 state = call ? call->state : 0;
    return q933_build_status_raw(buf, max_len, crv, flag, crv_len, state, cause);
}

int q933_build_status_enquiry(u8 *buf, size_t max_len, vfr_call_t *call) {
    if (!buf || !call) return -1;
    return q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_STATUS_ENQUIRY);
}

int q933_build_restart(u8 *buf, size_t max_len, u8 restart_class) {
    if (!buf) return -1;
    int offset = q933_build_header(buf, max_len, 0, 0, 2, Q933_MSG_RESTART);
    if (offset < 0) return -1;

    offset += q933_build_restart_indicator(&buf[offset], max_len - offset, restart_class);
    return offset;
}

int q933_build_restart_ack(u8 *buf, size_t max_len, u8 restart_class) {
    if (!buf) return -1;
    int offset = q933_build_header(buf, max_len, 0, 0, 2, Q933_MSG_RESTART_ACK);
    if (offset < 0) return -1;

    offset += q933_build_restart_indicator(&buf[offset], max_len - offset, restart_class);
    return offset;
}

/* ============================================================
 * ITU-T X.76 NNI-Specific IE Parsers & Builders
 * ============================================================ */

int q933_parse_call_ident(const u8 *ie_data, size_t ie_len, u32 *call_ident) {
    if (!ie_data || ie_len < 4) return -1;
    if (call_ident) {
        *call_ident = ((u32)ie_data[0] << 24) |
                      ((u32)ie_data[1] << 16) |
                      ((u32)ie_data[2] << 8)  |
                      ((u32)ie_data[3]);
    }
    return 0;
}

int q933_build_call_ident(u8 *buf, size_t max_len, u32 call_ident) {
    if (!buf || max_len < 6) return -1;
    buf[0] = Q933_IE_CALL_IDENT;
    buf[1] = 4;
    buf[2] = (u8)((call_ident >> 24) & 0xFF);
    buf[3] = (u8)((call_ident >> 16) & 0xFF);
    buf[4] = (u8)((call_ident >> 8)  & 0xFF);
    buf[5] = (u8)(call_ident & 0xFF);
    return 6;
}

int q933_parse_transit_net_id(const u8 *ie_data, size_t ie_len, char *net_id, size_t max_len, u8 *type_plan) {
    if (!ie_data || ie_len < 1 || !net_id || max_len == 0) return -1;
    if (type_plan) *type_plan = ie_data[0];
    size_t digits_len = ie_len - 1;
    if (digits_len >= max_len) digits_len = max_len - 1;
    if (digits_len > 0) {
        memcpy(net_id, &ie_data[1], digits_len);
    }
    net_id[digits_len] = '\0';
    return 0;
}

int q933_build_transit_net_id(u8 *buf, size_t max_len, const char *net_id, u8 type_plan) {
    if (!buf || !net_id) return -1;
    size_t id_len = strlen(net_id);
    if (max_len < 3 + id_len) return -1;
    buf[0] = Q933_IE_TRANSIT_NET_ID;
    buf[1] = (u8)(1 + id_len);
    buf[2] = 0x80 | (type_plan & 0x7F);
    memcpy(&buf[3], net_id, id_len);
    return (int)(3 + id_len);
}

int q933_parse_clearing_net_id(const u8 *ie_data, size_t ie_len, char *net_id, size_t max_len, u8 *type_plan) {
    if (!ie_data || ie_len < 1 || !net_id || max_len == 0) return -1;
    if (type_plan) *type_plan = ie_data[0];
    size_t digits_len = ie_len - 1;
    if (digits_len >= max_len) digits_len = max_len - 1;
    if (digits_len > 0) {
        memcpy(net_id, &ie_data[1], digits_len);
    }
    net_id[digits_len] = '\0';
    return 0;
}

int q933_build_clearing_net_id(u8 *buf, size_t max_len, const char *net_id, u8 type_plan) {
    if (!buf || !net_id) return -1;
    size_t id_len = strlen(net_id);
    if (max_len < 3 + id_len) return -1;
    buf[0] = Q933_IE_CLEARING_NET_ID;
    buf[1] = (u8)(1 + id_len);
    buf[2] = 0x80 | (type_plan & 0x7F);
    memcpy(&buf[3], net_id, id_len);
    return (int)(3 + id_len);
}

int q933_parse_transit_network_selection(const u8 *ie_data, size_t ie_len, char *net_id, size_t max_len, u8 *type_plan) {
    if (!ie_data || ie_len < 1 || !net_id || max_len == 0) return -1;
    if (type_plan) *type_plan = ie_data[0];
    size_t digits_len = ie_len - 1;
    if (digits_len >= max_len) digits_len = max_len - 1;
    if (digits_len > 0) {
        memcpy(net_id, &ie_data[1], digits_len);
    }
    net_id[digits_len] = '\0';
    return 0;
}

int q933_build_transit_network_selection(u8 *buf, size_t max_len, const char *net_id, u8 type_plan) {
    if (!buf || !net_id) return -1;
    size_t id_len = strlen(net_id);
    if (max_len < 3 + id_len) return -1;
    buf[0] = Q933_IE_TRANSIT_NETWORK;
    buf[1] = (u8)(1 + id_len);
    buf[2] = 0x80 | (type_plan & 0x7F);
    memcpy(&buf[3], net_id, id_len);
    return (int)(3 + id_len);
}

int q933_parse_gat(const u8 *ie_data, size_t ie_len, u8 *gat_buf, size_t max_len, size_t *out_len) {
    if (!ie_data || !gat_buf || max_len == 0) return -1;
    size_t copy_len = ie_len < max_len ? ie_len : max_len;
    memcpy(gat_buf, ie_data, copy_len);
    if (out_len) *out_len = copy_len;
    return 0;
}

int q933_build_gat(u8 *buf, size_t max_len, const u8 *gat_data, size_t gat_len) {
    if (!buf || !gat_data || max_len < 2 + gat_len || gat_len > 255) return -1;
    buf[0] = Q933_IE_GAT;
    buf[1] = (u8)gat_len;
    memcpy(&buf[2], gat_data, gat_len);
    return (int)(2 + gat_len);
}

/* ============================================================
 * ITU-T X.76 High-Level Message Builders (NNI)
 * ============================================================ */

int q933_build_nni_setup(u8 *buf, size_t max_len, vfr_call_t *call, vfr_svc_ctx_t *sctx) {
    if (!buf || !call || !sctx) return -1;

    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_SETUP);
    if (offset < 0) return -1;

    /* 1. Bearer Capability (0x04) */
    offset += q933_build_bearer_capability(&buf[offset], max_len - offset);

    /* 2. DLCI (0x19) - on NNI this is ingress_dlci or egress_dlci */
    u32 dlci_val = call->ingress_dlci ? call->ingress_dlci : call->egress_dlci;
    offset += q933_build_dlci_ie(&buf[offset], max_len - offset, dlci_val, 2);

    /* 3. Link Layer Core Parameters (0x48) */
    q933_llcore_params_t llcore = call->llcore;
    if (!llcore.present) {
        llcore.present = 1;
        llcore.fwd_cir = sctx->default_cir ? sctx->default_cir : 64000;
        llcore.bwd_cir = sctx->default_cir ? sctx->default_cir : 64000;
        llcore.fwd_bc  = sctx->default_bc  ? sctx->default_bc  : 64000;
        llcore.bwd_bc  = sctx->default_bc  ? sctx->default_bc  : 64000;
        llcore.fwd_be  = sctx->default_be;
        llcore.bwd_be  = sctx->default_be;
        llcore.fwd_fmif = sctx->default_fmif ? sctx->default_fmif : 1600;
        llcore.bwd_fmif = sctx->default_fmif ? sctx->default_fmif : 1600;
    }
    offset += q933_build_llcore_params(&buf[offset], max_len - offset, &llcore);

    /* 4. Reverse Charging Indication (0x4A) */
    if (call->rev_charge_requested) {
        offset += q933_build_reverse_charge_ind(&buf[offset], max_len - offset, 1);
    }

    /* 5. Transit Network Identification IEs (0x67) - All accumulated TNIs */
    for (int i = 0; i < call->tni_list.count && i < SVC_MAX_TRANSIT_NETWORKS; i++) {
        offset += q933_build_transit_net_id(&buf[offset], max_len - offset,
                                            call->tni_list.entries[i].net_id,
                                            call->tni_list.entries[i].type_plan);
    }

    /* 6. Call Identification (0x69) - Mandatory on NNI SETUP */
    if (call->call_ident > 0) {
        offset += q933_build_call_ident(&buf[offset], max_len - offset, call->call_ident);
    }

    /* 7. Priority & Service Class Parameters (0x6A) */
    if (call->priority_present) {
        offset += q933_build_priority_params(&buf[offset], max_len - offset,
                                             call->ftp_out, call->ftp_in,
                                             call->fdp_out, call->fdp_in,
                                             call->srv_class);
    }

    /* 8. Calling Party Number (0x6C) - Mandatory on NNI SETUP */
    offset += q933_build_calling_number(&buf[offset], max_len - offset,
                                       call->calling_number,
                                       call->calling_number_type ? call->calling_number_type : 1,
                                       call->calling_number_plan ? call->calling_number_plan : 3,
                                       0, 3);

    /* 9. Calling Party Subaddress (0x6D) */
    if (call->calling_subaddr_len > 0) {
        offset += q933_build_subaddress(&buf[offset], max_len - offset, Q933_IE_CALLING_SUBADDR, call->calling_subaddr, call->calling_subaddr_len);
    }

    /* 10. Called Party Number (0x70) - Mandatory on NNI SETUP */
    offset += q933_build_called_number(&buf[offset], max_len - offset,
                                      call->called_number,
                                      call->called_number_type ? call->called_number_type : 1,
                                      call->called_number_plan ? call->called_number_plan : 3);

    /* 11. Called Party Subaddress (0x71) */
    if (call->called_subaddr_len > 0) {
        offset += q933_build_subaddress(&buf[offset], max_len - offset, Q933_IE_CALLED_SUBADDR, call->called_subaddr, call->called_subaddr_len);
    }

    /* 12. Transit Network Selection (0x78) if present */
    if (call->transit_network_present && call->transit_network[0] != '\0') {
        offset += q933_build_transit_network_selection(&buf[offset], max_len - offset, call->transit_network, 0x33);
    }

    /* 13. Low Layer Compatibility (0x7C) */
    if (call->llc_len > 0) {
        if (max_len >= (size_t)offset && max_len - (size_t)offset >= 2 + (size_t)call->llc_len) {
            buf[offset++] = Q933_IE_LOW_LAYER_COMPAT;
            buf[offset++] = call->llc_len;
            memcpy(&buf[offset], call->llc_data, call->llc_len);
            offset += call->llc_len;
        }
    }

    /* 14. User-User (0x7E) */
    if (call->uu_len > 0) {
        if (max_len >= (size_t)offset && max_len - (size_t)offset >= 2 + (size_t)call->uu_len) {
            buf[offset++] = Q933_IE_USER_USER;
            buf[offset++] = call->uu_len;
            memcpy(&buf[offset], call->uu_data, call->uu_len);
            offset += call->uu_len;
        }
    }

    return offset;
}

int q933_build_nni_call_proceeding(u8 *buf, size_t max_len, vfr_call_t *call) {
    if (!buf || !call) return -1;
    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_CALL_PROCEEDING);
    if (offset < 0) return -1;

    offset += q933_build_dlci_ie(&buf[offset], max_len - offset, call->ingress_dlci, 2);
    return offset;
}

int q933_build_nni_connect(u8 *buf, size_t max_len, vfr_call_t *call) {
    if (!buf || !call) return -1;
    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_CONNECT);
    if (offset < 0) return -1;

    /* 1. Link Layer Core Parameters (0x48) - Mandatory in NNI CONNECT */
    offset += q933_build_llcore_params(&buf[offset], max_len - offset, &call->llcore);

    /* 2. Connected Number (0x4C) */
    if (call->connected_number[0] != '\0') {
        offset += q933_build_connected_number(&buf[offset], max_len - offset,
                                            call->connected_number,
                                            call->connected_number_type ? call->connected_number_type : 1,
                                            call->connected_number_plan ? call->connected_number_plan : 3,
                                            0, 3);
    }

    /* 3. Connected Subaddress (0x4D) */
    if (call->connected_subaddr_len > 0) {
        offset += q933_build_subaddress(&buf[offset], max_len - offset, Q933_IE_CONNECTED_SUBADDR, call->connected_subaddr, call->connected_subaddr_len);
    }

    /* 4. Transit Network Identification IEs (0x67) - All reflected TNIs in forward order */
    for (int i = 0; i < call->tni_list.count && i < SVC_MAX_TRANSIT_NETWORKS; i++) {
        offset += q933_build_transit_net_id(&buf[offset], max_len - offset,
                                            call->tni_list.entries[i].net_id,
                                            call->tni_list.entries[i].type_plan);
    }

    /* 5. Low Layer Compatibility (0x7C) */
    if (call->llc_len > 0) {
        if (max_len >= (size_t)offset && max_len - (size_t)offset >= 2 + (size_t)call->llc_len) {
            buf[offset++] = Q933_IE_LOW_LAYER_COMPAT;
            buf[offset++] = call->llc_len;
            memcpy(&buf[offset], call->llc_data, call->llc_len);
            offset += call->llc_len;
        }
    }

    /* 6. User-User (0x7E) */
    if (call->uu_len > 0) {
        if (max_len >= (size_t)offset && max_len - (size_t)offset >= 2 + (size_t)call->uu_len) {
            buf[offset++] = Q933_IE_USER_USER;
            buf[offset++] = call->uu_len;
            memcpy(&buf[offset], call->uu_data, call->uu_len);
            offset += call->uu_len;
        }
    }

    return offset;
}

int q933_build_nni_release(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause, const char *clearing_net_id, u8 cni_type_plan) {
    if (!buf || !call) return -1;
    int offset = q933_build_header(buf, max_len, call->call_ref, call->call_ref_flag, call->call_ref_len, Q933_MSG_RELEASE);
    if (offset < 0) return -1;

    offset += q933_build_cause(&buf[offset], max_len - offset, Q850_LOC_TRANSIT_NET, cause);

    if (clearing_net_id && clearing_net_id[0] != '\0') {
        offset += q933_build_clearing_net_id(&buf[offset], max_len - offset, clearing_net_id, cni_type_plan ? cni_type_plan : 0x33);
    }

    return offset;
}

int q933_build_nni_release_complete(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 cause, const char *clearing_net_id, u8 cni_type_plan) {
    if (!buf) return -1;
    int offset = q933_build_header(buf, max_len, crv, crv_flag, crv_len, Q933_MSG_RELEASE_COMPLETE);
    if (offset < 0) return -1;

    if (cause != 0) {
        offset += q933_build_cause(&buf[offset], max_len - offset, Q850_LOC_TRANSIT_NET, cause);
    }

    if (clearing_net_id && clearing_net_id[0] != '\0') {
        offset += q933_build_clearing_net_id(&buf[offset], max_len - offset, clearing_net_id, cni_type_plan ? cni_type_plan : 0x33);
    }

    return offset;
}
