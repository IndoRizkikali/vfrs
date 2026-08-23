/*
 * config.h - VFRS Configuration Parser API
 * Virtual Frame Relay Switch
 */

#ifndef VFR_CONFIG_H
#define VFR_CONFIG_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_OK = 0,
    CFG_EOF,
    CFG_SYNTAX_ERR,
    CFG_NO_MEM,
    CFG_FILE_ERR
} cfg_result_t;

typedef struct config_s config_t;

VFR_API config_t *config_create(const char *filename);
VFR_API void config_destroy(config_t *cfg);
VFR_API cfg_result_t config_next_line(config_t *cfg, char *line, size_t maxlen);
VFR_API int config_error_line(config_t *cfg);

/* Parse a line into tokens, returns token count */
VFR_API int get_token(char **p, char *tok, size_t tok_len);

/* Parse key=value pair */
VFR_API int parse_kv(char *tok, char *key, size_t klen, char *val, size_t vlen);

/* Parse port command */
VFR_API int config_parse_port(config_t *cfg, const char *cmd,
                             char tokens[][256], int tok_count,
                             char *name, size_t name_len,
                             char *transport, size_t trans_len,
                             char *args, size_t args_len);

/* Parse PVC command */
VFR_API int config_parse_pvc(config_t *cfg, const char *cmd,
                            char tokens[][256], int tok_count,
                            char *p1, size_t p1_len, u32 *d1,
                            char *p2, size_t p2_len, u32 *d2,
                            u32 *cir, u32 *bc, u32 *be,
                            u8 *ftp, u8 *fdp, u8 *srvcls);

/* Parse LMI command */
VFR_API int config_parse_lmi(config_t *cfg, const char *cmd,
                            char tokens[][256], int tok_count,
                            char *port, size_t port_len,
                            int *lmi_type,
                            u8 *n392, u8 *n393, u16 *t392);

/* Parse LMI DTE command */
VFR_API int config_parse_lmi_dte(config_t *cfg, const char *cmd,
                                char tokens[][256], int tok_count,
                                char *port, size_t port_len,
                                u8 *n391, u8 *n392, u8 *n393,
                                u16 *t391);

/* Parse capture command */
VFR_API int config_parse_capture(config_t *cfg, const char *cmd,
                                char tokens[][256], int tok_count,
                                char *port, size_t port_len,
                                char *filename, size_t filename_len);

/* Parse multicast commands */
VFR_API int config_parse_mcast(config_t *cfg, const char *cmd,
                              char tokens[][256], int tok_count,
                              char *name, size_t name_len,
                              char *src_port, size_t src_port_len,
                              u32 *src_dlci,
                              char *mode, size_t mode_len,
                              u32 *cir, u32 *bc, u32 *be);

VFR_API int config_parse_mcast_member(config_t *cfg, const char *cmd,
                                     char tokens[][256], int tok_count,
                                     char *group_name, size_t group_name_len,
                                     char *member_port, size_t member_port_len,
                                     u32 *member_dlci);

#ifdef __cplusplus
}
#endif

#endif /* VFR_CONFIG_H */
