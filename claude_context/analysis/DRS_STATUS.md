# DRS Project — Session Status
## Vollständiger Kontext für Workspace-Wechsel

**Datum:** 2026-05-05  
**Projekt:** DRS High-Precision User-Space Clock Synchronization (FHTW)  
**Aktueller Stand:** Phase 2 abgeschlossen — Architecture Anchor definiert  
**Nächster Schritt:** Phase 3 — Projektstruktur-Scaffold + erstes kompilierbares C-Skeleton

---

## 1. Was bisher produziert wurde

| Datei | Inhalt | Status |
|-------|--------|--------|
| `claude/DRS_Phase1_Requirements_Analysis.md` | Vollständige Requirements-Analyse: 12 F-Requirements, 11 NF-Requirements, 3 U-Requirements, 10 fehlende Requirements (M-1–M-10), 5 Widersprüche (IC-1–IC-5), 12 offene Fragen (OQ-1–OQ-12), 6 Risiken | ✅ Fertig |
| `claude/DRS_Phase2_Architecture_Anchor.md` | Alle OQ-1–OQ-12 aufgelöst, Architecture Anchor (Clock-Modell, Protokoll, Filter, Election, Error-Taxonomie, Verifikation), Mermaid-Component-Diagram, Mermaid-Sequenzdiagramm | ✅ Fertig |
| `claude/DRS_STATUS.md` | Diese Datei | ✅ Fertig |

---

## 2. Alle getroffenen Architekturentscheidungen (kompakt)

### Kern-Parameter

| Parameter | Wert | Begründung |
|-----------|------|-----------|
| Präzisionsmetrik | P99 < 100 μs, 10-min-Fenster, ≥6000 Samples | Entspricht PTP-Praxis |
| Sync-Intervall | 100 ms (10 Hz) | 100 ppm × 100 ms = 10 μs Drift/Intervall |
| Cluster-Größe | 2–16 Nodes | Lab-Scope |
| Multicast-Gruppe | `239.192.88.100:47200`, TTL=1 | Scoped auf ein LAN-Segment |
| Min. Hardware | **Raspberry Pi 4B** (BCM2711) | RPi 3 USB-NIC hat kein SO_TIMESTAMPING |
| Zeitbasis | Epoch-relativ (kein UTC/GPS) | Relative Synchronisation genügt |
| Failover-Budget | ≤ 3 Sekunden gesamt | 300ms Erkennung + 500ms Election + 2s Konvergenz |

### Technologieentscheidungen

| Bereich | Entscheidung |
|---------|-------------|
| Socket-Timestamping | `SO_TIMESTAMPING` mit `SOF_TIMESTAMPING_RX_SOFTWARE` + `SOF_TIMESTAMPING_TX_SOFTWARE` — explizit erlaubt (kein Kernelmodul) |
| Zeitquelle | `CLOCK_MONOTONIC_RAW` — immun gegen NTP-Slewing |
| NTP | `chronyd` + `systemd-timesyncd` werden beim Deploy **deaktiviert** |
| Scheduler | `SCHED_FIFO` via `pthread_setschedparam()` für den Sync-Thread |
| Virtual Clock Schutz | **Seqlock** (sequence number, gerade=lesbar, ungerade=Schreibvorgang) |
| Jitter-Filter | **NTP Clock Filter** (best-of-8, min-RTT-Selection) + **PI-Controller** |
| Leader Election | **Bully-Algorithmus, niedrigste UUID gewinnt**, UUID persistent in `/etc/drs/node_id` |
| Discovery | IPv4-Multicast `ANNOUNCE`-Heartbeat, TTL-basiertes Peer-Expiry (5s) |
| Error-Logging | Lock-free Ring-Buffer → `events.log` (O_DSYNC) → systemd journal |
| Externe Verifikation | GPIO-17 via `libgpiod` + USB-Logikanalysator (≥1 MHz) |
| Sicherheit | Kein Crypto (unverträglicher Overhead); Magic-Bytes + CRC-32 + Plausibilitäts-Gate |

---

## 3. Datenstrukturen & Protokoll (Kurzfassung)

### VirtualClock (seqlock-geschützt, 64-Byte cacheline-aligned)
```c
typedef struct __attribute__((aligned(64))) {
    _Atomic uint32_t  seq;           // seqlock: ungerade = write in progress
    uint32_t          _pad;
    int64_t           offset_ns;     // leader_time - local_mono_raw
    int64_t           drift_ppb;     // Drift-Korrektur in Parts-per-Billion
    uint64_t          mono_ref_ns;   // CLOCK_MONOTONIC_RAW beim letzten Update
    uint64_t          epoch_ref_ns;  // T_epoch des Leaders in lokaler mono_raw Zeit
    uint8_t           leader_id[16]; // UUID des aktuellen Leaders
    uint32_t          leader_seq;    // letzter SYNC-Sequenz-Nr vom Leader
    uint8_t           state;         // UNSYNC/SYNCING/SYNCED/FREERUN/ELECTING
    uint8_t           _pad2[3];
} VirtualClock;
```

### Paketformat (48 Bytes fix)
```
[Magic 4B "DRS\0"][Version 1B][MsgType 1B][Flags 1B][Reserved 1B]
[Node UUID 16B]
[Sequence 4B]
[Timestamp int64 8B]
[Aux Timestamp int64 8B]
[CRC-32 4B]
```

### Message Types
| Code | Name | Richtung |
|------|------|----------|
| 0x01 | SYNC | Leader → All (multicast), TWO_STEP flag |
| 0x02 | FOLLOW_UP | Leader → All (multicast), enthält T1_hw |
| 0x03 | DELAY_REQ | Follower → Leader (unicast), T3+T2 |
| 0x04 | DELAY_RESP | Leader → Follower (unicast), T4 |
| 0x10 | HEARTBEAT | Leader → All (multicast) |
| 0x20 | ANNOUNCE | Any → All (multicast), Discovery |
| 0x30 | ELECT_NOMINATE | Any → All (multicast) |
| 0x31 | ELECT_ACK | Any → Nominator (unicast) |
| 0x32 | ELECT_VICTORY | Winner → All (multicast) |

### 4-Nachrichten-Exchange (Timestamp-Zuordnung)
```
T1 = Leader Tx (aus MSG_ERRQUEUE nach sendmsg SYNC)
T2 = Follower Rx (aus SCM_TIMESTAMPING cmsg bei recvmsg SYNC)
T3 = Follower Tx (aus MSG_ERRQUEUE nach sendmsg DELAY_REQ)
T4 = Leader Rx   (aus MSG_ERRQUEUE nach Empfang DELAY_REQ)

offset = ((T2 - T1) - (T4 - T3)) / 2
RTT    = (T2 - T1) + (T4 - T3)
```

### PI-Controller Parameter
| Parameter | Wert |
|-----------|------|
| Kp | 0.1 |
| Ki | 0.01 |
| MAX_SLEW_PPB | 500 000 (500 ppm) |
| OUTLIER_GATE | 10 000 000 ns (10 ms) |
| STEP_THRESHOLD_NS | 1 000 000 ns (1 ms) |

### Leader-Election Timeouts
| Timer | Wert |
|-------|------|
| HEARTBEAT_INTERVAL | 100 ms |
| HEARTBEAT_TIMEOUT | 300 ms (3× missed) |
| ELECTION_TIMEOUT | 500 ms |
| MAX_ELECTION_ROUNDS | 3 |

---

## 4. Error-Taxonomie (Kurzübersicht)

| Error ID | Aktion | Indikation |
|----------|--------|------------|
| ERR_NTP_CONFLICT | Exit(2) | /var/log/drs/error.log + journal |
| ERR_HARDWARE_UNSUPPORTED | Exit(3) | /var/log/drs/error.log + journal |
| ERR_PEER_TABLE_FULL | Peer ablehnen | Ring-Buffer + stats-Counter |
| ERR_INVALID_PACKET | Paket droppen | Ring-Buffer + Counter |
| ERR_TIMESTAMP_FAILURE | Fallback + FREERUN | /var/log/drs/error.log |
| ERR_CLOCK_RUNAWAY | Sample verwerfen | Ring-Buffer; bei 10× → ERR_SYNC_DIVERGED |
| ERR_LEADER_TIMEOUT | Election starten | Ring-Buffer |
| ERR_PEER_LOST | Peer aus Tabelle | Ring-Buffer |
| ERR_SYNC_DIVERGED | FREERUN + re-elect | /var/log/drs/error.log |
| ERR_ELECTION_FAILED | FREERUN + retry 10s | /var/log/drs/error.log + journal |
| WARN_CLOCK_AHEAD | Slow slew-back −200ppm | Ring-Buffer |
| WARN_HIGH_JITTER | OUTLIER_GATE auf 50ms | Ring-Buffer + stats |

---

## 5. Externe Verifikation

- **Hardware:** GPIO-17 jedes Nodes → USB-Logikanalysator (Saleae Logic 8 o.ä., ≥1 MHz)
- **GPIO-Zugriff:** `libgpiod` (kein Kernelmodul, user-space)
- **Testprogramm:** `drs_trigger` — berechnet `T_trigger = vclock_now + 10s`, broadcastet an alle Nodes, jeder feuert GPIO-Puls bei T_trigger
- **Pass-Kriterium:** `max(edge_timestamps) - min(edge_timestamps) < 100 μs`

---

## 6. Deployment-Anforderungen

Auf jedem Node vor Start:
```bash
sudo systemctl disable --now chronyd systemd-timesyncd
sudo iw dev wlan0 set power_save off   # falls Wi-Fi genutzt
sudo setcap cap_net_admin,cap_sys_nice+eip /usr/local/bin/drs_syncd
```

Verzeichnisstruktur (noch nicht erstellt):
```
drs/
├── src/
│   ├── main.c
│   ├── vclock.{c,h}
│   ├── protocol.{c,h}
│   ├── clock_filter.{c,h}
│   ├── election.{c,h}
│   ├── discovery.{c,h}
│   ├── error.{c,h}
│   └── trigger.c          (drs_trigger binary)
├── include/
│   └── drs.h              (public API für consumer)
├── tools/
│   └── drs_mon.c          (monitoring tool)
├── deploy/
│   ├── deploy.sh
│   └── drs.service        (systemd unit)
├── test/
│   └── analysis.py        (GPIO-edge Auswertung)
└── CMakeLists.txt
```

---

## 7. Nächster Schritt (Phase 3)

**Prompt für neuen Workspace:**

> *Lies `claude/DRS_STATUS.md` und `claude/DRS_Phase2_Architecture_Anchor.md` für den vollen Kontext.*
>
> *Erstelle jetzt Phase 3:*
> *1. Den vollständigen Projektverzeichnis-Scaffold (CMakeLists.txt, alle Verzeichnisse, alle .h/.c Dateien als leere Skeleton-Dateien mit Includes und Stub-Funktionen)*
> *2. Die erste kompilierbaren C-Implementierung von:*
>    - *`vclock.h` / `vclock.c` — VirtualClock Datenstruktur + seqlock read/write + `vclock_read_ns()`*
>    - *`protocol.h` — Paketformat (struct + enum für Message Types + Flags)*
>    - *`clock_filter.h` / `clock_filter.c` — NTP-Clock-Filter (ring buffer 8 Samples, min-RTT-Selektion)*
>    - *`main.c` — Skeleton mit Initialisierung, Thread-Setup (SCHED_FIFO), Socket-Erstellung mit SO_TIMESTAMPING*
> *3. Kompilierungscheck: muss mit `gcc -Wall -Wextra -std=c17 -lm` fehlerfrei compilieren*
> *Target-Plattform: Raspberry Pi 4B, Linux, user-space only, keine Kernelmodule.*
