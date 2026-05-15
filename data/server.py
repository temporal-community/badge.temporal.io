#!/usr/bin/env python3
"""
Replay 2026 Badge — dev data server.

Usage:
    python3 server.py [badge_ip]

    badge_ip  (optional) IP of the badge, used only for display / log filtering.
              If omitted, requests from any IP are served.

Interactive commands (type in the terminal while the server is running):
    Y   set update flag ON  — next /v1/update request returns {"update": true}
                              and the flag auto-clears after that request
    N   clear update flag
    Q   quit

Endpoints:
    GET /v1/update    {"update": bool}   badge checks this on boot
    GET /v1/data      bundle.bin (header + 3 msgpack blobs + CRC32) — preferred
    GET /v1/schedule  msgpack blob       — legacy single-table
    GET /v1/speakers  msgpack blob       — legacy single-table
    GET /v1/floors    msgpack blob       — legacy single-table
"""

import os
import struct
import sys
import threading
import signal

try:
    from flask import Flask, Response, send_file, jsonify, request
except ImportError:
    sys.exit("ERROR: pip install flask")

# ── paths relative to this file ─────────────────────────────────────────────

_HERE = os.path.dirname(os.path.abspath(__file__))
_OUT  = os.path.join(_HERE, "out")


def _mp(name: str) -> str:
    return os.path.join(_OUT, f"{name}.msgpack.txt")


_BUNDLE = os.path.join(_OUT, "bundle.bin")

for _f in (_mp("schedule"), _mp("speakers"), _mp("floors"), _BUNDLE):
    if not os.path.exists(_f):
        sys.exit(f"ERROR: missing output file: {_f}\nRun build-data.py first.")


def _bundle_etag() -> str:
    # ETag = quoted hex of bundle.bin's header CRC32 (offsets 8..12). Re-read
    # on every request so build-data.py rebuilds while the server runs are
    # picked up without restart.
    with open(_BUNDLE, "rb") as f:
        head = f.read(12)
    crc = struct.unpack("<I", head[8:12])[0]
    return f'"{crc:08x}"'

# ── shared state ─────────────────────────────────────────────────────────────

update_flag  = False
flag_lock    = threading.Lock()
badge_ip     = None   # set from CLI arg or prompt; None = accept all

# ── Flask app ─────────────────────────────────────────────────────────────────

app = Flask(__name__)


def _tag() -> str:
    ip = request.remote_addr
    marker = " ◀ badge" if (badge_ip and ip == badge_ip) else ""
    return f"{ip}{marker}"


@app.route("/v1/update")
def route_update():
    global update_flag
    with flag_lock:
        pending      = update_flag
        update_flag  = False          # consumed on first read
    if pending:
        print(f"  [update]   {_tag()}  →  update=true  (flag cleared)")
    else:
        print(f"  [update]   {_tag()}  →  update=false")
    return jsonify({"update": pending})


@app.route("/v1/schedule")
def route_schedule():
    print(f"  [schedule] {_tag()}")
    return send_file(_mp("schedule"), mimetype="application/msgpack")


@app.route("/v1/speakers")
def route_speakers():
    print(f"  [speakers] {_tag()}")
    return send_file(_mp("speakers"), mimetype="application/msgpack")


@app.route("/v1/floors")
def route_floors():
    print(f"  [floors]   {_tag()}")
    return send_file(_mp("floors"), mimetype="application/msgpack")


@app.route("/v1/data")
def route_data():
    etag = _bundle_etag()
    inm  = request.headers.get("If-None-Match", "")
    if inm == etag:
        print(f"  [data]     {_tag()}  304 Not Modified  ({etag})")
        resp = Response(status=304)
        resp.headers["ETag"] = etag
        return resp
    print(f"  [data]     {_tag()}  200 bundle.bin     ({etag})")
    resp = send_file(_BUNDLE, mimetype="application/octet-stream")
    resp.headers["ETag"] = etag
    return resp


# ── interactive input loop ────────────────────────────────────────────────────

def input_loop():
    global update_flag
    print("  Y = set update flag ON   N = clear   Q = quit\n")
    while True:
        try:
            cmd = input("> ").strip().upper()
        except EOFError:
            break
        if cmd == "Y":
            with flag_lock:
                update_flag = True
            print("  update flag: ON  (will clear after next /v1/update request)")
        elif cmd == "N":
            with flag_lock:
                update_flag = False
            print("  update flag: OFF")
        elif cmd == "Q":
            print("  shutting down...")
            os.kill(os.getpid(), signal.SIGINT)
            break
        else:
            print("  Y / N / Q")


# ── main ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) > 1:
        badge_ip = sys.argv[1]
    else:
        raw = input("Badge IP address (press Enter to accept any): ").strip()
        badge_ip = raw if raw else None

    import socket
    hostname   = socket.gethostname()
    server_ip  = socket.gethostbyname(hostname)

    print(f"\nBadge IP : {badge_ip or 'any'}")
    print(f"Server   : http://{server_ip}:5000  (set DATA_SERVER_IP in badge_config.h)")
    print(f"Endpoints: /v1/update  /v1/data  /v1/schedule  /v1/speakers  /v1/floors\n")

    # Run Flask on a daemon thread so stdin stays on the main thread
    flask_thread = threading.Thread(
        target=lambda: app.run(host="0.0.0.0", port=5000,
                               debug=False, use_reloader=False),
        daemon=True,
    )
    flask_thread.start()

    input_loop()
