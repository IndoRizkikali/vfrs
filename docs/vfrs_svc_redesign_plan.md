# VFRS Grand Redesign Plan: Scalability, Concurrency, and Switched Virtual Circuits (SVC)

This document outlines the architectural blueprints and mathematical calculations required to redesign the **Virtual Frame Relay Switch (VFRS)** to support next-generation carrier-grade scaling, with a focus on implementing a dynamic **Switched Virtual Circuit (SVC)** signaling and routing engine (ITU-T X.36 Clause 10).

## Redesign Objectives
1.  **Interface Scaling**: $131,072$ interfaces ($2 \text{ types} \times 256 \text{ groups} \times 256 \text{ interfaces}$).
2.  **DLCI Densities**: 10-bit ($1024$ DLCIs/port) and 23-bit ($8,388,608$ DLCIs/port) configured per-port.
3.  **Routing Capacity**: Support up to $2,199,023,255,552$ (2.2 Trillion) theoretical dynamic one-way entries.
4.  **SVC System (X.36 Clause 10)**: Real-time call establishment (SETUP, CONNECT, RELEASE state machines), X.121 numbering/prefix-based routing, and dynamic DLCI allocation.
5.  **Advanced Services**: Lock-free LMI signaling, wiretapping/mirroring, NNI, and CLLM congestion management.

---

## 1. Worst-Case Capacity Calculations & Analysis

We analyze the system requirements under the absolute worst-case scenario: **all interfaces instantiated, and all DLCIs on every interface active and running dynamic SVC calls**.

### 1.1. Core Constants & Structure Sizes
We define the following optimized structures for the redesigned switch:
*   **Port Control Block (`vfr_port_t`)**: $256 \text{ bytes}$ (name, type, status, state variables, Mutex, and file/socket descriptor).
*   **Active Switching Entry (`vfr_pvc_t` / `vfr_svc_t`)**: $64 \text{ bytes}$ (compressed ingress/egress port IDs, output DLCI, active/tap flags).
*   **SVC Call Control Block (`vfr_call_t`)**: $128 \text{ bytes}$ (call reference value, signaling state, src/dst port IDs, src/dst DLCIs, src/dst X.121 address strings, and signaling timers).
*   **Pointer Size**: $8 \text{ bytes}$ (64-bit systems).

---

### 1.2. Scenario A: 10-bit-Only DLCI Addressing Mode

In this mode, every port is configured for 10-bit addressing, restricting DLCIs to the $0 - 1023$ range.

#### Mathematical Calculations:
*   **Total Interfaces ($I$)**: $131,072$
*   **DLCIs per Interface ($D_{10}$)**: $1,024$
*   **Maximum Active Endpoints ($E_{10}$)**:
    $$E_{10} = I \times D_{10} = 131,072 \times 1,024 = 134,217,728 \text{ active connections}$$
*   **Maximum Active Call Sessions ($C_{10}$)**:
    Since each call connects two endpoints (one-way mapping requires 2 entries per call):
    $$C_{10} = \frac{E_{10}}{2} = 67,108,864 \text{ active calls}$$
*   **Lookup Table Memory (Direct Array per Port)**:
    Each port allocates a flat array of 1,024 pointers to active switching entries.
    $$\text{Lookup RAM per Port} = 1,024 \text{ pointers} \times 8 \text{ bytes} = 8 \text{ KB}$$
    $$\text{Total Lookup RAM} = 131,072 \times 8 \text{ KB} = 1,048,576 \text{ KB} = 1.00 \text{ GB}$$
*   **Switching Entries Memory**:
    $$\text{Total Entry RAM} = 134,217,728 \times 64 \text{ bytes} = 8,589,934,592 \text{ bytes} = 8.00 \text{ GB}$$
*   **Call Control Blocks (CCB) Memory**:
    Allocated dynamically per active call session:
    $$\text{Total CCB RAM} = 67,108,864 \times 128 \text{ bytes} = 8,589,934,592 \text{ bytes} = 8.00 \text{ GB}$$
*   **Port Control Blocks Memory**:
    $$\text{Total Port RAM} = 131,072 \times 256 \text{ bytes} = 32.00 \text{ MB}$$

#### Scenario A Memory Totals:
$$\text{Total System Memory (Scenario A)} = 1.00 \text{ GB} + 8.00 \text{ GB} + 8.00 \text{ GB} + 0.03 \text{ GB} \approx \mathbf{17.03\text{ GB}}$$

#### Scenario A Analysis:
*   **Feasibility**: **Highly Feasible** on a single modern server.
*   **Throughput**: Dynamic call setup and teardown rate must be handled efficiently. Offloading the Q.933 signaling message parsing to the slow path ensures data-plane switching remains uninterrupted.

---

### 1.3. Scenario B: 23-bit-Only DLCI Addressing Mode

In this mode, every interface is configured for 23-bit addressing, enabling DLCIs up to $8,388,607$.

#### Mathematical Calculations:
*   **Total Interfaces ($I$)**: $131,072$
*   **DLCIs per Interface ($D_{23}$)**: $8,388,608$
*   **Maximum Active Endpoints ($E_{23}$)**:
    $$E_{23} = I \times D_{23} = 131,072 \times 8,388,608 = 1,099,511,627,776 \text{ active connections (1.1 Trillion)}$$
*   **One-Way Routing Entries (Bidirectional Worst Case)**:
    $$\text{Total Table Entries} = 2 \times 1,099,511,627,776 = \mathbf{2 ,199,023,255,552\text{ entries (2.2 Trillion)}}$$
*   **Maximum Active Call Sessions ($C_{23}$)**:
    $$C_{23} = \frac{E_{23}}{2} = 549,755,813,888 \text{ active calls (550 Billion)}$$
*   **Lookup Table Memory (Direct Array per Port - UNFEASIBLE)**:
    $$\text{Total Lookup RAM} = 131,072 \times 64 \text{ MB} = \mathbf{8.00\text{ TB}}$$
*   **Switching Entries Memory**:
    $$\text{Total Entry RAM} = 2.2 \times 10^{12} \times 64 \text{ bytes} = \mathbf{140.73\text{ TB}}$$
*   **Call Control Blocks (CCB) Memory**:
    $$\text{Total CCB RAM} = 550 \times 10^9 \times 128 \text{ bytes} = \mathbf{70.36\text{ TB}}$$

#### Scenario B Memory Totals:
$$\text{Total System Memory (Scenario B)} = 8.00 \text{ TB (Lookup)} + 140.73 \text{ TB (Entries)} + 70.36 \text{ TB (Call States)} = \mathbf{219.09\text{ TB}}$$

#### Scenario B Analysis:
*   **Feasibility**: **Physically Unfeasible** on a single machine. The memory footprint exceeds available SMP RAM capacities.
*   **Remediation Requirements**:
    1.  **Distributed Call Processing**: Distribute interfaces and signaling state machines across a cluster of compute nodes.
    2.  **Sparse Allocation**: Active calls per port will realistically not exceed a few thousands. With $1\%$ active call density, memory footprint drops to **2.19 TB** for the entire cluster.
    3.  **Dynamic DLCI Range Allocators**: Instead of huge allocation bitmaps (which would take 131 GB of RAM), use range-based free-lists (splitting ranges dynamically on allocation) to track free DLCIs.

---

### 1.4. Scenario C: Mixed 10-bit and 23-bit DLCI Addressing Mode

In this mode, the DLCI addressing mode is configured on a per-port basis. We analyze a worst-case hybrid deployment where **$90\%$ of interfaces are configured for 10-bit mode, and $10\%$ are configured for 23-bit mode**.

#### Mathematical Calculations:
*   **10-bit Interfaces ($I_{10}$)**: $117,965$ ports.
*   **23-bit Interfaces ($I_{23}$)**: $13,107$ ports.
*   **Active Endpoints ($E_{\text{mixed}}$)**:
    $$E_{10} = 117,965 \times 1,024 = 120,796,160 \text{ active connections}$$
    $$E_{23} = 13,107 \times 8,388,608 = 109,947,568,128 \text{ active connections}$$
    $$E_{\text{mixed}} = 120,796,160 + 109,947,568,128 = 110,068,364,288 \text{ active connections}$$
*   **Active Call Sessions ($C_{\text{mixed}}$)**:
    $$C_{\text{mixed}} = \frac{E_{\text{mixed}}}{2} = 55,034,182,144 \text{ active calls}$$
*   **Lookup Table Memory (Hybrid Scheme)**:
    *   *10-bit ports* use flat pointer arrays: $117,965 \times 8 \text{ KB} = 943.72 \text{ MB}$.
    *   *23-bit ports* use a per-port hash table of 2,048 buckets: $13,107 \times 16 \text{ KB} = 209.71 \text{ MB}$.
    *   *Total Lookup RAM (Hybrid)*: $943.72 \text{ MB} + 0.21 \text{ GB} = \mathbf{1.15\text{ GB}}$
*   **Switching Entries Memory**:
    $$\text{Total Entry RAM} = 220,136,728,576 \times 64 \text{ bytes} = \mathbf{14.08\text{ TB}}$$
*   **Call Control Blocks (CCB) Memory**:
    $$\text{Total CCB RAM} = 55,034,182,144 \times 128 \text{ bytes} = \mathbf{7.04\text{ TB}}$$

#### Scenario C Memory Totals:
$$\text{Total System Memory (Scenario C)} = 1.15 \text{ GB (Lookup)} + 14.08 \text{ TB (Entries)} + 7.04 \text{ TB (Call States)} \approx \mathbf{21.12\text{ TB}}$$

---

## 2. Carrier-Grade SVC Redesign Architecture

```mermaid
graph TD
    subgraph Ingress Processing (DPDK Line Card)
        Port_Rx[Port Physical Ingress] --> Parsing[Q.922 Frame Parsing]
        Parsing --> Demux{Demux DLCI}
        Demux -- DLCI 0 / 1023 --> Sig_Queue[Signaling Ring Buffer]
        Demux -- User DLCI --> Mode_Check{Check Port DLCI Mode}
        Mode_Check -- 10-bit --> Flat_Lookup[Direct Array O(1) Lookup]
        Mode_Check -- 23-bit --> Hash_Lookup[Local Port Hash Table Lookup]
    end

    subgraph Switching Core
        Flat_Lookup --> Engine[Redesigned Switching Engine]
        Hash_Lookup --> Engine
        Engine --> Rule_Eval{Evaluate Route Type}
        Rule_Eval -- Standard --> Unicast[Normal Rewrite & Send]
        Rule_Eval -- Tap / Snooper --> Wiretap[Snoop Copy & Send / Drop Ingress]
        Rule_Eval -- Multicast --> MCast[Replicate to Group]
    end

    subgraph Operations & Control Plane (Slow Path)
        Sig_Queue --> Sig_Engine[Q.933 Signaling Engine]
        Sig_Engine --> Route_Lookup[X.121 Route Lookup]
        Route_Lookup --> Allocator[Dynamic DLCI Range Allocator]
        Allocator --> Call_SM[Call State Machine Manager]
        Call_SM -->|Update Routing| Flat_Lookup
        Call_SM -->|Update Routing| Hash_Lookup
    end
```

### 2.1. Dynamic DLCI Range Allocators
To support 23-bit DLCIs without allocating large bitmaps (which consume 1 MB per port, or 131 GB total), VFRS uses a **Range-Based Allocator (Interval Tree)** per port.
*   Initially, the port contains a single free range: `[16, 8388607]`.
*   When a SETUP request allocates a DLCI, the allocator splits the range.
*   This drops the DLCI allocation RAM to less than **$100\text{ bytes}$ per port** under normal loads, scaling dynamically.

### 2.2. Lock-Free and RCU-Protected Lookup
By separating signaling (Slow Path) and switching (Fast Path), lookups run completely lock-free:
*   **10-bit lookup**: Direct array access `atomic_load(&port->pvc_lut[dlci])`.
*   **23-bit lookup**: Per-port local hash table with linear probing, read lock-free under RCU.
*   *Control Plane Writes*: Call state transitions (e.g., call teardown) remove the routing entry using atomic writes, followed by a synchronization barrier (`synchronize_rcu()`) before freeing the switching block.

### 2.3. Wiretapping & Port Mirroring Core Design
The dynamic switching entry supports multi-destination replication chains:
*   **Replication Entry**:
    ```c
    typedef struct vfr_destination_s {
        u16 dst_port_id;
        u32 dst_dlci;
        u8 is_tap;  /* 1 = read-only listener, 0 = active peer */
        struct vfr_destination_s *next;
    } vfr_destination_t;
    ```
*   **Ingress Guarding**: If the ingress port/DLCI is flagged as a snooper, the packet is instantly dropped. A tap destination cannot inject traffic into the bidirectional channel.

### 2.4. Q.933 Signaling Offloading (Slow Path)
To prevent signaling traffic (DLCI 0) from affecting packet forwarding:
1.  **Fast Path Demux**: Frame parser identifies frames with DLCI 0 (or 1023) and writes them to a Single Producer Single Consumer (SPSC) queue.
2.  **Slow Path Processing**: The slow-path engine pops signaling frames, parses the variable-length Q.933 Information Elements, processes the Call State Machine (SETUP $\rightarrow$ Call Routing $\rightarrow$ Egress allocation $\rightarrow$ CONNECT), and updates the fast-path lookup table using atomic operations.

---

## 3. Commodity Hardware Optimization (Celeron / 8 GB RAM)

For local development and testing on a dual-core Intel Celeron CPU with 8 GB RAM, we optimize VFRS to run as a single lightweight process.

### 3.1. Physical Bounds of Commodity Hardware
*   **Target Memory Ceiling**: $\le 512 \text{ MB}$ of RAM allocated for the entire VFRS process.
*   **Port Scaling Limits**: Active simulated interfaces (UDP/TCP/pipes) are restricted to $P_{\text{active}} \le 1,024$.

---

### 3.2. Worst-Case Memory Calculations (Commodity SVC)

We analyze a worst-case commodity simulation: **1,024 active ports, each configured with 1,024 active calls (524,288 active call sessions / 1,048,576 endpoints total)**.

#### Lookup Table Memory:
*   Flat sorted array of active PVC/SVC pointers. Cap size of 1,024 pointers per port.
    $$\text{Total Lookup RAM} = 1,024 \text{ ports} \times 8 \text{ KB} = \mathbf{8.19\text{ MB}}$$

#### Switching Entries & Call Control Blocks Memory:
*   $$\text{Switching Entries RAM} = 1,048,576 \times 64 \text{ bytes} = \mathbf{67.10\text{ MB}}$$
*   $$\text{Call Control Blocks RAM} = 524,288 \times 128 \text{ bytes} = \mathbf{67.10\text{ MB}}$$
*   **Port Control Blocks RAM**:
    $$\text{Port RAM} = 1,024 \text{ ports} \times 256 \text{ bytes} \approx \mathbf{0.25\text{ MB}}$$

#### Commodity Memory Total:
$$\text{Total System Memory (Commodity SVC)} = 8.19 \text{ MB} + 67.10 \text{ MB} + 67.10 \text{ MB} + 0.25 \text{ MB} = \mathbf{142.64\text{ MB}}$$
*   **Verdict**: This represents less than **$1.8\%$ of the available 8 GB RAM**, making it extremely safe for commodity laptops.

---

### 3.3. CPU & Packet Processing Budget (1 Gbps Wire-Speed)

On a dual-core Celeron CPU running at 2.0 GHz, we calculate the processing budget:
*   **Gigabit Ethernet (64-byte Frames)**: Requires processing **$1,488,095\text{ pps}$**. This provides a budget of **$1,344\text{ cycles per packet}$**. Standard user-kernel context switching will saturate the CPU.
*   **Gigabit Ethernet (512-byte Frames)**: Requires processing **$234,962\text{ pps}$**. This provides a budget of **$8,512\text{ cycles per packet}$**, which is highly manageable in user space.
*   **Signaling Overhead**: In a typical simulated lab, call setup rates are low ($\le 100\text{ setups/sec}$). Parsing Q.933 signaling frames requires around $1,500$ cycles per message. By offloading this to Thread 2, we completely isolate the switching fast-path from signaling overhead.

---

### 3.4. Simplified Commodity Redesign Blueprint

We implement a lightweight, dual-thread, IOCP-driven single-process layout:

#### 1. Flat Sorted Array Lookup (23-bit Ports):
2.3-bit ports store active PVC/SVC pointers in a flat contiguous array sorted by DLCI.
*   Lookups use `bsearch()`.
*   Since the array is contiguous and at most 1,024 elements (8 KB), **the entire lookup table fits in the CPU's L1 cache**. Lookup executes in less than **100 CPU cycles** with zero cache misses.

#### 2. Dual-Thread Concurrency Model:
We restrict VFRS to exactly **2 system threads**:
1.  **Thread 1 (Fast Path)**: Dedicated to IOCP/epoll socket polling, looking up PVCs/SVCs, and forwarding data frames. It operates completely lock-free.
2.  **Thread 2 (Slow Path)**: Dedicated to Q.933 signaling message parsing, X.121 route lookup, LMI polling timers, and CLI console inputs.
3.  *Thread Inter-communication*: Use a single lock-free Single Producer Single Consumer (SPSC) ring buffer to pass signaling frames (DLCI 0) from Thread 1 to Thread 2. When Thread 2 establishes a call, it atomically inserts the new switching entry into the port's sorted array and re-sorts it.

#### 3. Range-Based Allocator (Commodity Version):
Instead of complex tree structures, the Celeron-optimized range allocator uses a small flat array of up to 32 free ranges per port (e.g. `[16, 200]`, `[250, 8388607]`).
*   Searching and merging ranges is done linearly in user space.
*   Since the number of ranges is small ($\le 32$), the operations run in a few dozen cycles, with a negligible memory footprint of less than **$256\text{ bytes}$ per port**.
