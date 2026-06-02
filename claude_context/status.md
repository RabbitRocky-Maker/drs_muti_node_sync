# DRS Implementierungsstatus

**Erstellt:** 2026-05-29  
**Basis:** architecture_summary.md (V6 final) + project_context.md  
**Quellen maßgeblich:** architecture_summary.md > project_context.md bei Konflikten

---

## Side Notes (2026-05-29)

> **Web-UI:** Wird vorerst vollständig ausgelassen. Fokus liegt auf `drs_syncd` (C11) und `drs_agent` HTTP-API. Web-UI kommt erst nach der ersten vollständigen Implementierung des Rests.

> **DRS_MIN_DELAY_TOLERANCE_NS = 50 µs:** Bestätigt. Wird so implementiert.

> **Verbesserungsphase (nach erster Implementierung):** Nach der ersten lauffähigen Gesamtimplementierung soll das System die Netzwerk-Delay selbst adaptiv reduzieren — d.h. der Min-Delay-Filter oder ein übergeordneter Mechanismus soll die Toleranz dynamisch anpassen, um die tatsächliche Synchronisationspräzision weiter zu verbessern. Details werden in einer separaten Phase geplant.

---

## 2.1 Implementierungsbereitschaft

**Ergebnis: BEREIT — Implementierung kann beginnen.**

Alle kernkritischen Informationen sind vorhanden:
- Vollständige 7-Zustands-FSM mit allen Transitionen und Triggern
- Exakte Datenstrukturen für alle 3 Shared-Memory-Kanäle (`state`, `cmd`, `errors`)
- Vollständiges 64-Byte Wire-Protokoll (alle Offsets, alle Typen, CRC32)
- Exakte Festkomma-Arithmetik für PI-Regler und Rate-Updates
- Alle Bug-Fixes aus v2.2 (A.1–A.4) dokumentiert und in V6 eingearbeitet
- Alle Konstanten aus `drs_sync_config.h` definiert (mit einer Korrektur, siehe unten)
- GPIO-Zugriffspfad und BCM2711-Register-Layout beschrieben
- epoll-Event-Loop-Struktur vollständig spezifiziert

### Offene Punkte / Klärungsbedarf

| Nr. | Typ | Beschreibung | Auswirkung |
|-----|-----|-------------|-----------|
| O-1 | **Widerspruch** | `DRS_MIN_DELAY_TOLERANCE_NS` = 10.000 ns (10 µs) in `project_context.md` Section 13, aber V6 ändert diesen Wert auf **50.000 ns (50 µs)** laut `architecture_summary.md` Section 2.3 und 3.4. | **Verwende 50 µs** — V6 (`architecture_summary.md`) ist maßgeblich. |
| O-2 | **Unterspecifiziert** | Web-UI Tabs (Topology, Network, Measurement, Sync, System) sind nur namentlich erwähnt, internes Layout nicht beschrieben. | Nicht blockierend. API-Endpunkte sind vollständig spezifiziert → UI kann von diesen abgeleitet werden. |
| O-3 | **Unterspecifiziert** | 3 systemd-Units erwähnt, Inhalt nicht vollständig spezifiziert. | Nicht blockierend. Standardkonfiguration aus RT-Anforderungen ableitbar (CPUAffinity, SCHED_FIFO, mlockall). |
| O-4 | **Nummerierung** | F-Req-Nummerierung inkonsistent zwischen Dokumenten. | V6 (`architecture_summary.md`) ist autoritativ. `project_context.md` F-Req-Tabelle ist V3-Stand. |

---

## 2.2 Funktionsplan

### drs_syncd (C11)

#### Modul: `drs_sync_config.h`
| Attribut | Detail |
|----------|--------|
| Verantwortlichkeit | Alle Konstanten: Timing, Protokoll, RT-Parameter, GPIO-Pins |
| Priorität | **KERN** |
| Abhängigkeiten | keine |

#### Modul: `crc32.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | IEEE 802.3 CRC32 reflected (Polynom 0xEDB88320). Kompilierzeit-Test: "123456789" → 0xCBF43926 |
| Priorität | **KERN** |
| Abhängigkeiten | keine |

#### Modul: `proto_v2.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | Serialize/Deserialize 64-Byte UDP-Pakete (Big-Endian, kein struct-cast). `proto_encode()`, `proto_decode()`, `_Static_assert` für Paketgröße |
| Priorität | **KERN** |
| Abhängigkeiten | `crc32.{c,h}`, `drs_sync_config.h` |

#### Modul: `vclock.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | Virtuelle Uhr Q32.32. `vclock_init()`, `vclock_now()`, `vclock_step_offset()`, `vclock_set_rate()`, `vclock_local_for_global()`. Seqlock für concurrent reads. Kein Float, `__int128` für Zwischenergebnisse |
| Priorität | **KERN** |
| Abhängigkeiten | `drs_sync_config.h` |
| Formel | `T_global = (T_local_raw - base_local) * Rate >> 32 + base_global + Offset - LatencyCorrection` |

#### Modul: `net_io.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | UDP-Sockets (Multicast 239.192.88.100:47200, Unicast). Userspace-Timestamping via `clock_gettime(CLOCK_MONOTONIC_RAW)`. `net_io_init()`, `net_send_announce()`, `net_send_sync_req()`, `net_send_sync_resp()`. SOCK_NONBLOCK, IP_TOS=IPTOS_LOWDELAY |
| Priorität | **KERN** |
| Abhängigkeiten | `proto_v2.{c,h}`, `drs_sync_config.h` |

#### Modul: `min_delay.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | Rolling-Window N=10, Toleranz **50 µs** (V6). `min_delay_init()`, `min_delay_update()`, `min_delay_reset()`. Reset bei: Leader-Wechsel, Seq-Diskontinuität (>1024 rückwärts), Phase-Step >5 ms, HOLDOVER-Entry, LISTEN/CANDIDATE-Entry. Reset setzt auch `step_accum_ns` auf 0 |
| Priorität | **KERN** |
| Abhängigkeiten | `drs_sync_config.h` |

#### Modul: `calibrate.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | Loopback-Latenz-Messung via UDP 127.0.0.1:47201, 50 Samples, Outlier-Rejection >min+20 µs, Ergebnis = min/2. `calibrate_loopback()` → setzt `lat_corr` in vclock |
| Priorität | **KERN** |
| Abhängigkeiten | `net_io.{c,h}`, `vclock.{c,h}`, `drs_sync_config.h` |

#### Modul: `pi_ctrl.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | Dual-Loop PI (Step-Mode + Slew-Mode). `pi_ctrl_init()`, `pi_ctrl_update()`. Festkomma-Arithmetik (F-35): `/65536` statt `>>16`, `×1000000LL`. Step-Hysterese: 3 konsekutive Samples. Rate-Reset + PI-Reset bei Step (F-33, F-34) |
| Priorität | **KERN** |
| Abhängigkeiten | `vclock.{c,h}`, `drs_sync_config.h` |

#### Modul: `errors.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | 64-Slot Ring-Buffer in `/dev/shm/drs-sync.errors`, atomarer O(1) Push ohne Lock. 11 Fehlercodes definiert. Kein `printf`/`syslog` im Hot-Path |
| Priorität | **KERN** |
| Abhängigkeiten | `drs_sync_config.h` |

#### Modul: `cmd.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | Command-Channel via `/dev/shm/drs-sync.cmd` (64 Byte). `cmd_init()`, `cmd_poll()`. 7 Kommandos: RECALIBRATE, FORCE_HOLDOVER, FORCE_DEMOTE, RESET_FILTERS, FORCE_LEADER, FORCE_FOLLOWER, AUTO_MODE |
| Priorität | **WICHTIG** |
| Abhängigkeiten | `drs_sync_config.h` |

#### Modul: `telemetry.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | `/dev/shm/drs-sync.state` (128 Byte, Seqlock, 50-ms-Updates). `telemetry_init()`, `telemetry_update()`. F-36: alle Felder live befüllt. `struct drs_telemetry` mit 15 Feldern wie spezifiziert |
| Priorität | **KERN** |
| Abhängigkeiten | `vclock.{c,h}`, `drs_sync_config.h` |

#### Modul: `gpio_bcm2711.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | `mmap` auf `/dev/gpiomem`. BCM2711 GPSET0/GPCLR0/GPFSEL1/GPFSEL2-Register. `gpio_init()`, `gpio_write()`. gpio-Gruppe, kein Root, kein CAP_SYS_RAWIO. <1 µs pro Operation |
| Priorität | **KERN** |
| Abhängigkeiten | `drs_sync_config.h` |

#### Modul: `pulse_engine.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | GPIO 18 (1-Hz-Puls, 10 ms HIGH) + GPIO 23 (Health-Indikator). timerfd one-shot ABS für Rising-Edge und Falling-Edge. `pulse_engine_init()`, `pulse_engine_arm()`, `pulse_engine_on_rising()`, `pulse_engine_on_falling()`. Inverse-vClock für lokales Scheduling |
| Priorität | **KERN** |
| Abhängigkeiten | `vclock.{c,h}`, `gpio_bcm2711.{c,h}`, `drs_sync_config.h` |

#### Modul: `election.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | Sticky term-basierte Leadership. `election_on_announce()`, `election_promote()`, `election_become_follower()`. Entscheidungslogik: peer_term > own_term → Follower; Tiebreak bei gleichem Term via niedrigere NodeID |
| Priorität | **KERN** |
| Abhängigkeiten | `proto_v2.{c,h}`, `drs_sync_config.h` |

#### Modul: `statemachine.{c,h}`
| Attribut | Detail |
|----------|--------|
| Funktion | 7-Zustands-FSM. `sm_init()`, `sm_tick()`, `sm_on_announce()`, `sm_on_sync_req()`, `sm_on_sync_resp()`, `sm_is_active()`. Lock-Mode (LOCK_AS_LEADER / LOCK_AS_FOLLOWER / AUTO). Active-Before-Follower Guard (F-23). HOLDOVER-Semantik: Rate eingefroren, Offset eingefroren |
| Priorität | **KERN** |
| Abhängigkeiten | alle anderen Module |

#### Modul: `main.c` (drs_syncd.c)
| Attribut | Detail |
|----------|--------|
| Funktion | Main-Loop mit epoll_wait. 6 FDs: `sock_ucast`, `sock_mcast`, `tick_fd` (50 ms), `hb_fd` (100 ms), `pe.timer_fd`, `pe.pulse_low_fd`. RT-Setup: mlockall, CPU-Affinität, SCHED_FIFO/85. ~370 LOC |
| Priorität | **KERN** |
| Abhängigkeiten | alle Module |

#### Modul: `tools/drs_sync_inspect.c`
| Attribut | Detail |
|----------|--------|
| Funktion | CLI-Tool: liest `/dev/shm/drs-sync.state` via read-only mmap, gibt aktuellen Zustand aus |
| Priorität | **WICHTIG** |
| Abhängigkeiten | `telemetry.h` |

#### Unit-Tests (`tests/`)
| Test | Prüft | Priorität |
|------|-------|-----------|
| `test_crc32.c` | "123456789" → 0xCBF43926 | **WICHTIG** |
| `test_proto.c` | Serialize/Deserialize-Roundtrip | **WICHTIG** |
| `test_min_delay.c` | Outlier-Rejection | **WICHTIG** |
| `test_pi_ctrl.c` | Step + Slew + Rate-Clamp | **WICHTIG** |
| `test_vclock.c` | Rate-Skalierung, Step, Inverse | **WICHTIG** |

---

### drs_agent (Python 3)

#### Modul: `agent.py`
| Attribut | Detail |
|----------|--------|
| Funktion | HTTP-Server Port 8080. Liest `/dev/shm/drs-sync.state` (mmap). Schreibt `/dev/shm/drs-sync.cmd`. Alle 9 API-Endpunkte. Peer-Discovery via DHCP-Subnetz-Scan. WiFi-Integration via wpa_supplicant-Control-Socket. ~1700 LOC |
| Priorität | **KERN** |
| Abhängigkeiten | `requirements.txt` |

**API-Endpunkte:**

| Methode | Pfad | Funktion |
|---------|------|---------|
| GET | `/api/sync/state` | Live-Zustand drs_syncd (JSON) |
| GET | `/api/sync/health` | Pre-Flight-Checks (12 Checks) |
| POST | `/api/sync/control` | start/stop/restart/recalibrate/force-leader/etc. |
| GET | `/api/sync/cluster` | Aggregierte Cluster-Sicht |
| GET | `/api/sync/errors?limit=N` | Letzter N Error-Ring-Buffer-Einträge |
| POST | `/api/peers/auto_discover` | DHCP-Subnetz-Scan |
| POST | `/api/transport` | eth0↔wlan0-Switch |
| GET | `/api/wifi/scan` | SSID-Scan |
| POST | `/api/wifi/connect` | {ssid, psk} verbindet wlan0 |

#### Web-UI (`web/`)
| Attribut | Detail |
|----------|--------|
| Funktion | 5-Tab SPA (Vanilla JS): Topology, Network, Measurement, Sync, System |
| Priorität | ~~**WICHTIG**~~ **ZURÜCKGESTELLT** — wird nach der ersten vollständigen Implementierung umgesetzt |
| Abhängigkeiten | `agent.py` (API-Endpunkte) |

#### `requirements.txt`
| Attribut | Detail |
|----------|--------|
| Funktion | Python-Abhängigkeiten (aiohttp oder http.server + requests für Peer-Abfragen) |
| Priorität | **KERN** |
| Abhängigkeiten | keine |

---

## 2.3 Datei- & Modulstruktur

```
code/
├── daemon/
│   ├── include/
│   │   └── drs_sync_config.h          ← Alle Konstanten (V6-korrigiert: MIN_DELAY_TOLERANCE=50µs)
│   ├── src/
│   │   ├── main.c                     ← drs_syncd Haupt-Loop (~370 LOC), epoll
│   │   ├── vclock.c / vclock.h        ← Virtuelle Uhr, Seqlock, Q32.32
│   │   ├── proto_v2.c / proto_v2.h    ← 64-Byte Wire-Protokoll
│   │   ├── crc32.c / crc32.h          ← IEEE 802.3 CRC32
│   │   ├── net_io.c / net_io.h        ← UDP-Sockets, Multicast, Userspace-TS
│   │   ├── min_delay.c / min_delay.h  ← Rolling-Window Filter (N=10, 50µs)
│   │   ├── calibrate.c / calibrate.h  ← Loopback-Latenz (50 Samples)
│   │   ├── pi_ctrl.c / pi_ctrl.h      ← PI-Regler (Step+Slew, Festkomma)
│   │   ├── election.c / election.h    ← Sticky term-basierte Leadership
│   │   ├── statemachine.c / statemachine.h  ← 7-State FSM + Lock-Mode
│   │   ├── gpio_bcm2711.c / gpio_bcm2711.h  ← /dev/gpiomem mmap
│   │   ├── pulse_engine.c / pulse_engine.h  ← GPIO 18/23, timerfd
│   │   ├── telemetry.c / telemetry.h  ← /dev/shm/drs-sync.state (F-36)
│   │   ├── errors.c / errors.h        ← Ring-Buffer, 11 Fehlercodes
│   │   └── cmd.c / cmd.h              ← /dev/shm/drs-sync.cmd
│   ├── tests/
│   │   ├── test_crc32.c
│   │   ├── test_proto.c
│   │   ├── test_min_delay.c
│   │   ├── test_pi_ctrl.c
│   │   └── test_vclock.c
│   ├── tools/
│   │   └── drs_sync_inspect.c         ← CLI Telemetrie-Reader
│   └── Makefile
└── agent/
    ├── agent.py                        ← HTTP API + Web-UI Server (~1700 LOC)
    ├── web/
    │   ├── index.html                  ← 5-Tab SPA
    │   └── static/
    │       ├── app.js                  ← Vanilla JS
    │       └── style.css
    └── requirements.txt
```

---

## 2.4 Implementierungsreihenfolge

### Phase 1 — drs_syncd: Fundament (kein Netzwerk, kein GPIO)

| Schritt | Datei | Grund |
|---------|-------|-------|
| 1 | `include/drs_sync_config.h` | Alle anderen Module includieren dies. Erste Datei. |
| 2 | `src/crc32.{c,h}` | Reine Funktion, keine Abhängigkeiten, direkt testbar |
| 3 | `src/proto_v2.{c,h}` | Wire-Format abhängig von crc32. Muss vor net_io fertig sein |
| 4 | `src/vclock.{c,h}` | Kerndatenstruktur. Alle zeitkritischen Module hängen davon ab |
| 5 | `tests/test_crc32.c` + `tests/test_proto.c` + `tests/test_vclock.c` | Sofort validieren bevor Folgemodule aufbauen |

### Phase 2 — drs_syncd: Netzwerk & Filter

| Schritt | Datei | Grund |
|---------|-------|-------|
| 6 | `src/net_io.{c,h}` | UDP-Sockets, Multicast-Join, Userspace-Timestamping |
| 7 | `src/min_delay.{c,h}` | Filter unabhängig von Netzwerk, direkt testbar |
| 8 | `src/calibrate.{c,h}` | Braucht net_io + vclock; muss vor statemachine fertig sein |
| 9 | `tests/test_min_delay.c` | Outlier-Rejection validieren |

### Phase 3 — drs_syncd: Regelung & Steuerung

| Schritt | Datei | Grund |
|---------|-------|-------|
| 10 | `src/pi_ctrl.{c,h}` | Festkomma-Arithmetik, step_accum_ns Logik (F-33, F-34, F-35) |
| 11 | `src/errors.{c,h}` | Ring-Buffer ohne Abhängigkeiten, wird von statemachine gebraucht |
| 12 | `src/cmd.{c,h}` | Command-Channel, wird von statemachine gebraucht |
| 13 | `src/telemetry.{c,h}` | /dev/shm Seqlock, wird von statemachine gebraucht |
| 14 | `tests/test_pi_ctrl.c` | Step + Slew + Rate-Clamp validieren |

### Phase 4 — drs_syncd: GPIO & Pulse

| Schritt | Datei | Grund |
|---------|-------|-------|
| 15 | `src/gpio_bcm2711.{c,h}` | /dev/gpiomem mmap, BCM2711-Register |
| 16 | `src/pulse_engine.{c,h}` | timerfd one-shot ABS, Inverse-vClock. Braucht gpio + vclock |

### Phase 5 — drs_syncd: FSM & Integration

| Schritt | Datei | Grund |
|---------|-------|-------|
| 17 | `src/election.{c,h}` | Sticky term-Logic, braucht proto_v2 |
| 18 | `src/statemachine.{c,h}` | Integriert alle Module. Komplexestes Modul |
| 19 | `src/main.c` | epoll-Loop, RT-Setup (mlockall, CPU-Affinität, SCHED_FIFO) |
| 20 | `Makefile` | Build-System mit Cross-Compile-Support (`CROSS=aarch64-linux-gnu-`) |

### Phase 6 — Tools

| Schritt | Datei | Grund |
|---------|-------|-------|
| 21 | `tools/drs_sync_inspect.c` | CLI-Diagnose, braucht nur telemetry.h |

### Phase 7 — drs_agent

| Schritt | Datei | Grund |
|---------|-------|-------|
| 22 | `agent/requirements.txt` | Vor Code, damit Dependencies klar sind |
| 23 | `agent/agent.py` | HTTP-API + /dev/shm-Integration. Kann unabhängig von C-Code entwickelt werden |
| ~~24~~ | ~~`agent/web/index.html` + `app.js` + `style.css`~~ | **ZURÜCKGESTELLT** — Web-UI erst nach erster vollständiger Implementierung |

### Phase 8 — Verbesserungsphase (nach Phase 7)

> Geplant, aber noch nicht detailliert. Ziel: adaptive Delay-Reduktion. Der Min-Delay-Filter (und ggf. weitere Mechanismen) soll die Toleranz dynamisch anpassen um die Synchronisationspräzision zu steigern. Wird separat geplant wenn Phase 1–7 abgeschlossen sind.

---

## 2.5 Offene Punkte

| Nr. | Beschreibung | Vor Implementierung klären? | Handlungsempfehlung |
|-----|-------------|---------------------------|--------------------|
| O-1 | `DRS_MIN_DELAY_TOLERANCE_NS` = 10 µs vs. 50 µs | Nein — Entscheidung klar | **Verwende 50 µs** per V6/architecture_summary.md |
| O-2 | Web-UI Tab-Inhalte nicht detailliert spezifiziert | Nein — nicht blockierend | Tabs auf Basis der API-Endpunkte gestalten; Sync-Tab zeigt `offset_ns`, `rate_ppm`, `state`, `leader_id`; System-Tab zeigt Logs/Health |
| O-3 | 3 systemd-Units — genaue Namen und Reihenfolge | Nein | Ableiten: `drs-perf-governor.service` (F-12) + `drs_syncd.service` + `drs_agent.service` |
| O-4 | F-Req-Nummerierung: project_context.md vs. architecture_summary.md | Nein | architecture_summary.md (V6) ist autoritativ |
| O-5 | WiFi-Persistenz (F-30): wo gespeichert? | Optional | Datei in `/etc/drs_sync/transport.conf` oder `/dev/shm` (nicht persistent) — `/etc/` bevorzugt |
