#!/usr/bin/env python3
"""Log serial output from all connected Temporal Badges.

Each session clears the previous logs and writes fresh files:
  logs/BADGE-A_<usb-serial>.log, logs/BADGE-B_<usb-serial>.log, ...

Usage:
    ./serial_log.py
    ./serial_log.py --baud 115200
    ./serial_log.py --log-dir /tmp/badge-logs

Tip:
    tail -f logs/*.log
"""

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
        "ERROR: pyserial not found. Run with PlatformIO's Python or install pyserial:\n"
        "  ~/.platformio/penv/bin/python serial_log.py",
        file=sys.stderr,
    )
    sys.exit(1)


SCRIPT_DIR = Path(__file__).parent
DEFAULT_LOG_DIR = SCRIPT_DIR / "logs"
DEFAULT_BAUD = 115200


@dataclass(frozen=True)
class PortInfo:
    device: str
    serial_number: str
    label_suffix: str


def safe_name(value: str) -> str:
    cleaned = "".join(ch if ch.isalnum() else "-" for ch in value.strip())
    return cleaned.strip("-") or "unknown"


def find_ports() -> list[PortInfo]:
    ports: list[PortInfo] = []
    for port in list_ports.comports():
        device = port.device or ""
        if not (
            "usbmodem" in device
            or "usbserial" in device
            or "ttyACM" in device
            or "ttyUSB" in device
        ):
            continue
        serial_number = port.serial_number or Path(device).name
        suffix = safe_name(serial_number)
        ports.append(PortInfo(device=device, serial_number=serial_number, label_suffix=suffix))
    return sorted(ports, key=lambda p: p.device)


def write_log_line(log_path: Path, text: str) -> None:
    with log_path.open("a", encoding="utf-8") as f:
        f.write(text + "\n")
        f.flush()


def read_port(port: PortInfo, label: str, log_path: Path, baud: int) -> None:
    first_open = True
    while True:
        try:
            with serial.Serial(port.device, baud, timeout=1) as ser:
                if first_open:
                    log_path.write_text("", encoding="utf-8")
                    first_open = False
                header = f"=== {label} {port.device} serial={port.serial_number} baud={baud} ==="
                print(header, flush=True)
                write_log_line(log_path, header)

                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip()
                    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    entry = f"[{ts}] {line}"
                    print(f"[{label}] {entry}", flush=True)
                    write_log_line(log_path, entry)
        except serial.SerialException as exc:
            msg = f"[{label}] DISCONNECTED: {exc} -- retrying in 3s"
            print(msg, flush=True)
            try:
                write_log_line(log_path, msg)
            except OSError:
                pass
            time.sleep(3)
        except Exception as exc:  # keep logging alive during flaky hardware work
            msg = f"[{label}] ERROR: {exc} -- retrying in 3s"
            print(msg, flush=True)
            try:
                write_log_line(log_path, msg)
            except OSError:
                pass
            time.sleep(3)


def clear_log_files(log_dir: Path) -> None:
    if not log_dir.exists():
        return
    if not log_dir.is_dir():
        raise NotADirectoryError(f"{log_dir} is not a directory")
    for path in log_dir.glob("*.log"):
        if path.is_file() or path.is_symlink():
            path.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description="Log serial output from all connected badges")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default {DEFAULT_BAUD})")
    parser.add_argument("--log-dir", type=Path, default=DEFAULT_LOG_DIR, help="Directory for log files")
    parser.add_argument("--append", action="store_true", help="Append to existing logs instead of clearing log-dir")
    args = parser.parse_args()

    ports = find_ports()
    if not ports:
        print("ERROR: No badge serial ports detected.", file=sys.stderr)
        return 1

    if not args.append:
        clear_log_files(args.log_dir)
    args.log_dir.mkdir(parents=True, exist_ok=True)

    print("=== Temporal Badge Serial Logger ===")
    print(f"Found {len(ports)} badge(s). Logging to {args.log_dir}/")
    print("Ctrl+C to stop.\n")

    threads: list[threading.Thread] = []
    for i, port in enumerate(ports):
        label = f"BADGE-{chr(65 + i)}"
        log_path = args.log_dir / f"{label}_{port.label_suffix}.log"
        t = threading.Thread(
            target=read_port,
            args=(port, label, log_path, args.baud),
            daemon=True,
        )
        t.start()
        threads.append(t)

    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
