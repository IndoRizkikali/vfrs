/*
 * config.h - VFRS Configuration Engine API
 * Virtual Frame Relay Switch
 */

#ifndef VFR_CONFIG_H
#define VFR_CONFIG_H

#include "types.h"
#include "vfr/cfg_lexer.h"
#include "vfr/cfg_ast.h"
#include "vfr/cfg_schema.h"

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

typedef struct config_s {
    cfg_lexer_t *lexer;
    int          error_line;
} config_t;

/* Core Compilation API */
struct vfrs_ctx_s;
VFR_API int cfg_compile_file(struct vfrs_ctx_s *ctx, const char *filename);
VFR_API int cfg_compile_string(struct vfrs_ctx_s *ctx, const char *source, const char *sourcename);
VFR_API int cfg_validate_file(const char *filename, char *err_msg, size_t err_len);

/* Lexer / Stream Helper Functions */
VFR_API config_t    *config_create(const char *filename);
VFR_API void         config_destroy(config_t *cfg);
VFR_API cfg_result_t config_next_line(config_t *cfg, char *line, size_t maxlen);
VFR_API int          config_error_line(config_t *cfg);
VFR_API int          get_token(char **p, char *tok, size_t tok_len);
VFR_API int          parse_kv(char *tok, char *key, size_t klen, char *val, size_t vlen);

#ifdef __cplusplus
}
#endif

#endif /* VFR_CONFIG_H */
