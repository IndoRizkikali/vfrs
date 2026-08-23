/*
 * svc_numbering.h - VFRS SVC X.121 / E.164 Numbering Register & Expansion Engine
 * Virtual Frame Relay Switch
 */

#ifndef SVC_NUMBERING_H
#define SVC_NUMBERING_H

#include "vfr.h"
#include "svc/svc_sig_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize numbering register in switch context */
int svc_numbering_init(vfrs_ctx_t *ctx);

/* Expand D/d/dnic, G/g/sgrc/sgc, E/e/sysc/sic, DGE/dge macros in number string */
const char *svc_numbering_expand(vfrs_ctx_t *ctx, const char *raw, char *out_buf, size_t out_len);

/* Register a subscriber number / alias for a given port */
int svc_numbering_add(vfrs_ctx_t *ctx, const char *port_name,
                      const char *num_type, const char *raw_number,
                      const char *alias_type, const char *raw_alias,
                      int rev_charge_acc, int rev_charge_prev);

/* Register a subscriber number with mode ('manual' or 'autoprefix') */
int svc_numbering_add_mode(vfrs_ctx_t *ctx, const char *port_name,
                           const char *mode, const char *num_type,
                           const char *raw_number, const char *alias_type,
                           const char *raw_alias, int rev_charge_acc, int rev_charge_prev);

/* Look up a local UNI port by called number string (matches primary or alias) */
vfr_port_t *svc_find_subscriber(vfrs_ctx_t *ctx, const char *called_number);

/* Check if a number suffix or full number matches the switch's all-zeros self-number */
int svc_numbering_is_all_zeros(vfrs_ctx_t *ctx, const char *number);

/* Translate an alias or raw number to the primary international subscriber number */
int svc_get_primary_number(vfrs_ctx_t *ctx, const char *number, char *primary_buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* SVC_NUMBERING_H */
