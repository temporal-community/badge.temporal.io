#!/usr/bin/env python3
"""Drive IR Block Battle over badge USB serial for smoke tests."""

from __future__ import annotations

import argparse
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print(
        "ERROR: pyserial not found. Run with PlatformIO's Python:\n"
        "  ~/.platformio/penv/bin/python firmware/scripts/ir_block_battle_serial.py",
        file=sys.stderr,
    )
    sys.exit(1)


BAUD = 115200
APP_COMMAND = "exec(open('/apps/ir_block_battle/main.py').read())\r\n"

TOKENS = {
    "start": "\n",
    "left": "a",
    "right": "d",
    "soft": "s",
    "hard": "w",
    "drop": "w",
    "cw": "k",
    "ccw": "j",
    "hold": "h",
    "setup-single": "g",
    "setup-double": "f",
    "setup-triple": "r",
    "setup-tetris": "t",
    "send-double": "2",
    "send-triple": "3",
    "send-tetris": "4",
    "help": "?",
}


@dataclass(frozen=True)
class PortInfo:
    label: str
    device: str
    serial_number: str


def find_ports() -> list[PortInfo]:
    ports = []
    for port in list_ports.comports():
        device = port.device or ""
        if not (
            "usbmodem" in device
            or "usbserial" in device
            or "ttyACM" in device
            or "ttyUSB" in device
        ):
            continue
        ports.append((device, port.serial_number or Path(device).name))
    ports.sort()
    return [
        PortInfo(label=f"{chr(65 + i)}", device=device, serial_number=serial_number)
        for i, (device, serial_number) in enumerate(ports)
    ]


def parse_sequence(value: str) -> str:
    if not value:
        return ""
    chars = []
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        if len(token) == 1 and token not in TOKENS:
            chars.append(token)
            continue
        if token not in TOKENS:
            known = ", ".join(sorted(TOKENS))
            raise ValueError(f"unknown token {token!r}; known tokens: {known}")
        chars.append(TOKENS[token])
    return "".join(chars)


def reader(port: PortInfo, ser: serial.Serial, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            raw = ser.readline()
        except serial.SerialException as exc:
            print(f"[{port.label}] read failed: {exc}", flush=True)
            return
        if not raw:
            continue
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        line = raw.decode("utf-8", errors="replace").rstrip()
        print(f"[{port.label}] [{ts}] {line}", flush=True)


def write_chars(port: PortInfo, ser: serial.Serial, chars: str, delay: float) -> None:
    for ch in chars:
        label = "\\n" if ch == "\n" else ch
        print(f"[{port.label}] >>> {label}", flush=True)
        ser.write(ch.encode("utf-8"))
        ser.flush()
        time.sleep(delay)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument("--launch", action="store_true", help="Run IR Block Battle from the MicroPython REPL first")
    parser.add_argument("--launch-wait", type=float, default=3.0)
    parser.add_argument("--delay", type=float, default=0.35, help="Delay between command characters")
    parser.add_argument("--duration", type=float, default=12.0, help="Seconds to keep logging after commands")
    parser.add_argument("--a", default="", help="Comma-separated command tokens for badge A")
    parser.add_argument("--b", default="", help="Comma-separated command tokens for badge B")
    args = parser.parse_args()

    try:
        seq_a = parse_sequence(args.a)
        seq_b = parse_sequence(args.b)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    ports = find_ports()
    if not ports:
        print("ERROR: No badge serial ports detected.", file=sys.stderr)
        return 1
    if len(ports) > 2:
        ports = ports[:2]

    print("=== IR Block Battle serial driver ===")
    for port in ports:
        print(f"Badge {port.label}: {port.device} serial={port.serial_number}")

    stop = threading.Event()
    opened: list[tuple[PortInfo, serial.Serial]] = []
    threads: list[threading.Thread] = []
    try:
        for port in ports:
            ser = serial.Serial(port.device, args.baud, timeout=0.05, write_timeout=1)
            opened.append((port, ser))
            thread = threading.Thread(target=reader, args=(port, ser, stop), daemon=True)
            thread.start()
            threads.append(thread)

        time.sleep(0.25)
        if args.launch:
            for _port, ser in opened:
                ser.write(b"\x03\r\n")
                ser.flush()
            time.sleep(0.2)
            for port, ser in opened:
                print(f"[{port.label}] >>> launch app", flush=True)
                ser.write(APP_COMMAND.encode("utf-8"))
                ser.flush()
            time.sleep(args.launch_wait)

        sequences = {"A": seq_a, "B": seq_b}
        for port, ser in opened:
            write_chars(port, ser, sequences.get(port.label, ""), args.delay)

        time.sleep(args.duration)
    finally:
        stop.set()
        for _port, ser in opened:
            ser.close()
        for thread in threads:
            thread.join(timeout=0.5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
