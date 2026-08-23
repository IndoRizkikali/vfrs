# VFRS Code-Level Optimization Reference: Signaling Demultiplexer & Slow-Path Offloading

This document explains the technical optimizations implemented in VFRS across Phase 1, Phase 2, and Phase 3. It details what the lines of code are actually doing, why they were introduced, and their performance/thread-safety implications, so future sessions can safely build LAPF and SVC signalling systems on top.

---

## 1. The Ingress Signaling Demultiplexer (`fr_switch_input_processed`)

*   **Location**: [fr_switching/fr_switch.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/fr_switching/fr_switch.c#L953)
*   **Purpose**: Demultiplexes incoming frames based on both the DLCI and the LAPF control field, routing control frames to the slow-path queue (`sig_queue`) and data frames directly to the switching engine.

### Line-by-Line Code Breakdown:

```c
size_t addr_len = ((frame[0] & 0x01) == 0 && (frame[1] & 0x01) == 0) ? 4 : 2;
if (frame_len < addr_len) {
    port->stats.dropped++;
    return -1;
}
u8 ctrl_field = (frame_len > addr_len) ? frame[addr_len] : 0;
```
*   **What it does**: Computes the size of the Q.922 Address field. In Frame Relay, address fields are either 2 octets or 4 octets. It checks the Extended Address (EA) bits of the first two bytes (`frame[0]` and `frame[1]`). If both have `EA = 0`, it signifies a 4-octet address; otherwise, it is a 2-octet address. Once the address length (`addr_len`) is known, it extracts the subsequent byte as the **LAPF Control Field** (`ctrl_field`).

```c
if (addr.dlci == 0) {
    route_to_control = 1;
} else if (addr.dlci == 1023) {
    if (ctrl_field == 0x03) {
        route_to_control = 1;
    } else {
        drop_frame = 1;
    }
...
```
*   **What it does**: Directs DLCI 0 frames (LMI and future SVC/LAPF signaling) to the control plane.
*   **Cisco LMI (DLCI 1023)**: Cisco LMI only uses Unnumbered Information (UI) frames (`ctrl_field == 0x03`). Non-UI frames on DLCI 1023 are invalid and flagged for deletion (`drop_frame = 1`).

```c
} else if (addr.dlci == 1007) {
    if (ctrl_field == 0xAF) {
        route_to_control = 1;
    } else {
        drop_frame = 1;
    }
}
```
*   **What it does**: CLLM (Consolidated Link Layer Management) runs on DLCI 1007. CLLM specifies that these frames must be Exchange Identification (XID) frames, marked by the control field `0xAF`. Any non-XID frame on DLCI 1007 is dropped.

```c
} else if ((addr.dlci >= 1 && addr.dlci <= 15) || (addr.dlci >= 1008 && addr.dlci <= 1022)) {
    drop_frame = 1;
} else {
    if (ctrl_field != 0x03) {
        route_to_control = 1;
    }
}
```
*   **What it does**: Instantly drops reserved DLCIs (1-15, 1008-1022) to prevent them from hitting the lookup tables.
*   **User DLCIs**: For valid user channels (16-991, 1024-8388607), standard UI frames (`ctrl_field == 0x03`) are processed on the fast data plane. Non-UI frames on user DLCIs (indicating LAPF Layer 2 control/signaling running on a user VC) are routed to the control plane.

```c
if (drop_frame) {
    port->stats.dropped++;
    LOG_DEBUG("Frame on DLCI %u dropped...", addr.dlci, ctrl_field, port->name);
    return -1;
}
```
*   **What it does**: Discards reserved or malformed control frames immediately on the receiving thread, preventing them from entering the SPSC queue or the switching loop.

```c
int is_slow_path = 0;
if (ctx->slow_path_thread != 0) {
#ifdef _WIN32
    if (GetCurrentThreadId() == GetThreadId(ctx->slow_path_thread)) {
        is_slow_path = 1;
    }
#else
    if (pthread_equal(pthread_self(), ctx->slow_path_thread)) {
        is_slow_path = 1;
    }
#endif
}
```
*   **What it does**: Identifies whether the current executing thread is Thread 2 (the slow-path thread). If the slow-path thread is running, it compares the current thread's handle/ID with the registered slow-path thread ID. This prevents infinite loops: when Thread 2 pops a packet from the queue and calls `fr_switch_input_processed()`, `is_slow_path` will be `1`, causing it to bypass the queueing step and execute the protocol state machines directly.

```c
if (route_to_control && !is_slow_path) {
    int port_idx = -1;
    mutex_lock(&ctx->port_mutex);
    for (int p = 0; p < ctx->port_count; p++) {
        if (ctx->ports[p] == port) {
            port_idx = p;
            break;
        }
    }
    mutex_unlock(&ctx->port_mutex);

    if (port_idx != -1) {
        if (spsc_queue_push(&ctx->sig_queue, frame, frame_len, (u16)port_idx) < 0) {
            LOG_WARN("Signaling queue full, dropped control frame...");
        }
        return 0;
    }
}
```
*   **What it does**: If the packet is classified as control-plane traffic, and we are running on the fast path (Thread 1 or transport reader threads), we locate the ingress port's index under the `port_mutex` lock (held for a very short duration) and push the packet to the SPSC signaling queue. It then returns `0` immediately, offloading all subsequent parsing and processing to Thread 2.

---

## 2. Lock-Free SPSC Signaling Queue

*   **Location**: [ports/port_queue.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/ports/port_queue.c)
*   **Purpose**: Connects Thread 1 and Thread 2 via a lock-free Single Producer Single Consumer (SPSC) ring buffer, avoiding lock overhead.

### Line-by-Line Code Breakdown:

```c
int spsc_queue_push(vfr_spsc_queue_t *q, const u8 *data, size_t len, u16 port_idx)
{
    uint32_t current_head = q->head;
    uint32_t current_tail = q->tail;

    if (((current_head + 1) % SPSC_QUEUE_SIZE) == current_tail) {
        return -1; /* Queue Full */
    }
```
*   **What it does**: Reads local copies of the volatile `head` and `tail` pointers. If advancing the `head` pointer modulo the buffer size equals `tail`, the queue is full and it returns `-1`.

```c
    vfr_ctrl_frame_t *slot = &q->ring[current_head];
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->src_port_idx = port_idx;
```
*   **What it does**: Copies the frame's payload and port metadata directly into the pre-allocated array slot. This prevents dynamic memory allocations (`malloc`/`free`) on the fast path.

```c
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif

    q->head = (current_head + 1) % SPSC_QUEUE_SIZE;
    return 0;
}
```
*   **What it does**: Executes a hardware memory barrier. This forces the CPU and compiler to complete all writes to the ring slot *before* advancing the `head` pointer. Without this barrier, out-of-order CPU execution could publish the new `head` value before the packet data is fully written to RAM, causing Thread 2 to read garbage data.

```c
int spsc_queue_pop(vfr_spsc_queue_t *q, vfr_ctrl_frame_t *out_frame)
{
    uint32_t current_head = q->head;
    uint32_t current_tail = q->tail;

    if (current_tail == current_head) {
        return -1; /* Queue Empty */
    }
```
*   **What it does**: Reads local copies of `head` and `tail`. If they are equal, the queue is empty and it returns `-1`.

```c
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif

    *out_frame = q->ring[current_tail];
```
*   **What it does**: Executes a memory barrier to ensure that Thread 2 does not read slot data before verifying that `head != tail`. Once verified, it copies the packet data from the ring slot to local storage.

```c
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif

    q->tail = (current_tail + 1) % SPSC_QUEUE_SIZE;
    return 0;
}
```
*   **What it does**: Executes a final memory barrier to ensure that Thread 2 has fully completed reading the slot data *before* advancing the `tail` pointer. This prevents Thread 1 from overwriting the slot while Thread 2 is still reading it.

---

## 3. Fast-Path Event Loop (`run_main_loop`)

*   **Location**: [vfrs_main.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfrs_main.c#L565)
*   **Purpose**: Polls all active port sockets and routes data plane frames with minimal overhead.

### Line-by-Line Code Breakdown:

```c
nfds = 0;
mutex_lock(&ctx->port_mutex);
for (i = 0; i < ctx->port_count && nfds < MAX_PORTS; i++) {
    vfr_port_t *port = ctx->ports[i];
    if (port && port->fd != INVALID_SOCKET && port->status != PORT_STATUS_DOWN && port->ops->poll != NULL) {
        fds[nfds].fd = port->fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        poll_ports[nfds] = port;
        nfds++;
    }
}
mutex_unlock(&ctx->port_mutex);
```
*   **What it does**: Briefly locks the global `port_mutex` to copy active socket descriptors and port pointers into the local `fds` and `poll_ports` arrays. The lock is immediately released, meaning we do not hold the lock during the blocking `VFR_POLL` call, which prevents deadlocks.

```c
ret = VFR_POLL(fds, nfds, 100);
```
*   **What it does**: Calls `WSAPoll()` (on Windows) or `poll()` (on Linux) with a 100ms timeout. Because this runs outside of `port_mutex`, other threads (such as CLI or configuration threads) can add/remove ports or modify routes without blocking.

```c
if (ret > 0) {
    for (i = 0; i < nfds; i++) {
        if (fds[i].revents & POLLIN) {
            vfr_port_t *port = poll_ports[i];
            int len = port->ops->recv(port, buf, sizeof(buf));
            if (len > 0) {
                fr_switch_input(ctx, port, buf, len);
            }
...
```
*   **What it does**: Scans the poll results. If a port has readable data (`POLLIN`), it calls the port's `recv` operation to read the frame into `buf`, and dispatches it to `fr_switch_input()`.

---

## 4. Slow-Path Timer Thread (`run_slow_path_thread`)

*   **Location**: [vfrs_main.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/vfrs_main.c#L510)
*   **Purpose**: Consumes control plane frames from `sig_queue` and executes all periodic LMI, CLLM, and LAPF poll timers on a separate thread.

### Line-by-Line Code Breakdown:

```c
while (spsc_queue_pop(&ctx->sig_queue, &ctrl_frame) == 0) {
    vfr_port_t *port = NULL;
    mutex_lock(&ctx->port_mutex);
    if (ctrl_frame.src_port_idx < ctx->port_count) {
        port = ctx->ports[ctrl_frame.src_port_idx];
    }
    mutex_unlock(&ctx->port_mutex);

    if (port) {
        fr_addr_t addr;
        fr_decode_addr(ctrl_frame.data, &addr);
        fr_switch_input_processed(ctx, port, &addr, ctrl_frame.data, ctrl_frame.len);
    }
}
```
*   **What it does**: Continually pops control frames from `sig_queue`. If a frame is present, it looks up the source port pointer under `port_mutex`, decodes the Q.922 address, and calls `fr_switch_input_processed()`. Because `is_slow_path` is evaluated as `1` on this thread, it skips the queue push step and directly executes the protocol state machines.

```c
mutex_lock(&ctx->port_mutex);
int count = ctx->port_count;
for (i = 0; i < count; i++) {
    ports_copy[i] = ctx->ports[i];
}
mutex_unlock(&ctx->port_mutex);
```
*   **What it does**: Copies active port pointers into a local array `ports_copy` under `port_mutex`, then immediately releases the lock.

```c
for (i = 0; i < count; i++) {
    vfr_port_t *port = ports_copy[i];
    if (port) {
        if (port->lmi_ctx) lmi_poll_timer(port);
        if (port->cgst_ctx) cgst_poll_timer(port);
        if (port->lapf_ctx) lapf_poll_timer(port);
    }
}
msleep(100);
```
*   **What it does**: Ticks all active timers for LMI, congestion (CLLM), and LAPF state machines. Because it does not hold the lock during the timer executions, any lock acquisition inside LMI/LAPF cannot deadlock with the main thread. It then sleeps for 100ms before starting the next cycle.

---

## 5. Port-Local Lookup Tables

*   **Location**: [ports/port_common.c](file:///C:/Users/rizki/Programming/vfrns/vfr_switch/ports/port_common.c)
*   **Purpose**: Resolves DLCI routes using port-local arrays instead of a global hash table, avoiding global lock contention on the forwarding path.

### Line-by-Line Code Breakdown:

```c
vfr_pvc_t *port_lookup_dlci(vfr_port_t *port, u32 dlci)
{
    if (dlci < 1024) {
        return port->dlci_lut[dlci];
    }
```
*   **What it does**: If the port is configured for 10-bit DLCI mode (or the DLCI is less than 1024), it performs a direct array lookup `port->dlci_lut[dlci]`. Since pointer reading/writing is atomic on 64-bit systems, this lookup is completely lock-free and scales to millions of lookups per second.

```c
    mutex_lock(&port->mutex);
    vfr_pvc_t key_struct;
    key_struct.dlci = dlci;
    vfr_pvc_t *key_ptr = &key_struct;
    
    vfr_pvc_t **res = bsearch(&key_ptr, port->dlci_array, port->dlci_count,
                              sizeof(vfr_pvc_t *), compare_dlci_entries);
    vfr_pvc_t *pvc = res ? *res : NULL;
    mutex_unlock(&port->mutex);
    return pvc;
}
```
*   **What it does**: If the DLCI is 23-bit, it locks the port-local `port->mutex` and performs a binary search (`bsearch`) on the sorted pointer array `dlci_array`. Because the lock is local to the port, parallel lookups on other ports can proceed concurrently without blocking.
