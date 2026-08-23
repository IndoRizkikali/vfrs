# VFRS Redesign & Optimization Implementation Plan (Phases 1-4)

This document describes the unified implementation plan, architecture, and verification status for all four phases of the VFRS Redesign and Optimization project.

---

## 1. Concurrency & Performance Goals
1.  **Eliminate Global Lock Contention**: Replace the global routing lookup mutex serialization with lock-free port-local tables.
2.  **Ensure Deadlock Freedom**: Enforce a strict lock ordering hierarchy (`port_mutex` -> `pvc_mutex`).
3.  **Scalable Polling**: Remove `select()` socket limits and support multi-thousand port scales.
4.  **Data/Control Plane Separation**: Isolate frame forwarding (Thread 1 - Fast Path) from signaling protocols and timers (Thread 2 - Slow Path).

---

## 2. Phase-by-Phase Plan & Implementation Status

### Phase 1: Lock Inversion Deadlock Resolution [COMPLETED]
*   **Problem**: An $AB-BA$ lock ordering cycle existed between `port_mutex` and `pvc_mutex` (e.g. `vfrs_switch_frame` acquired `pvc_mutex` then queried ports, while `main_loop` acquired `port_mutex` then queried PVCs).
*   **Resolution**: 
    *   Replaced port lookup calls in the PVC status path with lock-free/unlocked variant `vfrs_find_port_unlocked()`.
    *   Modified the main loop in [vfrs_main.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfrs_main.c) to copy active port pointers to a local array under `port_mutex`, then release the lock and perform socket I/O completely outside the lock scope.

### Phase 2: Port-Local Unified Routing Tables [COMPLETED]
*   **Problem**: High-speed forwarding had to search a global hash table under `pvc_mutex`, causing serialization across all ports.
*   **Resolution**:
    *   Implemented port-local routing tables on every port in [ports/port_common.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/ports/port_common.c).
    *   **10-bit ports**: Direct array lookup (`dlci_lut[1024]`) yielding lock-free $O(1)$ performance.
    *   **23-bit ports**: Sorted pointer array (`dlci_array`) searched using binary search (`bsearch`) under a port-local lock (`port->mutex`) in $O(\log N)$ time.
    *   Updated LMI status builders to resolve PVCs via local port tables instead of the global hash table.

### Phase 3: Dual-Thread Concurrency Model & Portable Event Loop [COMPLETED]
*   **Problem**: Sockets were polled using `select()`, which is limited to 64 sockets on Windows, and timers ran on the same thread, causing keepalives to block data plane traffic.
*   **Resolution**:
    *   Abstracted Winsock `WSAPoll()` and POSIX `poll()` into `vfr_pollfd_t` and `VFR_POLL` in [vfr.h](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfr.h).
    *   Implemented a lock-free Single Producer Single Consumer (SPSC) queue (`ctx->sig_queue`) using memory barriers to pass control frames without lock contention.
    *   Replaced the main loop with an event-driven `poll()` loop (Thread 1) and spawned a background control thread (Thread 2) in [vfrs_main.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfrs_main.c) to run LMI/LAPF timers and process control frames asynchronously.

### Phase 4: Signaling Demultiplexer & Slow-Path Offloading [COMPLETED]
*   **Problem**: Interface reader threads and fast-path loops had to parse LMI/LAPF headers, consuming CPU cycles and risking thread contention.
*   **Resolution**:
    *   Implemented a link-plane **Signaling Demultiplexer** in `fr_switch_input_processed()`.
    *   Decodes both the DLCI and the LAPF control byte (UI vs non-UI, XID, etc.) to route incoming frames.
    *   Valid control frames (DLCIs 0, 1007, 1015, 1023, and non-UI frames on user VCs) are offloaded to Thread 2 via `sig_queue` and returned immediately.
    *   Reserved DLCI ranges (1-15, 1008-1022) are dropped instantly on the fast path.
    *   Uses thread ID comparison (`GetCurrentThreadId` / `pthread_self`) to allow Thread 2 to bypass queueing and execute protocol engines directly.

---

## 3. Verification

*   **Build**: Compiles cleanly with static linking on Windows (MSYS2 UCRT64 GCC).
*   **Switching & Keepalives**: Bidirectional data frames switch on the fast path while LMI ticks and keepalive exchanges run concurrently on Thread 2.
