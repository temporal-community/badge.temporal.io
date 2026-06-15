#!/usr/bin/env python3
"""Vendor upstream MicroPython source files into firmware/lib/micropython_embed.

The badge is an *embed* port of MicroPython (no Make-driven build, all sources
compiled directly by PlatformIO), but for the user-facing module surface we
want everything a normal `ports/esp32` build provides — `network.WLAN`,
`socket`, `ssl`, `_thread`, `espnow`, `bluetooth`, etc. The embed tarball
that ships upstream doesn't include those; this script grabs them from a
pinned MicroPython tag and drops them next to the existing vendored files.

Usage:
    python3 firmware/scripts/fetch_micropython_sources.py
    python3 firmware/scripts/fetch_micropython_sources.py --tag v1.27.0

Run once per checkout (also invoked from `ignition/setup.sh`). Re-running is
safe — files are only written when the upstream content differs.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import urllib.request
from pathlib import Path

DEFAULT_TAG = "v1.27.0"
RAW = "https://raw.githubusercontent.com/micropython/micropython/{tag}/{path}"

# (upstream path, local destination relative to lib/micropython_embed/src/)
# Keep this list in sync with `library.json` srcFilter.
UPSTREAM_FILES = [
    # ── extmod cross-port modules ──────────────────────────────────────────
    ("extmod/asyncio/__init__.py",            "extmod/asyncio/__init__.py"),
    ("extmod/asyncio/core.py",                "extmod/asyncio/core.py"),
    ("extmod/asyncio/event.py",               "extmod/asyncio/event.py"),
    ("extmod/asyncio/funcs.py",               "extmod/asyncio/funcs.py"),
    ("extmod/asyncio/lock.py",                "extmod/asyncio/lock.py"),
    ("extmod/asyncio/manifest.py",            "extmod/asyncio/manifest.py"),
    ("extmod/asyncio/stream.py",              "extmod/asyncio/stream.py"),
    ("extmod/asyncio/task.py",                "extmod/asyncio/task.py"),
    ("extmod/modasyncio.c",                   "extmod/modasyncio.c"),
    ("extmod/modbluetooth.c",                 "extmod/modbluetooth.c"),
    ("extmod/modbluetooth.h",                 "extmod/modbluetooth.h"),
    ("extmod/modonewire.c",                   "extmod/modonewire.c"),
    ("extmod/modselect.c",                    "extmod/modselect.c"),
    ("extmod/modsocket.c",                    "extmod/modsocket.c"),
    ("extmod/modtls_mbedtls.c",               "extmod/modtls_mbedtls.c"),
    ("extmod/modwebsocket.c",                 "extmod/modwebsocket.c"),
    ("extmod/modwebsocket.h",                 "extmod/modwebsocket.h"),
    ("extmod/modwebrepl.c",                   "extmod/modwebrepl.c"),
    ("extmod/machine_wdt.c",                  "extmod/machine_wdt.c"),
    ("extmod/machine_pulse.c",                "extmod/machine_pulse.c"),
    ("extmod/nimble/hal/hal_uart.c",          "extmod/nimble/hal/hal_uart.c"),
    ("extmod/nimble/hal/hal_uart.h",          "extmod/nimble/hal/hal_uart.h"),
    ("extmod/mpbthci.h",                      "extmod/mpbthci.h"),
    ("extmod/nimble/modbluetooth_nimble.c",   "extmod/nimble/modbluetooth_nimble.c"),
    ("extmod/nimble/modbluetooth_nimble.h",   "extmod/nimble/modbluetooth_nimble.h"),
    ("extmod/nimble/nimble/nimble_npl_os.h",  "extmod/nimble/nimble/nimble_npl_os.h"),

    # ── drivers ──────────────────────────────────────────────────────────────
    ("drivers/dht/dht.c",                     "drivers/dht/dht.c"),
    ("drivers/dht/dht.h",                     "drivers/dht/dht.h"),

    # ── ports/esp32 sources (we have a partial set already vendored) ───────
    ("ports/esp32/mpthreadport.c",            "ports/esp32/mpthreadport.c"),
    ("ports/esp32/mpthreadport.h",            "ports/esp32/mpthreadport.h"),
    ("ports/esp32/mpnimbleport.c",            "ports/esp32/mpnimbleport.c"),
    ("ports/esp32/modesp.c",                  "ports/esp32/modesp.c"),
    ("ports/esp32/modesp32.c",                "ports/esp32/modesp32.c"),
    ("ports/esp32/modesp32.h",                "ports/esp32/modesp32.h"),
    ("ports/esp32/modsocket.c",               "ports/esp32/modsocket.c"),
    ("ports/esp32/modtime.c",                 "ports/esp32/modtime.c"),
    ("ports/esp32/network_lan.c",             "ports/esp32/network_lan.c"),
    ("ports/esp32/network_ppp.c",             "ports/esp32/network_ppp.c"),
    ("ports/esp32/machine_dac.c",             "ports/esp32/machine_dac.c"),
    ("ports/esp32/machine_wdt.c",             "ports/esp32/machine_wdt.c"),
    ("ports/esp32/machine_touchpad.c",        "ports/esp32/machine_touchpad.c"),
    ("ports/esp32/usb.c",                     "ports/esp32/usb.c"),
    ("ports/esp32/usb_serial_jtag.c",         "ports/esp32/usb_serial_jtag.c"),
    ("ports/esp32/mphalport.c",               "ports/esp32/mphalport.c"),
]


def _sha(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()[:12]


def _fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=30) as resp:
        return resp.read()


def _vendor(root: Path, tag: str, paths: list[tuple[str, str]], force: bool) -> int:
    written = 0
    for upstream_path, local_path in paths:
        url = RAW.format(tag=tag, path=upstream_path)
        target = root / local_path
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            content = _fetch(url)
        except Exception as exc:
            print(f"  skip {upstream_path}: {exc}", file=sys.stderr)
            continue

        if target.exists() and not force:
            existing = target.read_bytes()
            if _sha(existing) == _sha(content):
                continue

        target.write_bytes(content)
        written += 1
        print(f"  wrote {local_path} ({len(content)} bytes)")
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tag", default=DEFAULT_TAG,
        help=f"MicroPython git tag/branch to fetch (default {DEFAULT_TAG})",
    )
    parser.add_argument(
        "--force", action="store_true",
        help="Overwrite even when content matches",
    )
    args = parser.parse_args()

    here = Path(__file__).resolve().parent.parent
    root = here / "lib" / "micropython_embed" / "src"
    if not root.is_dir():
        print(f"error: embed source root not found at {root}", file=sys.stderr)
        return 1

    print(f"Fetching MicroPython {args.tag} sources into {root}")
    written = _vendor(root, args.tag, UPSTREAM_FILES, args.force)
    print(f"Done — {written} file(s) updated.")
    print()
    print("To enable the full surface, add these to your env build flags in")
    print("firmware/platformio.ini (any combination):")
    print("  -DREPLAY_ENABLE_FULL_NETWORK=1   # network.WLAN / socket / ssl")
    print("  -DREPLAY_ENABLE_BLUETOOTH=1      # bluetooth (NimBLE) / aioble")
    print("  -DREPLAY_ENABLE_ESPNOW=1         # espnow")
    print("  -DREPLAY_ENABLE_THREAD=1         # _thread (FreeRTOS-backed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
