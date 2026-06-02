#!/usr/bin/env python3
"""
drs_agent — HTTP API for drs_syncd on port 8080.

Reads /dev/shm/drs-sync.state via mmap (seqlock).
Writes /dev/shm/drs-sync.cmd via mmap.
Reads /dev/shm/drs-sync.errors via mmap.
"""

import asyncio
import ctypes
import fcntl
import ipaddress
import json
import mmap
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

from aiohttp import web

# ── Constants ─────────────────────────────────────────────────────────────

SHM_STATE_PATH  = "/dev/shm/drs-sync.state"
SHM_CMD_PATH    = "/dev/shm/drs-sync.cmd"
SHM_ERRORS_PATH = "/dev/shm/drs-sync.errors"

PORT = 8080

RATE_NOMINAL_Q32 = 0x100000000
RATE_PPM_Q32     = 4295

DRS_FLAG_LEADER     = 1 << 0
DRS_FLAG_HOLDOVER   = 1 << 1
DRS_FLAG_CALIBRATED = 1 << 2
DRS_FLAG_FAULT      = 1 << 3

STATE_NAMES = {
    0: "GROUND",
    1: "CALIBRATION",
    2: "LISTEN",
    3: "CANDIDATE",
    4: "FOLLOWER",
    5: "LEADER",
    6: "HOLDOVER",
}

LOCK_NAMES = {0: "AUTO", 1: "LEADER", 2: "FOLLOWER"}

CMD_NONE           = 0
CMD_RECALIBRATE    = 1
CMD_FORCE_HOLDOVER = 2
CMD_FORCE_DEMOTE   = 3
CMD_RESET_FILTERS  = 4
CMD_FORCE_LEADER   = 5
CMD_FORCE_FOLLOWER = 6
CMD_AUTO_MODE      = 7

ERROR_RING_SLOTS = 64
ERROR_ENTRY_SIZE = 4 + 4 + 8 + 48  # code + count + timestamp_ns + msg

# ── Telemetry struct (128 bytes) ──────────────────────────────────────────
# Layout must match drs_telemetry_t in telemetry.h exactly.
#
# seqlock(4) pid(4) version(4) state(4) flags(4) leader_id(4) term(4)
# _pad(4) offset_ns(8) rate(8) lat_corr(8) last_rtt(8)
# convergence(8) holdover_ms(4) err_last(4) err_count(4) pulse(4) lock(4)
# reserved[20]

# Layout matches drs_telemetry_t in telemetry.h exactly.
# 8 × I (seqlock..election_term + _pad_align) = 32 bytes
# 4 × q (int64 fields) = 32 bytes → 64
# 1 × Q (convergence) = 8 bytes → 72
# 5 × I (holdover_ms..lock_mode) = 20 bytes → 92
# 1 × I (_pad2) = 4 bytes → 96
# 4 × I (counters) = 16 bytes → 112
# 16s (reserved) = 16 bytes → 128
TELEMETRY_FMT = "=IIIIIIIIqqqqQIIIIIIIIII16s"
TELEMETRY_SIZE = struct.calcsize(TELEMETRY_FMT)
assert TELEMETRY_SIZE == 128, f"Telemetry struct size {TELEMETRY_SIZE} != 128"

CMD_FMT  = "=IIQ48s"
CMD_SIZE = struct.calcsize(CMD_FMT)
assert CMD_SIZE == 64, f"Cmd struct size {CMD_SIZE} != 64"

# ── Shared memory helpers ─────────────────────────────────────────────────

class ShmReader:
    """Read-only mmap of a /dev/shm file."""

    def __init__(self, path: str, size: int):
        self._path = path
        self._size = size
        self._mm: Optional[mmap.mmap] = None
        self._fd = -1

    def open(self) -> bool:
        try:
            self._fd = os.open(self._path, os.O_RDONLY)
            self._mm = mmap.mmap(self._fd, self._size, access=mmap.ACCESS_READ)
            return True
        except OSError:
            return False

    def close(self):
        if self._mm:
            self._mm.close()
            self._mm = None
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1

    @property
    def available(self) -> bool:
        return self._mm is not None

    def read_bytes(self) -> bytes:
        if not self._mm:
            return b'\x00' * self._size
        self._mm.seek(0)
        return self._mm.read(self._size)


class ShmWriter:
    """Read-write mmap of a /dev/shm file."""

    def __init__(self, path: str, size: int):
        self._path = path
        self._size = size
        self._mm: Optional[mmap.mmap] = None
        self._fd = -1

    def open(self) -> bool:
        try:
            self._fd = os.open(self._path,
                               os.O_RDWR | os.O_CREAT, 0o666)
            os.ftruncate(self._fd, self._size)
            self._mm = mmap.mmap(self._fd, self._size)
            return True
        except OSError:
            return False

    def close(self):
        if self._mm:
            self._mm.close()
            self._mm = None
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1

    @property
    def available(self) -> bool:
        return self._mm is not None

    def write_cmd(self, code: int):
        if not self._mm:
            return
        self._mm.seek(0)
        raw = self._mm.read(CMD_SIZE)
        seq, old_code, unix_ns, payload = struct.unpack(CMD_FMT, raw)
        new_seq = (seq + 1) & 0xFFFFFFFF
        new_unix = int(time.monotonic_ns())
        packed = struct.pack(CMD_FMT, new_seq, code, new_unix, b'\x00' * 48)
        self._mm.seek(0)
        self._mm.write(packed)
        self._mm.flush()


# ── Telemetry decoder ─────────────────────────────────────────────────────

def decode_telemetry(raw: bytes) -> dict:
    if len(raw) < 128:
        return {}

    # Seqlock read: retry if seq is odd or changes
    for _ in range(10):
        seq1 = struct.unpack_from("=I", raw, 0)[0]
        if seq1 & 1:
            time.sleep(0.0001)
            continue
        fields = struct.unpack(TELEMETRY_FMT, raw)
        seq2 = struct.unpack_from("=I", raw, 0)[0]
        if seq1 == seq2:
            break
    else:
        fields = struct.unpack(TELEMETRY_FMT, raw)

    (seqlock, pid, version, state, flags, leader_id, term, _pad,
     offset_ns, rate_q32, lat_corr, last_rtt,
     convergence, holdover_ms, err_last, err_count, pulse_count, lock_mode,
     _pad2, sync_accepted, sync_rejected, step_count, holdover_count,
     _reserved) = fields

    rate_ppm = (rate_q32 - RATE_NOMINAL_Q32) / RATE_PPM_Q32
    total_sync = sync_accepted + sync_rejected
    accept_pct = round(100.0 * sync_accepted / total_sync, 1) if total_sync > 0 else 100.0

    return {
        "pid":               pid,
        "state":             STATE_NAMES.get(state, str(state)),
        "state_id":          state,
        "flags":             flags,
        "is_leader":         bool(flags & DRS_FLAG_LEADER),
        "is_holdover":       bool(flags & DRS_FLAG_HOLDOVER),
        "is_calibrated":     bool(flags & DRS_FLAG_CALIBRATED),
        "leader_node_id":    leader_id,
        "election_term":     term,
        "offset_ns":         offset_ns,
        "offset_us":         offset_ns / 1000.0,
        "rate_ppm":          round(rate_ppm, 4),
        "latency_corr_ns":   lat_corr,
        "last_rtt_ns":       last_rtt,
        "last_rtt_us":       last_rtt / 1000.0,
        "convergence_ns":    convergence,
        "converged":         convergence != 0,
        "holdover_ms":       holdover_ms,
        "error_last":        err_last,
        "error_count":       err_count,
        "pulse_count":       pulse_count,
        "lock_mode":         LOCK_NAMES.get(lock_mode, str(lock_mode)),
        "sync_accepted":     sync_accepted,
        "sync_rejected":     sync_rejected,
        "sync_accept_pct":   accept_pct,
        "step_count":        step_count,
        "holdover_count":    holdover_count,
    }


# ── Health checks ─────────────────────────────────────────────────────────

def health_checks(tel: dict) -> list:
    checks = []

    def add(name, severity, ok, msg):
        checks.append({"name": name, "severity": severity,
                       "ok": ok, "message": msg})

    add("daemon_running",  "critical",
        Path(SHM_STATE_PATH).exists(),
        "drs_syncd shared memory present")

    state_id = tel.get("state_id", -1)
    add("state_active", "critical",
        state_id >= 2,
        f"State = {tel.get('state', 'UNKNOWN')} (need >= LISTEN)")

    add("calibrated", "warning",
        tel.get("is_calibrated", False),
        "Latency calibration complete")

    add("converged", "warning",
        tel.get("converged", False),
        "|offset| < 100 µs for 10 s")

    abs_off = abs(tel.get("offset_ns", 999999))
    add("offset_ok", "warning",
        abs_off < 100_000,
        f"|offset| = {abs_off/1000:.1f} µs (threshold 100 µs)")

    add("not_holdover", "warning",
        not tel.get("is_holdover", True),
        "Not in HOLDOVER mode")

    rtt = tel.get("last_rtt_ns", 0)
    add("rtt_ok", "info",
        0 < rtt < 5_000_000,
        f"RTT = {rtt/1000:.1f} µs")

    add("gpio_stable", "info",
        tel.get("converged", False),
        "GPIO 23 stability (proxy: convergence)")

    add("lock_auto", "info",
        tel.get("lock_mode") == "AUTO",
        f"Lock mode = {tel.get('lock_mode')}")

    err_cnt = tel.get("error_count", 0)
    add("low_errors", "info",
        err_cnt < 10,
        f"Error ring count = {err_cnt}")

    add("pid_valid", "info",
        tel.get("pid", 0) > 0,
        f"Daemon PID = {tel.get('pid', 0)}")

    add("pulse_active", "info",
        tel.get("pulse_count", 0) > 0,
        f"Pulse count = {tel.get('pulse_count', 0)}")

    return checks


# ── Peer discovery ────────────────────────────────────────────────────────

def get_local_subnet() -> Optional[str]:
    """Return the local subnet CIDR from the primary non-loopback interface."""
    try:
        result = subprocess.run(
            ["ip", "-4", "route", "show", "default"],
            capture_output=True, text=True, timeout=2
        )
        # e.g. "default via 192.168.1.1 dev eth0 ..."
        for line in result.stdout.splitlines():
            if "default" in line:
                dev = None
                parts = line.split()
                if "dev" in parts:
                    dev = parts[parts.index("dev") + 1]
                if dev:
                    r2 = subprocess.run(
                        ["ip", "-4", "addr", "show", dev],
                        capture_output=True, text=True, timeout=2
                    )
                    for l2 in r2.stdout.splitlines():
                        l2 = l2.strip()
                        if l2.startswith("inet "):
                            cidr = l2.split()[1]
                            net = ipaddress.ip_interface(cidr).network
                            return str(net)
    except Exception:
        pass
    return None


async def scan_peers(subnet: str) -> list:
    """Ping-scan a /24 subnet for hosts with port 8080 open."""
    peers = []
    try:
        network = ipaddress.ip_network(subnet, strict=False)
        my_ip = socket.gethostbyname(socket.gethostname())
    except Exception:
        return peers

    async def probe(ip: str):
        try:
            _, writer = await asyncio.wait_for(
                asyncio.open_connection(ip, 8080), timeout=0.3
            )
            writer.close()
            await writer.wait_closed()
            return ip
        except Exception:
            return None

    tasks = [probe(str(h)) for h in list(network.hosts())[:254]
             if str(h) != my_ip]
    results = await asyncio.gather(*tasks)
    peers = [r for r in results if r is not None]
    return peers


# ── Application state ─────────────────────────────────────────────────────

class App:
    def __init__(self):
        self.state_shm  = ShmReader(SHM_STATE_PATH, 128)
        self.cmd_shm    = ShmWriter(SHM_CMD_PATH, 64)
        self.errors_shm = ShmReader(SHM_ERRORS_PATH,
                                    8 + ERROR_RING_SLOTS * ERROR_ENTRY_SIZE)
        self.peers: list = []

    def open_shm(self):
        self.state_shm.open()
        self.cmd_shm.open()
        self.errors_shm.open()

    def get_telemetry(self) -> dict:
        if not self.state_shm.available:
            if not self.state_shm.open():
                return {}
        return decode_telemetry(self.state_shm.read_bytes())

    def send_cmd(self, code: int):
        if not self.cmd_shm.available:
            self.cmd_shm.open()
        self.cmd_shm.write_cmd(code)

    def get_errors(self, limit: int = 20) -> list:
        if not self.errors_shm.available:
            if not self.errors_shm.open():
                return []
        raw = self.errors_shm.read_bytes()
        # head is first 4 bytes
        head = struct.unpack_from("=I", raw, 0)[0]
        entries = []
        for i in range(min(limit, ERROR_RING_SLOTS)):
            idx = (head - limit + i) % ERROR_RING_SLOTS
            off = 8 + idx * ERROR_ENTRY_SIZE
            if off + ERROR_ENTRY_SIZE > len(raw):
                break
            code, count, ts_ns = struct.unpack_from("=IIq", raw, off)
            msg_raw = raw[off + 16: off + 16 + 48]
            msg = msg_raw.rstrip(b'\x00').decode("utf-8", errors="replace")
            if code != 0:
                entries.append({
                    "code": code, "count": count,
                    "timestamp_ns": ts_ns, "msg": msg
                })
        return entries


# ── Route handlers ────────────────────────────────────────────────────────

async def handle_sync_state(request: web.Request) -> web.Response:
    app: App = request.app["drs"]
    tel = app.get_telemetry()
    return web.json_response(tel)


async def handle_sync_health(request: web.Request) -> web.Response:
    app: App = request.app["drs"]
    tel = app.get_telemetry()
    checks = health_checks(tel)
    critical_fail = any(c["severity"] == "critical" and not c["ok"]
                        for c in checks)
    return web.json_response({
        "healthy": not critical_fail,
        "checks":  checks,
        "telemetry": tel,
    })


async def handle_sync_control(request: web.Request) -> web.Response:
    app: App = request.app["drs"]
    try:
        body = await request.json()
    except Exception:
        raise web.HTTPBadRequest(text="Invalid JSON")

    action = body.get("action", "")
    cmd_map = {
        "recalibrate":    CMD_RECALIBRATE,
        "force_holdover": CMD_FORCE_HOLDOVER,
        "force_demote":   CMD_FORCE_DEMOTE,
        "reset_filters":  CMD_RESET_FILTERS,
        "force_leader":   CMD_FORCE_LEADER,
        "force_follower": CMD_FORCE_FOLLOWER,
        "auto_mode":      CMD_AUTO_MODE,
    }
    # start/stop/restart are handled via systemctl
    systemctl_map = {
        "start":   ["systemctl", "start",   "drs_syncd.service"],
        "stop":    ["systemctl", "stop",    "drs_syncd.service"],
        "restart": ["systemctl", "restart", "drs_syncd.service"],
    }

    if action in cmd_map:
        app.send_cmd(cmd_map[action])
        return web.json_response({"ok": True, "action": action})
    elif action in systemctl_map:
        try:
            subprocess.run(systemctl_map[action], timeout=5, check=True)
            return web.json_response({"ok": True, "action": action})
        except subprocess.CalledProcessError as e:
            return web.json_response({"ok": False, "error": str(e)}, status=500)
    else:
        raise web.HTTPBadRequest(text=f"Unknown action: {action}")


async def handle_sync_cluster(request: web.Request) -> web.Response:
    app: App = request.app["drs"]
    local = app.get_telemetry()
    cluster = [{"ip": "localhost", "telemetry": local}]

    # Query known peers
    async with request.app.loop if hasattr(request.app, "loop") \
            else asyncio.get_event_loop()._get_self():
        pass

    import aiohttp
    async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=1)) as session:
        for peer_ip in app.peers:
            try:
                async with session.get(
                    f"http://{peer_ip}:{PORT}/api/sync/state"
                ) as resp:
                    data = await resp.json()
                    cluster.append({"ip": peer_ip, "telemetry": data})
            except Exception:
                pass

    return web.json_response(cluster)


async def handle_sync_errors(request: web.Request) -> web.Response:
    app: App = request.app["drs"]
    limit = int(request.query.get("limit", "20"))
    limit = max(1, min(limit, ERROR_RING_SLOTS))
    return web.json_response(app.get_errors(limit))


async def handle_peers_discover(request: web.Request) -> web.Response:
    app: App = request.app["drs"]
    subnet = get_local_subnet() or "10.0.0.0/24"
    peers = await scan_peers(subnet)
    app.peers = peers
    return web.json_response({"subnet": subnet, "peers": peers})


async def handle_transport(request: web.Request) -> web.Response:
    try:
        body = await request.json()
    except Exception:
        raise web.HTTPBadRequest(text="Invalid JSON")

    iface = body.get("interface", "eth0")
    if iface not in ("eth0", "wlan0"):
        raise web.HTTPBadRequest(text="interface must be eth0 or wlan0")

    # Restart drs_syncd with new interface via systemd override or direct
    try:
        subprocess.run(
            ["systemctl", "restart", "drs_syncd.service"],
            env={**os.environ, "DRS_IFACE": iface},
            timeout=5,
        )
        # Persist choice
        Path("/etc/drs_sync").mkdir(parents=True, exist_ok=True)
        Path("/etc/drs_sync/transport.conf").write_text(f"IFACE={iface}\n")
        return web.json_response({"ok": True, "interface": iface})
    except Exception as e:
        return web.json_response({"ok": False, "error": str(e)}, status=500)


async def handle_wifi_scan(request: web.Request) -> web.Response:
    try:
        result = subprocess.run(
            ["wpa_cli", "-i", "wlan0", "scan_results"],
            capture_output=True, text=True, timeout=5
        )
        lines = result.stdout.strip().splitlines()
        networks = []
        for line in lines[1:]:  # skip header
            parts = line.split("\t")
            if len(parts) >= 5:
                networks.append({
                    "bssid":   parts[0],
                    "freq":    parts[1],
                    "signal":  parts[2],
                    "flags":   parts[3],
                    "ssid":    parts[4],
                })
        return web.json_response({"networks": networks})
    except Exception as e:
        return web.json_response({"error": str(e)}, status=500)


async def handle_wifi_connect(request: web.Request) -> web.Response:
    try:
        body = await request.json()
    except Exception:
        raise web.HTTPBadRequest(text="Invalid JSON")

    ssid = body.get("ssid", "")
    psk  = body.get("psk", "")
    if not ssid:
        raise web.HTTPBadRequest(text="ssid required")

    try:
        # Add network via wpa_cli
        r1 = subprocess.run(["wpa_cli", "-i", "wlan0", "add_network"],
                            capture_output=True, text=True, timeout=3)
        net_id = r1.stdout.strip()
        subprocess.run(["wpa_cli", "-i", "wlan0", "set_network",
                        net_id, "ssid", f'"{ssid}"'], timeout=3)
        if psk:
            subprocess.run(["wpa_cli", "-i", "wlan0", "set_network",
                            net_id, "psk", f'"{psk}"'], timeout=3)
        else:
            subprocess.run(["wpa_cli", "-i", "wlan0", "set_network",
                            net_id, "key_mgmt", "NONE"], timeout=3)
        subprocess.run(["wpa_cli", "-i", "wlan0", "enable_network", net_id], timeout=3)
        subprocess.run(["wpa_cli", "-i", "wlan0", "save_config"], timeout=3)
        return web.json_response({"ok": True, "ssid": ssid})
    except Exception as e:
        return web.json_response({"ok": False, "error": str(e)}, status=500)


# ── App setup ─────────────────────────────────────────────────────────────

def make_app() -> web.Application:
    drs = App()
    drs.open_shm()

    app = web.Application()
    app["drs"] = drs

    app.router.add_get ("/api/sync/state",        handle_sync_state)
    app.router.add_get ("/api/sync/health",        handle_sync_health)
    app.router.add_post("/api/sync/control",       handle_sync_control)
    app.router.add_get ("/api/sync/cluster",       handle_sync_cluster)
    app.router.add_get ("/api/sync/errors",        handle_sync_errors)
    app.router.add_post("/api/peers/auto_discover",handle_peers_discover)
    app.router.add_post("/api/transport",          handle_transport)
    app.router.add_get ("/api/wifi/scan",          handle_wifi_scan)
    app.router.add_post("/api/wifi/connect",       handle_wifi_connect)

    # Static web files
    web_path = Path(__file__).parent / "web"
    if web_path.exists():
        app.router.add_static("/", web_path, name="static")

    return app


if __name__ == "__main__":
    app = make_app()

    # CPU affinity: cores 0–2 (not core 3 which is reserved for drs_syncd)
    try:
        import ctypes
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        # sched_setaffinity(0, sizeof(cpu_set_t), &mask)
        # cpu_set_t is 128 bytes on Linux
        mask = ctypes.create_string_buffer(128)
        for cpu in range(3):
            cpu_word = cpu // 64
            cpu_bit  = cpu % 64
            val = struct.unpack_from("Q", mask, cpu_word * 8)[0]
            val |= (1 << cpu_bit)
            struct.pack_into("Q", mask, cpu_word * 8, val)
        libc.sched_setaffinity(0, ctypes.c_size_t(128), mask)
    except Exception:
        pass

    web.run_app(app, host="0.0.0.0", port=PORT,
                access_log=None)
