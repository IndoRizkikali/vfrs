/*
 * platform.h - VFRS Platform Abstraction Layer
 * Virtual Frame Relay Switch - Windows / POSIX Compatibility
 */

#ifndef VFR_PLATFORM_H
#define VFR_PLATFORM_H

#include "export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdatomic.h>    /* C11 _Atomic — memory ordering for SPSC queue and g_running */

/* Windows includes */
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>

    typedef SOCKET socket_fd_t;
    typedef HANDLE file_fd_t;
    typedef CRITICAL_SECTION mutex_t;
    typedef HANDLE thread_t;
    typedef DWORD thread_ret_t;
    #define THREAD_CALL __stdcall
    #define THREAD_RET 0

    typedef WSAPOLLFD vfr_pollfd_t;
    #define VFR_POLL WSAPoll
#else
    /* POSIX includes */
    #include <pthread.h>
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <poll.h>

    typedef int socket_fd_t;
    typedef int file_fd_t;
    typedef pthread_mutex_t mutex_t;
    typedef pthread_t thread_t;
    typedef void* thread_ret_t;
    #define THREAD_CALL
    #define THREAD_RET NULL

    typedef struct pollfd vfr_pollfd_t;
    #define VFR_POLL poll
    #define INVALID_HANDLE_VALUE (-1)
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR   (-1)
    #define closesocket close
    #define SD_BOTH SHUT_RDWR
    #define WSAGetLastError() errno
    #define WSAEWOULDBLOCK EINPROGRESS
    #define WSAECONNREFUSED ECONNREFUSED
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Mutex / Threading */
VFR_API int mutex_init(mutex_t *m);
VFR_API void mutex_destroy(mutex_t *m);
VFR_API void mutex_lock(mutex_t *m);
VFR_API void mutex_unlock(mutex_t *m);

VFR_API int thread_create(thread_t *tid, thread_ret_t (THREAD_CALL *func)(void*), void *arg);
VFR_API int thread_join(thread_t tid, thread_ret_t *retval);
VFR_API int thread_detach(thread_t tid);

/* ============================================================
 * Lock Order Enforcement (D1 / D2)
 *
 * Canonical acquisition order — MUST always be respected:
 *   port_mutex → pvc_mutex → dlci_mutex → mcast_mutex → svc_mutex
 *
 * ASSERT_LOCK_ORDER(level)  — call immediately BEFORE mutex_lock().
 * RELEASE_LOCK_LEVEL(level) — call immediately AFTER  mutex_unlock().
 * Both macros are compiled away in release builds (NDEBUG defined).
 * ============================================================ */

typedef enum {
    LOCK_LEVEL_PORT  = 0,
    LOCK_LEVEL_PVC   = 1,
    LOCK_LEVEL_DLCI  = 2,
    LOCK_LEVEL_MCAST = 3,
    LOCK_LEVEL_SVC   = 4
} vfr_lock_level_t;

#ifndef NDEBUG
#  ifdef _MSC_VER
     extern __declspec(thread) uint32_t vfr_tls_lock_bitmap;
#  else
     extern __thread uint32_t vfr_tls_lock_bitmap;
#  endif

void vfr_log(int level, const char *file, int line, const char *fmt, ...);

#  define ASSERT_LOCK_ORDER(level) \
     do { \
         uint32_t _higher = vfr_tls_lock_bitmap >> ((unsigned)(level) + 1u); \
         if (_higher) { \
             vfr_log(4 /* LOG_ERROR */, __FILE__, __LINE__, \
                 "LOCK ORDER VIOLATION: acquiring level %d while bitmap=0x%x", \
                 (int)(level), vfr_tls_lock_bitmap); \
             abort(); \
         } \
         vfr_tls_lock_bitmap |= (1u << (unsigned)(level)); \
     } while (0)

#  define RELEASE_LOCK_LEVEL(level) \
     do { vfr_tls_lock_bitmap &= ~(1u << (unsigned)(level)); } while (0)

#else /* NDEBUG */
#  define ASSERT_LOCK_ORDER(level)   do {} while (0)
#  define RELEASE_LOCK_LEVEL(level)  do {} while (0)
#endif /* NDEBUG */

/* ============================================================
 * Utility Functions & Time
 * ============================================================ */

#ifdef _WIN32
#define msleep(ms)     Sleep(ms)
/* usleep(µs): rounds up to avoid Sleep(0) for sub-millisecond values.
 * NOTE: unit is microseconds (µs), matching POSIX usleep() convention. */
#define usleep(us)     Sleep(((unsigned)(us) + 999u) / 1000u)
#define closesocket(s) closesocket(s)
#else
#define msleep(ms)     (void)usleep((unsigned)(ms) * 1000u)
#define closesocket(s) close(s)
#endif

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Get current time in milliseconds */
VFR_API uint64_t get_tick_count(void);

/* Time difference in milliseconds */
VFR_API uint64_t tick_diff(uint64_t start, uint64_t end);

/* Winsock / Socket helpers */
#ifdef _WIN32
VFR_API int winsock_init(void);
VFR_API void winsock_shutdown(void);
#else
#define winsock_init() 0
#define winsock_shutdown() do {} while(0)
#endif

VFR_API int set_nonblock(socket_fd_t fd, int nb);

/* File Utilities */
VFR_API file_fd_t file_open(const char *path, int for_write);
VFR_API void file_close(file_fd_t fd);
VFR_API size_t file_read(file_fd_t fd, void *buf, size_t len);
VFR_API size_t file_write(file_fd_t fd, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VFR_PLATFORM_H */
