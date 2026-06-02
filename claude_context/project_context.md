# DRS High-Precision Distributed Synchronization — Projektkontext

**Quellen:** Briefing_DRS.pdf + FINAL_PROJEKT_DOKUMENTATION.pdf (Messavilla/Kodritsch, 2026-05-25)  
**Status:** Implementiert und multi-node verifiziert (philippknode + drs-node-03)

---

## 1. Aufgabenstellung & Ziel

Ein deterministisches **User-Space-Synchronisationssystem** für einen dynamischen Cluster von Raspberry-Pi-Knoten — **ohne Kernel-Module, ohne externe Zeitquellen (NTP/PTP/GPS)**.

**Hartes Ziel (NF-1):**
```
Δt_physical < 100 µs
```
zwischen den GPIO-18-Sync-Pulsen verschiedener Nodes, gemessen extern via Logic-Analyzer oder Oszilloskop.

**Paradigma:** KISS (Keep It Simple, Stupid) — Zero-Config, autonome Node-Discovery beim Einstecken.

---

## 2. Technologie-Stack

| Komponente | Detail |
|---|---|
| Plattform | Raspberry Pi 4B, PREEMPT\_RT Linux |
| Sprache Daemon | C11 (`drs_syncd`) |
| Sprache Agent | Python 3 (HTTP API + Web-UI) |
| Web-UI | Vanilla JS (5-Tab SPA) |
| Transport | UDP/IPv4, 64-Byte-Pakete, Big-Endian |
| Kommunikation | Ethernet (NF-1-Garantie) + WLAN (ms-Klasse, optional) |

---

## 3. Architektur-Überblick

### 3.1 Prozess-Layout

```
drs_syncd  — Single-Threaded, CPU Core 3 (isolcpus=3), SCHED_FIFO/85, mlockall
drs_agent  — Python, CPU Cores 0–2, HTTP API Port 8080
```

**Core 3** sieht weder Scheduler-Ticks (`nohz_full=3`) noch RCU-Callbacks (`rcu_nocbs=3`) noch Netzwerk-IRQs. Einzige Userspace-Aktivität auf Core 3 = `drs_syncd`.

### 3.2 Haupt-Event-Loop (`epoll_wait`)

| File Descriptor | Typ | Trigger |
|---|---|---|
| `sock_ucast` | UDP-Socket :47200 | Eingehende SYNC\_REQ/RESP |
| `sock_mcast` | UDP-Socket :47200 | Eingehende ANNOUNCE |
| `tick_fd` | timerfd 50 ms | State-Machine-Tick, Telemetrie, Command-Polling |
| `hb_fd` | timerfd 100 ms | Leader: ANNOUNCE-Heartbeat |
| `pe.timer_fd` | timerfd one-shot ABS | Pulse-Rising-Edge auf GPIO 18 |
| `pe.pulse_low_fd` | timerfd one-shot ABS | Pulse-Falling-Edge (10 ms nach Rising) |

### 3.3 Datenfluss (PTP-Zwei-Wege-Messung)

```
Follower                          Leader
T1 = CLOCK_MONOTONIC_RAW
sendto(SYNC_REQ) ─────────────► recvfrom
                                 T2 = CLOCK_MONOTONIC_RAW
                                 T3 = CLOCK_MONOTONIC_RAW
recvfrom ◄───────────────────── sendto(SYNC_RESP)
T4 = CLOCK_MONOTONIC_RAW

θ = ((T2 - T1) + (T3 - T4)) / 2   ← Offset Follower vs Leader
δ = (T4 - T1) - (T3 - T2)         ← Reines Netzwerk-RTT
```

---

## 4. Module im Detail

### 4.1 Virtuelle Uhr (`vclock.{c,h}`, 210 LOC)

Globale Zeitachse, **unabhängig vom System-Clock** (OS-Clock wird nie angefasst).

```
T_global = (T_local_raw - base_local) * Rate >> 32 + base_global + Offset - LatencyCorrection
```

- **Rate:** Q32.32 Festkomma (Nominal = 1.0 = `0x100000000`)
- **Offset:** signed int64 in ns
- **LatencyCorrection:** statische Loopback-Latenz in ns
- **Concurrency:** Seqlock (Writer: sync-thread; Reader: agent, inspect, pulse\_engine — lock-frei)
- **Kein Float** im RT-Thread: `__int128` für Zwischenergebnisse

**Step-Mode** (|θ| > 1 ms): harter Sprung via `vclock_step_offset()`  
**Slew-Mode** (|θ| ≤ 1 ms): sanfte Frequenzkorrektur via PI-Regler

### 4.2 Wire-Protokoll (`proto_v2.{c,h}`, `crc32.{c,h}`)

**64-Byte UDP-Paket (Big-Endian, kein Struct-Cast):**

| Offset | Bytes | Feld | Bedeutung |
|---|---|---|---|
| 0 | 4 | magic | `0x44525354` = "DRST" |
| 4 | 1 | version | `0x02` |
| 5 | 1 | msg\_type | 1=ANNOUNCE, 2=SYNC\_REQ, 3=SYNC\_RESP |
| 6 | 1 | flags | bit0=LEADER, bit1=HOLDOVER, bit2=CALIBRATED, bit3=FAULT |
| 7 | 1 | reserved | = 0 |
| 8 | 2 | seq | Rolling u16 |
| 10 | 4 | node\_id | Eindeutig (letztes Oktett der routbaren IPv4) |
| 14 | 4 | election\_term | Monoton steigend |
| 18 | 8 | T1 | Follower send time (ns) |
| 26 | 8 | T2 | Leader receive time (ns) |
| 34 | 8 | T3 | Leader send time (ns) |
| 42 | 8 | T4 | Follower receive time (ns) |
| 50 | 4 | crc32 | IEEE 802.3 reflected |
| 54 | 10 | padding | Zero |

**Message-Types:**
- `ANNOUNCE` (0x01): Leader → Multicast alle 100 ms (Heartbeat)
- `SYNC_REQ` (0x02): Follower → Leader Unicast alle 50 ms
- `SYNC_RESP` (0x03): Leader → Follower Unicast

### 4.3 Netzwerk-I/O (`net_io.{c,h}`)

- **Multicast-Gruppe:** `239.192.88.100:47200`
- **node\_id:** letztes Oktett der routbaren DHCP-IPv4 (10.0.0.XY als Fallback)
- **Timestamping:** Userspace `clock_gettime(CLOCK_MONOTONIC_RAW)` direkt vor `sendto` und nach `recvfrom` — **kein** Kernel-SO\_TIMESTAMPING (würde CLOCK\_REALTIME mischen)
- **Socket-Options:** `SO_REUSEADDR`, `SO_REUSEPORT`, `IP_TOS=IPTOS_LOWDELAY`, `SOCK_NONBLOCK`
- **Interface-agnostisch:** eth0 oder wlan0, exklusiv umschaltbar via Agent

### 4.4 Min-Delay-Filter (`min_delay.{c,h}`)

Verwirft Ausreißer (Scheduler-Spikes, IRQ-Stürme, Congestion-Bursts):

1. Rolling Ring-Buffer der letzten N=10 RTT-Samples
2. `current_min = min(buffer)`
3. Sample-Akzeptanz: `rtt <= current_min + 10 µs`

**Reset-Trigger:** Leader-Change, Sequence-Discontinuity (>1024 rückwärts), Phase-Step >5 ms, HOLDOVER-Entry, LISTEN/CANDIDATE-Entry.

### 4.5 Kalibrierung (`calibrate.{c,h}`)

Ermittelt statische `LatencyCorrection_ns` (Userspace+Kernel+NIC-Stack-Latenz):
- UDP-Loopback auf `127.0.0.1:47201`, 50 Samples
- `mn = min(samples)`, Outlier-Rejection >mn+20 µs
- Ergebnis: `mn / 2` (einseitige Latenz) ≈ 3–4 µs auf Pi 4B

**Trigger:** Startup (GROUND→CALIBRATION), nach Leader-Election, nach HOLDOVER-Expiration, manuell via Command-Channel.

### 4.6 PI-Regler (`pi_ctrl.{c,h}`)

**Regelgröße:** `e = led_mid - vclock_now(fol_mid)` (vClock-relativer Offset, nicht rohes θ)

- **Kp = 0.05** (Q16.16 = 3276), **Ki = 0.005** (Q16.16 = 327)
- **Rate-Clamp:** ±1000 ppm vom Nominal
- **Anti-Windup:** Integrator auf ±1000 ppm geklemmt
- **Update-Periode:** 50 ms
- **Step-Trigger:** 3 konsekutive Samples mit |e| > 1 ms → `vclock_step_offset()`
- **Discipline-Reset bei Leader-Wechsel:** `min_delay_reset` + `pi_ctrl_init`

**Festkomma-Arithmetik (kein Float im RT-Thread):**
```c
int64_t kp_term_ppm = (KP_Q16 * e_ns * 1000000LL / period_ns) / 65536;
int64_t ki_term_ppm = (KI_Q16 * e_ns * 1000000LL / period_ns) / 65536;
integrator = clamp(integrator + ki_term_ppm, -1000, 1000);
int64_t rate_delta_q32 = total_ppm * 4295;  // 1 ppm = 2^32/1e6
new_rate = clamp(NOMINAL_Q32 + rate_delta_q32, MIN_Q32, MAX_Q32);
```

### 4.7 Leader-Election (`election.{c,h}`)

**Sticky term-basierte Leadership:** amtierender Leader bleibt bis Ausfall.

- **Höchster Election-Term gewinnt** (NodeID nur Tiebreak bei gleichem Term)
- **Election-Timeout:** randomisiert 250–500 ms (verhindert Kollisionen)
- **Demotion:** nur bei strikt höher-getermtem fremden ANNOUNCE
- **Rückkehrer** (frisch gestartet, Term=0) → wird automatisch FOLLOWER

```
if (peer_term > our_term)       → become_follower (übernehmen)
if (peer_term < our_term)       → ignorieren (wir sind frischer)
if (peer_term == our_term):
  if (peer_id < own_id)         → become_follower (Tiebreak)
  else                          → ignorieren
```

### 4.8 State Machine (`statemachine.{c,h}`)

**7 Zustände:**

| Zustand | Beschreibung |
|---|---|
| GROUND | 2 s Startup-Stabilisierung, TX suppressed |
| CALIBRATION | Loopback-Latenz messen (50 Samples) |
| LISTEN | Passive Discovery, Election-Timeout läuft |
| CANDIDATE | Election in Progress |
| FOLLOWER | Sync zu Leader, SYNC\_REQ alle 50 ms |
| LEADER | Authoritative Clock Source, ANNOUNCE alle 100 ms |
| HOLDOVER | Temporärer Freerun (Rate eingefroren, GPIO 23 HIGH) |

**Wichtige Transitionen:**

| Von | Trigger | Nach | Aktion |
|---|---|---|---|
| GROUND | 2 s elapsed | CALIBRATION | Start Self-Calibration |
| CALIBRATION | 50 Samples ok | LISTEN | vclock\_init, Pulse-Engine aktivieren |
| LISTEN | ANNOUNCE empfangen | FOLLOWER | Sync-Request beginnen |
| LISTEN | Election-Timeout | CANDIDATE | Timeout re-armen |
| CANDIDATE | Election-Timeout | LEADER | term++, Heartbeats starten |
| CANDIDATE | ANNOUNCE höher-getermt | FOLLOWER | leader\_id übernehmen |
| LEADER | ANNOUNCE strikt höherer Term | CANDIDATE | Demotion |
| FOLLOWER | 3 Heartbeats verpasst | HOLDOVER | Rate-Freeze, GPIO 23 HIGH |
| HOLDOVER | ANNOUNCE empfangen | FOLLOWER | Re-Sync (Discipline-Reset) |
| HOLDOVER | 10 s elapsed | CANDIDATE | Re-Election, Filter-Reset |

**Lock-Mode** (manuell für Tests):
- `LOCK_AS_LEADER`: fremde ANNOUNCEs ignoriert, term+1000
- `LOCK_AS_FOLLOWER`: Election-Timeout ignoriert, bleibt passiv
- `AUTO`: normale term-basierte Election

### 4.9 GPIO + Pulse Engine (`gpio_bcm2711.{c,h}`, `pulse_engine.{c,h}`)

- **GPIO-Zugriff:** `mmap` auf `/dev/gpiomem` (kein `/dev/mem`, kein `CAP_SYS_RAWIO`)
- **GPIO 18:** Sync-Puls — 10 ms HIGH ab globaler Sekunde, dann LOW (1 Hz)
- **GPIO 23:** Health-Indikator — LOW nur wenn |offset| < 100 µs für 10 s am Stück
- **Latenz pro `gpio_write()`:** 32-bit Store auf mmap'd Page → typisch < 1 µs

**Pulse-Scheduling-Logik:**
```c
T_now_global = vclock_now(vc, t_local_raw);
T_next_global_sec = ceil((T_now_global + 1ms) / 1e9) * 1e9;
T_next_local = vclock_local_for_global(vc, T_next_global_sec);
timerfd_settime(pe.timer_fd, TFD_TIMER_ABSTIME, T_next_local);
```

### 4.10 Telemetrie (`telemetry.{c,h}`)

`/dev/shm/drs-sync.state` — 128-Byte Shared Memory, lock-frei via Seqlock.

```c
struct drs_telemetry {
    uint32_t pid;
    uint32_t version;
    volatile uint32_t state;           // drs_state_t
    volatile uint32_t flags;           // bitmask
    volatile uint32_t leader_node_id;
    volatile uint32_t election_term;
    volatile int64_t  offset_ns;
    volatile int64_t  rate_q32_32;
    volatile int64_t  latency_corr_ns;
    volatile int64_t  last_rtt_ns;
    volatile uint64_t convergence_unix_ns;
    volatile uint32_t holdover_remaining_ms;
    volatile uint32_t error_code_last;
    volatile uint32_t error_count;
    volatile uint32_t pulse_count;
    volatile uint32_t lock_mode;
    uint8_t  reserved[36];
};
```

**Update-Frequenz:** 50-ms-Tick im Sync-Daemon. Writer: drs\_syncd (Single-Writer). Reader: agent.py, drs\_sync\_inspect (Multi-Reader, read-only mmap).

### 4.11 Strukturiertes Error-Handling (`errors.{c,h}`)

`/dev/shm/drs-sync.errors` — 4 KB Ring-Buffer, atomic O(1) ohne Lock.

| Code | Name | Ursache | Aktion | Indikation |
|---|---|---|---|---|
| 1 | CALIB\_TIMEOUT | Loopback keine Samples | Re-calibrate (max 3×) | GPIO 23 HIGH |
| 2 | HOLDOVER\_EXPIRED | 10 s ohne Heartbeat | Re-Election | GPIO 23 HIGH |
| 3 | CRC\_MISMATCH | Paket korrupt | Verwerfen + Counter++ | Telemetry only |
| 4 | SEQ\_DISCONTINUITY | Seq-Sprung >1024 | Filter-Reset | GPIO 23 HIGH |
| 5 | GPIO\_OPEN\_FAIL | /dev/gpiomem nicht zugänglich | Exit 70 | systemd-Restart |
| 6 | RT\_PRIO\_FAIL | sched\_setscheduler fail | Warnung, weiter | stderr |
| 7 | TX\_TIMESTAMP\_LOST | MSG\_ERRQUEUE leer | Sample verwerfen | Counter |
| 8 | RX\_TIMEOUT | epoll\_wait fail | Exit | journal |
| 9 | INVALID\_PACKET | Magic/Version/Größe falsch | Verwerfen + Counter++ | Counter |
| 10 | NET\_INIT\_FAIL | net\_io\_init fail | Exit 70 | — |
| 11 | SHM\_INIT\_FAIL | /dev/shm nicht beschreibbar | Telemetry-off, Sync läuft | stderr |

**Probe-Effekt-Disziplin:** Sync Hot-Path ruft **niemals** `printf`/`fprintf`/`syslog`. Agent drainiert Ring-Buffer asynchron.

### 4.12 Command-Channel (`cmd.{c,h}`)

`/dev/shm/drs-sync.cmd` — 64-Byte Shared Memory.

```c
struct drs_cmd_channel {
    volatile uint32_t seq;     // atomic monotone
    volatile uint32_t code;    // drs_cmd_code_t
    volatile uint64_t unix_ns; // Diagnostic timestamp
    uint8_t  payload[48];
};
```

| Code | Name | Aktion |
|---|---|---|
| 0 | NONE | (sentinel) |
| 1 | RECALIBRATE | calibrate\_loopback() + vclock\_init + Filter-Reset |
| 2 | FORCE\_HOLDOVER | State → HOLDOVER |
| 3 | FORCE\_DEMOTE | LEADER → CANDIDATE |
| 4 | RESET\_FILTERS | min\_delay\_reset + pi\_ctrl\_init |
| 5 | FORCE\_LEADER | Lock als LEADER (term + 1000) |
| 6 | FORCE\_FOLLOWER | Lock als FOLLOWER |
| 7 | AUTO\_MODE | Lock-Mode → AUTO |

---

## 5. Anforderungen

### Funktionale Anforderungen (32 gesamt)

| ID | Anforderung | Modul |
|---|---|---|
| F-1 | Autonomous multicast discovery | net\_io.c, 239.192.88.100:47200 |
| F-2 | Virtual global monotonic clock | vclock.{c,h} |
| F-3 | Min-delay filtering | min\_delay.{c,h}, Window=10 |
| F-4 | Deterministic leader election | election.{c,h}, sticky term-basiert |
| F-5 | Explicit fault recovery mapping | errors.{c,h} + ERRORS.md |
| F-6 | Lock-free telemetry | /dev/shm/drs-sync.state |
| F-7 | Stable convergence detection | GPIO 23 nach 10s-Stable |
| F-8 | Immediate leader demotion | sm\_on\_announce(), nur bei strikt höherem Term |
| F-9 | Non-blocking writer precedence | Seqlock in vclock.c |
| F-10 | Monotonic nanosecond representation | CLOCK\_MONOTONIC\_RAW |
| F-11 | GPIO validation pulse | pulse\_engine, GPIO 18 @ 1 Hz |
| F-12 | Fixed performance governor | drs-perf-governor.service |
| F-14 | Automatic filter reset | min\_delay\_reset() multiple Trigger |
| F-15 | Fixed 64-byte packets | \_Static\_assert Compile-Zeit |
| F-16 | 100 ms synchronization timeout | Heartbeat + 3-miss-Hysterese |
| F-17 | Step vs slew discipline | pi\_ctrl.c Dual-Loop |
| F-18 | Fail-silent GPIO indicator | GPIO 23 LOW nur bei Sync stable |
| F-19 | Seamless late joiners | FSM ohne Leader-Reset |
| F-20 | Deterministic isolated startup | GROUND 2s, TX suppressed |
| F-21 | Static latency calibration | calibrate.c 50 Samples |
| F-24 | Explicit endian-safe serialization | htonl/htons/htobe64 |
| F-25 | Holdover freerun support | Rate-Hold, Offset-Freeze |
| F-26 | CRC packet validation | IEEE 802.3 reflected |
| F-28 | Integral windup prevention | Integrator-Clamp ±1000 ppm |
| F-29 | Step confirmation hysteresis | 3 konsekutive Samples |
| F-31 | Automated calibration | Trigger auto on startup |
| F-32 | Calibration-on-election | Trigger nach LEADER-Promote |

### Nicht-funktionale Anforderungen (16 gesamt)

| ID | Anforderung | Umsetzung |
|---|---|---|
| NF-1 | Physical pulse delta < 100 µs | Externe Verifikation via Logic Analyzer |
| NF-2 | Pure user-space implementation | Keine Kernel-Module, kein adjtimex |
| NF-3 | Jitter resilience under mixed traffic | Min-Delay-Filter mit 10 µs Toleranz |
| NF-4 | Lock convergence within 10 s | Election + Calibration ≤ 5 s, Sync ≤ 10 s |
| NF-5 | 3-heartbeat hysteresis | Follower-Timeout = 3 × 100 ms |
| NF-6 | Writer never blocked by readers | Seqlock-Pattern in vclock + Telemetry |
| NF-7 | SCHED\_FIFO priority 85 | rt\_apply\_all(cpu=3, prio=85) + systemd |
| NF-8 | mlockall memory locking | rt\_lock\_memory() + LimitMEMLOCK=infinity |
| NF-9 | No hot-path disk or console I/O | Telemetrie via /dev/shm-mmap |
| NF-10 | Isolated Core 3 execution | isolcpus=3 nohz\_full=3 rcu\_nocbs=3 |
| NF-11 | Max slew = 1000 ppm | Rate-Clamp im PI-Controller |
| NF-12 | No dynamic heap allocation in hot path | Pre-allocated Buffer vor mlockall |
| NF-13 | No packet fragmentation | 64 B << MTU 1500 |
| NF-14 | Deterministic packet parsing | Feste Offsets, kein struct-cast |
| NF-15 | No mutex contention in sync loop | Seqlock + atomare 64-bit-Ops |
| NF-16 | Core 3 sanctity | systemd CPUAffinity=3 + IRQ-Pinning |

### Architektur-Verbote

- Kernel-Clocks modifizieren — vclock ist rein virtuell
- Custom Kernel-Module — nur Userspace + Standard-PREEMPT\_RT-Kernel
- TCP — nur `SOCK_DGRAM`
- Distributed-Consensus — kein Quorum, kein Paxos/Raft
- Mutex im Hot-Path — Seqlock + atomare Operationen
- Filesystem-I/O im Sync-Loop — nur `/dev/shm`-mmap
- Float im RT-Thread — Q32.32 / Q16.16 Festkomma, `__int128` für Zwischenergebnisse
- `printf`/`fprintf`/`syslog` im Sync-Thread
- `malloc`/`realloc`/`free` nach `mlockall`

---

## 6. Mathematische Grundlagen

### Offset-Schätzung (klassische PTP/NTP-Zwei-Wege-Messung)
```
θ = ((T2 - T1) + (T3 - T4)) / 2
```
Annahme: Path-Delay symmetrisch. Asymmetrie (z.B. Switch-Queue-Längen) → residueller Offset → wird vom Min-Delay-Filter minimiert.

### Round-Trip-Delay
```
δ = (T4 - T1) - (T3 - T2)
```
- T4 - T1 = Gesamt-Round-Trip am Follower
- T3 - T2 = Verarbeitungszeit beim Leader
- Differenz = reine Netzwerk-RTT

### Virtual-Clock-Mapping (vorwärts)
```
T_global = (T_local_raw - base_local) * Rate >> 32 + base_global + Offset - LatencyCorrection
```
`base_local` und `base_global` werden bei Rate-Change und Step-Offset simultan aktualisiert (kein Knick in T_global).

### Inverse (für Pulse-Timer-Scheduling)
```
T_local_raw = base_local + (y << 32) / Rate
  wobei y = T_global - base_global - Offset + LatencyCorrection
```

### PI-Regler (Slew-Update alle 50 ms, |e| ≤ 1 ms)
```
P_term     = Kp * e / period
I_term     = clamp(I_term + Ki * e / period, ±1000 ppm)
total_ppm  = P_term + I_term
rate_delta = total_ppm * 4295              // 1 ppm = 2^32/1e6
rate_new   = clamp(NOMINAL + rate_delta, MIN, MAX)   // positional
```

**Konstanten:**
- Kp = 0.05 ≈ 3276 (Q16.16)
- Ki = 0.005 ≈ 327 (Q16.16)
- 1 ppm = 2^32/1e6 ≈ 4295 (Q32.32 Rate-Tick)

---

## 7. Performance-Budget

| Komponente | Budget | Realität Pi 4B | Maßnahme |
|---|---|---|---|
| NIC-Timestamp-Jitter | < 20 µs | ~5–10 µs (Userspace-TS) | Min-Delay-Filter |
| Scheduler-Latency | < 30 µs | < 5 µs (PREEMPT\_RT) | SCHED\_FIFO 85 + isolcpus |
| Filter-Residual | < 20 µs | < 10 µs | Window=10, min-select |
| GPIO-Generation | < 20 µs | < 1 µs (/dev/gpiomem) | Register-Direkt |
| **Total worst** | **< 100 µs** | **p50=17 µs, max=26 µs** | **Closed-Loop PI** |

**Mess-Realität:**
- `lat_corr` (Loopback-Latenz) = 3900–4000 ns (≈ 4 µs)
- Round-Trip auf Loopback: ~8 µs
- Realer Network-RTT erwartet: 50–150 µs auf 1G-Switch

---

## 8. Test-Szenarien (Pflicht)

| Test | Beschreibung | Acceptance-Kriterium |
|---|---|---|
| T1 — Baseline | 3 Nodes auf Ethernet, 10 min Logic-Analyzer-Capture | max\|Δt\| < 100 µs |
| T2 — Dynamic Discovery | 2 Nodes laufen und konvergieren, dann Node 3 einstecken | Konvergenz in < 20 s |
| T3 — Crash Failure | Leader-Strom abziehen, verbleibende Nodes re-elektieren | Recovery < 11 s (10 s HOLDOVER + Election) |
| T4 — High Jitter | iperf3 saturiert WLAN, Sync läuft auf eth0 | Ethernet-Sync unbeeinträchtigt |

**Acceptance-Metriken (NF-1):**
- `max(|Δt|)` über 10 min: < 100 µs
- `p99(|Δt|)`: < 80 µs
- `p50(|Δt|)`: < 50 µs
- GPIO 23 LOW-Dauer: > 95 % der Messdauer
- Recovery nach Leader-Crash: < 11 s

---

## 9. Externe Verifikation (Falsifizierbarkeit)

**Problem:** Interne Software-Timestamps können die eigene Präzision nicht beweisen — sie unterliegen demselben OS-Jitter den sie messen sollen.

**Lösung:** Logic-Analyzer extern an GPIO 18 aller Nodes (GND shared — kritisch!).

**Hardware-Setup:**
```
Pi 4B GPIO-Header (40-pin)
Pin 12 (GPIO 18) --- Sync-Pulse-Output ----+
Pin 16 (GPIO 23) --- Health-Output    -----+--- Logic Analyzer (Saleae Logic Pro)
Pin 6  (GND)     --- Common Ground   ------+
```

**Tool `verify_pulse.py`** (CSV-Auswertung des Logic-Analyzer-Exports):
1. CSV einlesen (Time [s], Channel 0, Channel 1, ...)
2. Pro Kanal rising-edges via Sub-Sample-Linear-Interpolation
3. Pro Kanal-Paar (Referenz=Ch0): nächste Edge im ±50 ms-Fenster
4. Statistik: p50/p99/p99.9/max
5. Exit 0 wenn max < threshold, sonst 1

**Aktuell verifiziert (2026-05-25):** p50=17 µs, max=26 µs, Drift=0.055 ppm über 90 s → **PASS**

---

## 10. Hardware-Setup

### Pi 4B Konfiguration
```bash
# /boot/firmware/cmdline.txt
isolcpus=3 nohz_full=3 rcu_nocbs=3

# /boot/firmware/config.txt
arm_boost=0
force_turbo=1
arm_freq=1500
```
Deaktiviert DVFS → konstante CPU-Frequenz → keine frequenzabhängige Sync-Drift.

### Netzwerk
- Single-Hop Gigabit-Ethernet (Layer-2-switched, kein NAT, MTU 1500)
- DHCP primär (`192.168.1.x`), `10.0.0.XY` als Fallback (X=Team 1–8, Y=Node 1–3)
- eth0 und wlan0 **nie gleichzeitig im selben Subnetz** (exklusiver Switch via Agent)
- NF-1-Garantie (<100 µs) **nur kabelgebunden**; WiFi = ms-Klasse

**Lab-Fallback-IPs (Team 3):** 10.0.0.31 / .32 / .33

---

## 11. Web-UI & HTTP-API

**URL:** `http://<host>:8080/` — 5-Tab SPA (Topology, Network, Measurement, **Sync**, System)

### Wichtige API-Endpunkte

| Methode | Pfad | Funktion |
|---|---|---|
| GET | `/api/sync/state` | Live-Zustand des lokalen drs\_syncd (JSON) |
| GET | `/api/sync/health` | Pre-Flight-Checks (12 Checks, critical/warning/info) |
| POST | `/api/sync/control` | Steuerung: start/stop/restart/recalibrate/force-leader/etc. |
| GET | `/api/sync/cluster` | Aggregierte Cluster-Sicht (alle peers.json) |
| GET | `/api/sync/errors?limit=N` | Letzter N Error-Ring-Buffer-Einträge |
| POST | `/api/peers/auto_discover` | DHCP-Subnetz-Scan für Peer-Discovery |
| POST | `/api/transport` | Exklusiver eth0↔wlan0-Switch (WiFi sticky) |
| GET | `/api/wifi/scan` | SSID-Scan via wpa\_supplicant-Control-Socket |
| POST | `/api/wifi/connect` | Body {ssid, psk} — verbindet wlan0 |

---

## 12. Build & Deployment

### Build
```bash
make                              # baut build/drs_syncd + build/drs_sync_inspect
make test                         # baut + führt 5 Unit-Tests aus
make install                      # installiert Binaries + systemd-Units
CROSS=aarch64-linux-gnu- make     # Cross-Compile vom Dev-Host
```

**Compiler-Flags:** `-O2 -g -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE -lpthread -lrt`

**Unit-Tests:**

| Test | Zweck | Resultat |
|---|---|---|
| test\_crc32 | IEEE 802.3 reflected | "123456789" → 0xCBF43926 |
| test\_proto | Serialize/Deserialize-Roundtrip | ok |
| test\_min\_delay | Outlier-Rejection | ok |
| test\_pi\_ctrl | Step + Slew + Rate-Clamp | drift\_per\_iter ≈ 0 |
| test\_vclock | Rate-Skalierung, Step, Inverse | drift\_after\_1s ≈ 1 ms (1000 ppm) |

### Deployment-Scripts
```bash
./scripts/flash-sd.sh /dev/sdX [hostname]           # Pi-OS-Lite + SSH-Key + Hostname
./scripts/install-rt-kernel.sh                       # PREEMPT_RT, cmdline, gpio-Gruppe
./scripts/deploy-philippknode.sh                     # rsync → remote build → systemctl restart
./scripts/deploy-cluster.sh pi1.lan pi2.lan pi3.lan  # parallel für alle Nodes
```

### Verzeichnisstruktur (`drs_sync/`)
```
src/
  drs_syncd.c          # Main-Loop ~370 LOC
  proto_v2.{c,h}       # 64-Byte Wire-Protokoll
  crc32.{c,h}          # IEEE 802.3 reflected
  vclock.{c,h}         # Virtuelle Uhr (Seqlock + Q32.32)
  statemachine.{c,h}   # 7-State FSM + Lock-Mode
  election.{c,h}       # Sticky term-basierte Leadership
  pi_ctrl.{c,h}        # Dual-Loop PI (Step + Slew)
  min_delay.{c,h}      # Rolling-Window Outlier-Filter
  calibrate.{c,h}      # Loopback-Latenz
  net_io.{c,h}         # UDP-Sockets + Multicast
  gpio_bcm2711.{c,h}   # /dev/gpiomem mmap
  pulse_engine.{c,h}   # GPIO 18/23
  telemetry.{c,h}      # /dev/shm/drs-sync.state
  errors.{c,h}         # Ring-Buffer + Codes
  cmd.{c,h}            # /dev/shm/drs-sync.cmd
include/drs_sync_config.h   # Alle Konstanten
tests/                 # 5 Unit-Tests
tools/
  drs_sync_inspect.c   # CLI Telemetrie-Reader
  verify_pulse.py      # Logic-Analyzer-Auswertung
agent/agent.py         # Python HTTP API (~1700 LOC)
web/                   # 5-Tab SPA
scripts/               # Deploy-Scripts
systemd/               # 3 systemd-Units
Makefile
```

---

## 13. Wichtige Konstanten (`include/drs_sync_config.h`)

```c
DRS_MAGIC_V2               = 0x44525354
DRS_VERSION                = 0x02
DRS_PAYLOAD_BYTES          = 64
DRS_PORT                   = 47200
DRS_MCAST_GROUP            = "239.192.88.100"
DRS_CALIB_LOOPBACK_PORT    = 47201

DRS_GROUND_DURATION_NS     = 2 000 000 000  // 2 s
DRS_LEADER_ANNOUNCE_NS     = 100 000 000    // 100 ms
DRS_SYNC_EXCHANGE_NS       = 50 000 000     // 50 ms
DRS_FOLLOWER_TIMEOUT_NS    = 300 000 000    // 300 ms (3 × ANNOUNCE)
DRS_HOLDOVER_MAX_NS        = 10 000 000 000 // 10 s
DRS_ELECTION_MIN_NS        = 250 000 000    // 250 ms
DRS_ELECTION_MAX_NS        = 500 000 000    // 500 ms

DRS_PULSE_PERIOD_NS        = 1 000 000 000  // 1 s
DRS_PULSE_HIGH_DURATION_NS = 10 000 000     // 10 ms
DRS_STABLE_WINDOW_NS       = 10 000 000 000 // 10 s
DRS_STABLE_OFFSET_THRESH_NS= 100 000        // 100 µs

DRS_MIN_DELAY_WINDOW       = 10
DRS_MIN_DELAY_TOLERANCE_NS = 10 000         // 10 µs
DRS_CALIB_SAMPLES          = 50
DRS_CALIB_OUTLIER_NS       = 20 000         // 20 µs

DRS_PI_KP_Q16              = 3276           // ≈ 0.05
DRS_PI_KI_Q16              = 327            // ≈ 0.005
DRS_PI_UPDATE_PERIOD_NS    = 50 000 000     // 50 ms
DRS_STEP_THRESHOLD_NS      = 1 000 000      // 1 ms
DRS_STEP_CONFIRM_SAMPLES   = 3

DRS_RATE_NOMINAL_Q32       = 0x100000000    // 1.0
DRS_RATE_PPM_Q32           = 4295           // 2^32/1e6
DRS_RATE_MAX_PPM           = 1000

DRS_RT_CPU                 = 3
DRS_RT_PRIO                = 85
DRS_GPIO_PULSE_PIN         = 18
DRS_GPIO_HEALTH_PIN        = 23
```

---

## 14. Glossar

| Term | Bedeutung |
|---|---|
| DRS | Distributed Real-Time System |
| HOLDOVER | Freerun-Modus mit eingefrorener Rate, kein Sync-Update |
| Sticky Leadership | Amtierender Leader bleibt bis Ausfall; NodeID nur Tiebreak bei gleichem Term |
| Election-Term | Monoton steigende Runden-Nummer; maßgeblich für Leadership (höchster Term gewinnt) |
| Seqlock | Lock-frei via geraden/ungeraden Sequence-Counter |
| Step-Mode | Harter Phase-Sprung der vClock bei \|θ\| > 1 ms |
| Slew-Mode | Sanfte Frequenzkorrektur via PI-Regler bei \|θ\| ≤ 1 ms |
| SCHED\_FIFO | Real-Time-Scheduler, läuft bis es selbst blockt |
| Q32.32 | Festkomma: 32 Bit Integer + 32 Bit Fraction (signed 64-bit) |
| Q16.16 | Festkomma: 16 Bit Integer + 16 Bit Fraction (signed 32-bit) |
| θ (theta) | Phase-Offset zwischen Follower-Uhr und Leader-Uhr |
| δ (delta) | Round-Trip-Delay (reines Netzwerk-RTT) |
| ppm | parts-per-million, Frequenzabweichung (10⁻⁶) |
| PREEMPT\_RT | Linux-Real-Time-Kernel-Patch, deterministische Latenzen |
| mDNS | Multicast-DNS, Auflösung von .local-Hostnamen via avahi |
| T1/T2/T3/T4 | PTP-Zwei-Wege-Timestamps (Follower-Send, Leader-Recv, Leader-Send, Follower-Recv) |
