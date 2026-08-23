/*
 * config.h - Configuration Parser
 * VFRS - Virtual Frame Relay Switch
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "vfr.h"

/* Parse a line into tokens, returns token count */
int get_token(char **p, char *tok, size_t tok_len);

/* Parse key=value pair */
int parse_kv(char *tok, char *key, size_t klen, char *val, size_t vlen);

/* Parse port command */
int config_parse_port(config_t *cfg, const char *cmd,
                     char tokens[][256], int tok_count,
                     char *name, size_t name_len,
                     char *transport, size_t trans_len,
                     char *args, size_t args_len);

/* Parse PVC command */
int config_parse_pvc(config_t *cfg, const char *cmd,
                    char tokens[][256], int tok_count,
                    char *p1, size_t p1_len, u32 *d1,
                    char *p2, size_t p2_len, u32 *d2,
                    u32 *cir, u32 *bc, u32 *be,
                    u8 *ftp, u8 *fdp, u8 *srvcls);

/* Parse LMI command */
int config_parse_lmi(config_t *cfg, const char *cmd,
                    char tokens[][256], int tok_count,
                    char *port, size_t port_len,
                    int *lmi_type,
                    u8 *n392, u8 *n393, u16 *t392);

/* Parse LMI DTE command */
int config_parse_lmi_dte(config_t *cfg, const char *cmd,
                        char tokens[][256], int tok_count,
                        char *port, size_t port_len,
                        u8 *n391, u8 *n392, u8 *n393,
                        u16 *t391);

/* Parse capture command */
int config_parse_capture(config_t *cfg, const char *cmd,
                        char tokens[][256], int tok_count,
                        char *port, size_t port_len,
                        char *filename, size_t filename_len);

/* Parse multicast commands */
int config_parse_mcast(config_t *cfg, const char *cmd,
                      char tokens[][256], int tok_count,
                      char *name, size_t name_len,
                      char *src_port, size_t src_port_len,
                      u32 *src_dlci,
                      char *mode, size_t mode_len,
                      u32 *cir, u32 *bc, u32 *be);

int config_parse_mcast_member(config_t *cfg, const char *cmd,
                             char tokens[][256], int tok_count,
                             char *group_name, size_t group_name_len,
                             char *member_port, size_t member_port_len,
                             u32 *member_dlci);

#endif /* CONFIG_H */