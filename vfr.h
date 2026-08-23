/*
 * vfr.h - VFRS Core Header
 * Virtual Frame Relay Switch - Platform Foundation
 */

#ifndef VFRS_H
#define VFRS_H

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
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdatomic.h>    /* C11 _Atomic — memory ordering for SPSC queue and g_running */

/* ============================================================
 * Basic Types
 * ============================================================ */

#define MAX_PORTS          256
#define MAX_PVCS           4096
#define VFR_MAX_NAME_LEN   32    /* Maximum port / PVC name length */
#define MAX_ADDR_STR       64
#define VFR_MAX_PATH       260   /* Maximum file / path length */

typedef uint8_t     u8;
typedef uint16_t    u16;
typedef uint32_t    u32;
typedef uint64_t    u64;
typedef int8_t      s8;
typedef int16_t     s16;
typedef int32_t     s32;
typedef int64_t     s64;

/* Lock-Free Single Producer Single Consumer (SPSC) Queue for Control Frames */
#define SPSC_QUEUE_SIZE 256

typedef struct {
    u8     data[2048];
    size_t len;
    u16    src_port_idx; /* Index of the ingress port in ctx->ports */
} vfr_ctrl_frame_t;

typedef struct {
    vfr_ctrl_frame_t ring[SPSC_QUEUE_SIZE];
    _Atomic uint32_t head;  /* written by producer, read by consumer (memory_order_release/acquire) */
    _Atomic uint32_t tail;  /* written by consumer, read by producer (memory_order_release/acquire) */
} vfr_spsc_queue_t;

void spsc_queue_init(vfr_spsc_queue_t *q);
int spsc_queue_push(vfr_spsc_queue_t *q, const u8 *data, size_t len, u16 port_idx);
int spsc_queue_pop(vfr_spsc_queue_t *q, vfr_ctrl_frame_t *out_frame);

/* Frame Relay constants */
#define FR_DLCI_MIN        16     /* User DLCI range start (2-octet) */
#define FR_DLCI_MAX       991     /* User DLCI range end (2-octet) */
#define FR_DLCI_LMI         0     /* LMI DLCI (ANSI/ITU) */
#define FR_DLCI_MGMT_MIN  992     /* Layer 2 management range start (CLLM) */
#define FR_DLCI_MGMT_MAX 1007     /* Layer 2 management range end */
#define FR_DLCI_RESERVED 1007     /* Alias: highest management DLCI */
#define FR_DLCI_CISCO    1023     /* Cisco LMI DLCI / in-channel L2 mgmt */
#define FR_MAX_FRAMESZ   1600     /* Maximum info field size (N203) */

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
int  logger_init(const char *log_file, enum log_level con_level, enum log_level file_level);
int  logger_reopen(const char *new_path);
void logger_shutdown(void);
void logger_set_rotation(size_t max_bytes, int max_files);  /* F4: configure log rotation */
void vfr_log(enum log_level level, const char *file, int line, const char *fmt, ...);

/* ============================================================
 * Mutex / Threading
 * ============================================================ */

int mutex_init(mutex_t *m);
void mutex_destroy(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

int thread_create(thread_t *tid, thread_ret_t (THREAD_CALL *func)(void*), void *arg);
int thread_join(thread_t tid, thread_ret_t *retval);
int thread_detach(thread_t tid);

/* ============================================================
 * Lock Order Enforcement  (D1 / D2)
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

/* Asserts that no lock at a higher level than `level` is currently held by
 * this thread, then records the acquisition in the thread-local bitmap. */
#  define ASSERT_LOCK_ORDER(level) \
     do { \
         uint32_t _higher = vfr_tls_lock_bitmap >> ((unsigned)(level) + 1u); \
         if (_higher) { \
             vfr_log(LOG_ERROR, VFR_FILENAME, __LINE__, \
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
 * Utility Functions
 * ============================================================ */

#ifdef _WIN32
#define msleep(ms)     Sleep(ms)
/* usleep(µs): rounds up to avoid Sleep(0) for sub-millisecond values.
 * NOTE: unit is microseconds (µs), matching POSIX usleep() convention. */
#define usleep(us)     Sleep(((unsigned)(us) + 999u) / 1000u)
/* Do NOT redefine strdup — _strdup is available in MSYS2/UCRT and
 * strdup is available via <string.h> on GCC with -D_GNU_SOURCE.      */
#define closesocket(s) closesocket(s)
#else
#define msleep(ms)     (void)usleep((unsigned)(ms) * 1000u)
/* closesocket: on POSIX, sockets are plain fds. */
#define closesocket(s) close(s)
/* strdup is provided by <string.h> on POSIX — no macro override needed. */
#endif

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Get current time in milliseconds */
u64 get_tick_count(void);

/* Time difference in milliseconds */
u64 tick_diff(u64 start, u64 end);

/* Timer management */
typedef struct vfr_timer {
    u64 expire;
    u32 interval;
} vfr_timer_t;

int timer_is_expired(vfr_timer_t *t);
void timer_set(vfr_timer_t *t, u32 interval_ms);
void timer_cancel(vfr_timer_t *t);

/* ============================================================
 * Network Utilities (Windows Winsock2)
 * ============================================================ */

#ifdef _WIN32
int winsock_init(void);
void winsock_shutdown(void);
#else
#define winsock_init() 0
#define winsock_shutdown() do {} while(0)
#endif

int set_nonblock(socket_fd_t fd, int nb);

/* ============================================================
 * Configuration
 * ============================================================ */

typedef enum {
    CFG_OK = 0,
    CFG_EOF,
    CFG_SYNTAX_ERR,
    CFG_NO_MEM,
    CFG_FILE_ERR
} cfg_result_t;

typedef struct config_s config_t;

config_t *config_create(const char *filename);
void config_destroy(config_t *cfg);
cfg_result_t config_next_line(config_t *cfg, char *line, size_t maxlen);
int config_error_line(config_t *cfg);

/* ============================================================
 * File Utilities
 * ============================================================ */

file_fd_t file_open(const char *path, int for_write);
void file_close(file_fd_t fd);
size_t file_read(file_fd_t fd, void *buf, size_t len);
size_t file_write(file_fd_t fd, const void *buf, size_t len);

/* ============================================================
 * PCAP Support (DLT_FRELAY = 107)
 * ============================================================ */

#define PCAP_MAGIC           0xa1b2c3d4
#define PCAP_SWAPPED_MAGIC   0xd4c3b2a1
#define PCAP_VERSION_MAJOR    2
#define PCAP_VERSION_MINOR   4
#define PCAP_LINKTYPE_FRELAY 107

#pragma pack(push, 1)

typedef struct pcap_hdr_s {
    u32 magic;
    u16 version_major;
    u16 version_minor;
    u32 thiszone;
    u32 sigfigs;
    u32 snaplen;
    u32 network;
} pcap_hdr_t;

typedef struct pcaprec_hdr_s {
    u32 ts_sec;
    u32 ts_usec;
    u32 incl_len;
    u32 orig_len;
} pcaprec_hdr_t;

#pragma pack(pop)

/* PCAP writer context */
typedef struct pcap_writer_s {
    file_fd_t       fd;
    mutex_t         mutex;
    char            filename[VFR_MAX_PATH];
    u32             pkt_count;
    u32             snaplen;
} pcap_writer_t;

int pcap_writer_init(pcap_writer_t *pw, const char *filename, u32 snaplen);
int pcap_writer_write(pcap_writer_t *pw, const u8 *data, size_t len);
int pcap_writer_close(pcap_writer_t *pw);

/* ============================================================
 * Frame Relay Address Encoding (Q.922)
 * ============================================================ */

/* 2-byte Q.922 address format:
 * Byte 0: DLCI[9:4] | CR | EA=0
 * Byte 1: DLCI[3:0] | FECN | BECN | DE | EA=1
 */

/* Return the number of octets in the Q.922 address field (1–4) by walking
 * the EA (Extension Address) bits.  max_len prevents out-of-bounds reads
 * on short or partial buffers.  A return value of 1 is always invalid for
 * Frame Relay (minimum address is 2 octets); callers must check for this.
 * Spec ref: X.36 §9.3.3 / Q.922 §3.6. */
static inline size_t fr_get_addr_len(const u8 *addr, size_t max_len) {
    if (!addr || max_len < 1) return 0;
    if ((addr[0] & 0x01) != 0) return 1; /* Single octet address (invalid for FR) */
    if (max_len < 2) return 1;           /* Incomplete address */
    if ((addr[1] & 0x01) != 0) return 2; /* 2-octet address */
    if (max_len < 3) return 2;           /* Incomplete address */
    if ((addr[2] & 0x01) != 0) return 3; /* 3-octet address */
    if (max_len < 4) return 3;           /* Incomplete address (missing 4th octet) */
    return 4;                            /* 4-octet address */
}

static inline u32 fr_decode_dlci(const u8 *addr) {
    if ((addr[0] & 0x01) == 0 && (addr[1] & 0x01) == 0) {
        if ((addr[2] & 0x01) == 0) {
            /* 4-octet: X.36 Figure 9-2
             *   addr[0] bits 7..2 = DLCI[22:17],  addr[1] bits 7..4 = DLCI[16:13]
             *   addr[2] bits 7..1 = DLCI[12:6],   addr[3] bits 7..2 = DLCI[5:0]
             */
            return (((u32)(addr[0] & 0xFC) >> 2) << 17) |
                   (((u32)(addr[1] & 0xF0) >> 4) << 13) |
                   (((u32)(addr[2] & 0xFE) >> 1) << 6)  |
                   (((u32)(addr[3] & 0xFC) >> 2));
        } else {
            /* 3-octet: addr[0] EA=0, addr[1] EA=0, addr[2] EA=1
             * Check D/C bit (bit 1 of addr[2]) to determine DLCI width */
            u8 dc = (addr[2] >> 1) & 0x01;
            if (dc == 0) {
                /* D/C=0: lower DLCI bits present → 16-bit DLCI */
                return (((u32)(addr[0] & 0xFC) >> 2) << 10) |
                       (((u32)(addr[1] & 0xF0) >> 4) << 6) |
                       (((u32)(addr[2] & 0xFC) >> 2));
            } else {
                /* D/C=1: DL-CORE control → treat as 10-bit DLCI (same as 2-octet) */
                return ((((u32)addr[0] & 0xFC) >> 2) << 4) | ((addr[1] & 0xF0) >> 4);
            }
        }
    }
    /* Default 2-octet DLCI: bits 8..3 of addr[0] & 7..4 of addr[1] */
    return ((((u32)addr[0] & 0xFC) >> 2) << 4) | ((addr[1] & 0xF0) >> 4);
}

static inline void fr_encode_dlci(u8 *addr, u32 dlci, int dlci_len) {
    if (dlci_len == 4 || dlci > 1023) {
        /* 4-octet — X.36 Figure 9-2 */
        addr[0] = (u8)(((dlci >> 17) & 0x3F) << 2);   /* EA=0; DLCI[22:17] in bits 7..2 */
        addr[1] = (u8)(((dlci >> 13) & 0x0F) << 4);   /* EA=0; DLCI[16:13] in bits 7..4 */
        addr[2] = (u8)(((dlci >> 6)  & 0x7F) << 1);   /* EA=0; DLCI[12:6]  in bits 7..1 */
        addr[3] = (u8)(((dlci & 0x3F) << 2) | 0x01);  /* EA=1; DLCI[5:0]   in bits 7..2 */
    } else if (dlci_len == 3) {
        /* 3-octet — X.36 Figure 9-1b, D/C=0 (16-bit DLCI) */
        addr[0] = (u8)(((dlci >> 10) & 0x3F) << 2);          /* EA=0; DLCI[15:10] in bits 7..2 */
        addr[1] = (u8)(((dlci >> 6) & 0x0F) << 4);           /* EA=0; DLCI[9:6]   in bits 7..4 */
        addr[2] = (u8)(((dlci & 0x3F) << 2) | 0x01);         /* EA=1; D/C=0; DLCI[5:0] in bits 7..2 */
    } else {
        /* 2-octet — X.36 Figure 9-1 */
        addr[0] = (u8)(((dlci >> 4) & 0x3F) << 2);    /* EA=0; DLCI[9:4]   in bits 7..2 */
        addr[1] = (u8)((dlci & 0x0F) << 4) | 0x01;    /* EA=1; DLCI[3:0]   in bits 7..4 */
    }
}

/* Frame relay address structure */
typedef struct {
    u32  dlci;
    u8   cr:1;
    u8   fecn:1;
    u8   becn:1;
    u8   de:1;
    u8   ea:1;
} fr_addr_t;

/* ============================================================
 * CRC-16 (FCS) - Q.922 Frame Check Sequence
 * ============================================================ */

u16 crc16_fcs(const u8 *data, size_t len);
int crc16_check(const u8 *data, size_t len);

/* ============================================================
 * HDLC Bit Stuffing
 * ============================================================ */

size_t hdlc_stuff(const u8 *in, size_t in_len, u8 *out, size_t out_max);
size_t hdlc_unstuff(const u8 *in, size_t in_len, u8 *out, size_t out_max);

/* HDLC Flag byte (0x7E) — ISO 13239 §4.3.2 / Q.922 §3.8 */
#define HDLC_FLAG    0x7E

/* TCP port private data - forward declaration for use in main */
typedef struct tcp_priv_s tcp_priv_t;
struct tcp_priv_s {
    char        lhost[64];
    u16         lport;
    char        rhost[64];
    u16         rport;
    int         mode;
    int         connected;
    int         reconnect;
    u32         reconnect_delay;
    u32         max_reconnect_delay;
    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;
    thread_t    accept_thread;
    thread_t    reader_thread;
    socket_fd_t listen_fd;
    volatile int running;
};

/* ============================================================
 * Port Types
 * ============================================================ */

#define PORT_TYPE_UNI  0
#define PORT_TYPE_NNI  1

#define PORT_TRANS_UDP          0
#define PORT_TRANS_TCP          1
#define PORT_TRANS_TCP_CLI      2
#define PORT_TRANS_TCP_SER      3
#define PORT_TRANS_SERIAL       4
#define PORT_TRANS_PIPE         5
#define PORT_TRANS_UDP_CLI      6
#define PORT_TRANS_UDP_SER      7
#define PORT_TRANS_L2TPV3_FR    8
#define PORT_TRANS_L2TPV3_HDLC  9

typedef struct vfr_port_s vfr_port_t;

/* Port status */
#define PORT_STATUS_DOWN     0
#define PORT_STATUS_UP        1
#define PORT_STATUS_CONN     2   /* Connected (for connection-oriented) */

typedef struct vfr_port_stats_s {
    u64 tx_frames;
    u64 rx_frames;
    u64 tx_bytes;
    u64 rx_bytes;
    u64 fcs_errors;
    u64 dropped;
    u64 switched;
} vfr_port_stats_t;

/* Port interface function table */
typedef struct port_ops_s port_ops_t;
typedef int (*port_send_fn)(vfr_port_t *port, const u8 *frame, size_t len);
typedef int (*port_recv_fn)(vfr_port_t *port, u8 *buf, size_t max_len);
typedef int (*port_poll_fn)(vfr_port_t *port, u32 timeout_ms);
typedef void (*port_free_fn)(vfr_port_t *port);

struct port_ops_s {
    port_send_fn    send;
    port_recv_fn    recv;
    port_poll_fn    poll;
    port_free_fn    free;
};

/* Base port structure */
struct vfr_port_s {
    char            name[VFR_MAX_NAME_LEN];
    int             type;           /* UNI or NNI */
    int             transport;      /* UDP, TCP, etc. */
    u8              dlcibit;        /* DLCI length (10 or 23 bits) */
    socket_fd_t     fd;              /* Socket or file descriptor */
    int             status;
    mutex_t         mutex;
    vfr_timer_t     poll_timer;     /* For LMI polling */

    /* Statistics */
    vfr_port_stats_t stats;

    /* Capture */
    pcap_writer_t   *capture;

    /* LMI state pointer */
    void            *lmi_ctx;

    /* Congestion management state pointer */
    void            *cgst_ctx;

    /* LAPF state pointer */
    void            *lapf_ctxs[16];   /* Pointers to vfr_lapf_state_t */
    u32             lapf_dlcis[16];  /* DLCI for each LAPF context */
    int             lapf_ctx_count;

    /* SVC context pointer */
    void            *svc_ctx;

    /* Port-specific ops */
    const port_ops_t *ops;

    /* Port-specific data */
    void            *priv;

    /* Port-local routing table (Agent lookup caches pointing to Global DLCI Table) */
    struct vfr_dlci_entry_s *dlci_lut[1024];   /* Direct array lookup for 10-bit DLCI */
    struct vfr_dlci_entry_s **dlci_array;      /* Dynamic sorted array for 23-bit DLCI */
    int             dlci_count;
    int             dlci_capacity;
};

static inline void *port_get_lapf_ctx(struct vfr_port_s *port, u32 dlci) {
    if (!port) return NULL;
    for (int i = 0; i < port->lapf_ctx_count; i++) {
        if (port->lapf_dlcis[i] == dlci) {
            return port->lapf_ctxs[i];
        }
    }
    return NULL;
}

/* Token bucket state for traffic policing */
typedef struct {
    u64     tokens;          /* Current tokens (bits) */
    u64     last_update;     /* Last update time (ms) */
    u32     cir;             /* Committed Information Rate (bps) */
    u32     bc;              /* Committed Burst Size (bits) */
    u32     be;              /* Excess Burst Size (bits) */
} token_bucket_t;

#define VC_TYPE_PVC      0
#define VC_TYPE_SVC      1
#define VC_TYPE_MCAST    2
#define VC_TYPE_RESERVED 3

typedef struct vfr_dlci_entry_s {
    char        port_in[VFR_MAX_NAME_LEN];
    char        port_out[VFR_MAX_NAME_LEN];
    u32         dlci_in;
    u32         dlci_out;
    int         active;

    u8          link_type;       /* UNI or NNI */
    u8          vc_type;         /* VC_TYPE_PVC, VC_TYPE_SVC, VC_TYPE_MCAST, VC_TYPE_RESERVED */
    void        *detail_ptr;     /* Pointer to vfr_pvc_detail_t or vfr_call_t */

    token_bucket_t tb;           /* Token bucket state for traffic policing */
    u8          peer_congested;  /* Congestion state of the peer link */
    u64         last_cllm_time;  /* Timestamp of the last received CLLM frame */
    struct vfr_dlci_entry_s *reverse_entry; /* Reverse path routing entry */

    u8          ftp;             /* Frame Transfer Priority */
    u8          fdp;             /* Frame Discard Priority */
    u8          srvcls;          /* Service Class */

    /* Counters */
    u64         tx_frames;
    u64         rx_frames;
    u64         tx_bytes;
    u64         rx_bytes;

    struct vfr_dlci_entry_s *next; /* Hash table linkage */
} vfr_dlci_entry_t;

typedef struct vfr_pvc_detail_s {
    char        port_in[VFR_MAX_NAME_LEN];
    char        port_out[VFR_MAX_NAME_LEN];
    u32         dlci_in;
    u32         dlci_out;
    int         active;

    /* Traffic parameters */
    u32         cir;             /* Committed Information Rate (bps) */
    u32         bc;              /* Committed Burst Size (bits) */
    u32         be;              /* Excess Burst Size (bits) */
    u8          ftp;             /* Frame Transfer Priority */
    u8          fdp;             /* Frame Discard Priority */
    u8          srvcls;          /* Service Class */
    u32         access_rate;     /* Physical access rate */

    /* LMI compliance fields */
    u8          lmi_new;         /* 1 if newly added, 0 if acknowledged */
    u8          lmi_new_txsn;    /* The DCE TxSN at which the new status was last sent */
    u8          lmi_reported;    /* 1 if reported in current LMI Full Status cycle */

    struct vfr_pvc_detail_s *next; /* Hash table linkage */
} vfr_pvc_detail_t;

/* PVC / DLCI hash table parameters */
#define PVC_HASH_SIZE    256
#define PVC_HASH(dlci)   (((dlci) ^ ((dlci) >> 8)) & (PVC_HASH_SIZE - 1))

/*
 * DLCI_HASH: FNV-1a 32-bit hash over port name bytes XOR'd with DLCI.
 * Including the port name prevents hash collisions when multiple ports
 * share overlapping DLCI namespaces (e.g. uni0/0:dlci=100 vs uni0/1:dlci=100).
 * Spec rationale: X.36 §9 — DLCIs are locally significant per port.
 */
static inline uint32_t dlci_hash_fn(const char *port_name, u32 dlci)
{
    uint32_t h = 2166136261u;           /* FNV-1a 32-bit offset basis */
    const unsigned char *p = (const unsigned char *)port_name;
    while (*p) { h ^= *p++; h *= 16777619u; }  /* FNV-1a prime */
    h ^= (uint8_t)(dlci);        h *= 16777619u;
    h ^= (uint8_t)(dlci >> 8);   h *= 16777619u;
    h ^= (uint8_t)(dlci >> 16);  h *= 16777619u;
    return h & (PVC_HASH_SIZE - 1u);
}
#define DLCI_HASH(port, dlci) dlci_hash_fn(port, dlci)

/* ============================================================
 * Multicast Service Types and Structs
 * ============================================================ */

#define MCAST_MODE_ONEWAY 1
#define MCAST_MODE_TWOWAY 2
#define MCAST_MODE_NWAY   3

typedef struct vfr_mcast_member_s {
    char port_name[VFR_MAX_NAME_LEN];
    u32 dlci;
    u8 lmi_new;
    u8 lmi_new_txsn;
    u8 peer_congested;
    u64 last_cllm_time;
    struct vfr_mcast_member_s *next;
} vfr_mcast_member_t;

typedef struct vfr_mcast_group_s {
    char name[VFR_MAX_NAME_LEN];
    char source_port[VFR_MAX_NAME_LEN]; /* Root port for ONEWAY, TWOWAY */
    u32 source_dlci;                /* Root DLCI */
    int mode;                       /* ONEWAY, TWOWAY, NWAY */
    u8 root_lmi_new;
    u8 root_lmi_new_txsn;
    u8 peer_congested;
    u64 last_cllm_time;
    u32 cir;                        /* Committed Information Rate in bps */
    u32 bc;                         /* Committed Burst size in bits */
    u32 be;                         /* Excess Burst size in bits */
    token_bucket_t tb;              /* Token bucket state for traffic policing */
    vfr_mcast_member_t *members;
    struct vfr_mcast_group_s *next;
} vfr_mcast_group_t;

/* ============================================================
 * LMI Types
 * ============================================================ */

#define LMI_TYPE_NONE     0
#define LMI_TYPE_ANSI     1   /* ANSI T1.617 Annex D */
#define LMI_TYPE_Q933A    2   /* ITU-T Q.933 Annex A */
#define LMI_TYPE_CISCO    3   /* Cisco/Gang of Four */

/* LMI message types */
#define LMI_MSG_STATUS_ENQ    0x75    /* ANSI/Cisco/Q.933A */
#define LMI_MSG_STATUS        0x7D    /* ANSI/Cisco/Q.933A */

/* Information Element codes */
#define LMI_IE_REPORT_TYPE_ANSI    0x01   /* Codeset 5 */
#define LMI_IE_LINK_INTEGRITY_ANSI 0x03   /* Codeset 5 */
#define LMI_IE_PVC_STATUS_ANSI     0x07   /* Codeset 5 */

#define LMI_IE_REPORT_TYPE_Q933A    0x51   /* Codeset 0 */
#define LMI_IE_LINK_INTEGRITY_Q933A 0x53   /* Codeset 0 */
#define LMI_IE_PVC_STATUS_Q933A     0x57   /* Codeset 0 */

/* Report types */
#define LMI_REPORT_FULL_STATUS        0x00
#define LMI_REPORT_LINK_INTEGRITY     0x01
#define LMI_REPORT_ASYNC_STATUS       0x02

/* PVC status flags (Q.933 Annex A / X.36)
 * Bit 8 (MSB) = extension bit (1)
 * Bit 4 = New (N)
 * Bit 3 = Delete (D)
 * Bit 2 = Active (A)
 */
#define LMI_PVC_EXT      0x80
#define LMI_PVC_NEW      0x08
#define LMI_PVC_DELETE   0x04
#define LMI_PVC_ACTIVE   0x02

/* LMI state structure */
typedef struct {
    int         type;               /* LMI type */

    /* DCE parameters (network side) */
    u8          n392;              /* Error threshold (default: 3) */
    u8          n393;              /* Monitored events (default: 4) */
    u16         t392;              /* Poll verification interval (default: 15s) */

    /* DTE parameters (user side) */
    u8          n391;              /* Full status poll counter (default: 6) */
    u8          n392_dte;          /* DTE N392 (default: 3) */
    u8          n393_dte;          /* DTE N393 (default: 4) */
    u16         t391;              /* DTE poll interval (default: 10s) */

    /* State variables */
    u8          dce_errors;
    u8          dce_events;
    u8          dte_errors;
    u8          dte_events;
    u8          dte_seq_send;
    u8          dte_seq_recv;
    u8          dce_seq_send;
    u8          dce_seq_recv;
    u8          dte_dce_seq_recv;   /* Stores the remote DCE's TxSN received by our DTE-side */

    u8          polls_since_full;
    u8          last_report_type;  /* Last Report Type IE from DTE */

    /* LMI compliance state variables */
    u16         dce_history;       /* Error history bitmask of last n393 events */
    u8          dce_link_down;     /* 1 if LMI link is declared down */
    u8          segment_active;    /* 1 if segmented Full Status is active */
    u16         pvc_send_index;    /* Next PVC to send in segmented Full Status */
    u8          dce_last_was_full; /* 1 if the last STATUS was Full Status */

    /* DTE-side error monitoring (X.36 §11.4.1.6.2) */
    u8          dte_status_received; /* 1 if a valid STATUS was received since last T391 */
    u16         dte_history;         /* DTE error-event sliding window (N393 bits) */
    u8          dte_link_down;       /* 1 = DTE declares service-affecting condition */
    u8          dte_enabled;         /* 1 = DTE polling is enabled on this port */
    u8          dte_immediate_poll;  /* 1 = Annex G: immediate re-poll on Full-Status-Continued */
    u8          async_enabled;       /* 1 = DCE is allowed to transmit asynchronous PVC status */
    u8          dte_last_request_was_full; /* 1 = last STATUS ENQUIRY requested Full Status */

    /* Timers (DCE side) */
    vfr_timer_t t392_timer;
    vfr_timer_t t391_timer;

} vfr_lmi_state_t;

/* ============================================================
 * Global Switch Context
 * ============================================================ */

typedef struct {
    char            swid[64];       /* Switch identifier */

    /* Numbering plan for SVC */
    u16             dcc;            /* Data Country Code */
    u16             nd;             /* Network Digit */
    u16             dnic;           /* Data Network ID */
    u32             pnic;           /* Private Data Network ID */
    u8              sgclen;         /* SGC length */
    u8              sgc;            /* System Group Code */
    u8              siclen;         /* SIC length */
    u8              sic;            /* System Identification Code */
    u8              subnumlen;      /* Subscriber number length */

    /* SVC subscriber table */
    struct {
        char port_name[VFR_MAX_NAME_LEN];
        char x121_number[21];
        u8   reverse_charging_acceptance;
        u8   reverse_charging_prevention;
        u8   number_type;
    } svc_subscribers[512];
    int             svc_subscriber_count;

    /* SVC NNI route table */
    struct {
        char prefix[21];
        char egress_port[VFR_MAX_NAME_LEN];
        char transit_net_id[16];
        u8   metric;
    } svc_routes[256];
    int             svc_route_count;

    /* Global DLCI Table */
    vfr_dlci_entry_t *dlci_table[PVC_HASH_SIZE];
    mutex_t         dlci_mutex;

    /* PVC table */
    vfr_pvc_detail_t *pvc_table[PVC_HASH_SIZE];
    mutex_t         pvc_mutex;

    /* SVC table (forward declared struct vfr_call_s) */
    struct vfr_call_s *svc_table[PVC_HASH_SIZE];
    mutex_t         svc_mutex;

    /* Multicast groups */
    vfr_mcast_group_t *mcast_groups;
    mutex_t         mcast_mutex;

    /* Port list */
    vfr_port_t      *ports[MAX_PORTS];
    int             port_count;
    mutex_t         port_mutex;

    /* Global capture */
    pcap_writer_t   *global_capture;

    /* Default parameters */
    u16             default_n391;
    u16             default_n392;
    u16             default_n393;
    u16             default_t391;
    u16             default_t392;
    u16             default_n392_dte;
    u16             default_n393_dte;

    u16             default_lapf_k;
    u16             default_lapf_n200;
    u16             default_lapf_n201;
    u16             default_lapf_t200;
    u16             default_lapf_t203;

    u32             default_svc_t301;
    u32             default_svc_t303;
    u32             default_svc_t305;
    u32             default_svc_t308;
    u32             default_svc_t310;
    u32             default_svc_t316;
    u32             default_svc_t317;
    u32             default_svc_t322;

    u32             default_cir;
    u32             default_bc;
    u32             default_be;
    u16             default_svc_fmif;
    u8              default_svc_ftp;
    u8              default_svc_fdp;
    u8              default_svc_class;
    u32             access_rate;

    /* SPSC signaling queue & slow-path thread (Phase 3) */
    _Atomic u16     next_crv;
    vfr_spsc_queue_t sig_queue;
    thread_t        slow_path_thread;
} vfrs_ctx_t;

/* Global context (singleton) */
extern vfrs_ctx_t  *g_vfrs;
extern _Atomic int  g_running;   /* 1 = running; 0 = shutdown requested */

/* Context initialization */
vfrs_ctx_t *vfrs_create(const char *swid);
void vfrs_destroy(vfrs_ctx_t *ctx);

/* Graceful SVC teardown: send RELEASE COMPLETE (Cause 102) to all active
 * calls before the slow-path thread is joined.  Must be called from main()
 * with g_running already set to 0.  (D8 / issue #3.3) */
void vfrs_shutdown_svc(vfrs_ctx_t *ctx);

/* PVC & DLCI management */
int vfrs_add_pvc(vfrs_ctx_t *ctx, const char *port_in, u32 dlci_in,
                 const char *port_out, u32 dlci_out, u32 cir, u32 bc, u32 be,
                 u8 ftp, u8 fdp, u8 srvcls);
int vfrs_del_pvc(vfrs_ctx_t *ctx, const char *port_in, u32 dlci_in);
vfr_pvc_detail_t *vfrs_lookup_pvc(vfrs_ctx_t *ctx, const char *port, u32 dlci);
vfr_pvc_detail_t *vfrs_lookup_pvc_unlocked(vfrs_ctx_t *ctx, const char *port, u32 dlci);
vfr_dlci_entry_t *dlci_table_lookup(vfrs_ctx_t *ctx, const char *port, u32 dlci);
int dlci_table_add(vfrs_ctx_t *ctx, vfr_dlci_entry_t *entry);
int dlci_table_del(vfrs_ctx_t *ctx, const char *port, u32 dlci);
int vfrs_dlci_is_active(vfrs_ctx_t *ctx, vfr_port_t *port, vfr_dlci_entry_t *entry);
int vfrs_dlci_is_active_unlocked(vfrs_ctx_t *ctx, vfr_port_t *port, vfr_dlci_entry_t *entry);
void vfrs_propagate_port_status_change(vfrs_ctx_t *ctx, vfr_port_t *changed_port);

/* Multicast management */
#include "pvc/pvc_mcast_uni.h"
#include "pvc/pvc_mcast_nni.h"

/* Port management */
vfr_port_t *vfrs_find_port(vfrs_ctx_t *ctx, const char *name);
vfr_port_t *vfrs_find_port_unlocked(vfrs_ctx_t *ctx, const char *name);
int vfrs_add_port(vfrs_ctx_t *ctx, vfr_port_t *port);
int vfrs_remove_port(vfrs_ctx_t *ctx, vfr_port_t *port);

/* Frame switching */
int vfrs_switch_frame(vfrs_ctx_t *ctx, vfr_port_t *src_port,
                      u32 dlci, const u8 *frame, size_t len);

/* ============================================================
 * Port Operations
 * ============================================================ */

/* UDP port */
vfr_port_t *port_udp_create(const char *name, int type,
                            const char *lhost, u16 lport,
                            const char *rhost, u16 rport);
vfr_port_t *port_udp_cli_create(const char *name, int type,
                                const char *rhost, u16 rport);
vfr_port_t *port_udp_ser_create(const char *name, int type,
                                const char *lhost, u16 lport);
vfr_port_t *port_l2tpv3_create(const char *name, int type, int transport,
                               const char *lhost, const char *rhost, u32 vcid);

/* TCP port */
vfr_port_t *port_tcp_create(const char *name, int type,
                            const char *lhost, u16 lport,
                            const char *rhost, u16 rport);
vfr_port_t *port_tcp_cli_create(const char *name, int type,
                                const char *rhost, u16 rport);
vfr_port_t *port_tcp_ser_create(const char *name, int type,
                                const char *lhost, u16 lport);

/* Serial port */
vfr_port_t *port_serial_create(const char *name, int type,
                               const char *device, u32 baudrate);

/* Named pipe */
vfr_port_t *port_pipe_create(const char *name, int type,
                             const char *pipename, int server);

/* Free port */
void port_free(vfr_port_t *port);

/* Port creation base function */
vfr_port_t *port_create(const char *name, int type, int transport);
void port_stats_init(vfr_port_t *port);
int port_set_status(vfr_port_t *port, int status);
int port_parse_name(const char *name, int *type, int *group, int *index);

/* Default socket operations */
int port_default_send(vfr_port_t *port, const u8 *frame, size_t len);
int port_default_recv(vfr_port_t *port, u8 *buf, size_t max_len);
int port_default_poll(vfr_port_t *port, u32 timeout_ms);

/* Port-local unified routing table operations (Option A / future Option C) */
vfr_dlci_entry_t *port_lookup_dlci(vfr_port_t *port, u32 dlci);
int port_add_dlci_entry(vfr_port_t *port, vfr_dlci_entry_t *entry);
int port_del_dlci_entry(vfr_port_t *port, u32 dlci);

/* ============================================================
 * LMI Operations
 * ============================================================ */

/* LMI Common Helpers */
void lmi_init_state(vfr_lmi_state_t *lmi, int type);
size_t lmi_build_header(vfr_port_t *port, u8 *buf, size_t max_len, int protocol_disc);

/* ANSI LMI IE Builders/Parsers */
size_t lmi_ansi_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type);
size_t lmi_ansi_build_link_integrity_ie(u8 *buf, size_t max_len, u8 send_seq, u8 recv_seq);
size_t lmi_ansi_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len);
int lmi_ansi_parse_pvc_status_ie(const u8 *buf, size_t len, u32 *dlci, u8 *status);

/* Q.933 Annex A LMI IE Builders/Parsers */
size_t lmi_q933a_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type);
size_t lmi_q933a_build_link_integrity_ie(u8 *buf, size_t max_len, u8 send_seq, u8 recv_seq);
size_t lmi_q933a_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len);
int lmi_q933a_parse_pvc_status_ie(const u8 *buf, size_t len, u32 *dlci, u8 *status);

/* Cisco / GoF LMI IE Builders/Parsers */
size_t lmi_gof_build_report_type_ie(u8 *buf, size_t max_len, u8 report_type);
size_t lmi_gof_build_pvc_status_ie(u8 *buf, size_t max_len, u32 dlci, u8 status, int dlci_len);

/* ANSI LMI */
int lmi_ansi_init(vfr_port_t *port, int t392, int n392, int n393);
int lmi_ansi_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
size_t lmi_ansi_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len);

/* Q.933 Annex A LMI */
int lmi_q933a_init(vfr_port_t *port, int t392, int n392, int n393);
int lmi_q933a_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
size_t lmi_q933a_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len);

/* Cisco/Gang of Four LMI */
int lmi_gof_init(vfr_port_t *port, int t392, int n392, int n393);
int lmi_gof_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
size_t lmi_cisco_build_enquiry(vfr_port_t *port, u8 *buf, size_t max_len);

/* LMI polling (shared timer handler) */
int lmi_poll_timer(vfr_port_t *port);

/* LMI state management */
void lmi_reset_dce_state(vfr_lmi_state_t *lmi);
int  lmi_send_async_status(vfr_port_t *port, u32 dlci, u8 status_flags);

/* Free the LMI context (vfr_lmi_state_t) allocated by lmi_*_init().
 * Dispatches based on lmi_ctx != NULL; sets port->lmi_ctx = NULL.
 * Called from port_free() to prevent the lmi_ctx memory leak. (D6/#2.27) */
void lmi_free(vfr_port_t *port);

/* Sequence number helper (exported for fr_switch.c use) */
u8 lmi_inc_seq(u8 seq);

/* Enable DTE-side polling on a port */
int lmi_dte_enable(vfr_port_t *port, u8 n391, u16 t391, u8 n392_dte, u8 n393_dte);

/* TCP client reconnect */
int tcp_connect(vfr_port_t *port);

/* Build LMI STATUS ENQUIRY frame */
size_t lmi_build_enquiry(u8 *buf, size_t max_len, int type);

/* Build LMI STATUS reply */
size_t lmi_build_status(vfr_port_t *port, u8 *buf, size_t max_len,
                        int type, int full_status);

/* ANSI LMI STATUS builder */
size_t lmi_ansi_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status);

/* Q.933A LMI STATUS builder */
size_t lmi_q933a_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status);

/* ============================================================
 * Switch Core Functions
 * ============================================================ */

/* Frame processing */
int fr_switch_input(vfrs_ctx_t *ctx, vfr_port_t *port, u8 *frame, size_t len);
int fr_switch_input_processed(vfrs_ctx_t *ctx, vfr_port_t *port, const fr_addr_t *addr, u8 *frame, size_t frame_len);
thread_ret_t THREAD_CALL run_slow_path_thread(void *arg);

/* Frame parsing — frame_len is set to the destuffed frame body length
 * (address + ctrl + data, excluding the 2-byte FCS). */
int fr_parse_frame(u8 *frame, size_t len, fr_addr_t *addr, size_t *frame_len, int is_serial);
void fr_decode_addr(const u8 *addr, fr_addr_t *result);
void fr_encode_dlci_with_flags(u8 *addr, u32 dlci, int fecn, int becn, int de, int cr, int dlci_len);
int fr_dlci_reserved(u32 dlci);
int fr_dlci_valid(u32 dlci);

/* DLCI address rewriting */
void fr_rewrite_dlci(u8 *frame, u32 old_dlci, u32 new_dlci);

/* Frame building — fr_build_ui_frame: builds a UI frame with HDLC framing
 * (flags, zero-bit stuffing) for serial transmission. Pass flags!=NULL to
 * preserve C/R, FECN, BECN, DE from the original address. */
size_t fr_build_ui_frame(u8 *buf, size_t max_len, u32 dlci,
                          const u8 *data, size_t data_len,
                          const fr_addr_t *flags);

/* Cisco/Gang-of-Four LMI STATUS builder */
int lmi_cisco_build_status(vfr_port_t *port, u8 *buf, size_t max_len, int full_status);
int lmi_port_dlci_len(vfr_port_t *port);

/* ============================================================
 * Congestion Management & CLLM (X.36 / Q.922 / Q.933)
 * ============================================================ */
#define CLLM_LEVEL_NONE        0x00
#define CLLM_LEVEL_RECEIVED     0x01   /* FECN received */
#define CLLM_LEVEL_INDICATED    0x02   /* BECN or DE received */
#define CLLM_LEVEL_PERSISTENT   0x03   /* Sustained congestion */

/* Q.922 §A.7.3 — Parameter Identifiers inside group value */
#define CLLM_PI_PARAM_SET_ID   0x00   /* PI=0: Parameter Set Identifier */
#define CLLM_PI_CAUSE          0x02   /* PI=2: Cause Identifier */
#define CLLM_PI_DLCI_LIST      0x03   /* PI=3: DLCI List */

int cgst_init(vfr_port_t *port, u32 access_rate, u32 rate_threshold);
u32 cgst_get_access_rate(vfr_port_t *port);
void cgst_dlci_tb_init(vfr_dlci_entry_t *entry, struct vfr_port_s *port, u32 cir, u32 bc, u32 be);
int cgst_process_frame(vfr_port_t *port, vfr_dlci_entry_t *entry, u8 *frame, size_t len, fr_addr_t *addr);
int cgst_process_frame_tb(vfr_port_t *port, token_bucket_t *tb, u8 *frame, size_t len, fr_addr_t *addr);
int cgst_should_set_de(vfr_port_t *port);
void cgst_set_congested(vfr_port_t *port, int congested);
void cgst_free(vfr_port_t *port);
void cgst_poll_timer(vfr_port_t *port);
void cgst_set_clear_threshold(vfr_port_t *port, u32 clear_threshold);
void cgst_set_cllm_enabled(vfr_port_t *port, int enabled);
void cgst_set_cllm_tx_interval(vfr_port_t *port, u32 interval_ms);
void cgst_set_write_failure_threshold(vfr_port_t *port, u32 threshold);
void cgst_handle_write_failure(vfr_port_t *port);
void cgst_handle_write_success(vfr_port_t *port);
size_t cllm_build_xid(u8 *buf, size_t max_len, u8 congestion_level,
                      const u32 *dlci_list, int dlci_count, int dlci_len);
int cllm_parse_xid(const u8 *buf, size_t len, u8 *congestion_level,
                   u32 *dlci_list, int *dlci_count, int max_dlcis);
int cllm_send_notification(vfr_port_t *port, u8 congestion_level,
                           const u32 *dlci_list, int dlci_count);
int cllm_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
int cllm_should_include_dlci(u32 dlci, u8 congestion_level);

/* ============================================================
 * LAPF Operations
 * ============================================================ */
int lapf_port_init(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203);
void lapf_poll_timer(vfr_port_t *port);
int lapf_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);
int lapf_send_l3(vfr_port_t *port, u32 dlci, const u8 *data, size_t len);
int lapf_establish_link(vfr_port_t *port, u32 dlci);
int lapf_release_link(vfr_port_t *port, u32 dlci);
void lapf_l3_recv_cb(vfr_port_t *port, const u8 *data, size_t len);
void lapf_free(vfr_port_t *port);
void svc_free_port(vfr_port_t *port);

/* ============================================================
 * Statistics
 * ============================================================ */

void vfrs_show_ports(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_pvcs(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_stats(vfrs_ctx_t *ctx, const char *port_name, FILE *out);
void vfrs_show_svc_calls(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_svc_call_detail(vfrs_ctx_t *ctx, const char *arg, FILE *out);
void vfrs_show_svc_stats(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_svc_subscribers(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_svc_routes(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_config(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_defaults(vfrs_ctx_t *ctx, FILE *out);
void vfrs_show_swconfig(vfrs_ctx_t *ctx, FILE *out);

#endif /* VFRS_H */