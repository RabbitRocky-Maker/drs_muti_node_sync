#import "../template/src/uastw-thesis-lib.typ": *
= Implementation

The daemon `drs_syncd` is implemented in C11 (`-std=c11 -D_GNU_SOURCE`) and structured into fourteen source modules plus two tool binaries and five unit-test programs.

== Module Overview

#figure(
  table(
    columns: (auto, 1fr, auto),
    align: (left, left, left),
    table.header([*Module*], [*Responsibility*], [*LOC (approx.)*]),
    [`drs_sync_config.h`], [All compile-time constants: timing, protocol, RT parameters, GPIO pin numbers, SHM paths. `DRS_PROTO_VER` (default 2) selects the wire-frame size and version byte.], [120],
    [`crc32.{c,h}`], [IEEE 802.3 reflected CRC32 (polynomial 0xEDB88320). Self-tests at startup verify "123456789" → 0xCBF43926.], [60],
    [`proto_v2.{c,h}`], [Serialize/deserialize packets. Size is compile-time: 64 B (PROTO_VER=2, default, version byte `0x02`) or 66 B (PROTO_VER=1, version byte `0x01`). Explicit byte-offset reads/writes; no struct-cast. Validates magic, version, and CRC on decode. `_Static_assert` enforces size/version consistency.], [100],
    [`vclock.{c,h}`], [Virtual clock: Q32.32 fixed-point rate scaling, seqlock, `vclock_step_offset`, `vclock_set_rate`, inverse mapping. `__int128` for intermediate products.], [210],
    [`net_io.{c,h}`], [Dual-socket setup (mcast + ucast), `IP_MULTICAST_IF`, multicast join/leave, carrier-loss recovery, userspace timestamping, `send_packet`/`net_recv`.], [300],
    [`min_delay.{c,h}`], [Rolling min-delay filter: N=10 ring buffer, tolerance 200 µs. Rejects RTT outliers via `rtt <= min + DRS_MIN_DELAY_TOLERANCE_NS`.], [80],
    [`calibrate.{c,h}`], [50-sample UDP loopback on 127.0.0.1:47201. Outlier rejection (> min+20 µs). Result `min/2` stored in `vclock.lat_corr_ns`.], [120],
    [`pi_ctrl.{c,h}`], [Fixed-point PI controller: Kp=0.1 (Q16.16: 6554), Ki=0.01 (Q16.16: 655). Anti-windup ±1000 ppm. Step-confirmation hysteresis (3 samples).], [100],
    [`election.{c,h}`], [Term-based election with lower-NodeID invariant. `election_on_announce` yields only to lower-ID peers; adopts term silently when challenged by a higher-ID peer so the next `election_promote()` wins. xorshift64 PRNG for random timeout. Lock-mode support (AUTO/LEADER/FOLLOWER).], [100],
    [`statemachine.{c,h}`], [7-state FSM. Active-Before-Follower Guard. SYNC_REQ scheduling. PI discipline. Leader-IP logging on transition. Follower heartbeat telemetry. Same-term leader fast-path guarded by `peer_id < own_id` to prevent higher-ID incumbents from retaining leadership via flag check.], [490],
    [`gpio_bcm2711.{c,h}`], [`mmap` on `/dev/gpiomem`. BCM2711 GPSET0/GPCLR0/GPFSEL register access. `gpio_init`, `gpio_write`. < 1 µs per operation.], [80],
    [`pulse_engine.{c,h}`], [Dedicated pulse thread. `timerfd` one-shot ABSTIME for rising/falling edges of GPIO 18. Inverse-vClock scheduling. GPIO 23 health update.], [200],
    [`telemetry.{c,h}`], [128-byte seqlock SHM (`/dev/shm/drs-sync.state`). Seqlock writer helpers. `telemetry_get`, `telemetry_begin_write`, `telemetry_end_write`.], [120],
    [`telemetry_udp.{c,h}`], [64-slot lock-free ring buffer (power-of-2). Non-RT sender thread polling at 10 ms. `telem_udp_init(dest_ip, node_id)`, `telem_udp_emit`.], [155],
    [`errors.{c,h}`], [64-slot atomic ring buffer in `/dev/shm/drs-sync.errors`. O(1) push, no lock. 11 error codes.], [80],
    [`cmd.{c,h}`], [64-byte SHM command channel (`/dev/shm/drs-sync.cmd`). `cmd_poll` reads new commands. 7 command codes (recalibrate, force-holdover, …).], [60],
    [`main.c`], [`epoll` event loop: `sock_mcast`, `sock_ucast`, `tick_fd` (50 ms), `hb_fd` (100 ms). RT setup (affinity, SCHED_FIFO, mlockall). Argument parsing.], [290],
    [`tools/drs_mon.c`], [ANSI TUI live monitor. Reads `/dev/shm/drs-sync.state` via read-only mmap. Seqlock snapshot. Displays state, offset bar, rate, RTT, counters.], [295],
    [`tools/drs_sync_inspect.c`], [Single-shot SHM snapshot. Human-readable text output.], [100],
  ),
  caption: [Source modules in `bigpickle/code/daemon/`.],
)

== Key Optimizations

=== Lock-Free Design Throughout

The synchronization hot-path contains no mutexes, no condition variables, and no `pthread_mutex_lock` calls:

- *Virtual clock reads:* Seqlock — readers spin on an even sequence counter; writer increments it before and after every update. Reads are always consistent without blocking the writer.
- *Telemetry SHM:* Same seqlock pattern. `drs_mon` can snapshot the 128-byte struct while the daemon writes freely.
- *Error ring buffer:* Single atomic fetch-and-add for the write index; no lock.
- *UDP telemetry ring:* Single-producer (RT thread), single-consumer (sender thread); monotonic 64-bit indices with atomic load/store — no CAS required.

=== No Dynamic Allocation in Hot Path

All memory is allocated and pre-faulted before `mlockall(MCL_CURRENT | MCL_FUTURE)`:

```c
// All stack frames, global arrays and SHM mappings are touched
// before rt_apply() locks them into RAM.
telemetry_init();   // mmap /dev/shm/drs-sync.state
errors_init();      // mmap /dev/shm/drs-sync.errors
cmd_init();         // mmap /dev/shm/drs-sync.cmd
net_io_init();      // socket buffers
pulse_engine_init();
// ... then rt_apply() calls mlockall()
```

After `mlockall`, `malloc`/`free`/`realloc` are never called on the RT thread.

=== Userspace Timestamping

`CLOCK_MONOTONIC_RAW` is read with `clock_gettime` immediately before `sendto` (T1, T3) and immediately after `recvfrom` (T2, T4). This avoids `SO_TIMESTAMPING` complexity and, critically, avoids mixing with `CLOCK_REALTIME` — the only clock that NTP or the kernel can slew.

The residual timestamping error (OS scheduling jitter between the `clock_gettime` call and the actual packet transmission) is the dominant noise source, mitigated by the Min-Delay filter.

=== epoll + timerfd — No Busy Waiting

The main loop uses `epoll_wait` with a 200 ms timeout (safety net only). All scheduled events arrive via file descriptors:

```
sock_mcast  — incoming ANNOUNCE / SYNC_REQ / SYNC_RESP (mcast path)
sock_ucast  — incoming SYNC_REQ / SYNC_RESP (unicast path)
tick_fd     — 50 ms periodic: sm_tick, link monitor, command poll
hb_fd       — 100 ms periodic: leader ANNOUNCE transmit
```

The pulse engine owns two additional `timerfd` file descriptors in its private thread. This design yields deterministic wake-up latency bounded by the RT scheduler and eliminates any spin-wait.

=== Multicast Interface Forcing (`IP_MULTICAST_IF`)

On multi-homed hosts (eth0 + wlan0), the kernel's routing table would normally select the default route interface for multicast output. `IP_MULTICAST_IF` is set on `sock_ucast` to force multicast packets out the explicitly configured sync interface, preventing silent delivery on the wrong link.

=== Carrier-Loss Recovery

After an Ethernet cable reconnect the NIC hardware multicast filter is often cleared, causing silent loss of multicast reception. `net_check_recover` is called every 500 ms (10 ticks): it reads `/sys/class/net/<iface>/carrier`, detects a rising edge (down→up), and re-issues `IP_DROP_MEMBERSHIP` + `IP_ADD_MEMBERSHIP` to re-program the filter.

=== SYNC_RESP on Both Receive Paths

Foreign DRS implementations may send SYNC_RESP as a unicast packet to the bound port 47200 (our `sock_mcast`) rather than to the ephemeral source port of the SYNC_REQ (our `sock_ucast`). The `main.c` receive loop now dispatches `DRS_MSG_SYNC_RESP` on *both* `sock_mcast` and `sock_ucast`, making the follower interoperable with any conformant foreign implementation.

=== Follower Heartbeat Telemetry

Previously, `telem_udp_emit` was only called inside `sm_on_sync_resp`, leaving the monitoring stream silent whenever the SYNC exchange was stalled (e.g., foreign leader that does not respond). The FOLLOWER branch of `sm_tick` now always calls `telem_udp_emit` with the last known offset and RTT, ensuring the monitoring station always receives a state update at the 20 Hz tick rate.

== Build System

=== Compilation

```bash
cd bigpickle/code/daemon
make all          # builds drs_syncd, drs_mon, drs_sync_inspect
make test         # compiles and runs 5 unit tests (no GPIO / network needed)
CROSS=aarch64-linux-gnu- make   # cross-compile from x86 host
```

Compiler flags: `-O2 -g -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE -I include -DDRS_PROTO_VER=$(PROTO_VER)`, linked with `-lpthread -lrt`.

=== Configurable Variables

#figure(
  table(
    columns: (auto, auto, 1fr),
    align: (left, left, left),
    table.header([*Variable*], [*Default*], [*Effect*]),
    [`IFACE`],     [`eth0`],      [Network interface passed as `argv[1]` to `drs_syncd` in the generated service file.],
    [`TELEM_IP`],  [`127.0.0.1`], [UDP telemetry destination IP, embedded in the generated `ExecStart` line of `drs-sync.service`.],
    [`PROTO_VER`], [`2`],         [Protocol wire-frame version. `1` = 66-byte packet, version byte `0x01`. `2` = 64-byte packet, version byte `0x02` (default). Passed to the compiler as `-DDRS_PROTO_VER`. All nodes on the same cluster must use the same value.],
    [`CROSS`],     [(empty)],     [Cross-compiler prefix, e.g. `aarch64-linux-gnu-`.],
  ),
  caption: [Makefile variables.],
)

=== Installation and Deployment

```bash
# Build and install binaries + systemd units in one command (V2 default):
sudo make install-systemd IFACE=eth0 TELEM_IP=192.168.1.100

# Build with protocol V1 (66 bytes, version 0x01):
sudo make install-systemd IFACE=eth0 TELEM_IP=192.168.1.100 PROTO_VER=1

# This command:
#  1. Compiles and installs drs_syncd, drs_mon, drs_sync_inspect to /usr/local/bin/
#  2. Generates /etc/systemd/system/drs-sync.service with:
#       ExecStart=/usr/local/bin/drs_syncd eth0 auto 192.168.1.100
#  3. Installs the four hardening services unchanged
#  4. Enables all five units (survive reboot)
#  5. Restarts the full stack immediately
```

After this, no further operator action is needed. The system starts automatically at boot.

=== Startup Sequence

```
systemd
  └── drs-sync-precond  (masks conflicting time daemons)
  └── drs-perf-governor  (performance cpufreq governor)
  └── drs-irq-affinity   (Ethernet IRQs → CPUs 0–2)
  └── drs-nic-tune       (ethtool coalescing disable)
  └── drs-sync           (drs_syncd — after all preconditions)
        ├── GROUND   (2 s)
        ├── CALIBRATION  (UDP loopback, ~50 ms)
        ├── LISTEN / CANDIDATE  (0.25–0.5 s)
        └── FOLLOWER or LEADER  (steady-state sync)
```

== Node ID Derivation

The node ID is derived automatically from the last octet of the first routable IPv4 address on the configured interface:

```c
net->node_id = ntohl(iface_addr_ne) & 0xFF;
```

Example: `10.0.0.31` → node ID 31. No manual configuration is required. The fallback IP `10.0.0.31` is used only if `getifaddrs` finds no routable address.

== Runtime Monitoring

```bash
# Live TUI dashboard (reads /dev/shm/drs-sync.state):
drs_mon               # 500 ms refresh (default)
drs_mon 200           # 200 ms refresh

# Single snapshot:
drs_sync_inspect

# Verify service status:
systemctl status drs-sync.service
journalctl -u drs-sync.service -f
```

`drs_mon` displays state, leader ID, election term, offset bar, rate, RTT, convergence status, sync accept/reject counters, phase step count, holdover events, pulse count, and error log.
