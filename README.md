# VFRS (Virtual Frame Relay Switch)

VFRS is a C-based software Frame Relay switch that emulates core UNI/NNI behavior, DLCI switching, LMI signaling, congestion management, multicast replication, and SVC call control (Q.933 / ITU-T X.36 and X.76 oriented workflows).

The project includes:
- A runtime binary (`vfrs.exe`) and C source code for the switching engine.
- A text-based configuration system with multi-pass validation.
- Multiple transport backends (UDP/TCP/serial/named pipe/L2TPv3 draft path).
- Test programs and scripts for frame encoding and SVC signaling behavior.

---

## Table of Contents

- [1. What VFRS Does](#1-what-vfrs-does)
- [2. Core Capabilities](#2-core-capabilities)
- [3. High-Level Architecture](#3-high-level-architecture)
- [4. Repository and Codebase Layout](#4-repository-and-codebase-layout)
- [5. Build and Runtime Requirements](#5-build-and-runtime-requirements)
- [6. Build Instructions](#6-build-instructions)
- [7. Running VFRS](#7-running-vfrs)
- [8. Command-Line Options](#8-command-line-options)
- [9. Configuration System](#9-configuration-system)
- [10. Configuration Command Reference](#10-configuration-command-reference)
- [11. Interactive Console Commands](#11-interactive-console-commands)
- [12. Internal Switching and Control Flow](#12-internal-switching-and-control-flow)
- [13. Testing and Validation](#13-testing-and-validation)
- [14. Logging and Capture](#14-logging-and-capture)
- [15. Troubleshooting](#15-troubleshooting)
- [16. Development Notes](#16-development-notes)

---

## 1. What VFRS Does

At runtime, VFRS accepts Frame Relay frames from configured interfaces and switches them according to DLCI mappings and call-state outcomes:

- **PVC path**: static, preconfigured two-way virtual circuits.
- **SVC path**: dynamic call setup/teardown using Q.933 signaling over LAPF.
- **LMI path**: link management and PVC status signaling for UNI/NNI operation.
- **Congestion path**: token-bucket traffic conditioning plus CLLM signaling.

It is designed as a virtual network element for lab, interoperability, and protocol-behavior testing rather than end-user application traffic services.

---

## 2. Core Capabilities

### 2.1 Frame Relay Data Plane
- Parses and rewrites Frame Relay addresses (10-bit and extended addressing cases).
- Validates FCS (CRC-16) and handles HDLC framing/stuffing paths.
- Maintains per-port and global DLCI lookup structures for forwarding.

### 2.2 Transport Interfaces
`port` supports:
- `udp`, `udp-client`, `udp-server`
- `tcp`, `tcp-client`, `tcp-server`
- `serial`
- `pipe-server`, `pipe-client`
- `l2tpv3-fr`, `l2tpv3-hdlc` (draft/roadmap implementation path)

### 2.3 PVCs
- Bi-directional PVC creation from one config line.
- Optional service parameters (`cir`, `bc`, `be`, `ftp`, `fdp`, `srvcls`).

### 2.4 LMI (Local Management Interface)
- ANSI (`ansi`), Q.933 Annex A (`q933a`), Cisco/GoF (`cisco`), or `none`.
- DCE and DTE behaviors, including `lmi_dte` polling controls.
- NNI behavior constrains active LMI type to Q.933A/none in parser flow.

### 2.5 SVC Signaling and Routing
- `svc_int` enables per-port SVC control contexts.
- `svc_addr` registers subscriber numbering and aliases.
- `svc_route` provides prefix/structural route mapping to egress ports.
- Uses timers and call control state machinery for setup/proceed/connect/release workflows.

### 2.6 Congestion Management and CLLM
- Per-port congestion controls via thresholded frame-rate/write-failure signals.
- CLLM command (`cllm`) parsed in final pass to override defaults explicitly.

### 2.7 Multicast
- Group + members with one-way/two-way/n-way replication models.

### 2.8 Capture and Observability
- Per-port or global PCAP output (`capture`).
- Runtime `show ...` console commands for config/status/stats/SVC views.

---

## 3. High-Level Architecture

### 3.1 Two-Plane Runtime

VFRS runs with a split model:

1. **Fast path (main loop)**
   - Polls active ports.
   - Receives frames and feeds the switching input path.
2. **Slow path (control-plane thread)**
   - Processes signaling/control frames from an SPSC queue.
   - Runs LMI/LAPF/SVC/congestion timer polling at ~100ms cycle.

### 3.2 Shared Context

`vfrs_ctx_t` (in `vfr.h`) holds:
- Port registry
- PVC and global DLCI tables
- Multicast groups
- SVC subscribers/routes
- Defaults and timers
- SPSC signaling queue and slow-path thread handle

### 3.3 Locking and Ordering

Code enforces lock ordering in debug builds:
`port_mutex -> pvc_mutex -> dlci_mutex -> mcast_mutex -> svc_mutex`

This is encoded in macros and used throughout critical sections.

---

## 4. Repository and Codebase Layout

Top-level core:
- `/home/runner/work/vfrs/vfrs/vfrs_main.c` – entrypoint, argument parsing, config parsing, main loops, console command processing.
- `/home/runner/work/vfrs/vfrs/vfr.h` – central shared types, constants, APIs, and subsystem interfaces.
- `/home/runner/work/vfrs/vfrs/config.c`, `config.h` – config reader/tokenizer/parsing helpers.
- `/home/runner/work/vfrs/vfrs/logger.c` – logger and rotation support.

Subsystem directories:
- `/home/runner/work/vfrs/vfrs/fr_switching/`
  - `fr_switch.c` – switch context, PVC/DLCI operations, forwarding logic, show commands.
  - `fr_frame.c` – frame parsing/building, CRC/FCS, address encode/decode.
- `/home/runner/work/vfrs/vfrs/ports/`
  - transport-specific port implementations (`udp`, `tcp`, `serial`, `pipe`, `l2tpv3`).
  - `lapf/` for LAPF handling.
  - `pcap/` for packet capture writing.
  - `svc_numbering/` for subscriber numbering logic.
- `/home/runner/work/vfrs/vfrs/pvc/`
  - LMI protocol handlers and multicast PVC handling.
- `/home/runner/work/vfrs/vfrs/congestion_mgnt/`
  - congestion detection, token bucket management, CLLM parser/builder.
- `/home/runner/work/vfrs/vfrs/svc_signalling/`
  - Q.933 IE parser/builder and UNI/NNI call signaling flows.

Testing and examples:
- `/home/runner/work/vfrs/vfrs/tests/` – C/Python tests and helper scripts.
- `/home/runner/work/vfrs/vfrs/scratch/` – additional diagnostics/integration experiments.
- `/home/runner/work/vfrs/vfrs/example_config.conf` – extensive commented config reference.
- `/home/runner/work/vfrs/vfrs/vfrsTestSvc.conf` – SVC-oriented test config.

---

## 5. Build and Runtime Requirements

### 5.1 Compiler/Tooling
- GCC toolchain
- `make`
- Python 3 (for Python-based test scripts)

### 5.2 Platform Notes
- Current `Makefile` links with Windows-specific libraries (`-lws2_32 -lwinmm -ladvapi32`) and outputs `vfrs.exe`.
- Source also includes POSIX branches (`poll`, `pthread`, etc.), but stock Makefile is tuned for Windows/MSYS2 UCRT64 style builds.

### 5.3 Runtime Inputs
- A config file is optional for startup but required for practical switching behavior.
- Interface endpoints (sockets/pipes/serial) must be reachable and matched by peer configuration.

---

## 6. Build Instructions

From repository root `/home/runner/work/vfrs/vfrs`:

```bash
make clean
make
```

Other targets:

```bash
make debug   # Debug flags, non-static link profile in Makefile
make check   # Builds debug target then runs pipe loopback shell wrapper
make test    # Builds + runs ie_test.exe and svc_compliance_test.py
```

Binary target from Makefile:
- `vfrs.exe`

---

## 7. Running VFRS

### 7.1 Basic launch

```bash
./vfrs.exe /absolute/or/relative/path/to/config.conf
```

### 7.2 Console mode (interactive commands)

```bash
./vfrs.exe -c /path/to/config.conf
```

### 7.3 Configuration dry-run and validation

```bash
./vfrs.exe -t /path/to/config.conf
```

This performs parse + cross-validation and prints summary totals (ports, PVCs, SVC subscribers/routes, etc.).

---

## 8. Command-Line Options

From `print_usage` and argument parsing:

- `-h`, `--help` – show usage.
- `-t`, `--check-config`, `--test-config` – parse/validate config and exit.
- `-d`, `--debug` – elevate console/file log levels.
- `-c`, `--console` – console mode (no log file by default).
- `-s`, `--log-size <mb>` – max log size in MB.
- `-n`, `--log-files <n>` – max rotated log file count.

---

## 9. Configuration System

### 9.1 Parsing model

Configuration is parsed in **three passes**:

1. **Pass 1**: global defaults and logging (`swconfig`, `log_level`, `log_file`, `log_rotation`, `defaults`).
2. **Pass 2**: interfaces and service objects (`port`, `pvc`, `lmi`, `lmi_dte`, `capture`, `congestion`, `lapf`, `mcast`, `mcast_member`, `svc_int`, `svc_addr`, `svc_route`).
3. **Pass 3**: `cllm` commands only (explicit override behavior).

### 9.2 General syntax rules

- `#` starts comments.
- Tokens are whitespace-separated.
- Quoted tokens are supported.
- `key=value` parameters are parsed by helper logic.
- `\` line continuation is supported for long statements.

---

## 10. Configuration Command Reference

Below is the effective command surface implemented in `vfrs_main.c` and `config.c`.

### 10.1 Global / identity
- `swconfig swid=<id> dnic=<num> ...`

### 10.2 Logging
- `log_level con=<trace|debug|info|warn|error> [txt=...]`
- `log_file <path>`
- `log_rotation size=<mb> files=<n>`

### 10.3 Defaults
- `defaults <key=value> ...`
- Supports LMI/LAPF/SVC timers and QoS defaults (e.g. `lapf_t200`, `lmi_t392`, `svc_default_cir`, etc.).

### 10.4 Port definition
- `port <name> udp ...`
- `port <name> udp-client ...`
- `port <name> udp-server ...`
- `port <name> tcp ...`
- `port <name> tcp-client ...`
- `port <name> tcp-server ...`
- `port <name> serial ...`
- `port <name> pipe-server <name>`
- `port <name> pipe-client <name>`
- `port <name> l2tpv3-fr ...`
- `port <name> l2tpv3-hdlc ...`

Optional key-value on ports includes `dlcibit` and `pcap`.

### 10.5 PVC
- `pvc <port1> <dlci1> <port2> <dlci2> [cir=...] [bc=...] [be=...] [ftp=...] [fdp=...] [srvcls=...]`

### 10.6 LMI
- `lmi <port> [q933a|ansi|cisco|none] [t392=...] [n392=...] [n393=...] [async=true|false]`
- `lmi_dte <port> [t391=...] [n391=...] [n392=...] [n393=...]`

### 10.7 Capture
- `capture <port|all> <filename> [svc=iframe|ui]`

### 10.8 Congestion / CLLM
- `congestion <port> rate=<fps> [clear=<fps>] [threshold=<n>] [cllm=on|off] [access_rate=<bps>]`
- `cllm <port> [tx=<seconds>]`

### 10.9 LAPF
- `lapf <port> [k=...] [n200=...] [n201=...] [t200=...] [t203=...] [dlci=...] [role=active]`

### 10.10 Multicast
- `mcast|mcast_group <group> <source_port> <source_dlci> [oneway|twoway|nway] [cir=...] [bc=...] [be=...]`
- `mcast_member <group> <member_port> <member_dlci>`

### 10.11 SVC
- `svc_int <port> [dlci_low=...] [dlci_high=...] [timers/defaults/... ]`
- `svc_addr <port> [manual|autoprefix] [x121|e164] <number> [alias=x121|e164,<alias>] [rev_charge_acc=0|1] [rev_charge_prev=0|1]`
- `svc_route ...` (prefix or structural route forms)

For real examples and extensive comments, use:
- `/home/runner/work/vfrs/vfrs/example_config.conf`
- `/home/runner/work/vfrs/vfrs/vfrsTestSvc.conf`

---

## 11. Interactive Console Commands

When launched in console mode (`-c`), commands include:

- `show config`
- `show defaults`
- `show swconfig`
- `show ports`
- `show pvc`
- `show stats` / `show stats <port>`
- `show svc calls`
- `show svc call <crv>`
- `show svc stats`
- `show svc subscribers`
- `show svc routes`
- `pvc add ...`
- `pvc del ...`
- `lmi set <port> type <ansi|q933a|cisco>`
- `congestion set <port> cir=<n> [bc=<n>] [be=<n>]`
- `svc restart <port>`
- `svc clear <port> <crv>`
- `reload <config_file>`
- `clear stats [<port>]`
- `help`
- `quit` / `exit`

---

## 12. Internal Switching and Control Flow

### 12.1 Fast path (`run_main_loop`)
- Build pollfd list from active ports.
- Poll with 100ms timeout.
- Receive frame from ready ports.
- Send frame into `fr_switch_input`.

### 12.2 Slow path (`run_slow_path_thread`)
- Pop control frames from SPSC queue.
- Route control/signaling through `fr_switch_input_processed`.
- Poll LMI, congestion, LAPF, and SVC timers.

### 12.3 Data structures and lookups
- Global DLCI hash table (`ctx->dlci_table`) keyed by `(port, dlci)` hash.
- Port-local direct lookup array for 10-bit and dynamic array for larger ranges.
- PVC table and multicast state kept in dedicated structures with mutex control.

---

## 13. Testing and Validation

### 13.1 Config validation (recommended first)

```bash
./vfrs.exe -t /home/runner/work/vfrs/vfrs/example_config.conf
```

### 13.2 Make targets

```bash
make check
make test
```

### 13.3 Included tests
- `/home/runner/work/vfrs/vfrs/tests/ie_test.c`
  - Unit checks for Q.933 IE encoding/parsing.
- `/home/runner/work/vfrs/vfrs/tests/loopback_test.py`
  - Named-pipe loopback forwarding behavior.
- `/home/runner/work/vfrs/vfrs/tests/svc_compliance_test.py`
  - SVC signaling compliance scenarios over named pipes.
- `/home/runner/work/vfrs/vfrs/tests/svc_test.py`
  - Additional SVC feature and value-added IE checks.

Note: Python integration tests are oriented to environments that support the configured named-pipe behavior used by these scripts.

---

## 14. Logging and Capture

### 14.1 Log behavior
- Console and file levels are independently configurable.
- Default file path in normal mode is `vfrs.log` unless changed.
- Rotation configurable by config (`log_rotation`) and CLI (`-s`, `-n`).

### 14.2 PCAP capture
- `capture all <file>` for global capture.
- `capture <port> <file>` for per-port capture.
- Port line option `pcap=...` can also enable capture during port setup.

---

## 15. Troubleshooting

- **Config parse errors**: run `-t` mode and fix the referenced line.
- **No traffic switching**: verify port endpoints and matching peer addresses/ports.
- **LMI not up**: confirm LMI type/timers and UNI/NNI role assumptions.
- **SVC calls not routing**: confirm both `svc_addr` registration and matching `svc_route` entries.
- **No output in console mode**: ensure `-c` is used and process has interactive stdin.
- **Unexpected drops**: inspect congestion/CLLM settings and per-port stats.

---

## 16. Development Notes

- Core shared API and structures are centralized in `vfr.h`.
- Config parsing is intentionally strict for command format and parameter validity ranges.
- The switch uses lock ordering assertions in debug builds to catch mutex misuse.
- Existing markdown docs in repository (`vfrs_svc_architecture.md`, optimization and implementation plans) provide deeper design evolution context.

---

## Quick Start Example

1. Build:

```bash
cd /home/runner/work/vfrs/vfrs
make
```

2. Validate config:

```bash
./vfrs.exe -t /home/runner/work/vfrs/vfrs/example_config.conf
```

3. Run with config:

```bash
./vfrs.exe /home/runner/work/vfrs/vfrs/example_config.conf
```

4. Run in interactive console mode:

```bash
./vfrs.exe -c /home/runner/work/vfrs/vfrs/example_config.conf
```

