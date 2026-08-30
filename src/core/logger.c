/*
 * logger.c - Logging System
 * VFRS - Virtual Frame Relay Switch
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vfr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
/* Note: pthread.h is NOT included directly; mutex_t is provided by vfr.h. */

/* Log levels */
enum log_level g_log_console = LOG_INFO;
enum log_level g_log_file = LOG_DEBUG;

#ifndef NDEBUG
#  ifdef _MSC_VER
     __declspec(thread) uint32_t vfr_tls_lock_bitmap = 0;
#  else
     __thread uint32_t vfr_tls_lock_bitmap = 0;
#  endif
#endif

static FILE    *g_log_fp       = NULL;
static mutex_t  g_log_mutex;
static int      g_logger_init  = 0;

/* Log rotation state (F4 / D14) */
static size_t   g_log_max_bytes = 0;     /* 0 = disabled */
static int      g_log_max_files = 3;     /* number of rotated files to keep */
static char     g_log_file_base[VFR_MAX_PATH] = {0};  /* base filename, e.g. "vfrs.log" */

static const char *level_str[] = {
    [LOG_TRACE] = "TRACE",
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO]  = "INFO ",
    [LOG_WARN]  = "WARN ",
    [LOG_ERROR] = "ERROR"
};

#ifndef _WIN32
static const char *level_color[] = {
    [LOG_TRACE] = "\x1b[90m",  /* Dark Gray */
    [LOG_DEBUG] = "\x1b[36m",  /* Cyan */
    [LOG_INFO]  = "",
    [LOG_WARN]  = "\x1b[33m",  /* Yellow */
    [LOG_ERROR] = "\x1b[31m"  /* Red */
};
#endif

#ifndef _WIN32
#define RESET_COLOR  "\x1b[0m"
#else
#define RESET_COLOR  ""
#endif

int logger_init(const char *log_file, enum log_level con_level, enum log_level file_level)
{
    if (g_logger_init) {
        return 0;
    }

    mutex_init(&g_log_mutex);
    g_log_console = con_level;
    g_log_file = file_level;

    if (log_file && *log_file) {
        g_log_fp = fopen(log_file, "a");
        if (!g_log_fp) {
            fprintf(stderr, "Failed to open log file '%s': %s\n",
                    log_file, strerror(errno));
            return -1;
        }
        /* Remember base filename for rotation */
        strncpy(g_log_file_base, log_file, sizeof(g_log_file_base) - 1);
    }

    g_logger_init = 1;
    LOG_INFO("Logger initialized (console=%s, file=%s)",
             level_str[con_level], log_file ? log_file : "none");
    return 0;
}

void logger_shutdown(void)
{
    if (!g_logger_init) return;

    if (g_log_fp) {
        LOG_INFO("Logger shutdown");
        fclose(g_log_fp);
        g_log_fp = NULL;
    }

    mutex_destroy(&g_log_mutex);
    g_logger_init = 0;
}

int logger_reopen(const char *new_path)
{
    if (!g_logger_init) return -1;

    mutex_lock(&g_log_mutex);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    if (new_path && *new_path) {
        g_log_fp = fopen(new_path, "a");
        if (!g_log_fp) {
            mutex_unlock(&g_log_mutex);
            return -1;
        }
        strncpy(g_log_file_base, new_path, sizeof(g_log_file_base) - 1);
    }
    mutex_unlock(&g_log_mutex);
    return 0;
}

/* ============================================================
 * Log Rotation  (F4 / D14)
 *
 * Rotates log files in the pattern:
 *   <base>.log  (current)
 *   <base>.1.log
 *   <base>.2.log
 *   ...
 *   <base>.<max_files>.log  (oldest)
 *
 * Called with g_log_mutex already held.
 * ============================================================ */

void logger_set_rotation(size_t max_bytes, int max_files)
{
    /* Can be called before or after logger_init; takes effect immediately. */
    mutex_lock(&g_log_mutex);
    g_log_max_bytes = max_bytes;
    g_log_max_files = (max_files > 0) ? max_files : 1;
    mutex_unlock(&g_log_mutex);
}

/* Internal: rotate log files.  Must be called with g_log_mutex held. */
static void logger_rotate(void)
{
    if (!g_log_file_base[0]) return;

    /* Close current file */
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }

    /* Shift existing rotated files: .N.log -> .(N+1).log, drop oldest */
    char old_path[VFR_MAX_PATH + 32];
    char new_path[VFR_MAX_PATH + 32];

    /* Strip trailing ".log" from base path to get the stem, then
     * build the rotated filenames as  <stem>.<N>.log. */
    char stem[VFR_MAX_PATH];
    strncpy(stem, g_log_file_base, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    size_t stem_len = strlen(stem);
    if (stem_len >= 4 && strcmp(stem + stem_len - 4, ".log") == 0) {
        stem[stem_len - 4] = '\0';
    }

    /* Delete the oldest file if it exists */
    snprintf(old_path, sizeof(old_path), "%s.%d.log", stem, g_log_max_files);
    remove(old_path);

    /* Shift: <stem>.(N-1).log -> <stem>.N.log */
    for (int i = g_log_max_files - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d.log", stem, i);
        snprintf(new_path, sizeof(new_path), "%s.%d.log", stem, i + 1);
        rename(old_path, new_path);
    }

    /* Rename current log to <stem>.1.log */
    snprintf(new_path, sizeof(new_path), "%s.1.log", stem);
    rename(g_log_file_base, new_path);

    /* Reopen fresh log at the original base path */
    g_log_fp = fopen(g_log_file_base, "w");
    if (!g_log_fp) {
        fprintf(stderr, "[logger] Failed to reopen log after rotation: %s\n",
                g_log_file_base);
    }
}

void vfr_log(enum log_level level, const char *file, int line,
             const char *fmt, ...)
{
    va_list args;
    time_t now;
    struct tm *tm_info;
    char time_buf[32];
    /*
     * msg_buf is capped at 256 bytes (D16).  vsnprintf writes at most
     * 255 chars + NUL.  If the formatted message exceeds this, the last
     * three characters are replaced with "..." to indicate truncation.
     */
    char msg_buf[256];

    if (level < g_log_console && (!g_log_fp || level < g_log_file)) {
        return;
    }

    if (!g_logger_init) {
        /* Early logging before init - output to stderr */
        fprintf(stderr, "[%s] %s:%d: ", level_str[level], file, line);
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
        return;
    }

    /* Format message into the fixed 256-byte buffer */
    va_start(args, fmt);
    int written = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* Mark truncated messages (written >= sizeof buffer means overflow) */
    if (written >= (int)sizeof(msg_buf)) {
        msg_buf[sizeof(msg_buf) - 4] = '.';
        msg_buf[sizeof(msg_buf) - 3] = '.';
        msg_buf[sizeof(msg_buf) - 2] = '.';
        msg_buf[sizeof(msg_buf) - 1] = '\0';
    }

    mutex_lock(&g_log_mutex);

    /* Get timestamp */
    now = time(NULL);
    tm_info = localtime(&now);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /* Console output */
    if (level >= g_log_console) {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            WORD attr = FOREGROUND_RED;
            if (level == LOG_WARN) attr = FOREGROUND_RED | FOREGROUND_GREEN;
            if (level == LOG_INFO) attr = FOREGROUND_GREEN;
            if (level == LOG_DEBUG) attr = FOREGROUND_BLUE | FOREGROUND_GREEN;
            if (level == LOG_TRACE) attr = FOREGROUND_INTENSITY;
            SetConsoleTextAttribute(h, attr);
        }
#endif
#ifndef _WIN32
        fprintf(stderr, "%s[%s] %s:%d: %s%s\n",
                level_color[level], time_buf, file, line, msg_buf, RESET_COLOR);
#else
        fprintf(stderr, "[%s] %s:%d: %s\n",
                time_buf, file, line, msg_buf);
#endif
#ifdef _WIN32
        if (h && h != INVALID_HANDLE_VALUE) {
            SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        }
#endif
    }

    /* File output */
    if (g_log_fp && level >= g_log_file) {
        fprintf(g_log_fp, "[%s] %s:%d: %s\n",
                time_buf, file, line, msg_buf);
        fflush(g_log_fp);

        /* Log rotation check (F4 / D14) */
        if (g_log_max_bytes > 0) {
            long pos = ftell(g_log_fp);
            if (pos > 0 && (size_t)pos >= g_log_max_bytes) {
                logger_rotate();  /* mutex already held */
            }
        }
    }

    mutex_unlock(&g_log_mutex);
}