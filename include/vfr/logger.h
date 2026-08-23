/*
 * logger.h - VFRS Logging Subsystem
 * Virtual Frame Relay Switch
 */

#ifndef VFR_LOGGER_H
#define VFR_LOGGER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Logging System
 * ============================================================ */

enum log_level {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_MAX
};

extern enum log_level g_log_console;
extern enum log_level g_log_file;

/*
 * VFR_FILENAME — compile-time file basename for log output.
 * Avoids leaking build-tree absolute paths in release binaries.
 * Uses __FILE_NAME__ (GCC 12+ / Clang 9+) when available;
 * otherwise constant-folds via __builtin_strrchr (GCC/Clang optimizer);
 * falls back to raw __FILE__ for other compilers.
 */
#if defined(__FILE_NAME__)
#  define VFR_FILENAME  __FILE_NAME__
#elif defined(__GNUC__) || defined(__clang__)
#  define VFR_FILENAME \
     (__builtin_strrchr(__FILE__, '/') ? __builtin_strrchr(__FILE__, '/') + 1 : \
      (__builtin_strrchr(__FILE__, '\\') ? __builtin_strrchr(__FILE__, '\\') + 1 : __FILE__))
#else
#  define VFR_FILENAME  __FILE__
#endif

#define LOG(level, ...) vfr_log(level, VFR_FILENAME, __LINE__, __VA_ARGS__)
#define LOG_TRACE(...)  LOG(LOG_TRACE, __VA_ARGS__)
#define LOG_DEBUG(...)  LOG(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)   LOG(LOG_INFO, __VA_ARGS__)
#define LOG_WARN(...)   LOG(LOG_WARN, __VA_ARGS__)
#define LOG_ERROR(...)  LOG(LOG_ERROR, __VA_ARGS__)

/* Initialize logging system */
VFR_API int  logger_init(const char *log_file, enum log_level con_level, enum log_level file_level);
VFR_API int  logger_reopen(const char *new_path);
VFR_API void logger_shutdown(void);
VFR_API void logger_set_rotation(size_t max_bytes, int max_files);  /* F4: configure log rotation */
VFR_API void vfr_log(enum log_level level, const char *file, int line, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* VFR_LOGGER_H */
