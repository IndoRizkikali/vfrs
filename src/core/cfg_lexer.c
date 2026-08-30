/*
 * cfg_lexer.c - VFRS Configuration Stream Lexer
 * Virtual Frame Relay Switch
 */

#include "vfr/cfg_lexer.h"
#include <ctype.h>
#include <errno.h>

cfg_lexer_t *cfg_lexer_create_file(const char *filename)
{
    if (!filename) return NULL;

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0) {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc((size_t)sz + 2);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);

    buf[read_bytes] = '\0';

    cfg_lexer_t *lex = calloc(1, sizeof(cfg_lexer_t));
    if (!lex) {
        free(buf);
        return NULL;
    }

    lex->filename = filename;
    lex->source_buffer = buf;
    lex->source_len = read_bytes;
    lex->cursor = 0;
    lex->line = 1;
    lex->col = 1;

    return lex;
}

cfg_lexer_t *cfg_lexer_create_string(const char *source, const char *sourcename)
{
    if (!source) return NULL;

    size_t sz = strlen(source);
    char *buf = malloc(sz + 2);
    if (!buf) return NULL;
    memcpy(buf, source, sz);
    buf[sz] = '\0';

    cfg_lexer_t *lex = calloc(1, sizeof(cfg_lexer_t));
    if (!lex) {
        free(buf);
        return NULL;
    }

    lex->filename = sourcename ? sourcename : "<string>";
    lex->source_buffer = buf;
    lex->source_len = sz;
    lex->cursor = 0;
    lex->line = 1;
    lex->col = 1;

    return lex;
}

void cfg_lexer_destroy(cfg_lexer_t *lex)
{
    if (!lex) return;
    if (lex->source_buffer) free(lex->source_buffer);
    free(lex);
}

const char *cfg_lexer_error(cfg_lexer_t *lex)
{
    return lex ? lex->err_msg : "Invalid lexer context";
}

static char peek_char(cfg_lexer_t *lex)
{
    if (lex->cursor >= lex->source_len) return '\0';
    return lex->source_buffer[lex->cursor];
}

static char next_char(cfg_lexer_t *lex)
{
    if (lex->cursor >= lex->source_len) return '\0';
    char c = lex->source_buffer[lex->cursor++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

int cfg_lexer_next_token(cfg_lexer_t *lex, cfg_token_t *tok)
{
    if (!lex || !tok) return 0;
    memset(tok, 0, sizeof(*tok));

    while (lex->cursor < lex->source_len) {
        char c = peek_char(lex);

        /* Skip horizontal whitespace */
        if (c == ' ' || c == '\t') {
            next_char(lex);
            continue;
        }

        /* Line continuation with backslash '\' */
        if (c == '\\') {
            size_t saved_cur = lex->cursor;
            int saved_line = lex->line;
            int saved_col = lex->col;

            next_char(lex); /* consume '\\' */
            /* Skip any trailing whitespace on current line */
            while (peek_char(lex) == ' ' || peek_char(lex) == '\t') {
                next_char(lex);
            }
            if (peek_char(lex) == '\r') next_char(lex);
            if (peek_char(lex) == '\n') {
                next_char(lex); /* consume '\n' */
                /* Continue reading tokens on next line seamlessly */
                continue;
            } else {
                /* Not an end-of-line continuation, restore and treat as character */
                lex->cursor = saved_cur;
                lex->line = saved_line;
                lex->col = saved_col;
            }
        }

        /* Comment line / inline comment */
        if (c == '#') {
            while (lex->cursor < lex->source_len && peek_char(lex) != '\n' && peek_char(lex) != '\0') {
                next_char(lex);
            }
            continue;
        }

        /* Newline / Statement terminator */
        if (c == '\r' || c == '\n') {
            tok->line = lex->line;
            tok->col = lex->col;
            tok->type = TOK_NEWLINE;
            strcpy(tok->text, "\n");
            if (c == '\r') next_char(lex);
            if (peek_char(lex) == '\n') next_char(lex);
            return 1;
        }

        /* Quoted string "..." */
        if (c == '"') {
            tok->line = lex->line;
            tok->col = lex->col;
            tok->type = TOK_STRING;
            next_char(lex); /* consume opening quote */

            size_t out_idx = 0;
            while (lex->cursor < lex->source_len) {
                char sc = next_char(lex);
                if (sc == '"') {
                    break; /* End of quoted string */
                }
                if (sc == '\\') {
                    char esc = next_char(lex);
                    if (esc == 'n') sc = '\n';
                    else if (esc == 't') sc = '\t';
                    else if (esc == 'r') sc = '\r';
                    else if (esc == '\\') sc = '\\';
                    else if (esc == '"') sc = '"';
                    else sc = esc;
                }
                if (sc == '\n' || sc == '\0') {
                    tok->type = TOK_ERROR;
                    snprintf(lex->err_msg, sizeof(lex->err_msg),
                             "Unterminated quoted string at line %d, col %d", tok->line, tok->col);
                    return 0;
                }
                if (out_idx < sizeof(tok->text) - 1) {
                    tok->text[out_idx++] = sc;
                }
            }
            tok->text[out_idx] = '\0';
            return 1;
        }

        /* Unquoted Word or Key=Value pair */
        tok->line = lex->line;
        tok->col = lex->col;

        size_t out_idx = 0;
        int has_equals = 0;
        size_t eq_pos = 0;

        while (lex->cursor < lex->source_len) {
            char wc = peek_char(lex);
            if (isspace((unsigned char)wc) || wc == '#' || wc == '\0') {
                break;
            }
            if (wc == '=' && !has_equals) {
                has_equals = 1;
                eq_pos = out_idx;
            }
            next_char(lex);
            if (out_idx < sizeof(tok->text) - 1) {
                tok->text[out_idx++] = wc;
            }
        }
        tok->text[out_idx] = '\0';

        if (has_equals && eq_pos > 0) {
            tok->type = TOK_KEYVAL;
            size_t klen = eq_pos;
            if (klen >= sizeof(tok->key)) klen = sizeof(tok->key) - 1;
            memcpy(tok->key, tok->text, klen);
            tok->key[klen] = '\0';

            const char *val_src = tok->text + eq_pos + 1;
            strncpy(tok->val, val_src, sizeof(tok->val) - 1);
            tok->val[sizeof(tok->val) - 1] = '\0';
        } else {
            tok->type = TOK_WORD;
        }

        return 1;
    }

    tok->type = TOK_EOF;
    tok->line = lex->line;
    tok->col = lex->col;
    strcpy(tok->text, "<EOF>");
    return 1;
}
