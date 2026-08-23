/*
 * config.c - Configuration File Parser
 * VFRS - Virtual Frame Relay Switch
 */

#include "vfr.h"
#include <ctype.h>
#include <errno.h>

struct config_s {
    FILE        *fp;
    char        *filename;
    int         line_num;
    char        line_buf[1024];
    char        *cursor;
    int         error;
};

config_t *config_create(const char *filename)
{
    config_t *cfg;

    cfg = calloc(1, sizeof(config_t));
    if (!cfg) {
        LOG_ERROR("Failed to allocate config context");
        return NULL;
    }

    cfg->filename = strdup(filename);
    cfg->fp = fopen(filename, "r");
    if (!cfg->fp) {
        LOG_ERROR("Failed to open config file '%s': %s",
                  filename, strerror(errno));
        free(cfg->filename);
        free(cfg);
        return NULL;
    }

    cfg->line_num = 0;
    cfg->error = 0;
    LOG_INFO("Opened configuration file: %s", filename);
    return cfg;
}

void config_destroy(config_t *cfg)
{
    if (!cfg) return;
    if (cfg->fp) fclose(cfg->fp);
    if (cfg->filename) free(cfg->filename);
    free(cfg);
}

int config_error_line(config_t *cfg)
{
    return cfg ? cfg->line_num : 0;
}

/* Skip whitespace */
static void skip_ws(char **p)
{
    while (**p && (**p == ' ' || **p == '\t')) (*p)++;
}

/* Trim trailing whitespace */
static void trim_end(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

/* Get next token (quoted string or word) */
int get_token(char **p, char *tok, size_t tok_len)
{
    skip_ws(p);

    if (!**p || **p == '#' || **p == '\n' || **p == '\r') {
        tok[0] = '\0';
        return 0;
    }

    /* Quoted string */
    if (**p == '"') {
        (*p)++;
        char *start = *p;
        while (**p && **p != '"' && **p != '\n' && **p != '\r') (*p)++;
        size_t len = *p - start;
        if (len > tok_len - 1) len = tok_len - 1;
        memcpy(tok, start, len);
        tok[len] = '\0';
        if (**p == '"') (*p)++;
        return 1;
    }

    /* Unquoted word */
    char *start = *p;
    while (**p && !isspace(**p) && **p != '#' && **p != '\n' && **p != '\r') {
        (*p)++;
    }
    size_t len = *p - start;
    if (len > tok_len - 1) len = tok_len - 1;
    memcpy(tok, start, len);
    tok[len] = '\0';
    return tok[0] != '\0';
}

/* Parse key=value pairs */
int parse_kv(char *tok, char *key, size_t klen, char *val, size_t vlen)
{
    char *eq = strchr(tok, '=');
    if (!eq) return 0;

    size_t ksz = eq - tok;
    if (ksz > klen - 1) ksz = klen - 1;
    memcpy(key, tok, ksz);
    key[ksz] = '\0';

    eq++;
    size_t vsz = strlen(eq);
    if (vsz > vlen - 1) vsz = vlen - 1;
    memcpy(val, eq, vsz);
    val[vsz] = '\0';

    return 1;
}

cfg_result_t config_next_line(config_t *cfg, char *line, size_t maxlen)
{
    if (!cfg || !cfg->fp) return CFG_FILE_ERR;

    char *out = line;
    size_t rem = maxlen - 1;
    int continuation = 0;

    while (1) {
        if (!fgets(cfg->line_buf, sizeof(cfg->line_buf), cfg->fp)) {
            if (feof(cfg->fp)) {
                if (out != line) {
                    *out = '\0';
                    return CFG_OK;
                }
                return CFG_EOF;
            }
            return CFG_FILE_ERR;
        }

        cfg->line_num++;
        trim_end(cfg->line_buf);

        /* Skip empty lines and comments (only if not in a continuation) */
        if (!continuation && (cfg->line_buf[0] == '\0' || cfg->line_buf[0] == '#')) {
            continue;
        }

        char *p = cfg->line_buf;
        if (continuation) {
            /* Trim leading spaces on continuation lines */
            while (*p == ' ' || *p == '\t') p++;
        }

        continuation = 0;

        while (*p && rem > 0) {
            if (*p == '\\') {
                if (*(p + 1) == '\\') {
                    p += 2;
                    *out++ = '\\';
                    rem--;
                } else if (*(p + 1) == '\0') {
                    continuation = 1;
                    break;
                } else {
                    *out++ = *p++;
                    rem--;
                }
                continue;
            }
            *out++ = *p++;
            rem--;
        }

        if (continuation) {
            continue;
        }

        *out = '\0';
        return CFG_OK;
    }
}

/* Higher-level parsing helpers */

/* Parse port command: port <name> <transport> [args...] */
typedef int (*config_handler_t)(config_t *cfg, const char *cmd,
                                char tokens[][256], int tok_count, void *user);

typedef struct {
    const char *cmd;
    config_handler_t handler;
    void *user;
} cmd_handler_t;

/* Common configuration token parsing */
int config_parse_port(config_t *cfg, const char *cmd,
                      char tokens[][256], int tok_count,
                      char *name, size_t name_len,
                      char *transport, size_t trans_len,
                      char *args, size_t args_len)
{
    (void)cmd;  /* unused */
    if (tok_count < 3) {
        LOG_ERROR("Line %d: port command requires name and transport", cfg->line_num);
        return -1;
    }

    /* tokens[0] = "port", tokens[1] = port name, tokens[2] = transport */
    strncpy(name, tokens[1], name_len - 1);
    name[name_len - 1] = '\0';
    strncpy(transport, tokens[2], trans_len - 1);
    transport[trans_len - 1] = '\0';

    /* Collect remaining args (from index 3 onwards) */
    if (args && args_len > 0) {
        args[0] = '\0';
        for (int i = 3; i < tok_count; i++) {
            char *a = args + strlen(args);
            size_t rem = args_len - (a - args) - 1;
            snprintf(a, rem, "%s%s", i > 3 ? " " : "", tokens[i]);
        }
    }

    return 0;
}

/* Parse PVC: pvc <port1> <dlci1> <port2> <dlci2> [key=value...] */
int config_parse_pvc(config_t *cfg, const char *cmd,
                     char tokens[][256], int tok_count,
                     char *p1, size_t p1_len, u32 *d1,
                     char *p2, size_t p2_len, u32 *d2,
                     u32 *cir, u32 *bc, u32 *be,
                     u8 *ftp, u8 *fdp, u8 *srvcls)
{
    (void)cmd;  /* unused */
    if (tok_count < 5) {
        LOG_ERROR("Line %d: pvc requires port1 dlci1 port2 dlci2", cfg->line_num);
        return -1;
    }

    /* tokens[0] = "pvc", tokens[1] = port1, tokens[2] = dlci1, tokens[3] = port2, tokens[4] = dlci2 */
    strncpy(p1, tokens[1], p1_len - 1);
    p1[p1_len - 1] = '\0';
    *d1 = atoi(tokens[2]);
    strncpy(p2, tokens[3], p2_len - 1);
    p2[p2_len - 1] = '\0';
    *d2 = atoi(tokens[4]);

    /* Reset optional parameters */
    *cir = 0;
    *bc = 0;
    *be = 0;
    *ftp = 0;
    *fdp = 0;
    *srvcls = 0;

    /* Parse optional key=value pairs */
    for (int i = 5; i < tok_count; i++) {
        char key[64], val[64];
        if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
            if (strcmp(key, "cir") == 0) *cir = atoi(val);
            else if (strcmp(key, "bc") == 0) *bc = atoi(val);
            else if (strcmp(key, "be") == 0) *be = atoi(val);
            else if (strcmp(key, "ftp") == 0) *ftp = atoi(val);
            else if (strcmp(key, "fdp") == 0) *fdp = atoi(val);
            else if (strcmp(key, "srvcls") == 0) *srvcls = atoi(val);
            else {
                LOG_WARN("Line %d: Unknown PVC parameter '%s'", cfg->line_num, key);
            }
        }
    }

    return 0;
}

/* Parse LMI: lmi <port> [type] [key=value...] */
int config_parse_lmi(config_t *cfg, const char *cmd,
                    char tokens[][256], int tok_count,
                    char *port, size_t port_len,
                    int *lmi_type,
                    u8 *n392, u8 *n393, u16 *t392)
{
    (void)cmd;  /* unused */
    if (tok_count < 2) {
        LOG_ERROR("Line %d: lmi requires port name", cfg->line_num);
        return -1;
    }

    /* tokens[0] = "lmi", tokens[1] = port name */
    strncpy(port, tokens[1], port_len - 1);
    port[port_len - 1] = '\0';

    *lmi_type = LMI_TYPE_Q933A;  /* Default */

    for (int i = 2; i < tok_count; i++) {
        if (strcmp(tokens[i], "ansi") == 0) {
            *lmi_type = LMI_TYPE_ANSI;
        } else if (strcmp(tokens[i], "q933a") == 0) {
            *lmi_type = LMI_TYPE_Q933A;
        } else if (strcmp(tokens[i], "cisco") == 0) {
            *lmi_type = LMI_TYPE_CISCO;
        } else if (strcmp(tokens[i], "none") == 0) {
            *lmi_type = LMI_TYPE_NONE;
        } else {
            char key[64], val[64];
            if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
                if (strcmp(key, "n392") == 0) *n392 = atoi(val);
                else if (strcmp(key, "n393") == 0) *n393 = atoi(val);
                else if (strcmp(key, "t392") == 0) *t392 = atoi(val);
            }
        }
    }

    return 0;
}

/* Parse LMI DTE: lmi_dte <port> [key=value...] */
int config_parse_lmi_dte(config_t *cfg, const char *cmd,
                         char tokens[][256], int tok_count,
                         char *port, size_t port_len,
                         u8 *n391, u8 *n392, u8 *n393,
                         u16 *t391)
{
    (void)cmd;  /* unused */
    if (tok_count < 2) {
        LOG_ERROR("Line %d: lmi_dte requires port name", cfg->line_num);
        return -1;
    }

    /* tokens[0] = "lmi_dte", tokens[1] = port name */
    strncpy(port, tokens[1], port_len - 1);
    port[port_len - 1] = '\0';

    for (int i = 2; i < tok_count; i++) {
        char key[64], val[64];
        if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
            if (strcmp(key, "n391") == 0) *n391 = atoi(val);
            else if (strcmp(key, "n392") == 0) *n392 = atoi(val);
            else if (strcmp(key, "n393") == 0) *n393 = atoi(val);
            else if (strcmp(key, "t391") == 0) *t391 = atoi(val);
        }
    }

    return 0;
}

/* Parse capture: capture <port|all> <filename> */
int config_parse_capture(config_t *cfg, const char *cmd,
                         char tokens[][256], int tok_count,
                         char *port, size_t port_len,
                         char *filename, size_t filename_len)
{
    (void)cmd;  /* unused */
    if (tok_count < 3) {
        LOG_ERROR("Line %d: capture requires port and filename", cfg->line_num);
        return -1;
    }

    /* tokens[0] = "capture", tokens[1] = port, tokens[2] = filename */
    strncpy(port, tokens[1], port_len - 1);
    port[port_len - 1] = '\0';

    strncpy(filename, tokens[2], filename_len - 1);
    filename[filename_len - 1] = '\0';

    return 0;
}

/* Parse multicast group: mcast <group_name> <source_port> <source_dlci> [oneway|twoway|nway] [cir=X] [bc=Y] [be=Z] */
int config_parse_mcast(config_t *cfg, const char *cmd,
                      char tokens[][256], int tok_count,
                      char *name, size_t name_len,
                      char *src_port, size_t src_port_len,
                      u32 *src_dlci,
                      char *mode, size_t mode_len,
                      u32 *cir, u32 *bc, u32 *be)
{
    (void)cmd;  /* unused */
    if (tok_count < 4) {
        LOG_ERROR("Line %d: mcast requires group_name, source_port, source_dlci", cfg->line_num);
        return -1;
    }

    strncpy(name, tokens[1], name_len - 1);
    name[name_len - 1] = '\0';

    strncpy(src_port, tokens[2], src_port_len - 1);
    src_port[src_port_len - 1] = '\0';

    *src_dlci = atoi(tokens[3]);
    *cir = 0;
    *bc = 0;
    *be = 0;

    int kv_start_idx = 5;
    if (tok_count >= 5) {
        if (strchr(tokens[4], '=') != NULL) {
            strncpy(mode, "oneway", mode_len - 1);
            mode[mode_len - 1] = '\0';
            kv_start_idx = 4;
        } else {
            strncpy(mode, tokens[4], mode_len - 1);
            mode[mode_len - 1] = '\0';
            kv_start_idx = 5;
        }
    } else {
        strncpy(mode, "oneway", mode_len - 1);
        mode[mode_len - 1] = '\0';
    }

    for (int i = kv_start_idx; i < tok_count; i++) {
        char key[64], val[64];
        if (parse_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
            if (strcmp(key, "cir") == 0) *cir = atoi(val);
            else if (strcmp(key, "bc") == 0) *bc = atoi(val);
            else if (strcmp(key, "be") == 0) *be = atoi(val);
            else {
                LOG_WARN("Line %d: Unknown multicast group parameter '%s'", cfg->line_num, key);
            }
        }
    }

    return 0;
}

/* Parse multicast member: mcast_member <group_name> <member_port> <member_dlci> */
int config_parse_mcast_member(config_t *cfg, const char *cmd,
                             char tokens[][256], int tok_count,
                             char *group_name, size_t group_name_len,
                             char *member_port, size_t member_port_len,
                             u32 *member_dlci)
{
    (void)cmd;  /* unused */
    if (tok_count < 4) {
        LOG_ERROR("Line %d: mcast_member requires group_name, member_port, member_dlci", cfg->line_num);
        return -1;
    }

    strncpy(group_name, tokens[1], group_name_len - 1);
    group_name[group_name_len - 1] = '\0';

    strncpy(member_port, tokens[2], member_port_len - 1);
    member_port[member_port_len - 1] = '\0';

    *member_dlci = atoi(tokens[3]);

    return 0;
}