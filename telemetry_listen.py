#!/usr/bin/env python3
"""Listen for DRS telemetry UDP packets and print CSV to stdout.

Usage:
  python3 telemetry_listen.py [--port PORT] [--duration SECS]

Expected packet format:
  timestamp_ns(int64) state(int32) offset_ns(int64) rtt_ns(int64)
  rate_q32(int64) node_id(uint32)  = 40 bytes total, little-endian
"""
import socket
import struct
import argparse
import time
import sys

TELEM_PORT = 4242
# Record format (little-endian from RPi):
# timestamp_ns  int64   offset  0
# state         int32   offset  8
# offset_ns     int64   offset 12
# rtt_ns        int64   offset 20
# rate_q32      int64   offset 28
# node_id       uint32  offset 36
RECORD_SIZE = 40

STATE_NAMES = {0: "GROUND", 1: "CALIBRATION", 2: "LISTEN",
               3: "CANDIDATE", 4: "FOLLOWER", 5: "LEADER", 6: "HOLDOVER"}

def unpack_record(data: bytes):
    if len(data) != RECORD_SIZE:
        return None
    ts_ns,   = struct.unpack_from("<q", data,  0)
    state,   = struct.unpack_from("<i", data,  8)
    off_ns,  = struct.unpack_from("<q", data, 12)
    rtt_ns,  = struct.unpack_from("<q", data, 20)
    rate,    = struct.unpack_from("<q", data, 28)
    node_id, = struct.unpack_from("<I", data, 36)
    return ts_ns, state, off_ns, rtt_ns, rate, node_id

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port",     type=int, default=TELEM_PORT,
                    help="UDP port to listen on (default 4242)")
    ap.add_argument("--duration", type=float, default=0,
                    help="Stop after N seconds (0 = run forever)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    sock.settimeout(1.0)

    print("node_id,timestamp_ns,state,offset_ns,rtt_ns,rate_q32", flush=True)

    deadline = time.monotonic() + args.duration if args.duration > 0 else None
    try:
        while True:
            if deadline and time.monotonic() >= deadline:
                break
            try:
                data, addr = sock.recvfrom(256)
            except socket.timeout:
                continue
            rec = unpack_record(data)
            if rec is None:
                continue
            ts_ns, state, off_ns, rtt_ns, rate, node_id = rec
            state_name = STATE_NAMES.get(state, str(state))
            print(f"{node_id},{ts_ns},{state_name},{off_ns},{rtt_ns},{rate}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

if __name__ == "__main__":
    main()
