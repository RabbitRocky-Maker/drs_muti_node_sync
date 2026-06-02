#import "../template/src/uastw-thesis-lib.typ": *
= Requirements

This section captures the resolved architectural decisions and the full set of functional, non-functional, and user requirements that shaped the `bigpickle` implementation. Requirements are derived from the project briefing, the Master Architecture v2.1, and lessons learned during iterative hardware bring-up.

== Resolved Architectural Decisions

#figure(
  table(
    columns: (auto, auto, 1fr),
    align: (left, left, left),
    table.header([*ID*], [*Topic*], [*Decision*]),
    [Q-1], [Time Reference], [Internal monotonic synchronization only. The leader's `CLOCK_MONOTONIC_RAW` defines the cluster reference timeline. System clocks (`CLOCK_REALTIME`, NTP, GPS) are never touched.],
    [Q-2], [Discovery], [UDP Multicast group `239.192.88.100:47200`, TTL=1 — zero-configuration, scoped to a single LAN segment.],
    [Q-3], [Election], [Term-based leadership with lower-NodeID invariant: the node with the lowest NodeID (last octet of routable IPv4) always wins, even against a higher-term incumbent. Terms provide race stability; NodeID provides the deterministic preference. A node with a lower ID that joins late adopts the incumbent's term and challenges on its next promotion.],
    [Q-4], [Timestamping], [Pure userspace timestamping via `clock_gettime(CLOCK_MONOTONIC_RAW)` immediately before `sendto` and immediately after `recvfrom`. No `SO_TIMESTAMPING` — avoids mixing with `CLOCK_REALTIME`.],
    [Q-5], [OS Environment], [PREEMPT_RT Linux. Core 3 isolated (`isolcpus=3 nohz_full=3 rcu_nocbs=3`). Five `systemd` units enforce all RT constraints automatically on boot.],
    [Q-6], [Hardware], [Raspberry Pi 4B, BCM2711. Wired Gigabit Ethernet is the exclusive sync transport. WLAN is tolerated for management only.],
    [Q-7], [Telemetry], [Dual telemetry path: lock-free seqlock SHM (`/dev/shm/drs-sync.state`, 128 B) for local monitoring, plus UDP stream (port 4242) for remote monitoring. Destination IP configurable at build time via `TELEM_IP` Makefile variable.],
  ),
  caption: [Resolved architectural decisions.],
)

== Functional Requirements

#figure(
  table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([*ID*], [*Requirement Description*]),
    [F-1], [Autonomous UDP multicast discovery (`239.192.88.100:47200`). Nodes require no pre-configuration.],
    [F-2], [Establish and maintain a virtual global monotonic clock based exclusively on `CLOCK_MONOTONIC_RAW`.],
    [F-3], [Min-delay rolling-window filter (N=10, tolerance 200 µs) to reject scheduler spikes and network jitter outliers.],
    [F-4], [Term-based leader election with lower-NodeID invariant: a node with a lower NodeID always eventually wins leadership, regardless of whether a higher-ID node accumulated a higher term by booting alone. Election timeout randomized 250–500 ms.],
    [F-5], [Explicit fault recovery: HOLDOVER mode (rate frozen, offset frozen) active for up to 10 seconds on leader loss.],
    [F-6], [Lock-free telemetry and clock access for all concurrent readers via seqlock.],
    [F-7], [Stable convergence detection: GPIO 23 transitions LOW only after |offset| < 100 µs is sustained for 10 continuous seconds.],
    [F-8], [Leader demotion when an ANNOUNCE with a strictly higher election term is received from a peer with a lower NodeID. A higher-term ANNOUNCE from a higher-ID peer does not cause demotion — the lower-ID incumbent challenges instead.],
    [F-9], [Non-blocking writer precedence in virtual clock layer (seqlock; writer never blocks on readers).],
    [F-10], [Monotonic nanosecond timestamps using Q32.32 fixed-point rate scaling. No floating-point in the RT hot-path.],
    [F-11], [GPIO 18: 10 ms HIGH pulse per global virtual second (1 Hz). Rising edge is the external measurement reference.],
    [F-12], [CPU frequency scaling disabled via performance governor (`drs-perf-governor.service`).],
    [F-14], [Automatic filter and PI controller reset on leader change, sequence discontinuity (delta > 1024), phase step > 5 ms, or HOLDOVER entry.],
    [F-15], [Fixed-size binary packets (64 B for PROTO_VER=2 default; 66 B for PROTO_VER=1), Big-Endian field encoding, validated by IEEE 802.3 CRC32. Version byte encodes the protocol variant; mismatched versions reject each other's packets automatically.],
    [F-16], [Leader ANNOUNCE heartbeat every 100 ms. Follower SYNC_REQ every 50 ms. Follower timeout = 3 missed heartbeats (300 ms).],
    [F-17], [Dual-loop clock discipline: hard phase step for |θ| > 1 ms (confirmed 3 consecutive samples); PI frequency slew for |θ| ≤ 1 ms.],
    [F-18], [Fail-silent GPIO health indicator (GPIO 23 HIGH = fault or holdover; LOW = converged).],
    [F-19], [Seamless late-joiner integration: a new node discovers the active leader and becomes a follower without disrupting existing synchronization.],
    [F-20], [Deterministic 2-second GROUND state at startup suppresses all transmissions during OS stabilization.],
    [F-21], [Automated static latency calibration via UDP loopback (50 samples, port 47201) before entering any sync state.],
    [F-22], [Carrier-loss recovery: multicast IGMP membership is re-established automatically after an Ethernet cable reconnect.],
    [F-23], [UDP telemetry stream: 40-byte records sent to a configurable destination IP:4242. Follower emits heartbeat records every 50 ms tick even without an active SYNC exchange.],
  ),
  caption: [Functional requirements.],
)

== Non-Functional Requirements

#figure(
  table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([*ID*], [*Requirement Description*]),
    [NF-1], [Physical pulse delta (GPIO 18) shall be < 100 µs between any two nodes over a 10-minute window.],
    [NF-2], [Pure user-space implementation. No custom kernel modules; no `adjtimex`; no `CLOCK_REALTIME` modification.],
    [NF-3], [Jitter resilience under moderate background traffic on switched Gigabit Ethernet.],
    [NF-4], [Clock convergence within 10 seconds of startup or leader change.],
    [NF-5], [3-heartbeat hysteresis for leader failure detection (follower timeout = 300 ms).],
    [NF-6], [Writer (sync thread) is never blocked by telemetry readers. Seqlock pattern throughout.],
    [NF-7], [SCHED_FIFO priority 85, pinned to isolated Core 3.],
    [NF-8], [`mlockall(MCL_CURRENT | MCL_FUTURE)` prevents page faults in the hot path.],
    [NF-9], [No disk I/O or `printf`/`fprintf`/`syslog` on the synchronization hot-path. All persistent state via `/dev/shm` mmap.],
    [NF-10], [Core 3 receives no Ethernet or WLAN IRQs (`drs-irq-affinity.service`).],
    [NF-11], [Maximum slew rate clamped to ±1000 ppm. Integrator anti-windup enforced at the same limit.],
    [NF-12], [No dynamic heap allocation (`malloc`/`free`) after `mlockall`. All buffers pre-allocated at startup.],
    [NF-13], [64-byte (V2) and 66-byte (V1) packets are both well below the 1500-byte MTU; no IP fragmentation possible.],
    [NF-14], [Deterministic packet parsing via explicit byte-offset reads; no struct-cast aliasing.],
    [NF-15], [No mutex contention in the sync loop. Lock-free ring buffers and atomics throughout.],
  ),
  caption: [Non-functional requirements.],
)

== User Requirements

#figure(
  table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([*ID*], [*Requirement Description*]),
    [U-1], [Nodes join the synchronization group automatically upon Ethernet connection — zero manual configuration.],
    [U-2], [Operators inspect node state non-intrusively via `drs_mon` (live TUI, reads SHM) or via UDP telemetry stream.],
    [U-3], [The telemetry destination IP shall be configurable at install time via `make install-systemd TELEM_IP=<ip>` without recompilation.],
    [U-4], [The sync daemon and all RT-hardening services shall start automatically after reboot via `systemd`.],
  ),
  caption: [User requirements.],
)
