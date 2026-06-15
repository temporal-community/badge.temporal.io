#!/usr/bin/env python3
"""Capture a dev badge OLED framebuffer and write a scaled PNG.

The firmware's dev MicroPython API exposes ``badge.dev("fb")``, which prints
the SSD1306 framebuffer as eight page-hex rows. This helper drives the badge's
raw REPL, runs a small capture snippet, parses the dump, and writes a PNG
without external image dependencies.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import time
import zlib
from pathlib import Path

try:
    import serial
except ImportError:
    print(
        "ERROR: pyserial not found. Run with PlatformIO's Python:\n"
        "  ~/.platformio/penv/bin/python scripts/capture_oled_fb.py ...",
        file=sys.stderr,
    )
    sys.exit(1)


SCREEN_W = 128
SCREEN_H = 64
FB_RE = re.compile(r"^P([0-7]) ([0-9a-fA-F]{256})$")


SYNTH_IMPORT = r"""
import badge

src = open('/apps/synth/main.py').read()
cut = src.find('\nrun_app("Synth"')
if cut < 0:
    cut = src.find("\nrun_app('Synth'")
if cut < 0:
    cut = len(src)
ns = {'__name__': 'synth_capture'}
exec(src[:cut], ns)
"""


SYNTH_LIVE_SNIPPET = SYNTH_IMPORT + r"""
ns['recording'] = False
ns['looping'] = False
ns['loop'] = []
ns['loop_len'] = 0
ns['draw_live_screen'](0, '---', 0, 0, 0)
print(badge.dev('fb'))
"""


SYNTH_SOUNDS_SNIPPET = SYNTH_IMPORT + r"""
ns['draw_sound_screen']()
print(badge.dev('fb'))
"""


def png_chunk(kind: bytes, data: bytes) -> bytes:
    body = kind + data
    return (
        struct.pack(">I", len(data))
        + body
        + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    )


def write_png(path: Path, pixels: list[list[int]], scale: int) -> None:
    width = len(pixels[0]) * scale
    height = len(pixels) * scale
    rows = bytearray()
    for row in pixels:
        scaled_row = bytearray()
        for bit in row:
            value = 0 if bit else 255
            scaled_row.extend([value, value, value] * scale)
        packed = bytes([0]) + bytes(scaled_row)
        for _ in range(scale):
            rows.extend(packed)

    data = b"".join(
        (
            b"\x89PNG\r\n\x1a\n",
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)),
            png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)),
            png_chunk(b"IEND", b""),
        )
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def unpack_framebuffer(pages: list[bytes]) -> list[list[int]]:
    pixels = [[0 for _ in range(SCREEN_W)] for _ in range(SCREEN_H)]
    for page, data in enumerate(pages):
        for x, value in enumerate(data):
            for bit in range(8):
                y = page * 8 + bit
                pixels[y][x] = 1 if (value >> bit) & 1 else 0
    return pixels


def read_until(ser: serial.Serial, needles: tuple[bytes, ...], timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        chunk = ser.read(waiting or 1)
        if chunk:
            data.extend(chunk)
            if any(needle in data for needle in needles):
                return bytes(data)
        else:
            time.sleep(0.01)
    return bytes(data)


def exec_raw(port: str, code: str, baud: int) -> str:
    with serial.Serial(port, baud, timeout=0.1, write_timeout=2) as ser:
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(b"\x03\x03")
        ser.flush()
        time.sleep(0.15)
        ser.reset_input_buffer()
        ser.write(b"\x01")
        ser.flush()
        banner = read_until(ser, (b">",), 3.0)
        if b">" not in banner:
            raise RuntimeError("raw REPL did not respond; saw: " + repr(banner[-120:]))

        ser.write(code.replace("\r\n", "\n").encode("utf-8"))
        ser.write(b"\x04")
        ser.flush()

        ack = read_until(ser, (b"OK",), 3.0)
        if b"OK" not in ack:
            raise RuntimeError("raw REPL did not accept code; saw: " + repr(ack[-200:]))

        stdout = read_until(ser, (b"\x04",), 10.0)
        stderr = read_until(ser, (b"\x04",), 2.0)
        ser.write(b"\x02")
        ser.flush()

    out = stdout.replace(b"\x04", b"").decode("utf-8", errors="replace")
    err = stderr.replace(b"\x04", b"").decode("utf-8", errors="replace")
    if err.strip() and err.strip() != ">":
        raise RuntimeError("badge stderr:\n" + err)
    return out


def parse_pages(output: str) -> list[bytes]:
    pages: list[bytes | None] = [None] * 8
    for line in output.splitlines():
        stripped = line.strip()
        match = FB_RE.match(stripped)
        if match:
            page = int(match.group(1))
            pages[page] = bytes.fromhex(match.group(2))
        elif pages[0] is None and len(stripped) == 256:
            try:
                pages[0] = bytes.fromhex(stripped)
            except ValueError:
                pass
    missing = [str(i) for i, page in enumerate(pages) if page is None]
    if missing:
        raise RuntimeError(
            "missing framebuffer pages " + ", ".join(missing) + "\nOutput:\n" + output
        )
    return [page for page in pages if page is not None]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port", required=True, help="Badge serial port, e.g. /dev/cu.usbmodem101"
    )
    parser.add_argument("--out", required=True, type=Path, help="PNG output path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--scale", type=int, default=6)
    parser.add_argument(
        "--screen",
        default="synth-live",
        choices=("synth-live", "synth-sounds"),
        help="Built-in screen capture snippet to run",
    )
    args = parser.parse_args()

    snippet = {
        "synth-live": SYNTH_LIVE_SNIPPET,
        "synth-sounds": SYNTH_SOUNDS_SNIPPET,
    }[args.screen]
    output = exec_raw(args.port, snippet, args.baud)
    pages = parse_pages(output)
    write_png(args.out, unpack_framebuffer(pages), max(1, args.scale))
    print(os.fspath(args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
