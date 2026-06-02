# DRS Architecture Summary — V3 → V6 Evolution

**Quellen:** architecture/0–6 (vollständig gelesen)
**Primärquellen:** architecture/3 (Master-Baseline v2.1), architecture/5 (v2.2 Bug-Fixes), architecture/6 (finale Implementierung)

---

## 1. Überblick Version 3 (Master Architecture v2.1)

`architecture/3_drs_architecture_reviewed_fixed_v_21.md` ist das zentrale Baseline-Dokument. Es enthält die erste vollständige, implementierungsreife Spezifikation aller Kernkonzepte.

### Kernkonzepte V3

**Zeithaltung:**
- Virtueller Takt `Tglobal = (Tlocal_raw × Rate) + Offset − LatencyCorrection_ns`
- Rate als Q32.32-Festkomma, nie Floating-Point im RT-Thread
- Ausschließlich `CLOCK_MONOTONIC_RAW` — `CLOCK_REALTIME` verboten
- Seqlock für lock-freie Concurrent-Reads (Writer: Sync-Thread; Reader: Agent, Pulse-Engine)

**Synchronisationsprotokoll:**
- 4-Timestamp PTP-Stil: T1 (Follower send), T2 (Leader recv), T3 (Leader send), T4 (Follower recv)
- Offset θ = ((T2−T1)+(T3−T4))/2
- RTT δ = (T4−T1)−(T3−T2)
- Userspace-Timestamping via `clock_gettime(CLOCK_MONOTONIC_RAW)` direkt vor/nach sendto/recvfrom (kein SO_TIMESTAMPING)

**Jitter-Filterung:**
- Min-Delay Rolling Window N=10: akzeptiere RTT ≤ min+10 µs (V3-Wert; in V6 auf 50 µs geändert — siehe Abschnitt 2.3)

**Regler:**
- PI-Regler: Kp=0,05, Ki=0,005, 50 ms Update, ±1000 ppm Clamp
- Step-Mode (|θ|>1 ms, 3 konsekutive Samples) vs. Slew-Mode (|θ|≤1 ms, Frequenzkorrektur)
- Positionale PI-Law (nicht inkrementell): `rate_new = NOMINAL + Kp·e + Ki·∫e dt`

**Leader-Election in V3:**
- Modified-Bully-Algorithmus: **niedrigste NodeID gewinnt**
- NodeID ist primäres Entscheidungsmerkmal

**7-Zustands-FSM:**
`GROUND → CALIBRATION → LISTEN → CANDIDATE ↔ FOLLOWER/LEADER`, `HOLDOVER`

**Anforderungsumfang V3:**
- 32 funktionale Anforderungen (F-1–F-32)
- 16 nicht-funktionale Anforderungen (NF-1–NF-16)
- Vollständige systemd-Unit-Beispielkonfiguration

**Performance-Budget V3 (theoretisch):**

| Komponente | Budget |
|------------|--------|
| NIC-Timestamp-Jitter | < 20 µs |
| Scheduler-Latency | < 30 µs |
| Filter-Residual | < 20 µs |
| GPIO-Generation | < 20 µs |
| **Total worst** | **< 100 µs** |

---

## 2. Änderungen Version 3 → 6

### Evolutionspfad

```
V0 (Anchor)  →  V1  →  V2  →  V3/v2.1 (Baseline)  →  v2.2 (Bug-Fixes)  →  V6 (Final)
```

### 2.1 V0 → V1 → V2 (Vorgeschichte zu V3)

Diese Versionen sind nur als Änderungshistorie relevant; V3 ersetzt sie vollständig.

| Aspekt | V0 (Anchor Rev.7) | V1 | V2 |
|--------|-------------------|----|----|
| Sync-Algorithmus | Unspezifiziert | Cristian's Algorithm + EWMA | 4-Timestamp (spezifiziert) |
| Virtuelle Uhr | CNI/NBW Telemetrie | `t_global = t_MONO + Δoffset` (kein Rate) | Vollständig wie V3 |
| Leader-Election | Lowest-IP gewinnt | Bully + Quorum (⌈N/2⌉+1) | Dual-Master (primary + backup) |
| Multicast | Nicht spezifiziert | 239.255.0.1:49152 | 239.192.88.100:47200 |
| RT-Anforderung | SCHED_FIFO >80 | Nicht explizit | PREEMPT_RT als Pflicht erkannt |
| GPIO Health | LOW wenn >200 µs | Nicht erwähnt | Nicht erwähnt |

### 2.2 V3 → v2.2: Kritische Bug-Fixes (Real-Hardware Bring-Up)

`architecture/5_ARCHITECTURE_V2_2.pdf` dokumentiert 4 Implementierungsfehler aus dem Hardware-Bring-Up, die im V3-Spec latent waren, sowie neue Anforderungen F-33–F-36:

**Bug A.1 — Step-Richtung invertiert**

| | V3 (fehlerhaft) | v2.2 (korrekt) |
|--|-----------------|----------------|
| Step-Offset-Anwendung | `Offset -= θ_residual` | `Offset += θ_residual` |
| Symptom | θ_residual ≈ 0 im Telemetry, aber GPIO zeigt 50–100 ms Offset | Korrekte Konvergenz |
| Ursache | −θ geht rechnerisch zu null, alignt die vClock aber in falsche Richtung | +θ alignt Follower physikalisch zum Leader |

**Bug A.2 — Wiederholtes Stepping ohne Accumulator**

| | V3 (fehlerhaft) | v2.2 (korrekt) |
|--|-----------------|----------------|
| Step-Anwendung | Jeder Step-Aufruf wendet vollen θ an | `θ_residual = θ + step_accum_ns` |
| Symptom | Puls-Periode oszilliert 1,13–1,17 s, Phase wandert ~52 ms pro 200 ms | Einmaliger Schritt, stabile Konvergenz |
| Ursache | θ kommt von CLOCK_MONOTONIC_RAW, ändert sich nicht mit vClock-Steps → 3-confirm-Hysterese feuert wiederholt | step_accum_ns = laufende Summe von −θ_residual der bisherigen Steps |

**Sign-Convention für step_accum_ns (normativ):**

| Symbol | Definition | Vorzeichen |
|--------|-----------|------------|
| θ | ((T2−T1)+(T3−T4))/2 | Positiv wenn Leader früher gebootet |
| step_accum_ns | Laufende Summe von −θ_residual je Step | Negativ nach erstem Step (typisch) |
| θ_residual | θ + step_accum_ns | ≈ 0 nach korrekter Ausrichtung |
| Δ auf vClock | +θ_residual | Positiv (alignt Follower zum Leader) |

**Bug A.3 — Rate nach Transient bei −1000 ppm festgeklemmt**

| | V3 (fehlerhaft) | v2.2 (korrekt) |
|--|-----------------|----------------|
| PI-Integer-Arithmetik | `kp_term = (KP_Q16 * e_ns) >> 16` | `kp_term = (KP_Q16 * e_ns * 1000000LL / period_ns) / 65536` |
| Ursache 1 | `>>16` sign-extends bei negativen int64 → leckt −1 ppm pro Tick | `/65536` trunciert symmetrisch gegen null |
| Ursache 2 | Step resetete θ_residual aber nicht vClock-Rate → Slew lief weiter | Rate-Reset bei jedem Step: `vclock_set_rate(NOMINAL)` + `pi_ctrl_init()` |
| Symptom | GPIO-Phase wächst exakt 1 ms/s, API zeigt `rate_ppm = −1000` | Korrekte Rückkehr zum Nominal |

**Rate-Reset bei Step (F-34) — drei Aufrufe simultan mit `vclock_step_offset()`:**
```c
vclock_set_rate(vc, DRS_RATE_NOMINAL_Q32, t_now);
pi_ctrl_init(&sm.pi);
sm.last_rate_q32_32 = DRS_RATE_NOMINAL_Q32;
```

**Bug A.4 — Telemetrie-Felder still 0**

| | V3 | v2.2 |
|--|-----|------|
| Betroffene Felder | `last_rtt_ns`, `convergence_unix_ns` immer 0 | Explizite Update-Aufrufe in allen Sync-Pfaden |
| Effekt | Agent/Inspector blind für Sync-Health | Alle Felder live befüllt (F-36) |

**Neue Anforderungen v2.2 (F-33–F-36):**

| ID | Anforderung |
|----|-------------|
| F-33 | Step accumulator (§5.5.3) — one-shot boot-delta correction |
| F-34 | Rate reset on step (§5.5.4) — discard obsolete slew state |
| F-35 | Sign-symmetric controller arithmetic (§5.6.1) — no >>-leak |
| F-36 | Telemetry completeness (§9.2) — all fields populated live |

**NF-1-Hardware-Caveat (v2.2 Section 14.1):**
BCM54213PE PHY implementiert **kein** IEEE 1588 Hardware-Timestamping. SO_TIMESTAMPING (Software) fügt 5–20 µs Jitter pro Richtung hinzu. Realistischer Boden auf Pi 4B + Standard-Switch: **100–200 µs** GPIO-Delta. Das Dokument sagt explizit: *"achieving it on Pi 4B is not expected from the stock platform"* ohne PTP-PHY oder externe 1-PPS-Referenz. Die gemessenen 17 µs/26 µs in V6 sind trotzdem real — erreicht durch PREEMPT_RT + isolierten Core 3, nicht durch Hardware-Timestamping.

### 2.3 v2.2 → V6: Architektur-Erweiterungen

`architecture/6_ARCHITECTURE.pdf` ist das finale Implementierungsdokument. Enthält alle v2.2-Fixes plus:

**Änderung 1 — Leadership-Modell: Paradigmenwechsel**

| Aspekt | V3/v2.2 (Modified Bully) | V6 (Sticky Term-Based) |
|--------|--------------------------|------------------------|
| Entscheidungskriterium | Niedrigste NodeID gewinnt | Höchster Election-Term gewinnt |
| NodeID-Rolle | Primäres Entscheidungsmerkmal | Nur Tiebreak bei gleichem Term |
| Rückkehrender Knoten | Kann Leader werden wenn niedrigste ID | Wird immer Follower (Term=0 < laufender Term) |
| Amtierender Leader | Kann durch niedrigere ID verdrängt werden | Bleibt bis Ausfall (sticky) |
| Rationale | Deterministisch, einfach | Verhindert Leadership-Flapping bei Neustart |

**Änderung 2 — Min-Delay-Toleranz erhöht**

| | V3 | V6 |
|--|----|----|
| `DRS_MIN_DELAY_TOLERANCE_NS` | 10 µs | **50 µs** |
| Grund | Zu eng für Software-Timestamping RTT-Verteilung | 50-µs-Band passt genug Samples durch, filtert echte Congestion (>100 µs) |

**Änderung 3 — Neue funktionale Anforderungen in V6**

| Neue F-Req | Inhalt |
|------------|--------|
| F-16 | Routbares Interface (nicht Loopback) für NodeID-Ableitung |
| F-17 | Interface-agnostisch: eth0 oder wlan0, exklusiv umschaltbar |
| F-19 | WiFi-Support via wpa_supplicant |
| F-22 | SYNC_REQ-Timeout (200 ms) bei ausstehender Antwort |
| F-23 | Active-before-Follower Guard (verhindert Sync vor CALIBRATION-Abschluss) |
| F-24 | Telemetrie via /dev/shm |
| F-25 | Command-Channel via /dev/shm |
| F-27 | Lock-Mode (LOCK_AS_LEADER / LOCK_AS_FOLLOWER / AUTO) |
| F-28 | /dev/gpiomem ohne Root (gpio-Gruppe) |
| F-29 | Step-Hysterese (3 konsekutive Samples) |
| F-30 | Transport-Persistenz (Interface-Wahl überlebt Neustart) |

**Änderung 4 — WiFi-Transport (war in v2.2 noch verboten)**

| | v2.2 | V6 |
|--|------|----|
| WiFi | Explizit in Architectural Constraints verboten | Unterstützt als ms-Klasse Transport |
| Koexistenz | N/A | eth0 XOR wlan0, niemals beide im selben Subnetz |
| NF-1-Gültigkeit | Ethernet only | Nur Ethernet; WiFi = ms-Klasse |
| Gemessene WiFi-Performance | — | RTT mean ≈ 0,87 ms, Offset ≈ 2,6 µs (bei ruhigem AP) |

**Änderung 5 — NF-16: Explizites Float-Verbot**

V6 macht das implizite Verbot aus V3 zur expliziten nicht-funktionalen Anforderung: kein Floating-Point im RT-Thread.

---

## 3. Aktuelle Architektur (Version 6)

### 3.1 System-Überblick

**Paradigma:** KISS — Zero-Config autonome Discovery, rein interner Monotonic-Cluster-Takt, keine externen Zeitquellen.

**Laufzeitumgebung:**
```
drs_syncd  — C11-Daemon, Core 3 (isolcpus=3 nohz_full=3 rcu_nocbs=3), SCHED_FIFO/85, mlockall
drs_agent  — Python 3, Cores 0–2, HTTP-API Port 8080
```

### 3.2 Virtuelle Uhr

```
Tglobal = (Tlocal_raw − base_local) × Rate >> 32 + base_global + Offset − LatencyCorrection_ns
```

- Rate: Q32.32 Festkomma, Nominal = 0x100000000
- Seqlock: Writer seq++ (ungerade=aktiv) → schreibe → seq++ (gerade=konsistent)
- Reader: `do { s1=load(seq); if(s1&1) continue; lese...; s2=load(seq); } while(s1≠s2)`
- Kein Float, kein Mutex im Lesepfad; Offset/Rate-Updates mit acquire/release Speicher-Ordnung
- Kalibrierung: UDP-Loopback 127.0.0.1:47201, 50 Samples, Outlier-Rejection >min+20 µs, min/2 → `lat_corr` ≈ 3900–4100 ns auf Pi 4B

### 3.3 Synchronisationsprotokoll

**Paket:** 64 Byte, Big-Endian, CRC32 (IEEE 802.3, Polynom 0xEDB88320 reflected)

| Offset | Bytes | Feld |
|--------|-------|------|
| 0 | 4 | Magic (0x44525354 = "DRST") |
| 4 | 1 | Version (0x02) |
| 5 | 1 | MsgType (0x01=ANNOUNCE, 0x02=SYNC_REQ, 0x03=SYNC_RESP) |
| 6 | 1 | Flags (bit0=LEADER, bit1=HOLDOVER, bit2=CALIBRATED, bit3=FAULT) |
| 7 | 1 | Reserved (zero) |
| 8 | 2 | Seq (u16 rollover) |
| 10 | 4 | NodeID (letztes Oktett der routbaren IPv4) |
| 14 | 4 | ElectionTerm |
| 18 | 8 | T1 |
| 26 | 8 | T2 |
| 34 | 8 | T3 |
| 42 | 8 | T4 |
| 50 | 4 | CRC32 (Feld selbst = 0 bei Berechnung, Padding eingeschlossen) |
| 54 | 10 | Padding (zero) |

**Timing:**
- ANNOUNCE (Leader → Multicast): alle 100 ms
- SYNC_REQ (Follower → Leader Unicast): alle 50 ms
- Follower-Timeout: 300 ms (3× ANNOUNCE) → HOLDOVER
- SYNC_REQ-Timeout: 200 ms → verwerfen, neuen Request senden

**Netzwerk:** Multicast 239.192.88.100:47200, SOCK_DGRAM, IP_TOS=IPTOS_LOWDELAY, SOCK_NONBLOCK

### 3.4 Min-Delay-Filter

Rolling Window N=10:
1. Berechne RTT = (T4−T1)−(T3−T2)
2. `current_min = min(window)`
3. Akzeptiere Sample nur wenn RTT ≤ current_min + **50 µs** (DRS_MIN_DELAY_TOLERANCE_NS)

Reset-Trigger: Leader-Wechsel, Seq-Diskontinuität (>1024 rückwärts), Phase-Step >5 ms, HOLDOVER-Entry, LISTEN/CANDIDATE-Entry. Bei Filter-Reset wird auch `step_accum_ns` auf 0 gesetzt.

### 3.5 PI-Regler & Step-Modus

**Telemetrie-Feld `offset_ns` trägt `θ_residual` — nicht rohes θ.** Rohes θ ist eine ~10-Sekunden-große Konstante zur Laufzeit und für den Operator bedeutungslos.

**Integer-Arithmetik (kein Float, zwei Korrektheitseigenschaften aus F-35):**
```c
// 1. /65536 statt >>16 (verhindert sign-extension bei negativen int64)
// 2. × 1000000LL statt nichts (dimensionale Vollständigkeit: ergibt ppm, nicht Fraktion)
int64_t kp_term_ppm = (KP_Q16 * e_ns * 1000000LL / period_ns) / 65536;
int64_t ki_term_ppm = (KI_Q16 * e_ns * 1000000LL / period_ns) / 65536;
integrator = clamp(integrator + ki_term_ppm, -1000, +1000);
int64_t rate_delta_q32 = total_ppm * 4295;   // 1 ppm = 2^32/1e6
new_rate = clamp(NOMINAL_Q32 + rate_delta_q32, MIN_Q32, MAX_Q32);
```

- KP_Q16 = 3276 (≈0,05), KI_Q16 = 327 (≈0,005)
- Max Slew: 1000 ppm = 50 µs pro 50-ms-Zyklus
- Positionale Law (nicht inkrementell)

**Step-Then-Slew Lifecycle — zwei Micro-States in FOLLOWER:**

| Micro-State | Bedingung | Verhalten |
|-------------|-----------|-----------|
| Aligning | \|θ_residual\| > 1 ms | Step-Counter zählt bis 3, dann Step; rate ← nominal; PI ← zero |
| Slewing | \|θ_residual\| ≤ 1 ms | PI-Controller aktiv, keine vClock-Sprünge |

Normalbetrieb: < 200 ms in Aligning beim Boot, Rest der Laufzeit in Slewing. Re-Entry in Aligning typisch erst nach ≥ 2 min (Kristall-Drift auf Pi 4B).

**Step-Prozedur (alle Aufrufe simultan, F-33 + F-34):**
```c
θ_residual = θ + step_accum_ns;
vclock_step_offset(vc, +θ_residual);          // vClock springt, CLOCK_MONOTONIC_RAW unberührt
step_accum_ns += -θ_residual;                  // akkumuliere angewandten Schritt
vclock_set_rate(vc, DRS_RATE_NOMINAL_Q32, t_now);  // Rate reset (F-34)
pi_ctrl_init(&sm.pi);                          // PI reset (F-34)
```

### 3.6 Leader-Election (Sticky Term-Based)

Election-Timeout seeded aus `/dev/urandom` (xorshift64) — verhindert Kollisionen bei gleichzeitigem Boot.

```
Entscheide bei empfangenem ANNOUNCE:
  peer_term > own_term  →  become_follower(peer)      [übernehmen, term adoptieren]
  peer_term < own_term  →  ignorieren                 [veraltet]
  peer_term == own_term:
    peer_id < own_id    →  become_follower(peer)       [Tiebreak: niedrigere ID]
    else                →  ignorieren
```

- Election-Timeout: randomisiert 250–500 ms
- Promotion: `election_promote()` inkrementiert lokalen Term, installiert Knoten als Leader
- Rückkehrender Knoten (Term=0 nach Reboot): wird immer Follower — kein Leadership-Flapping

**Failover Re-Acquisition bei Leader-Wechsel:**
1. Ausstehenden SYNC_REQ verwerfen (nicht ewig auf verschwundenen Leader warten)
2. Neuen Leader in `locked_leader_id` speichern
3. Min-Delay-Filter + PI-Controller resetten
4. vClock wird **nicht** resettet → 1-Hz-Puls läuft ohne Sprung weiter (F-20)

### 3.7 7-Zustands-FSM

`sm_is_active()` ist true für alle Zustände außer GROUND und CALIBRATION.

| Zustand | Funktion |
|---------|----------|
| GROUND | 2 s Startup-Stabilisierung, TX suppressed, Multicast-Listen aktiv |
| CALIBRATION | Loopback-Latenz messen (50 Samples), lat_corr setzen |
| LISTEN | Passive Discovery, Election-Timeout läuft |
| CANDIDATE | Election in Progress, Timeout läuft |
| FOLLOWER | Sync zu Leader, SYNC_REQ alle 50 ms |
| LEADER | Authoritative Clock, ANNOUNCE alle 100 ms |
| HOLDOVER | Rate eingefroren, **Offset eingefroren**, kein Sync-Update, GPIO 23 HIGH |

**Vollständige Transitions-Tabelle:**

| Von | Nach | Trigger |
|-----|------|---------|
| GROUND | CALIBRATION | 2 s elapsed |
| CALIBRATION | LISTEN | Kalibrierung fertig; lat_corr gesetzt; Pulse-Engine armed |
| LISTEN | FOLLOWER | Gültiges ANNOUNCE von erkanntem Leader |
| LISTEN | CANDIDATE | Kein ANNOUNCE im Listen-Fenster |
| CANDIDATE | LEADER | Timeout ohne höher-getermten Leader → promote |
| CANDIDATE | FOLLOWER | Höher- oder gleich-getermt-aber-niedrigere-ID Leader beobachtet |
| FOLLOWER | HOLDOVER | Leader silent für 300 ms |
| FOLLOWER | FOLLOWER | Neuer Leader (höherer Term) → discipline reset, re-acquire |
| LEADER | FOLLOWER | Strikt höher-getermtes ANNOUNCE beobachtet |
| HOLDOVER | FOLLOWER | Leader reappears innerhalb 10 s |
| HOLDOVER | CANDIDATE | 10 s Budget exhausted → re-elect + recalibrate |

**Active-Before-Follower Guard (F-23):** Ein Knoten kann FOLLOWER erst werden wenn `sm_is_active()` gilt — d.h. nach GROUND + CALIBRATION. Ein ANNOUNCE während GROUND/CALIBRATION kann nicht in FOLLOWER abkürzen. Garantiert: jeder Follower hat valide lat_corr und aktiven Pulse-Output.

**Lock-Mode:**
- LOCK_AS_LEADER: term+1000, ignoriert fremde ANNOUNCEs
- LOCK_AS_FOLLOWER: Election-Timeout ignoriert, bleibt passiv
- AUTO: normale term-basierte Election

### 3.8 HOLDOVER-Modus

Wenn Follower Leader verliert (kein ANNOUNCE für 300 ms):
- Letzter bekannter **Rate-Wert bleibt** (Rate eingefroren)
- **Offset wird eingefroren** — keine weitere Drift-Adaptation
- GPIO 23 geht HIGH
- Puls-Train läuft weiter auf letzter disziplinierter Rate → kurze Ausfälle/Leader-Handover sind am GPIO unsichtbar
- Maximum: 10 s; dann Recalibration + Re-Election

**Retry-Storm-Guard:** Nach 5 konsekutiven SYNC_REQ-Fehlern wird Rate auf 5 Hz reduziert (statt 20 Hz).

### 3.9 GPIO

- **GPIO 18:** 1-Hz-Puls, 10 ms HIGH, ausgerichtet an globalem Sekundentakt (externe Verifikation)
  - Nächste Pulse-Zeit: vClock-Inverse des nächsten globalen Sekunden-Boundary → absolutes timerfd
  - Frequenzkorrekturen verschieben Puls sanft (kein Sprung); Leader-Wechsel erzeugt keinen Sprung (vClock nicht resettet)
- **GPIO 23:** LOW wenn |offset| < 100 µs für 10 s kontinuierlich; HIGH bei HOLDOVER/Fehler
- Zugriff via `/dev/gpiomem` mmap (gpio-Gruppe, kein Root, kein CAP_SYS_RAWIO)
- BCM2711 GPSET0/GPCLR0 Register + GPFSEL1/2 für Function-Select, < 1 µs pro Operation

### 3.10 Telemetrie & Steuerung

```
/dev/shm/drs-sync.state   128 Byte, Seqlock, 50-ms-Updates
/dev/shm/drs-sync.cmd     64 Byte, atomic seq, Command-Channel
/dev/shm/drs-sync.errors  64-Slot Ring-Buffer (atomic push, kein Lock)
```

**Telemetrie-Update-Frequenzen (normativ v2.2, F-36):**

| Feld | Wann aktualisiert |
|------|-------------------|
| state, flags, leader_node_id, election_term | jeden 100-ms-Tick |
| offset_ns (`= θ_residual`) | jedes akzeptierte Sync-Sample |
| rate_q32_32 | jede Controller-Invokation |
| latency_corr_ns | nach jeder Kalibrierung |
| last_rtt_ns | jedes akzeptierte SYNC_RESP |
| convergence_unix_ns | beim ersten \|offset\| < 100 µs; gelöscht bei Lock-Verlust |
| holdover_remaining_ms | jeden Tick im HOLDOVER |
| lock_mode | bei User-Override |

Command-Codes: 1=RECALIBRATE, 2=FORCE_HOLDOVER, 3=FORCE_DEMOTE, 4=RESET_FILTERS, 5=FORCE_LEADER, 6=FORCE_FOLLOWER, 7=AUTO_MODE

### 3.11 Real-Time-Konfiguration

```
Kernel:    PREEMPT_RT
CPU:       Core 3 (isolcpus=3 nohz_full=3 rcu_nocbs=3)
Scheduler: SCHED_FIFO Priorität 85, LimitRTPRIO=95
Memory:    mlockall(MCL_CURRENT|MCL_FUTURE), LimitMEMLOCK=infinity
Frequenz:  force_turbo=1 arm_freq=1500 (kein DVFS)
```

**Pflicht:** Der Sync-Loop MUSS periodisch mit `clock_nanosleep()` yielden — verhindert softirq-Starvation, Scheduler-Deadlock und SSH-Lockout.

Verbote im Hot-Path: kein printf/syslog, kein malloc/free nach mlockall, kein Float, kein Mutex.

### 3.12 Modul-Übersicht (14 Module)

| Datei | Funktion |
|-------|----------|
| vclock.{c,h} | Virtuelle Uhr, Seqlock, Q32.32 |
| proto_v2.{c,h} | 64-Byte Wire-Protokoll, Serialisierung |
| crc32.{c,h} | IEEE 802.3 reflected CRC |
| net_io.{c,h} | UDP-Sockets, Multicast, Userspace-TS |
| min_delay.{c,h} | Rolling-Window Min-Delay-Filter (50 µs Toleranz) |
| calibrate.{c,h} | Loopback-Latenz (lat_corr) |
| pi_ctrl.{c,h} | Dual-Loop PI (Step + Slew), F-35 Arithmetik |
| election.{c,h} | Sticky Term-Based Leadership |
| statemachine.{c,h} | 7-State FSM + Lock-Mode |
| gpio_bcm2711.{c,h} | /dev/gpiomem mmap, BCM2711-Register |
| pulse_engine.{c,h} | GPIO 18/23 Timing via timerfd |
| telemetry.{c,h} | /dev/shm/drs-sync.state (F-36 compliant) |
| errors.{c,h} | 64-Slot Ring-Buffer, atomic push |
| cmd.{c,h} | /dev/shm/drs-sync.cmd |

### 3.13 Acceptance-Kriterien & Verifikationsergebnisse

**Acceptance-Kriterien (NF-1, V6 Section 15.1):**
- max|Δt| < 100 µs über 10-min Wired-Run mit 3 Nodes
- p99 < 80 µs
- p50 < 50 µs
- GPIO 23 LOW > 95 % der Messzeit
- Recovery nach Leader-Crash: < 11 s

**Gemessene Ergebnisse (philippknode + drs-node-03, Wired Ethernet):**

| Metrik | Ergebnis |
|--------|----------|
| p50 | 17 µs |
| max | 26 µs |
| Drift | 0,055 ppm über 90 s |
| lat_corr | 3900–4100 ns |
| **NF-1 (<100 µs)** | **PASS** |

**WiFi (ruhiger AP):** Offset ≈ 2,6 µs, RTT mean ≈ 0,87 ms vs. 0,15 ms Wired — Millisekunden-Klasse wie erwartet.

---

## 4. Offene Punkte / Inkonsistenzen

### 4.1 NF-1-Attainability unter ungünstigen Bedingungen *(Caveat bestätigt)*

v2.2 Caveat (Section 14.1) ist normativ: BCM54213PE hat kein IEEE 1588 Hardware-Timestamping. SO_TIMESTAMPING (Software) fügt 5–20 µs Jitter pro Richtung hinzu → realistischer Boden Pi 4B + Standard-Switch: 100–200 µs. Die <100 µs gelten als architektonisches Ziel, sind auf Stock-Hardware ohne PTP-PHY "not expected".

Die gemessenen 17 µs/26 µs sind real, aber unter idealen Bedingungen (dedizierter Core, kein Hintergrund-Traffic). Unter produktionsnaher Last nicht vollständig verifiziert.

### 4.2 WiFi-Performance *(gelöst)*

V6 Section 15.3 liefert Messwerte: RTT mean ≈ 0,87 ms, Offset ≈ 2,6 µs bei ruhigem AP. Millisekunden-Klasse bestätigt. NF-1 gilt explizit nur für Wired.

### 4.3 F-Anforderungs-Nummerierung inkonsistent zwischen Versionen

v2.2 kennt F-1–F-36. V6 verwendet eine andere Nummerierung (z.B. andere Inhalte für F-16, F-22, F-27). Die F-Req-Nummerierung ist zwischen Versionen nicht konsistent. **V6 ist die maßgebliche Version für den Code.**

### 4.4 Leadership-Modell ohne explizite Migrationsdokumentation

Wechsel von Lowest-NodeID-Wins (V3/v2.2) zu Highest-Term-Wins (V6) ist die bedeutendste semantische Änderung, hat aber keinen eigenen Changelog-Eintrag. Mischbetrieb V3- und V6-Knoten im selben Cluster würde Split-Brain erzeugen.

### 4.5 V6-Dokument vollständig gelesen *(gelöst)*

Alle 18 Seiten gelesen. Keine weiteren unbekannten Abschnitte.

### 4.6 step_accum_ns Reset-Bedingungen *(gelöst)*

v2.2 Section 9.2 normativ: *"a filter reset SHALL also zero step_accum_ns (it is only meaningful relative to the current vClock state)"*. Reset bei: Reboot, Seq-Diskontinuität, Leader-Wechsel, Timestamp-Overflow, Phase-Correction >5 ms.

### 4.7 HOLDOVER-Semantik *(gelöst)*

v2.2 Section 7.6 normativ: Rate wird gehalten, **Offset wird eingefroren** (nicht weiter akkumuliert). GPIO-Puls läuft weiter auf letzter disziplinierter Rate. Kurze Ausfälle und Leader-Handover sind am GPIO unsichtbar.
