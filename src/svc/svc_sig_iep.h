/*
 * svc_sig_iep.h - Q.933 Information Element Parser & Message Builder
 * Virtual Frame Relay Switch - ITU-T Q.933 / X.36 Clause 10
 */

#ifndef SVC_SIG_IEP_H
#define SVC_SIG_IEP_H

#include "svc_sig_common.h"
#include "svc_sig_iel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Header parser & builder */
int q933_parse_header(const u8 *data, size_t len, q933_msg_header_t *hdr);
int q933_build_header(u8 *buf, size_t max_len, u16 call_ref, u8 call_ref_flag, u8 call_ref_len, u8 msg_type);

/* IE Parsers */
int q933_parse_bearer_capability(const u8 *ie_data, size_t ie_len);
int q933_parse_cause(const u8 *ie_data, size_t ie_len, u8 *location, u8 *cause_value);
int q933_parse_cause_full(const u8 *ie_data, size_t ie_len, u8 *location, u8 *cause_value, u8 *diag_buf, size_t max_diag, u8 *diag_len);
int q933_parse_call_state(const u8 *ie_data, size_t ie_len, u8 *state);
int q933_parse_dlci_ie(const u8 *ie_data, size_t ie_len, u32 *dlci, u8 *dlci_len);
int q933_parse_llcore_params(const u8 *ie_data, size_t ie_len, q933_llcore_params_t *params);
int q933_parse_number_ie(const u8 *ie_data, size_t ie_len, char *num_buf, size_t max_len,
                        u8 *type, u8 *plan, u8 *presentation, u8 *screening);
int q933_parse_reverse_charge_ind(const u8 *ie_data, size_t ie_len, u8 *rev_charge);
int q933_parse_priority_params(const u8 *ie_data, size_t ie_len, u8 *ftp_out, u8 *ftp_in, u8 *fdp_out, u8 *fdp_in, u8 *srv_class);
int q933_parse_call_ident(const u8 *ie_data, size_t ie_len, u32 *call_ident);
int q933_parse_transit_net_id(const u8 *ie_data, size_t ie_len, char *net_id, size_t max_len, u8 *type_plan);
int q933_parse_clearing_net_id(const u8 *ie_data, size_t ie_len, char *net_id, size_t max_len, u8 *type_plan);
int q933_parse_transit_network_selection(const u8 *ie_data, size_t ie_len, char *net_id, size_t max_len, u8 *type_plan);
int q933_parse_gat(const u8 *ie_data, size_t ie_len, u8 *gat_buf, size_t max_len, size_t *out_len);
int q933_parse_subaddress(const u8 *ie_data, size_t ie_len, u8 *subaddr_buf, size_t max_buf, u8 *out_len);

/* ITU-T X.76 Annex A SPVC IEs */
typedef struct {
    u8  selection_type; /* 1=Any DLCI, 2=Specific DLCI, 3=Assigned DLCI, 4=SPVC Correlator */
    u32 dlci;
    u8  dlci_len;
} q933_spvc_ie_t;

int q933_parse_called_spvc_ie(const u8 *ie_data, size_t ie_len, q933_spvc_ie_t *spvc_ie);
int q933_parse_calling_spvc_ie(const u8 *ie_data, size_t ie_len, u32 *calling_dlci, u8 *dlci_len);

/* IE Builders */
int q933_build_called_spvc_ie(u8 *buf, size_t max_len, u8 selection_type, u32 dlci, u8 dlci_len);
int q933_build_calling_spvc_ie(u8 *buf, size_t max_len, u32 calling_dlci, u8 dlci_len);
int q933_build_bearer_capability(u8 *buf, size_t max_len);
int q933_build_cause(u8 *buf, size_t max_len, u8 location, u8 cause_value);
int q933_build_cause_ex(u8 *buf, size_t max_len, u8 location, u8 cause_value, u8 diag_byte, int has_diag);
int q933_build_call_state(u8 *buf, size_t max_len, u8 state);
int q933_build_dlci_ie(u8 *buf, size_t max_len, u32 dlci, u8 dlci_len);
int q933_build_llcore_params(u8 *buf, size_t max_len, const q933_llcore_params_t *params);
int q933_build_called_number(u8 *buf, size_t max_len, const char *number, u8 type, u8 plan);
int q933_build_calling_number(u8 *buf, size_t max_len, const char *number, u8 type, u8 plan, u8 presentation, u8 screening);
int q933_build_connected_number(u8 *buf, size_t max_len, const char *number, u8 type, u8 plan, u8 presentation, u8 screening);
int q933_build_subaddress(u8 *buf, size_t max_len, u8 ie_id, const u8 *subaddr_data, u8 subaddr_len);
int q933_build_reverse_charge_ind(u8 *buf, size_t max_len, u8 rev_charge);
int q933_build_priority_params(u8 *buf, size_t max_len, u8 ftp_out, u8 ftp_in, u8 fdp_out, u8 fdp_in, u8 srv_class);
int q933_build_restart_indicator(u8 *buf, size_t max_len, u8 restart_class);
int q933_build_call_ident(u8 *buf, size_t max_len, u32 call_ident);
int q933_build_transit_net_id(u8 *buf, size_t max_len, const char *net_id, u8 type_plan);
int q933_build_clearing_net_id(u8 *buf, size_t max_len, const char *net_id, u8 type_plan);
int q933_build_transit_network_selection(u8 *buf, size_t max_len, const char *net_id, u8 type_plan);
int q933_build_gat(u8 *buf, size_t max_len, const u8 *gat_data, size_t gat_len);

/* High-level Message Builders (UNI) */
int q933_build_setup(u8 *buf, size_t max_len, vfr_call_t *call, vfr_svc_ctx_t *sctx);
int q933_build_call_proceeding(u8 *buf, size_t max_len, vfr_call_t *call);
int q933_build_connect(u8 *buf, size_t max_len, vfr_call_t *call);
int q933_build_connect_ack(u8 *buf, size_t max_len, vfr_call_t *call);
int q933_build_disconnect(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause);
int q933_build_release(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause);
int q933_build_release_complete(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 cause);
int q933_build_release_complete_ex(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 cause, u8 diag_byte, int has_diag);
int q933_build_status_raw(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 state, u8 cause);
int q933_build_status_raw_ex(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 state, u8 cause, u8 diag_byte, int has_diag);
int q933_build_status(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause);
int q933_build_status_ex(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause, u8 diag_byte, int has_diag);
int q933_build_status_enquiry(u8 *buf, size_t max_len, vfr_call_t *call);
int q933_build_restart(u8 *buf, size_t max_len, u8 restart_class);
int q933_build_restart_ack(u8 *buf, size_t max_len, u8 restart_class);

/* High-level Message Builders (NNI - ITU-T X.76 Clause 10) */
int q933_build_nni_setup(u8 *buf, size_t max_len, vfr_call_t *call, vfr_svc_ctx_t *sctx);
int q933_build_nni_call_proceeding(u8 *buf, size_t max_len, vfr_call_t *call);
int q933_build_nni_connect(u8 *buf, size_t max_len, vfr_call_t *call);
int q933_build_nni_release(u8 *buf, size_t max_len, vfr_call_t *call, u8 cause, const char *clearing_net_id, u8 cni_type_plan);
int q933_build_nni_release_complete(u8 *buf, size_t max_len, u16 crv, u8 crv_flag, u8 crv_len, u8 cause, const char *clearing_net_id, u8 cni_type_plan);

#ifdef __cplusplus
}
#endif

#endif /* SVC_SIG_IEP_H */
