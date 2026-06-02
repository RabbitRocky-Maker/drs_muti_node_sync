# DRS High-Precision User-Space Synchronization
## Phase 1 — Requirements Critique & Architecture Anchor

---

## 1. Extracted & Numbered Requirements

### Functional Requirements (F)

| ID | Requirement | Source |
|----|-------------|--------|
| **F-1** | The system shall synchronize a global time base across all cluster nodes to a precision of < 100 μs. | §1 |
| **F-2** | Nodes shall discover each other autonomously via the network, requiring zero manual configuration of addresses or roles. | §1 |
| **F-3** | The synchronization protocol shall run entirely in Linux user-space; no kernel modules or drivers shall be used. | §1 |
| **F-4** | The system shall maintain a virtual global clock in user-space without modifying the Linux system clock (`CLOCK_REALTIME`) or the hardware RTC. | §2 |
| **F-5** | The system shall dynamically detect and filter network latency outliers (particularly from Wi-Fi CSMA/CA jitter). | §2 |
| **F-6** | The system shall implement leader election so that if the current primary time reference crashes, remaining nodes elect a new one and maintain synchronization. | §2 |
| **F-7** | The system shall implement structured error handling: each detectable error must have a defined root cause, a system-level action, and a storage/indication strategy. Logging to stdout alone is not acceptable. | §2 |
| **F-8** | The system shall pass Test Scenario 1: 3 Ethernet nodes, synchronized for 10 minutes with precision logged. | §3 |
| **F-9** | The system shall pass Test Scenario 2: a 3rd node plugged in after initial 2-node sync integrates seamlessly without disrupting existing sync. | §3 |
| **F-10** | The system shall pass Test Scenario 3: power loss of the primary node does not break synchronization of the remaining nodes. | §3 |
| **F-11** | The system shall pass Test Scenario 4: the cluster operates over saturated Wi-Fi and degradation is measured and bounded. | §3 |
| **F-12** | The system shall include an external, non-intrusive verification mechanism to objectively prove < 100 μs physical simultaneity, independent of the protocol's own software timestamps. | §4 |

---

### Non-Functional Requirements (NF)

| ID | Requirement | Source |
|----|-------------|--------|
| **NF-1** | Synchronization precision: steady-state offset between any two nodes shall be < 100 μs (P99 or mean — **undefined**, see Open Questions). | §1 |
| **NF-2** | The protocol shall comply with the KISS principle: minimal components, no unnecessary complexity. | §1 |
| **NF-3** | The virtual global clock shall be monotonic from the perspective of all local consumers; no backward jumps. | §2 |
| **NF-4** | The system shall be resilient to asymmetric network delay (different one-way delays in each direction). | §2 |
| **NF-5** | The introduction of debug/monitoring instrumentation shall not measurably degrade synchronization precision (anti-probe-effect design). | §2 |
| **NF-6** | Leader failover shall complete within a bounded time window — **duration unspecified** (see Open Questions). | §2, §3 |
| **NF-7** | The system shall operate on commodity ARM hardware (Raspberry Pi class), subject to Linux scheduler jitter and limited clock hardware. | §2 |
| **NF-8** | The transport protocol shall be UDP over IPv4/IPv6 on mixed Ethernet/Wi-Fi networks. | Context |
| **NF-9** | The software shall be deployable simultaneously across multiple nodes without manual per-node configuration. | §2 |
| **NF-10** | Error states shall be persisted or signalled through a durable, out-of-band mechanism (not lost on crash). | §2 |
| **NF-11** | The system clock discipline shall use `CLOCK_MONOTONIC` or `CLOCK_MONOTONIC_RAW` as its reference to avoid NTP-induced jumps in the local system clock. | §2 (implied) |

---

### Usability / Deployment Requirements (U)

| ID | Requirement | Source |
|----|-------------|--------|
| **U-1** | The software shall be distributable and deployable to all cluster nodes via a single automated mechanism (e.g., script, container, package). | §2 |
| **U-2** | Node configuration (e.g., sync interval, jitter thresholds) shall be manageable via a single, shared configuration artifact (file, environment variable, or compiled-in defaults). | §2 (implied) |
| **U-3** | The system shall provide a non-intrusive monitoring interface that allows inspection of node state and sync offset without interfering with timing-critical paths (no `printf` on hot paths). | §2 |
| **U-4** | The external verification procedure shall be documented and reproducible by a third party. | §4 |

---

## 2. Missing Requirements

These are **entirely absent** from the briefing but are architecturally critical:

| Gap | Implication |
|-----|-------------|
| **M-1: Cluster size bounds** | Is this 3–5 nodes, or 50+? Gossip protocols scale differently than broadcast. |
| **M-2: Precision metric definition** | Is < 100 μs the *mean* offset, *max*, *P99*, or *RMS*? These lead to radically different filter designs. |
| **M-3: Sync convergence time** | How long may it take for a new node to reach < 100 μs after joining? No bound is given. |
| **M-4: Failover time budget** | How long is synchronization allowed to degrade during leader election? |
| **M-5: Network topology** | Is this a flat LAN? Can nodes be on different subnets? This affects discovery (UDP broadcast vs. multicast). |
| **M-6: Acceptable steady-state drift rate** | Between sync exchanges, local oscillators drift. What is the maximum tolerable inter-sync drift? |
| **M-7: Security model** | Is the network trusted? A rogue node broadcasting false timestamps could corrupt the entire cluster. |
| **M-8: Application consumer interface** | How does application code *read* the virtual global time? A C API? A shared-memory segment? A local socket? |
| **M-9: Operating conditions for Wi-Fi** | "Saturated" is unquantified. What packet loss rate and jitter magnitude must be tolerated? |
| **M-10: Clock hardware diversity** | Do all Pis use the same crystal oscillator quality? RPi 3 vs. RPi 4 have different clock stability characteristics. |

---

## 3. Inconsistent or Contradictory Requirements

| ID | Conflict |
|----|----------|
| **IC-1: KISS vs. Resilience** | F-6 (leader election) and F-5 (jitter filtering) require sophisticated algorithms (Raft-like election, Kalman/median filters). This is in direct tension with NF-2 (KISS). A naive implementation will violate one. You must pick a complexity ceiling explicitly. |
| **IC-2: Zero configuration vs. Determinism** | F-2 (zero-config autodiscovery) using UDP broadcast/multicast is fundamentally unreliable on Wi-Fi (broadcast frames are not acknowledged). Zero-config and Wi-Fi resilience (F-11) are partially contradictory. |
| **IC-3: User-space only vs. < 100 μs** | F-3 restricts use of kernel facilities. However, hardware timestamping (`SO_TIMESTAMPING`), HPET access, and PPS (pulse-per-second) signals are the standard mechanisms for sub-100 μs precision. Pure user-space without `SO_TIMESTAMPING` makes the 100 μs target extremely difficult to guarantee and potentially impossible on Wi-Fi. This is the single most critical inconsistency. |
| **IC-4: No clock modification vs. Monotonic guarantee** | F-4 forbids modifying the system clock. NF-3 requires a monotonic virtual clock. If the primary reference changes after leader failover (F-6), the new reference's local time may be *behind* the old one. Maintaining monotonicity across a leader transition requires explicit forward-only slewing logic in the virtual clock, which is non-trivial. |
| **IC-5: External verification vs. User-space only** | F-12 requires *external* verification of physical simultaneity. The most practical external tools (logic analyzers, oscilloscopes on GPIO pins, PPS signals) require kernel-level GPIO drivers or hardware access — potentially conflicting with F-3 if interpreted strictly. The boundary of "user-space only" must be clarified: does it apply to the production sync daemon only, or also to test harnesses? |

---

## 4. Implementation Implications

### 4.1 Clock Architecture

The system cannot use a "floating" `gettimeofday()`. It must maintain a **software clock model**:

```
virtual_global_time = CLOCK_MONOTONIC_RAW + offset_estimate + drift_correction
```

This model must be updated atomically (lock-free or with appropriate barriers) so application readers never observe a torn value or backward jump. A 64-bit atomic store of a nanosecond timestamp is the standard approach.

### 4.2 Timestamp Quality is Everything

The largest source of error in PTP/NTP-style protocols is the timestamp capture point. A timestamp taken *before* the UDP `sendto()` syscall can be delayed by the kernel send buffer by hundreds of microseconds. You must use **kernel receive/transmit hardware timestamps** (`SO_TIMESTAMPING` with `SOF_TIMESTAMPING_RX_HARDWARE`) if the NIC supports it, or at minimum **software timestamping at the driver layer** (`SOF_TIMESTAMPING_RX_SOFTWARE`). Raspberry Pi's onboard NICs have varying support for this — this must be audited per hardware revision.

### 4.3 Jitter Filter Design

On Wi-Fi, one-way delay samples will have a heavy right tail (CSMA/CA backoff produces occasional 10–50 ms delays). A simple mean filter will catastrophically bias the offset estimate. The minimum viable approach is:

- **Minimum filter** (use the minimum RTT sample as a proxy for symmetric path delay) — simple but biased
- **Cristian's Algorithm with outlier rejection** — moderate complexity
- **NTP-style clock filter + clock discipline (PLL/FLL)** — state of the art for user-space

The choice here directly determines whether the 100 μs target is achievable on Wi-Fi at all.

### 4.4 Discovery Protocol

UDP broadcast to `255.255.255.255:PORT` is the simplest zero-config mechanism, but:

- Broadcasts are blocked at router boundaries (constrains you to a single LAN segment)
- Wi-Fi APs may suppress or rate-limit broadcasts
- **IPv4 multicast** (e.g., `239.x.x.x`) is more robust and scoped

The node must simultaneously be a server (listening for peers) and a client (announcing itself). A heartbeat-based discovery with TTL expiry is the minimal viable design.

### 4.5 Leader Election

Full Raft is complex. For a small cluster (3–5 nodes), a **Bully Algorithm** or **lowest-UUID wins** election is KISS-compliant. The failure detector must use heartbeat timeouts, not TCP liveness (since transport is UDP). The critical design question: during election, does synchronization pause, or does each node free-run using its last known offset?

### 4.6 Anti-Probe Effect

Debug outputs on the hot path (`printf`, `write()` to a file) invoke the kernel, introduce scheduling delays, and can cause jitter of 50–500 μs — potentially exceeding the entire precision budget. Solutions:

- **Lock-free ring buffer in shared memory**, read by a separate observer process
- **Separate monitoring UDP port** sampled at low frequency
- **`mmap`-ed log file** written with lock-free appends
- **In-kernel `ftrace`/`perf`** for post-hoc analysis without probe effect

### 4.7 Deployment

For Raspberry Pi clusters, `rsync` + `ssh` + a shell script, or an **Ansible playbook**, is the simplest KISS-compliant deployment. Docker on RPi adds overhead and complexity without clear benefit for a timing-sensitive daemon. A **systemd unit file** for auto-start and crash restart is appropriate.

---

## 5. Hardware & Technology Constraints

| Constraint | Impact |
|------------|--------|
| **RPi Linux scheduler** | The default `SCHED_OTHER` scheduler can preempt the sync thread for 1–10 ms. Using `SCHED_FIFO` with `pthread_setschedparam()` in user-space is required to bound jitter. This is user-space accessible but requires `CAP_SYS_NICE`. |
| **RPi NIC timestamping** | RPi 3B uses a USB-attached LAN9514 which has **no hardware TX/RX timestamping**. RPi 4 has a native Gigabit NIC with limited timestamping support. This makes sub-100 μs over Ethernet hard on RPi 3. |
| **Wi-Fi PHY layer** | 802.11 MAC adds variable latency from contention, EDCA queuing, and power-save modes. **Disable power-save mode** (`iw dev wlan0 set power_save off`) — this is a mandatory configuration step not mentioned in the briefing. |
| **Crystal oscillator drift** | RPi clocks drift ~10–100 ppm. At 1 second sync intervals, this is 10–100 μs of uncorrected drift between exchanges. Sync intervals must be well under 1 second to maintain the target. |
| **UDP send buffer delays** | Linux may batch UDP packets in the send queue. Using `IP_TOS` to mark packets as high priority, and setting small socket buffer sizes, reduces queuing delay. |
| **`CLOCK_MONOTONIC_RAW` vs `CLOCK_MONOTONIC`** | `CLOCK_MONOTONIC` is disciplined by NTP if `ntpd`/`chronyd` is running on the host — this will interfere with your virtual clock model. `CLOCK_MONOTONIC_RAW` is unaffected. **If NTP is running on the nodes, it must be disabled or configured to not slew the clock.** |

---

## 6. Open Architectural Questions

These must be resolved before any code is written:

| # | Question | Blocks |
|---|----------|--------|
| **OQ-1** | **What is the exact definition of "< 100 μs precision"?** Mean, P99, max, RMS? Over what measurement window? | NF-1, F-12 |
| **OQ-2** | **Is `SO_TIMESTAMPING` (kernel-assisted socket timestamping) permitted?** It is user-space API but accesses kernel/driver facilities. The answer determines feasibility. | F-3, IC-3 |
| **OQ-3** | **What is the maximum cluster size?** 3 fixed? Dynamic up to N? This governs protocol scalability. | M-1, F-2 |
| **OQ-4** | **Is NTP/`chronyd` running on the nodes?** If yes, it must be neutralized to avoid interference with `CLOCK_MONOTONIC`. | NF-11 |
| **OQ-5** | **What is the tolerable failover window?** During leader re-election, nodes free-run. What is the maximum acceptable offset overshoot before a new leader is established? | NF-6, F-6 |
| **OQ-6** | **Which Raspberry Pi revisions are in the cluster?** RPi 3 vs. 4 have fundamentally different NIC capabilities affecting timestamp precision. | NF-7 |
| **OQ-7** | **What is the sync message exchange interval?** Too long → drift accumulates. Too short → network load and CPU overhead increase. This must be chosen to bound drift below 100 μs between exchanges. | F-1, NF-7 |
| **OQ-8** | **Does the virtual global clock need to be wall-clock aligned?** Or is it a relative epoch (time since cluster start)? This determines whether the initial reference node must be GPS- or NTP-seeded. | F-4, M-8 |
| **OQ-9** | **What is the external verification mechanism?** GPIO-toggled oscilloscope? Logic analyzer? Networked GPS PPS? The answer must be confirmed before the test harness is designed. | F-12, U-4 |
| **OQ-10** | **What jitter filter algorithm is required?** Minimum-delay filter, Cristian's Algorithm, or a PLL/FLL discipline? The choice is the core algorithmic decision of the entire system. | F-5, NF-1 |
| **OQ-11** | **Is the network trusted?** Can a malicious or misconfigured node corrupt the cluster time base? | M-7 |
| **OQ-12** | **What is the structured error taxonomy?** What specific error classes exist (network partition, clock runaway, leader timeout, invalid timestamp), and what are the prescribed responses for each? | F-7, NF-10 |

---

## 7. Critical Risk Assessment

| Risk | Severity | Likelihood |
|------|----------|------------|
| **The < 100 μs target is physically unachievable on Wi-Fi in user-space without hardware timestamping** | 🔴 Critical | High |
| **RPi USB NIC (RPi 3) lacks hardware TX timestamps, making Ethernet precision worse than expected** | 🔴 Critical | High (if RPi 3 used) |
| **NTP running concurrently on nodes will randomly slew `CLOCK_MONOTONIC` and corrupt the virtual clock model** | 🟠 High | High (default RPi OS runs `chrony`) |
| **Leader election during simultaneous crash + join (split-brain) causes oscillation** | 🟠 High | Medium |
| **Broadcast-based discovery fails silently on Wi-Fi APs with broadcast suppression** | 🟡 Medium | Medium |
| **`printf`/logging on sync thread causes periodic 500 μs jitter spikes** | 🟡 Medium | High (if not designed out) |

---

## 8. Recommended Next Step — Phase 2 Prompt

Copy the following prompt into your AI assistant (Claude in VS Code) together with this document:

> *"Given this Phase 1 analysis, resolve OQ-1 through OQ-12 with concrete architecture decisions. Then define the Architecture Anchor:*
> *- The virtual clock model (data structure, atomic update strategy)*
> *- The message exchange protocol (packet format and 4-message PTP-style exchange sequence)*
> *- The chosen jitter filter algorithm with justification*
> *- The leader election mechanism (algorithm, timeout values, failover sequence)*
> *- The structured error taxonomy: a table of error class → root cause → system action → indication strategy*
> *- The external verification method (hardware, procedure, pass/fail criterion)*
>
> *Produce a component diagram and a sequence diagram for the sync exchange. Then generate a project directory scaffold and the first compilable C/C++ skeleton."*
