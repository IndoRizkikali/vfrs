/*
 * cfg_lexer.h - VFRS Configuration Stream Lexer
 * Virtual Frame Relay Switch
 */

#ifndef VFR_CFG_LEXER_H
#define VFR_CFG_LEXER_H

#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_WORD,
    TOK_STRING,
    TOK_KEYVAL,
    TOK_ERROR
} cfg_tok_type_t;

typedef struct {
    cfg_tok_type_t type;
    char           text[512];
    char           key[128];
    char           val[384];
    int            line;
    int            col;
} cfg_token_t;

typedef struct cfg_lexer_s {
    const char *filename;
    char       *source_buffer;
    size_t      source_len;
    size_t      cursor;
    int         line;
    int         col;
    int         has_error;
    char        err_msg[256];
} cfg_lexer_t;

VFR_API cfg_lexer_t *cfg_lexer_create_file(const char *filename);
VFR_API cfg_lexer_t *cfg_lexer_create_string(const char *source, const char *sourcename);
VFR_API void         cfg_lexer_destroy(cfg_lexer_t *lex);
VFR_API int          cfg_lexer_next_token(cfg_lexer_t *lex, cfg_token_t *tok);
VFR_API const char  *cfg_lexer_error(cfg_lexer_t *lex);

#ifdef __cplusplus
}
#endif

#endif /* VFR_CFG_LEXER_H */
