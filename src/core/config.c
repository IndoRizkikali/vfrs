/*
 * config.c - VFRS Configuration Helpers & Bridging
 * Virtual Frame Relay Switch
 */

#include "vfr/config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

config_t *config_create(const char *filename)
{
    if (!filename) return NULL;
    cfg_lexer_t *lex = cfg_lexer_create_file(filename);
    if (!lex) return NULL;

    config_t *cfg = calloc(1, sizeof(config_t));
    if (!cfg) {
        cfg_lexer_destroy(lex);
        return NULL;
    }
    cfg->lexer = lex;
    return cfg;
}

void config_destroy(config_t *cfg)
{
    if (!cfg) return;
    if (cfg->lexer) cfg_lexer_destroy(cfg->lexer);
    free(cfg);
}

int config_error_line(config_t *cfg)
{
    return cfg ? cfg->error_line : 0;
}

cfg_result_t config_next_line(config_t *cfg, char *line, size_t maxlen)
{
    if (!cfg || !cfg->lexer || !line || maxlen == 0) return CFG_FILE_ERR;
    line[0] = '\0';

    cfg_token_t tok;
    size_t out_len = 0;

    while (cfg_lexer_next_token(cfg->lexer, &tok)) {
        if (tok.type == TOK_ERROR) {
            cfg->error_line = tok.line;
            return CFG_SYNTAX_ERR;
        }
        if (tok.type == TOK_EOF) {
            if (out_len > 0) return CFG_OK;
            return CFG_EOF;
        }
        if (tok.type == TOK_NEWLINE) {
            if (out_len > 0) return CFG_OK;
            continue;
        }

        /* Append token text */
        size_t tlen = strlen(tok.text);
        if (out_len > 0 && out_len + 1 < maxlen) {
            line[out_len++] = ' ';
            line[out_len] = '\0';
        }
        if (out_len + tlen < maxlen) {
            strcpy(&line[out_len], tok.text);
            out_len += tlen;
        }
    }

    return (out_len > 0) ? CFG_OK : CFG_EOF;
}

int get_token(char **p, char *tok, size_t tok_len)
{
    if (!p || !*p || !tok || tok_len == 0) return 0;
    char *s = *p;

    while (*s && (*s == ' ' || *s == '\t')) s++;
    if (*s == '\0' || *s == '#' || *s == '\r' || *s == '\n') {
        *p = s;
        return 0;
    }

    size_t idx = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && idx + 1 < tok_len) {
            tok[idx++] = *s++;
        }
        if (*s == '"') s++;
    } else {
        while (*s && !isspace((unsigned char)*s) && *s != '#' && idx + 1 < tok_len) {
            tok[idx++] = *s++;
        }
    }
    tok[idx] = '\0';
    *p = s;
    return 1;
}

int parse_kv(char *tok, char *key, size_t klen, char *val, size_t vlen)
{
    if (!tok || !key || !val || klen == 0 || vlen == 0) return -1;
    char *eq = strchr(tok, '=');
    if (!eq) return -1;

    size_t kl = (size_t)(eq - tok);
    if (kl >= klen) kl = klen - 1;
    memcpy(key, tok, kl);
    key[kl] = '\0';

    strncpy(val, eq + 1, vlen - 1);
    val[vlen - 1] = '\0';
    return 0;
}