#import "../template/src/uastw-thesis-lib.typ": *
= Test Concept and Falsifiability

== Software Unit Tests

Five unit-test programs are compiled and executed as part of `make test`. They require no network, no GPIO, and no root privileges — they can run on any Linux host (including the development machine via WSL).

```bash
cd bigpickle/code/daemon
make test
# === Running unit tests ===
# --- build/test_crc32 ---
# --- build/test_proto ---
# --- build/test_min_delay ---
# --- build/test_pi_ctrl ---
# --- build/test_vclock ---
# === All tests done ===
```

#figure(
  table(
    columns: (auto, auto, 1fr),
    align: (left, left, left),
    table.header([*Binary*], [*Source*], [*Verification Goal*]),
    [`test_crc32`],      [`tests/test_crc32.c`],      [IEEE 802.3 reflected CRC32: "123456789" → 0xCBF43926. Validates the CRC implementation used for every packet.],
    [`test_proto`],      [`tests/test_proto.c`],      [Serialize/deserialize round-trip for ANNOUNCE, SYNC_REQ, SYNC_RESP with all fields set. Verifies Big-Endian encoding and CRC re-validation on decode.],
    [`test_min_delay`],  [`tests/test_min_delay.c`],  [Rolling window of N=10: verify minimum is tracked correctly; verify outlier rejection at `min + 200 µs`; verify `min_delay_reset` clears the window.],
    [`test_pi_ctrl`],    [`tests/test_pi_ctrl.c`],    [Step-mode trigger (3 consecutive |e| > 1 ms); slew-mode rate computation; rate clamp at ±1000 ppm; integrator anti-windup.],
    [`test_vclock`],     [`tests/test_vclock.c`],     [Rate scaling: 1000 ppm offset → 1 ms drift per second. Phase step consistency. Inverse mapping round-trip accuracy.],
  ),
  caption: [Unit-test suite (`make test`).],
)

== System-Level Test Scenarios

#figure(
  table(
    columns: (auto, 1fr, 1fr),
    align: (left, left, left),
    table.header([*ID*], [*Scenario*], [*Pass Criterion*]),
    [TS-1], [*Baseline:* 2–3 nodes on wired GbE switch, running for 10 min.], [max|Δt| (GPIO 18 rising edge) < 100 µs; GPIO 23 LOW on all nodes during steady state.],
    [TS-2], [*Dynamic discovery:* 2 nodes running, plug in a 3rd node.], [3rd node reaches FOLLOWER (GPIO 23 LOW) within 10 s without disrupting existing sync.],
    [TS-3], [*Leader failure:* power-off the leader node.], [Followers enter HOLDOVER, re-elect a new leader, and re-converge within 11 s (10 s HOLDOVER + 1 s election).],
    [TS-4], [*Protocol isolation:* a node from another team's implementation (different PROTO_VER / version byte) is present on the same multicast group.], [Our node's `proto_decode` rejects the foreign ANNOUNCE (version mismatch); our cluster elects its own leader independently and does not interact with the foreign node.],
    [TS-5], [*Cable reconnect:* disconnect and reconnect the Ethernet cable on a follower.], [Multicast re-join (carrier recovery) completes automatically; node re-enters FOLLOWER within 2 s.],
    [TS-6], [*Calibration stability:* restart the daemon 5 times; compare `lat_corr_ns`.], [Variation < 500 ns across all restarts on the same hardware.],
  ),
  caption: [System-level test scenarios.],
)

== Falsifiability — External Physical Verification

Software-reported offsets cannot self-prove sub-100 µs precision because they are subject to the same OS scheduling jitter they aim to measure. Physical external measurement is mandatory.

=== Hardware Setup

```
Pi 4B (Node A)    Pi 4B (Node B)    Pi 4B (Node C)
  GPIO 18 ─────────────────────────────────┐
  GPIO 23 ────────────────────────┐        │
  GND  ───────────────────┐       │        │
                          └───────┴────────┴── Logic Analyzer
                                               (≥ 1 MHz sample rate)
```

*Critical:* all nodes must share a common GND reference. Without a common ground the measured edge timestamps reflect ground-potential differences, not clock offsets.

=== Measurement Procedure

+ Ensure `drs_syncd` is running and GPIO 23 is LOW on all nodes (convergence declared).
+ Arm the logic analyzer across all GPIO 18 channels plus at least one GPIO 23 channel.
+ Capture for ≥ 10 minutes (≥ 6000 pulse pairs per node pair).
+ Export rising-edge timestamps. Compute per-pair delta statistics: p50, p99, max.

=== Pass/Fail Criteria

#figure(
  table(
    columns: (auto, auto, auto),
    align: (left, left, left),
    table.header([*Metric*], [*Requirement*], [*Target*]),
    [max|Δt| over 10 min], [< 100 µs], [< 30 µs],
    [p99 |Δt|], [< 100 µs], [< 20 µs],
    [GPIO 23 LOW fraction], [> 90 %], [> 98 %],
    [Leader-loss recovery time], [< 11 s], [< 8 s],
  ),
  caption: [Physical pass/fail criteria.],
)

== OS Hardening Pre-flight Checklist

Before any test run, verify the RT environment is correctly configured:

```bash
# Kernel isolation active
grep -E 'isolcpus|nohz_full|rcu_nocbs' /proc/cmdline

# Sync daemon is running with correct scheduler
systemctl is-active drs-sync.service
chrt -p $(pgrep drs_syncd)   # expect: SCHED_FIFO prio 85

# CPU governor
cat /sys/devices/system/cpu/cpu3/cpufreq/scaling_governor  # expect: performance

# No competing time service
systemctl is-active systemd-timesyncd chronyd ntpd   # all should be inactive
```
