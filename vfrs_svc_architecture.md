# VFRS Switched Virtual Circuit (SVC) Signaling & Routing Architecture

This document specifies the internal architectures, structures, and state machines required to implement **Switched Virtual Circuits (SVCs)** under ITU-T X.36 Clause 10.

---

## 1. Signaling Offloading & Flow (The Link Plane to Control Plane)

The signaling path divides execution cleanly between the high-speed data plane and the control plane:

```
[Ingress Port]
      │ (Raw Packet)
      ▼
[fr_switch_input_processed] ──(DLCI 0, non-UI)──► [spsc_queue_push]
                                                        │
                                           (SPSC Lock-Free Queue)
                                                        │
                                                        ▼
[run_slow_path_thread] ◄───────────────────────── [spsc_queue_pop]
      │
      ├─► [lapf_handle_frame] (Maintains L2 sequencing/timers)
      │         │
      │         └─► (Sequenced Layer 3 Frame Delivered)
      │                   │
      ▼                   ▼
[lapf_l3_recv_cb] ──► [vfr_svc_parse_l3]
                              │
                              ├─► Decodes Message Type (SETUP/CONNECT/etc.)
                              ├─► Matches X.121 Called Prefix
                              ├─► Allocates Egress DLCI via Range Allocator
                              ├─► Updates Fast-Path Port DLCI Table
                              └─► Drives Call State Machine (T303/T310/etc.)
```

---

## 2. Dynamic DLCI Range Allocators

To support 23-bit addressing up to DLCI $8,388,607$ without allocating massive bitmaps (which consume 1 MB of RAM per port), each port uses an **Interval-Based Range Allocator**.

### Data Structures:
```c
#define MAX_FREE_RANGES 32

typedef struct {
    u32 start;
    u32 end;
} vfr_dlci_range_t;

typedef struct {
    vfr_dlci_range_t ranges[MAX_FREE_RANGES];
    int              count;
} vfr_dlci_allocator_t;
```

### Operations:
1.  **Initialization**: At port startup, the allocator is populated with a single range: `[16, 8388607]`.
2.  **Allocation (`port_alloc_dlci`)**:
    *   Scan ranges for the first block.
    *   Extract the first available DLCI (e.g. `start`).
    *   If `start == end`, remove the range from the array (shift remaining left).
    *   If `start < end`, increment `start` by 1.
3.  **Deallocation (`port_free_dlci`)**:
    *   Insert the freed DLCI as a single-unit range `[dlci, dlci]` into the array in sorted order.
    *   Perform a merging pass: if `range[i].end + 1 == range[i+1].start`, merge them into `[range[i].start, range[i+1].end]` and shift remaining left.

---

## 3. X.121 Prefix Routing

When a `SETUP` message arrives, the switch determines the egress port using prefix matching on the **Called Party Number** (X.121 address format).

### Data Structures:
```c
#define MAX_ROUTES 128

typedef struct {
    char prefix[MAX_ADDR_STR];            /* DCC + Network Digit + Subscriber prefix */
    char egress_port_name[MAX_NAME_LEN];  /* Ingress/Egress target port */
} vfr_route_t;

typedef struct {
    vfr_route_t routes[MAX_ROUTES];
    int         count;
} vfr_route_table_t;
```

### Routing Lookup:
*   **Longest Prefix Match (LPM)**: The Called Party Number string is matched against all prefix strings in the table. The entry with the longest matching prefix determines the egress port.

---

## 4. Q.933 Layer 3 Message Format & Parser

Signaling frames delivered by LAPF are formatted according to ITU-T Q.933 Chapter 4.

### Message Header Layout:
```
┌──────────────────────────────┐
│ Protocol Discriminator (0x08)│  Octet 1
├──────────────────────────────┤
│  Call Ref Length (0x01/0x02) │  Octet 2
├──────────────────────────────┤
│   Call Reference Value       │  Octet 3 (and 4 if length = 2)
│  (Flag in MSB: 0=orig, 1=dst)│
├──────────────────────────────┤
│     Message Type (0xXX)      │  Octet 4 (or 5)
└──────────────────────────────┘
```

### Information Elements (IEs):
Each IE starts with a 1-byte **IE Identifier** and a 1-byte **Length Indicator**:
```
┌──────────────────────────────┐
│   IE Identifier (0xXX)       │  Octet 1
├──────────────────────────────┤
│    Length Indicator (0xYY)   │  Octet 2
├──────────────────────────────┤
│        IE Content            │  Octet 3 to 3+YY
└──────────────────────────────┘
```
Key IEs to parse:
*   **Bearer Capability (0x04)**: Checks for Frame Relay carrier service.
*   **Called Party Number (0x70)**: Contains the Called Party X.121 address.
*   **Calling Party Number (0x6C)**: Contains the Calling Party X.121 address.
*   **Link Layer Core Parameters (0x48)**: Contains negotiated throughput metrics (Ingress/Egress CIR, Bc, Be, Frame Size).

---

## 5. SVC Call State Machine (ITU-T X.36 Clause 10 / Q.933)

Active SVC signaling sessions are managed using Call Control Blocks (`vfr_call_t`) mapped to the following standard states:

| Call State | Description | Active Timers | Trigger Event | Next State |
| :--- | :--- | :--- | :--- | :--- |
| **0: NULL** | Call idle / unallocated | None | SETUP received (ingress) | **3: PRESENT** (incoming) |
| | | | SETUP sent (egress) | **1: INITIATED** (outgoing) |
| **1: INITIATED** | Setup sent, awaiting response | T303 (4s) | CALL PROCEEDING received | **2: OUTGOING PROC** |
| | | | CONNECT received | **10: ACTIVE** |
| **2: OUTGOING PROC**| Call proceeding to destination | T310 (35s)| CONNECT received | **10: ACTIVE** |
| **3: PRESENT** | Setup delivered, awaiting DTE | T303 (4s) | CONNECT received from DTE | **10: ACTIVE** (after sending CONNECT to ingress) |
| **10: ACTIVE** | Bidirectional PVC/SVC data active | None | RELEASE received | **19: RELEASE REQ** (send REL_COMP) |
| **19: RELEASE REQ**| Release sent, awaiting complete | T308 (4s) | RELEASE COMPLETE received | **0: NULL** (free resources) |

### Synchronization & Memory Consistency:
When a call transitions to **10: ACTIVE**:
1.  Thread 2 allocates the ingress/egress DLCI pairs.
2.  Thread 2 instantiates two unidirectional `vfr_pvc_t` entries (ingress $\rightarrow$ egress and egress $\rightarrow$ ingress) with `is_static = 0`.
3.  Thread 2 calls `port_add_dlci_entry()` to register them in the ports' local tables.
4.  Memory barriers are executed during pointer updates, allowing Thread 1 to instantly route the data plane traffic lock-free.
