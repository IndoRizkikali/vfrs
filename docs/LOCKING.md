# VFRS Lock Hierarchy

This document describes the thread-safety design and locking hierarchy of the Virtual Frame Relay Switch (VFRS).

## Lock Hierarchy Order

To prevent deadlocks from out-of-order lock acquisition, threads must acquire locks strictly in order of increasing level numbers:

| Level | Lock Level Identifier | Mutex / Scope | Purpose |
| :--- | :--- | :--- | :--- |
| **0** | `LOCK_LEVEL_PORT` | `vfrs_ctx_t->port_mutex` | Synchronizes additions, removals, and lookup of ports. |
| **1** | `LOCK_LEVEL_PVC` | `vfrs_ctx_t->pvc_mutex` | Protects global PVC tables and operations (adding/deleting PVCs). |
| **2** | `LOCK_LEVEL_DLCI` | `vfr_port_t->mutex` | Serializes operations on a single port (such as queueing and transmitting). |
| **3** | `LOCK_LEVEL_MCAST` | `vfrs_ctx_t->mcast_mutex` | Protects multicast group structures and subscriber tables. |
| **4** | `LOCK_LEVEL_SVC` | `vfrs_ctx_t->svc_mutex` (and per-port `svc_ctx->calls`) | Coordinates SVC signaling and Call Control Blocks (CCBs). |

### Rules for Mutex Acquisition

1. **Strict Order**: A thread holding a lock at level $L_1$ can only acquire another lock at level $L_2$ if $L_2 > L_1$.
2. **Lock Order Assertions**: Use the `ASSERT_LOCK_ORDER(level)` macro immediately before calling `mutex_lock()` to verify at runtime that no lock of higher priority (level) is already held by the calling thread.
3. **Lock Release Tracking**: Use the `RELEASE_LOCK_LEVEL(level)` macro immediately after calling `mutex_unlock()` to update the tracking state for the current thread.

## Usage Example

```c
/* Correct nested lock acquisition sequence: Level 1 -> Level 3 */
ASSERT_LOCK_ORDER(LOCK_LEVEL_PVC);
mutex_lock(&ctx->pvc_mutex);

ASSERT_LOCK_ORDER(LOCK_LEVEL_MCAST);
mutex_lock(&ctx->mcast_mutex);

/* ... perform operations ... */

mutex_unlock(&ctx->mcast_mutex);
RELEASE_LOCK_LEVEL(LOCK_LEVEL_MCAST);

mutex_unlock(&ctx->pvc_mutex);
RELEASE_LOCK_LEVEL(LOCK_LEVEL_PVC);
```
