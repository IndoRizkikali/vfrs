/*
 * types.h - VFRS Fundamental Types, Constants & Queue Definitions
 * Virtual Frame Relay Switch
 */

#ifndef VFR_TYPES_H
#define VFR_TYPES_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* ============================================================
 * Frame Relay Constants
 * ============================================================ */

#define FR_DLCI_MIN        16     /* User DLCI range start (2-octet) */
#define FR_DLCI_MAX       991     /* User DLCI range end (2-octet) */
#define FR_DLCI_LMI         0     /* LMI DLCI (ANSI/ITU) */
#define FR_DLCI_MGMT_MIN  992     /* Layer 2 management range start (CLLM) */
#define FR_DLCI_MGMT_MAX 1007     /* Layer 2 management range end */
#define FR_DLCI_RESERVED 1007     /* Alias: highest management DLCI */
#define FR_DLCI_CISCO    1023     /* Cisco LMI DLCI / in-channel L2 mgmt */
#define FR_MAX_FRAMESZ   1600     /* Maximum info field size (N203) */

/* Virtual Circuit Types */
#define VC_TYPE_PVC      0
#define VC_TYPE_SVC      1
#define VC_TYPE_MCAST    2
#define VC_TYPE_RESERVED 3

/* ============================================================
 * Thread-Safe Multi-Producer Single-Consumer (MPSC) Control Queue
 * ============================================================ */

#define SPSC_QUEUE_SIZE 256

typedef struct {
    u8     data[2048];
    size_t len;
    u16    src_port_idx; /* Index of the ingress port in ctx->ports */
} vfr_ctrl_frame_t;

typedef struct {
    vfr_ctrl_frame_t ring[SPSC_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    mutex_t  lock;
} vfr_spsc_queue_t;

VFR_API void spsc_queue_init(vfr_spsc_queue_t *q);
VFR_API void spsc_queue_destroy(vfr_spsc_queue_t *q);
VFR_API int spsc_queue_push(vfr_spsc_queue_t *q, const u8 *data, size_t len, u16 port_idx);
VFR_API int spsc_queue_pop(vfr_spsc_queue_t *q, vfr_ctrl_frame_t *out_frame);

/* ============================================================
 * Timer Management
 * ============================================================ */

typedef struct vfr_timer {
    u64 expire;
    u32 interval;
} vfr_timer_t;

VFR_API int timer_is_expired(vfr_timer_t *t);
VFR_API void timer_set(vfr_timer_t *t, u32 interval_ms);
VFR_API void timer_cancel(vfr_timer_t *t);

/* ============================================================
 * Traffic Policing / Token Bucket
 * ============================================================ */

typedef struct {
    u64     tokens;          /* Current tokens (bits) */
    u64     last_update;     /* Last update time (ms) */
    u32     cir;             /* Committed Information Rate (bps) */
    u32     bc;              /* Committed Burst Size (bits) */
    u32     be;              /* Excess Burst Size (bits) */
} token_bucket_t;

#ifdef __cplusplus
}
#endif

#endif /* VFR_TYPES_H */
