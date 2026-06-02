# DRS Telemetry Requirements

**Version:** 1.0  
**Date:** 2026-06-01  
**Applies to:** All cluster nodes running `drs_sync`

---

## Overview

Every node emits telemetry records over UDP unicast to a designated receiver (e.g. a logging machine or visualizer). The telemetry stream is fire-and-forget — packet loss is acceptable. The sync hot path is never blocked by telemetry.

---

## Transport

| Parameter | Value |
|-----------|-------|
| Protocol | UDP unicast |
| Destination port | **4242** |
| Destination IP | Passed as the third CLI argument to `drs_sync` (e.g. `drs_sync eth0 leader 10.0.0.1`) |
| Source | Any ephemeral port assigned by the OS |
| Byte order | **Little-endian** (native RPi/x86 byte order — no byte-swapping) |

If no destination IP is provided, the binary defaults to `127.0.0.1` (loopback only).

---

## Packet Format

Each packet is exactly **40 bytes**, binary, with no framing or length prefix.

| Offset | Size (B) | C type   | Field          | Description |
|--------|----------|----------|----------------|-------------|
| 0      | 8        | `int64`  | `timestamp_ns` | `CLOCK_MONOTONIC_RAW` on the sending node, nanoseconds |
| 8      | 4        | `int32`  | `state`        | Node state enum value (see [State Values](#state-values)) |
| 12     | 8        | `int64`  | `offset_ns`    | Measured clock offset from leader, nanoseconds. **0 for leader packets.** |
| 20     | 8        | `int64`  | `rtt_ns`       | Round-trip time of the sync exchange, nanoseconds. **0 for leader packets.** |
| 28     | 8        | `int64`  | `rate_q32`     | Virtual clock rate as Q32.32 fixed-point (see [Rate Field](#rate-field-q3232)) |
| 36     | 4        | `uint32` | `node_id`      | Sender's node ID (matches last octet of its IP: `10.0.0.<node_id>`) |

> All fields are stored in native little-endian byte order. There is no magic number, version field, or CRC in the telemetry packet.

---

## State Values

The `state` field carries the current node state machine value at the time the record is emitted.

| Value | Name | Meaning |
|-------|------|---------|
| 0 | `GROUND` | Startup hold-off (~2 s), not yet participating |
| 1 | `CALIBRATION` | Measuring self-latency; one-shot at startup |
| 2 | `LISTEN` | Waiting to hear a leader before entering election |
| 3 | `CANDIDATE` | Running for election |
| 4 | `FOLLOWER` | Synchronized to leader — `offset_ns` and `rtt_ns` are valid |
| 5 | `LEADER` | This node is the time reference — `offset_ns` and `rtt_ns` are always 0 |
| 6 | `HOLDOVER` | Leader lost; free-running on last known rate for up to 10 s |

---

## Rate Field (Q32.32)

`rate_q32` is a signed 64-bit integer in Q32.32 fixed-point format.

- The value `4294967296` (`1 << 32`) represents a rate of exactly **1.0** (nominal, no adjustment).
- The PI controller clamps the rate to **±1000 ppm** from nominal.

**To convert to PPM deviation:**

```python
RATE_ONE = 1 << 32  # 4294967296
ppm = (rate_q32 - RATE_ONE) / RATE_ONE * 1_000_000
```

For leader packets, `rate_q32` is always `RATE_ONE` (1.0).

---

## Emission Rules

### Follower node

One record is emitted per **accepted sync exchange** — i.e. after a `SYNC_REQ`/`SYNC_RESP` round-trip that passes the min-delay filter. Rejected exchanges (outliers filtered by the RTT window) produce no record.

- Nominal rate: ~**10 packets/s** (one per 50 ms sync period, assuming all exchanges are accepted)
- If the sync filter is rejecting frequently, the actual rate will be lower

`offset_ns` and `rtt_ns` are computed from the 4-timestamp (T1–T4) PTP-style exchange.  
`rate_q32` is the rate value applied by the PI controller for that exchange.

### Leader node

One record is emitted per **50 ms tick**, regardless of incoming requests.

- Nominal rate: ~**20 packets/s**
- `offset_ns = 0`, `rtt_ns = 0`, `rate_q32 = RATE_ONE`

---

## Implementation Requirements

Each node **MUST**:

1. Allocate a `TelemCtx` with a 64-entry power-of-2 ring buffer (`TELEM_BUF_SIZE = 64`).
2. Write records from the RT sync thread using a non-blocking atomic write — if the ring is full, **drop the record silently**.
3. Drain the ring and send UDP packets from a **separate non-RT thread**, polling at ~10 ms intervals.
4. Send each record as a single `sendto()` call of exactly 40 bytes.
5. Accept the destination IP as the second CLI argument; default to `127.0.0.1` if omitted.
6. Never block the sync thread on socket I/O.

---

## Receiver Requirements

Any receiver must accept 40-byte UDP datagrams on port **4242** and unpack them as little-endian. Packets shorter than 40 bytes should be discarded silently.

### Python reference unpacker

```python
import struct

RECORD_FMT  = "<qiqqqI"   # little-endian: int64, int32, int64, int64, int64, uint32
RECORD_SIZE = 40

STATE_NAMES = {
    0: "GROUND", 1: "CALIBRATION", 2: "LISTEN",
    3: "CANDIDATE", 4: "FOLLOWER", 5: "LEADER", 6: "HOLDOVER",
}

def unpack(data: bytes) -> dict:
    if len(data) < RECORD_SIZE:
        return None
    ts, state, offset, rtt, rate, node_id = struct.unpack_from(RECORD_FMT, data)
    return {
        "node_id":      node_id,
        "timestamp_ns": ts,
        "state":        STATE_NAMES.get(state, f"UNKNOWN({state})"),
        "offset_ns":    offset,
        "offset_us":    offset / 1_000,
        "rtt_ns":       rtt,
        "rtt_us":       rtt / 1_000,
        "rate_ppm":     (rate - (1 << 32)) / (1 << 32) * 1e6,
    }
```

### Minimal UDP listener

```python
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 4242))

while True:
    data, addr = sock.recvfrom(64)
    rec = unpack(data)
    if rec:
        print(f"node {rec['node_id']:>2}  state={rec['state']:<12} "
              f"offset={rec['offset_us']:+8.1f} us  "
              f"rtt={rec['rtt_us']:6.1f} us  "
              f"rate={rec['rate_ppm']:+7.3f} ppm")
```

---

## Important Notes

- `timestamp_ns` is `CLOCK_MONOTONIC_RAW` from each node's **local** clock. It is not comparable across nodes. Use `offset_ns` to understand inter-node alignment.
- Multiple nodes send to the same destination IP/port. Use `node_id` to demultiplex streams on the receiver.
- There is no sequence number in the telemetry packet. Reordering and loss are possible under load, but the sync path is unaffected.
- Telemetry is disabled (no packets sent) during `GROUND` and `CALIBRATION` states, as no sync exchanges occur in those states.
