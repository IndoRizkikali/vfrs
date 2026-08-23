# VFRS Redesign & Optimization Walkthrough (Phases 1-4)

This document provides a detailed step-by-step walkthrough of the code modifications made during the VFRS Redesign and Optimization project.

---

## Phase 1: Lock Inversion Resolution

*   **Goal**: Prevent deadlocks between the global `port_mutex` and `pvc_mutex`.
*   **Changes**:
    *   **Main Loop (`run_main_loop`)**: Updated to acquire `port_mutex`, copy active port pointers to a local array, release `port_mutex`, and then poll sockets without holding the lock.
    *   **Port Status Propagation**: Modified status path lookups in [fr_switching/fr_switch.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/fr_switching/fr_switch.c) to query ports via the unlocked lookup function `vfrs_find_port_unlocked()`.

---

## Phase 2: Port-Local Unified Routing Tables

*   **Goal**: Relieve contention on the global hash table.
*   **Changes**:
    *   **Port Structures**: Added local lookup tables to `struct vfr_port_s` in [vfr.h](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfr.h):
        *   `dlci_lut[1024]`: Flat pointer array for 10-bit ports.
        *   `dlci_array`: CONTIGUOUS pointer array sorted by DLCI for 23-bit ports.
    *   **Table Helpers**: Implemented in [ports/port_common.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/ports/port_common.c):
        *   `port_lookup_dlci()`: O(1) direct array load (lock-free) for DLCIs < 1024; local-locked binary search (`bsearch`) on `dlci_array` for 23-bit DLCIs.
        *   `port_add_dlci_entry()`: Dynamic insertion sort.
        *   `port_del_dlci_entry()`: Deletes entry and shifts elements left to maintain a sorted array.
    *   **Forwarding Integration**: Updated `vfrs_switch_frame()` and `vfrs_pvc_is_active_unlocked()` to perform lookups via `port_lookup_dlci()`.
    *   **LMI Updates**: Replaced global `vfrs_lookup_pvc()` queries inside [pvc/pvc_lmi_ansi.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/pvc/pvc_lmi_ansi.c), [pvc/pvc_lmi_gof.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/pvc/pvc_lmi_gof.c), and [pvc/pvc_lmi_q933a.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/pvc/pvc_lmi_q933a.c) with `port_lookup_dlci()`.

---

## Phase 3: Dual-Thread Concurrency & Event Loop

*   **Goal**: Transition to an asynchronous event loop and offload timers to a background thread.
*   **Changes**:
    *   **Polling Abstraction**: Defined `vfr_pollfd_t` and `VFR_POLL` in [vfr.h](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfr.h) mapping to `WSAPoll()` on Windows and `poll()` on Linux.
    *   **SPSC Queue**: Implemented lock-free Single Producer Single Consumer queue structures `vfr_ctrl_frame_t` and `vfr_spsc_queue_t` using memory barriers (`MemoryBarrier()` / `__sync_synchronize()`) in [ports/port_queue.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/ports/port_queue.c).
    *   **Main Event Loop**: Modified `run_main_loop()` in [vfrs_main.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfrs_main.c) to build active port lists, poll sockets via `VFR_POLL` outside of locks, and dispatch read frames.
    *   **Slow-Path Thread**: Implemented `run_slow_path_thread()` in [vfrs_main.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfrs_main.c) to consume queue frames and run all port timers (`lmi_poll_timer()`, `cgst_poll_timer()`, `lapf_poll_timer()`) asynchronously on a 100ms tick.

---

## Phase 4: Signaling Demultiplexer & Slow-Path Offloading

*   **Goal**: Intercept and offload signaling/control frames on the ingress path.
*   **Changes**:
    *   **Demuxer Frontend**: Refactored `fr_switch_input()` in [fr_switching/fr_switch.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/fr_switching/fr_switch.c) into a parser frontend and a backend `fr_switch_input_processed()`.
    *   **Ingress Classification Rules**:
        *   Frames on DLCI 0, 1007, 1015, or 1023 are inspected for LAPF control bytes (UI vs non-UI, XID, etc.) to determine control-plane routing.
        *   Non-UI frames on user VCs are also routed to control.
        *   If control and not on Thread 2, push to `sig_queue` and return `0`.
        *   Reserved DLCI ranges (1-15, 1008-1022) are dropped instantly.
    *   **Thread Identification**: Used `GetCurrentThreadId()` (Windows) / `pthread_self()` (Linux) to identify Thread 2. If execution is already on Thread 2, queueing is bypassed to prevent recursive locks and loops.
