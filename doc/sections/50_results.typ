#import "../template/src/uastw-thesis-lib.typ": *
= Results and Analysis

== Implementation Status

The `bigpickle` daemon is fully implemented and passes all five software unit tests. The following table summarizes the implementation completeness against the functional requirements.

#figure(
  table(
    columns: (auto, auto, 1fr),
    align: (left, left, left),
    table.header([*Requirement*], [*Status*], [*Notes*]),
    [F-1 — Multicast discovery],   [✓ Complete], [`239.192.88.100:47200` via `sock_mcast`.],
    [F-2 — Virtual clock],         [✓ Complete], [Q32.32 seqlock; `__int128` intermediates; no float.],
    [F-3 — Min-Delay filter],      [✓ Complete], [N=10, 200 µs tolerance.],
    [F-4 — Term-based election],   [✓ Complete], [xorshift64 timeout; lock-modes supported. Lower-NodeID invariant enforced: higher-ID incumbents yield once lower-ID node promotes.],
    [F-5 — Holdover],              [✓ Complete], [10 s max; rate frozen; GPIO 23 HIGH.],
    [F-6 — Lock-free telemetry],   [✓ Complete], [Seqlock SHM + lock-free UDP ring buffer.],
    [F-7 — Convergence detection], [✓ Complete], [GPIO 23 LOW after 10 s with |offset| < 100 µs.],
    [F-8 — Leader demotion],       [✓ Complete], [Immediate on strictly higher term in ANNOUNCE.],
    [F-11 — GPIO 18 pulse],        [✓ Complete], [1 Hz, 10 ms HIGH via `timerfd` ABSTIME.],
    [F-12 — Performance governor], [✓ Complete], [`drs-perf-governor.service`.],
    [F-14 — Filter reset],         [✓ Complete], [Leader-change, seq-jump, phase-step, holdover.],
    [F-15 — Fixed-size packets],    [✓ Complete], [`_Static_assert` enforced at compile time. Default V2: 64 B, version `0x02`. V1: 66 B, version `0x01` (via `PROTO_VER=1`).],
    [F-16 — Heartbeat/sync rates], [✓ Complete], [ANNOUNCE 100 ms; SYNC_REQ 50 ms; timeout 300 ms.],
    [F-17 — Dual-loop discipline], [✓ Complete], [Step at |θ| > 1 ms (×3); slew otherwise.],
    [F-19 — Late joiners],         [✓ Complete], [Term adoption in LISTEN/CANDIDATE/FOLLOWER.],
    [F-20 — GROUND state],         [✓ Complete], [2 s TX suppression on boot.],
    [F-21 — Static calibration],   [✓ Complete], [50 loopback samples; lat_corr ≈ 4 µs on Pi 4B.],
    [F-22 — Carrier recovery],     [✓ Complete], [`net_check_recover` every 500 ms.],
    [F-23 — Follower telemetry],   [✓ Complete], [Heartbeat tick even without active SYNC exchange.],
    [NF-7 — SCHED_FIFO/85],        [✓ Complete], [Both via `sched_setscheduler` and `drs-sync.service`.],
    [NF-8 — mlockall],             [✓ Complete], [`MCL_CURRENT | MCL_FUTURE` after all buffers pre-faulted.],
    [NF-9 — No hot-path I/O],      [✓ Complete], [All telemetry via `/dev/shm` mmap; UDP ring non-RT.],
    [NF-12 — No dynamic alloc],    [✓ Complete], [All buffers pre-allocated before `mlockall`.],
  ),
  caption: [Requirement implementation status.],
)

== Observed Behavior in Multi-Implementation Scenario

Nodes running different protocol versions are naturally isolated: `proto_decode` checks the version byte before any further processing, so foreign packets (version `0x01` when our build uses `0x02`, or vice versa) are silently discarded. Our nodes do not enter FOLLOWER state for foreign leaders and remain leader-elected solely among themselves.

When operating within our own cluster:

+ *Single-node boot (alone):* The node times out through LISTEN → CANDIDATE → LEADER and begins sending ANNOUNCEs with `election_term=1`.
+ *Lower-ID node joins later:* The joining node sees `peer_term (1) > own_term (0)` but also `peer_id > own_id`. It adopts the peer's term without yielding. After its election timeout fires, `election_promote()` produces `term=2`. The incumbent receives the ANNOUNCE with `term=2` from a lower-ID peer and correctly demotes to FOLLOWER.
+ *Simultaneous boot:* Both nodes start with `term=0`; neither sends ANNOUNCEs. After independent timeouts both promote to `term=1`. The higher-ID node then receives the lower-ID node's ANNOUNCE at the same term and yields via the NodeID tiebreak.
+ *Telemetry:* `drs_mon` displays state, leader node ID, and election term throughout all transitions, including the challenge-and-promote cycle.

== Performance Characterization

The following estimates are based on the BCM2711 platform with PREEMPT_RT and the RT configuration enforced by the five systemd units.

#figure(
  table(
    columns: (auto, auto, auto),
    align: (left, left, left),
    table.header([*Component*], [*Budget*], [*Observed / Expected*]),
    [Calibration `lat_corr_ns`], [—],        [≈ 3900–4000 ns (≈ 4 µs) on Pi 4B loopback],
    [GbE RTT (1G switch)],       [< 300 µs], [50–150 µs typical; spikes to 300 µs filtered by Min-Delay],
    [Scheduler latency (Core 3)],  [< 30 µs],  [< 5 µs with PREEMPT_RT + isolcpus],
    [GPIO write latency],          [< 5 µs],   [< 1 µs (direct `/dev/gpiomem` register write)],
    [timerfd abs wake-up jitter],  [< 20 µs],  [< 10 µs with SCHED_FIFO/85],
    [Expected steady-state |θ|],   [< 100 µs], [10–40 µs (dominant term: path-delay asymmetry)],
  ),
  caption: [Performance characterization on Raspberry Pi 4B.],
)

== Known Limitations

+ *Path-delay asymmetry:* The PTP-style estimator assumes symmetric uplink and downlink delays. Managed switches with per-port input queues may introduce asymmetric delays of 5–20 µs that appear as a constant offset. The Min-Delay filter selects the "luckiest" packet but cannot eliminate structural asymmetry.
+ *Single-switch assumption:* NF-1 (< 100 µs) is only guaranteed on a single-hop Gigabit switched Ethernet segment. Multi-hop topologies or WLAN links introduce additional variable delay.
+ *Interoperability:* Full SYNC exchange with foreign implementations requires packet format compatibility (matching PROTO_VER, same magic, same CRC polynomial). With the version byte now encoding the protocol variant, mismatched builds are automatically isolated at `proto_decode` — no ANNOUNCE, SYNC_REQ, or SYNC_RESP from a foreign version is ever processed.
+ *GPIO availability:* On platforms without `/dev/gpiomem` (e.g., WSL), GPIO is disabled gracefully. The daemon continues to run and provide telemetry; only the physical pulse output is absent.
