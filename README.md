# DRS Multi-Node Clock Sync

Distributed Real-Time Synchronization System für Raspberry Pi 4B.

---

## Verzeichnisstruktur

```
claude_programmingV3/
└── code/
    ├── daemon/          ← Dieser Ordner kommt auf den Pi
    │   ├── include/
    │   ├── src/
    │   ├── tests/
    │   ├── tools/
    │   └── Makefile
    └── agent/           ← Dieser Ordner kommt auf den Pi
        ├── agent.py
        └── requirements.txt
```

> Beide Ordner (`daemon/` und `agent/`) auf den Pi kopieren.

---

## Build

```bash
# Im daemon/ Ordner auf dem Pi
make
```

Erzeugt unter `build/`:
| Binary | Beschreibung |
|--------|-------------|
| `build/drs_syncd` | Haupt-Daemon (Sync-Engine) |
| `build/drs_sync_inspect` | Einmalige Telemetrie-Ausgabe |
| `build/drs_mon` | Live-Monitor |

```bash
# Unit-Tests ausführen (kein Netzwerk, kein GPIO nötig)
make test

# Cross-Compile vom Dev-Rechner (optional)
CROSS=aarch64-linux-gnu- make
```

---

## Daemon starten

```bash
# Voraussetzung: User muss in der gpio-Gruppe sein
sudo usermod -aG gpio $USER   # einmalig, danach neu einloggen

# Starten (Node-ID wird automatisch aus IP-Adresse abgeleitet)
sudo ./build/drs_syncd              # eth0, AUTO-Mode
sudo ./build/drs_syncd eth0         # explizit eth0
sudo ./build/drs_syncd wlan0        # WLAN
sudo ./build/drs_syncd eth0 leader  # Lock als Leader (für Tests)
sudo ./build/drs_syncd eth0 follower# Lock als Follower (für Tests)
```

> `sudo` ist nötig für `SCHED_FIFO` (RT-Scheduler) und `mlockall`.  
> Die Node-ID wird **automatisch** aus dem letzten IP-Oktett abgeleitet (z.B. `192.168.1.11` → Node-ID 11).

---

## Monitoring

```bash
# Einmalige Snapshot-Ausgabe
./build/drs_sync_inspect

# Live-Monitor (aktualisiert automatisch)
./build/drs_mon            # 500 ms Refresh (Standard)
./build/drs_mon 1000       # 1 s Refresh
./build/drs_mon 200        # 200 ms Refresh
```

---

## Agent (optional)

Der Agent ist **nicht notwendig** für die Synchronisation. `drs_syncd` läuft vollständig unabhängig davon.

| Aufgabe | Ohne Agent | Mit Agent |
|---------|-----------|----------|
| Sync & GPIO | `drs_syncd` alleine | — |
| Monitoring | `drs_mon` / `drs_sync_inspect` | `GET /api/sync/state` |
| Commands | direkt via `/dev/shm` | `POST /api/sync/control` |

Den Agent nur starten wenn Fernzugriff per Browser oder HTTP gewünscht ist.

```bash
# Im agent/ Ordner auf dem Pi
pip install -r requirements.txt
python3 agent.py
```

HTTP-API erreichbar unter `http://<pi-ip>:8080/api/sync/state`

---

## GPIO-Pins

| Pin | Funktion |
|-----|---------|
| GPIO 18 (Pin 12) | 1-Hz Sync-Puls — an Logic Analyzer |
| GPIO 23 (Pin 16) | Health: LOW = konvergiert, HIGH = Fehler/HOLDOVER |
| GND (Pin 6) | Gemeinsame Masse — **zwingend verbinden** |
