#import "../template/src/uastw-thesis-lib.typ": *
= Architecture and Design

== Planning Process and Architecture Evolution

The architecture went through six documented revisions before reaching the final `bigpickle` implementation. Each iteration addressed concrete deficiencies found in the previous version.

#figure(
  table(
    columns: (auto, auto, 1fr),
    align: (left, left, left),
    table.header([*Rev.*], [*Key Change*], [*Rationale*]),
    [V0], [Unspecified sync; lowest-IP election; no fixed packet format.], [Initial anchor — established multicast group `239.192.88.100:47200`.],
    [V1], [Cristian's Algorithm + EWMA; Bully+Quorum election; 239.255.0.1.], [First complete sync model; quorum removed later due to split-brain risk.],
    [V2], [4-timestamp PTP-style handshake; Dual-Master topology; 239.192.88.100:47200; PREEMPT_RT recognized.], [RTT-symmetric offset estimator adopted; group address finalized.],
    [V3/v2.1], [7-state FSM; Min-Delay filter N=10; lowest NodeID election; 64-byte packets (version 0x02); Kp=0.05/Ki=0.005.], [First complete, hardware-ready specification.],
    [v2.2], [Bug-fix round: multicast interface binding, seqlock writer protocol, PI integral windup.], [Real-hardware bring-up revealed several corner cases.],
    [V6], [Term-based sticky leadership replaces lowest-NodeID rule; tolerance raised to 50 µs; `step_accum_ns` semantics clarified.], [NodeID-only election caused dual-leader on simultaneous boots; term ordering is more robust.],
    [bigpickle], [Default 64 B packets (version 0x02); compile-time PROTO_VER switch (V1=66 B/0x01, V2=64 B/0x02 default); tolerance 200 µs; Kp=0.1/Ki=0.01; dual-socket model; 5 systemd units; UDP telemetry; TELEM_IP and PROTO_VER Makefile variables; lower-NodeID always wins election even against a higher-term incumbent.], [Real GbE jitter exceeds 50 µs; doubled PI gains for faster convergence; telemetry separated from SHM for remote monitoring; version byte now encodes the protocol variant, providing automatic isolation from foreign implementations.],
  ),
  caption: [Architecture revision history — V0 to `bigpickle`.],
)

=== Key Divergences from V6 Specification

The `bigpickle` implementation intentionally deviates from the V6 specification in four points, all driven by real-hardware observations:

+ *Protocol version selection (PROTO_VER):* The wire-frame size is a compile-time choice. The default build (`PROTO_VER=2`) produces 64-byte packets with version byte `0x02`, matching the V3/v2.1 specification. A `PROTO_VER=1` build produces 66-byte packets with version byte `0x01` for backward compatibility. Because `proto_decode` checks the version byte, nodes built for different protocol versions naturally reject each other's packets without any additional filtering logic.
+ *Min-Delay tolerance 200 µs (not 50 µs):* Switched GbE in the lab exhibits software-interrupt–induced RTT spikes of 80–150 µs; 50 µs caused excessive sample rejection and stalled clock discipline. 200 µs accepts all physically valid samples on 1G switched Ethernet.
+ *PI gains Kp=0.1, Ki=0.01 (not 0.05/0.005):* Doubled gains halve the convergence time to within 100 µs without causing oscillation. Anti-windup and rate-clamp remain at ±1000 ppm.
+ *Lower-NodeID always wins election:* V6 made term ordering absolute — a late joiner always yielded to the established leader regardless of NodeID, meaning a higher-ID node that booted alone would retain leadership permanently. `bigpickle` corrects this: when `peer_term > own_term` but `peer_id > own_id`, the node adopts the peer's term silently without yielding. Its next `election_promote()` produces `peer_term + 1`; the incumbent then sees a higher-term ANNOUNCE from a lower-ID peer and yields correctly. The lowest-NodeID node always eventually holds leadership.

== System Overview

The system consists of one C11 daemon per node (`drs_syncd`) and a set of `systemd` units that enforce the RT environment. No user interaction is required after initial deployment.

```
┌─────────────────────────────────────────────────────────────┐
│  Raspberry Pi 4B  —  Core 3 (isolated, SCHED_FIFO/85)      │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  drs_syncd  (single RT thread, epoll event loop)     │  │
│  │                                                      │  │
│  │  statemachine ──▶ vclock ──▶ pulse_engine (GPIO 18)  │  │
│  │       │              │                               │  │
│  │  election   pi_ctrl + min_delay                      │  │
│  │       │              │                               │  │
│  │  net_io (sock_mcast + sock_ucast)                    │  │
│  │  calibrate ──▶ vclock.lat_corr_ns                    │  │
│  │                                                      │  │
│  │  [non-RT] telemetry_udp sender thread ──▶ UDP:4242   │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                             │
│  /dev/shm/drs-sync.state  (128 B seqlock — drs_mon reads)  │
│  /dev/shm/drs-sync.cmd    (64 B command channel)           │
│  /dev/shm/drs-sync.errors (4 KB error ring buffer)         │
└─────────────────────────────────────────────────────────────┘
          │ GbE                         │ UDP:4242
          ▼                             ▼
    Other DRS nodes               Telemetry listener
```

== Virtual Clock Model

The virtual clock provides a cluster-wide monotonic nanosecond timeline that is independent of the OS wall clock. The mapping from local hardware time to global virtual time is:

$ T_"global" = (T_"local\_raw" - b_"local") times "Rate" >> 32 + b_"global" + "Offset" - "LatencyCorrection" $

where `Rate` is a signed 64-bit Q32.32 fixed-point value (nominal = `0x100000000` = 1.0). Both base values ($b_"local"$, $b_"global"$) are updated atomically with every rate change or phase step so $T_"global"$ is always a smooth, monotonic function. The inverse mapping:

$ T_"local\_raw" = b_"local" + ((T_"global" - b_"global" - "Offset" + "LatencyCorrection") << 32) / "Rate" $

is used by the pulse engine to schedule GPIO 18 rising edges at precise virtual second boundaries via `timerfd_settime(TFD_TIMER_ABSTIME)`.

A seqlock protects all concurrent readers (pulse engine, `drs_mon`, `telemetry_udp`) from torn reads without blocking the RT writer.

== Synchronization Protocol

The 4-timestamp PTP-style handshake exchanges the following fixed-size packet (64 bytes for the default V2 build; 66 bytes when built with `PROTO_VER=1`):

#figure(
  table(
    columns: (auto, auto, auto, 1fr),
    align: (left, left, left, left),
    table.header([*Offset*], [*Type*], [*Size*], [*Field*]),
    [0],  [uint32], [4],  [Magic `0x44525354` ("DRST")],
    [4],  [uint8],  [1],  [Version: `0x02` (PROTO_VER=2, default) or `0x01` (PROTO_VER=1)],
    [5],  [uint8],  [1],  [MsgType: 0x01=ANNOUNCE, 0x02=SYNC_REQ, 0x03=SYNC_RESP],
    [6],  [uint8],  [1],  [Flags: bit0=LEADER, bit1=HOLDOVER, bit2=CALIBRATED, bit3=FAULT],
    [7],  [uint8],  [1],  [Reserved (zero)],
    [8],  [uint16], [2],  [Seq — rolling sequence number],
    [10], [uint32], [4],  [NodeID — last octet of routable IPv4],
    [14], [uint32], [4],  [ElectionTerm — monotonically increasing],
    [18], [int64],  [8],  [T1 — follower send time (CLOCK_MONOTONIC_RAW, ns)],
    [26], [int64],  [8],  [T2 — leader receive time (virtual clock, ns)],
    [34], [int64],  [8],  [T3 — leader send time (virtual clock, ns)],
    [42], [int64],  [8],  [T4 — follower receive time (CLOCK_MONOTONIC_RAW, ns)],
    [50], [uint32], [4],  [CRC32 — IEEE 802.3 reflected (field zeroed during calculation)],
    [54], [—],      [10 or 12], [Zero padding — 10 bytes (V2, 64 B total) or 12 bytes (V1, 66 B total)],
  ),
  caption: [DRS wire-protocol packet layout (64 bytes V2 default / 66 bytes V1, Big-Endian).],
)

All numeric fields are encoded in network byte order (Big-Endian) via explicit `hton`/`ntoh` calls — no struct-cast aliasing.

The offset $theta$ and RTT $delta$ are computed as:

$ theta = ((T_2 - T_1^') + (T_3 - T_4^')) / 2 $
$ delta = (T_4 - T_1) - (T_3 - T_2) $

where $T_1^'$ and $T_4^'$ are the virtual-clock equivalents of the physical timestamps, ensuring the PI feedback loop operates on the virtual domain. Physical timestamps are used only for the RTT computation fed to the Min-Delay filter.

== Network I/O Architecture

Two sockets are opened per node to separate reception semantics from routing:

#figure(
  table(
    columns: (auto, auto, 1fr),
    align: (left, left, left),
    table.header([*Socket*], [*Binding*], [*Purpose*]),
    [`sock_mcast`], [`INADDR_ANY:47200`], [Receives ANNOUNCE, SYNC_REQ, and — for interoperability — SYNC_RESP. Sends ANNOUNCE via IP_MULTICAST_IF forced to the sync interface.],
    [`sock_ucast`], [`iface_ip:0` (OS-assigned port)], [Sends SYNC_REQ (unicast to leader) and SYNC_RESP (unicast to follower). Receives SYNC_RESP from correctly-implemented peers.],
  ),
  caption: [Dual-socket model.],
)

`IP_MULTICAST_IF` is set on `sock_ucast` to force multicast packets out the correct physical interface on multi-homed hosts. A carrier-loss recovery routine (`net_check_recover`) re-joins the IGMP multicast group after every Ethernet link-up event, preventing silent loss of multicast reception after cable reconnects.

SYNC_RESP is dispatched on *both* sockets. This interoperability fix accommodates foreign DRS implementations that reply to the bound port (47200) rather than the ephemeral source port of the SYNC_REQ.

== Leader Election

Election is term-based with the invariant that the node with the lowest NodeID always wins. Terms provide stability against split-brain races; NodeID provides a deterministic tie-breaking hierarchy that cannot be overridden by term order alone:

#figure(
  table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([*Condition*], [*Action*]),
    [`peer_term > own_term` AND `peer_id < own_id`], [Lower-ID peer has a higher term — legitimately won. Adopt term, update `leader_node_id`, reset election timeout, become FOLLOWER.],
    [`peer_term > own_term` AND `peer_id > own_id`], [Higher-ID peer bootstrapped alone and incremented its term, but we have the preferred (lower) NodeID. Adopt peer term silently, return NO_CHANGE — do not yield. The next `election_promote()` produces `peer_term + 1`; the incumbent then yields on seeing a higher-term ANNOUNCE from us.],
    [`peer_term < own_term`], [Ignore — stale ANNOUNCE.],
    [`peer_term == own_term` AND `peer_id < own_id`], [Tiebreak: lower NodeID wins, become FOLLOWER.],
    [`peer_term == own_term` AND `DRS_FLAG_LEADER` set AND `peer_id < own_id`], [Fast-path for confirmed active leader at same term — become FOLLOWER immediately without waiting for timeout.],
    [Election timeout (LISTEN or CANDIDATE)], [Promote self: `own_term++`, become LEADER.],
    [`LOCK_AS_LEADER`], [Ignore all foreign ANNOUNCEs; term boosted by 1000 at startup.],
    [`LOCK_AS_FOLLOWER`], [Election timeout never fires; stays passive.],
  ),
  caption: [Election decision logic.],
)

The election timeout is randomized in [250 ms, 500 ms] using an xorshift64 PRNG seeded from `/dev/urandom` to prevent simultaneous candidacy collisions.

== 7-State Finite State Machine

#figure(
  table(
    columns: (auto, 1fr, 1fr),
    align: (left, left, left),
    table.header([*State*], [*Entry Condition*], [*Key Activity*]),
    [GROUND],       [Boot],                                 [2 s TX suppression, OS stabilization.],
    [CALIBRATION],  [GROUND timeout],                       [50-sample UDP loopback; derive `lat_corr_ns`.],
    [LISTEN],       [Calibration success],                  [Passive discovery; election timeout armed.],
    [CANDIDATE],    [Election timeout],                     [Election timeout re-armed; no ANNOUNCE sent yet.],
    [LEADER],       [Candidate timeout],                    [ANNOUNCE every 100 ms; serve SYNC_REQ.],
    [FOLLOWER],     [ANNOUNCE with higher term or tiebreak],[SYNC_REQ every 50 ms; PI discipline; UDP heartbeat every tick.],
    [HOLDOVER],     [3 missed leader heartbeats],           [Rate frozen; offset frozen; GPIO 23 HIGH. Max 10 s.],
  ),
  caption: [7-state FSM.],
)

The *Active-Before-Follower Guard* prevents any transition to FOLLOWER before CALIBRATION completes, ensuring `lat_corr_ns` is valid before the PI controller runs.

== Clock Discipline

The PI controller operates in the virtual-clock domain to close the control loop correctly:

$ P = K_p dot.c theta / T_"period" $
$ I_(n) = "clamp"(I_(n-1) + K_i dot.c theta / T_"period",  -1000 "ppm",  +1000 "ppm") $
$ "rate\_delta"_"Q32" = (P + I_n) times 4295 $
$ "Rate"_"new" = "clamp"("Rate"_"nominal" + "rate\_delta"_"Q32", "Rate"_"min", "Rate"_"max") $

where $K_p = 0.1$ (Q16.16: 6554), $K_i = 0.01$ (Q16.16: 655), and 4295 ≈ $2^{32}/10^6$ converts parts-per-million to Q32.32 rate units. All arithmetic is integer-only; intermediate products use `int64_t` with explicit division to avoid overflow.

A hard phase step (`vclock_step_offset`) fires when three consecutive samples confirm |θ| > 1 ms. After a step, the PI integrator is reset to prevent windup from the large historical error.

== RT Deployment — Five systemd Units

#figure(
  table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([*Unit*], [*Responsibility*]),
    [`drs-perf-governor.service`], [Sets all CPU cores to the `performance` cpufreq governor. Eliminates DVFS-induced frequency drift.],
    [`drs-irq-affinity.service`], [Pins all Ethernet IRQs to CPUs 0–2 via `/proc/irq/*/smp_affinity_list`. Core 3 receives no NIC interrupts.],
    [`drs-nic-tune.service`], [Disables NIC interrupt coalescing via `ethtool -C eth0 rx-usecs 0 tx-usecs 0`. Minimizes driver-level batching latency.],
    [`drs-sync-precond.service`], [Stops and masks any conflicting time services (`systemd-timesyncd`, `chronyd`, `ntpd`, `ptp4l`, `phc2sys`) before the daemon starts.],
    [`drs-sync.service`], [Runs `drs_syncd`. CPUAffinity=3, CPUSchedulingPolicy=fifo, CPUSchedulingPriority=85, LimitMEMLOCK=infinity. ExecStart line is generated with IFACE and TELEM_IP at install time.],
  ),
  caption: [Five systemd units and their responsibilities.],
)

All five units are enabled and started by `sudo make install-systemd IFACE=eth0 TELEM_IP=<ip>`, making the system persistent across reboots without further operator intervention.

== Telemetry Architecture

Two independent telemetry paths coexist:

+ *SHM Telemetry (`/dev/shm/drs-sync.state`):* A 128-byte seqlock-protected region updated every 50 ms by the RT thread. `drs_mon` reads it via read-only mmap with no system calls in steady state.
+ *UDP Telemetry (port 4242):* The RT thread enqueues 40-byte records into a 64-slot power-of-2 lock-free ring buffer. A dedicated non-RT sender thread drains the ring every 10 ms and transmits over UDP. This design keeps the RT thread free of blocking `sendto` calls. The LEADER emits one record per 50 ms tick; the FOLLOWER emits one record per tick plus one record per successful SYNC exchange.

#figure(
  table(
    columns: (auto, auto, 1fr),
    align: (left, left, left),
    table.header([*Offset*], [*Type*], [*Field*]),
    [0],  [int64],  [Timestamp (CLOCK_MONOTONIC_RAW, ns)],
    [8],  [int32],  [State (drs_state_t)],
    [12], [int64],  [Offset from leader (ns)],
    [20], [int64],  [Round-trip time (ns)],
    [28], [int64],  [Clock rate (Q32.32)],
    [36], [uint32], [Node ID],
  ),
  caption: [40-byte UDP telemetry record layout.],
)
