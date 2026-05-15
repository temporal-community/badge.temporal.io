"""Temporal activities for badge build and flash operations.

All subprocess output is captured and returned as part of the activity result —
Temporal history IS the log. Browse past runs at http://localhost:8233.

Only the last LOG_TAIL_LINES of output are stored to stay within Temporal's
2 MB payload limit per event.
"""
import asyncio
import json
import os
import re
import shutil
import textwrap
import time
from datetime import datetime, timezone
from pathlib import Path

from temporalio import activity

# Default firmware directory: ../firmware relative to this file's repo root.
# Override by passing firmware_dir explicitly to each activity.
_DEFAULT_FIRMWARE_DIR = str(
    Path(__file__).resolve().parent.parent.parent / "firmware"
)

LOG_TAIL_LINES = 150
DEFAULT_UPLOAD_BAUD = "921600"
ONLY_ENV = "replay2026"
PORT_BUSY_RETRY_ATTEMPTS = 6
PORT_BUSY_RETRY_DELAY_S = 1.5
USB_BOOTLOADER_RECOVERY_ATTEMPTS = 3
USB_BOOTLOADER_RECOVERY_CONCURRENCY = 8
USB_BOOTLOADER_RECOVERY_BOOT_HOLD_S = 0.1
USB_BOOTLOADER_RECOVERY_RELEASE_HOLD_S = 0.05
USB_BOOTLOADER_RECOVERY_SETTLE_S = 1.5
USB_BOOTLOADER_RECOVERY_DETECT_TRIES = 8
USB_BOOTLOADER_RECOVERY_DETECT_DELAY_S = 0.75
_PREPARED_FLASH_ARTIFACTS: set[str] = set()
_PREPARE_LOCKS: dict[str, asyncio.Lock] = {}
_ESPTOOL_PERCENT_RE = re.compile(r"(?:\(|\s)(\d+(?:\.\d+)?)\s*%\)?")
_HEARTBEAT_EVERY_S = 2.0


def _fw(firmware_dir: str) -> Path:
    return Path(firmware_dir) if firmware_dir else Path(_DEFAULT_FIRMWARE_DIR)


def _env_error(env: str) -> str:
    return f"Ignition only builds/flashes {ONLY_ENV}; got {env!r}."


def _env_supported(env: str) -> bool:
    return env == ONLY_ENV


def _resolve_pio() -> str:
    candidates = []
    penv = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if penv.exists():
        candidates.append(str(penv))
    from_path = shutil.which("pio")
    if from_path:
        candidates.append(from_path)
    for c in candidates:
        try:
            import subprocess
            subprocess.run([c, "--version"], check=True, capture_output=True)
            return c
        except Exception:
            continue
    raise RuntimeError("PlatformIO not found. Run ./setup.sh to install it.")


def _resolve_python() -> Path:
    penv_py = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if penv_py.exists():
        return penv_py
    return Path(shutil.which("python3") or "python3")


def _upload_baud() -> str:
    return os.environ.get("IGNITION_UPLOAD_BAUD", DEFAULT_UPLOAD_BAUD)


def _tail(text: str, n: int) -> str:
    lines = text.splitlines()
    return "\n".join(lines[-n:]) if len(lines) > n else text


def _is_port_busy_output(output: str) -> bool:
    return any(
        marker in output
        for marker in (
            "Could not exclusively lock port",
            "port is busy or doesn't exist",
            "Resource temporarily unavailable",
        )
    )


def _heartbeat_payload(label: str, line: str = "") -> dict:
    payload = {"step": label}
    line = line.strip()
    if line:
        payload["line"] = line[-240:]
        percent = _ESPTOOL_PERCENT_RE.search(line)
        if percent:
            value = float(percent.group(1))
            payload["percent"] = int(value) if value.is_integer() else value
    return payload


def _heartbeat_line_is_interesting(line: str) -> bool:
    return bool(
        _ESPTOOL_PERCENT_RE.search(line)
        or any(
            marker in line
            for marker in (
                "Connecting",
                "Detecting chip type",
                "Chip is",
                "Writing at",
                "Hash of data verified",
                "Hard resetting",
                "Leaving",
                "[WiFi]",
                "[verify]",
                "[BLE]",
                "[ble]",
                "[badgebeacon]",
                "[map/ble]",
                "Connected",
                "timeout waiting",
            )
        )
    )


async def _communicate_with_heartbeats(proc: asyncio.subprocess.Process, label: str) -> str:
    """Read subprocess output while surfacing useful progress in heartbeats.

    Temporal UI shows the latest heartbeat details for pending activities, so
    progress like esptool's "Writing at ... (42%)" belongs here rather than in
    the workflow query state.
    """
    chunks: list[str] = []
    pending = ""
    last_heartbeat = 0.0

    activity.heartbeat(_heartbeat_payload(label))

    assert proc.stdout is not None
    while True:
        try:
            raw = await asyncio.wait_for(proc.stdout.read(1024), timeout=1)
        except asyncio.TimeoutError:
            now = time.monotonic()
            if now - last_heartbeat >= 8:
                activity.heartbeat(_heartbeat_payload(label, pending))
                last_heartbeat = now
            continue

        if not raw:
            break

        text = raw.decode(errors="replace")
        chunks.append(text)
        pending += text
        parts = re.split(r"[\r\n]+", pending)
        pending = parts.pop() if parts else ""

        for part in parts:
            line = part.strip()
            if not line or not _heartbeat_line_is_interesting(line):
                continue
            now = time.monotonic()
            if now - last_heartbeat >= _HEARTBEAT_EVERY_S or _ESPTOOL_PERCENT_RE.search(line):
                activity.heartbeat(_heartbeat_payload(label, line))
                last_heartbeat = now

    if pending.strip():
        activity.heartbeat(_heartbeat_payload(label, pending))

    await proc.wait()
    return "".join(chunks)


async def _run_esptool(
    args: list[str],
    *,
    port: str,
    fw_dir: Path,
    sections: list[str],
    tail_lines: int,
    heartbeat_label: str,
) -> bool:
    for attempt in range(1, PORT_BUSY_RETRY_ATTEMPTS + 1):
        proc = await asyncio.create_subprocess_exec(
            *args,
            cwd=str(fw_dir),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        output = _tail(await _communicate_with_heartbeats(proc, heartbeat_label), tail_lines)
        sections.append(output)

        if proc.returncode == 0:
            return True
        if not _is_port_busy_output(output):
            return False
        if attempt == PORT_BUSY_RETRY_ATTEMPTS:
            return False

        delay = PORT_BUSY_RETRY_DELAY_S * attempt
        sections.append(
            f"direct esptool: {port} is busy; retrying in {delay:.1f}s "
            f"({attempt}/{PORT_BUSY_RETRY_ATTEMPTS})"
        )
        activity.heartbeat(_heartbeat_payload(
            heartbeat_label,
            f"port busy retry {attempt}/{PORT_BUSY_RETRY_ATTEMPTS}",
        ))
        await asyncio.sleep(delay)

    return False


def _build_dir(fw_dir: Path, env: str) -> Path:
    return fw_dir / ".pio" / "build" / env


def _find_boot_app0() -> Path | None:
    candidates = (
        Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin",
        Path.home() / ".platformio" / "packages" / "framework-espidf" / "components" / "partition_table" / "boot_app0.bin",
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


_FILESYSTEM_IMAGE_NAMES = ("fatfs.bin", "littlefs.bin", "spiffs.bin")


def _find_filesystem_image(build_dir: Path) -> Path | None:
    return next(
        (build_dir / n for n in _FILESYSTEM_IMAGE_NAMES
         if (build_dir / n).exists()),
        None,
    )


def _remove_filesystem_images(build_dir: Path) -> None:
    for name in _FILESYSTEM_IMAGE_NAMES:
        try:
            (build_dir / name).unlink()
        except FileNotFoundError:
            pass


def _partition_info(build_dir: Path, *, name: str = "", subtypes: set[int] | None = None) -> tuple[int, int] | None:
    import struct

    partitions_bin = build_dir / "partitions.bin"
    if not partitions_bin.exists():
        return None

    data = partitions_bin.read_bytes()
    for i in range(0, len(data) - 31, 32):
        entry = data[i:i + 32]
        if struct.unpack_from("<H", entry, 0)[0] != 0x50AA:
            continue
        entry_name = entry[12:28].split(b"\x00", 1)[0].decode(errors="ignore")
        entry_type = entry[2]
        entry_subtype = entry[3]
        offset = struct.unpack_from("<I", entry, 4)[0]
        size = struct.unpack_from("<I", entry, 8)[0]
        if name and entry_name == name:
            return offset, size
        if subtypes is not None and entry_type == 0x01 and entry_subtype in subtypes:
            return offset, size
    return None


def _partition_offset(build_dir: Path, *, name: str = "", subtypes: set[int] | None = None) -> int | None:
    info = _partition_info(build_dir, name=name, subtypes=subtypes)
    return info[0] if info else None


def _doom_wad_present(fw_dir: Path) -> bool:
    data_dir = fw_dir / "data"
    return any((data_dir / name).is_file()
               for name in ("doom1.wad", "DOOM1.WAD", "Doom1.wad", "Doom1.WAD"))


_VID_PID_RE = re.compile(r"VID:PID=([0-9A-F]{4}:[0-9A-F]{4})", re.IGNORECASE)
_SER_RE = re.compile(r"\bSER=([^\s]+)", re.IGNORECASE)
_LOC_RE = re.compile(r"\bLOCATION=([^\s]+)", re.IGNORECASE)
_QA_USBMODEM_RE = re.compile(r"^usbmodem[0-9A-Fa-f]{8,}$")
_MACOS_USB_SERIAL_GLOBS = (
    "cu.usbmodem*",
    "tty.usbmodem*",
    "cu.usbserial*",
    "tty.usbserial*",
)
_MACOS_USB_SERIAL_PREFIXES = ("usbmodem", "usbserial")
_BADGE_PORT_HINTS = ("usbmodem", "usbserial", "ttyacm")


def _safe_activity_heartbeat(payload: object) -> None:
    try:
        activity.heartbeat(payload)
    except RuntimeError:
        pass


def _serial_device_name(port: str) -> str:
    name = Path(port).name
    for prefix in ("cu.", "tty."):
        if name.startswith(prefix):
            return name[len(prefix):]
    return name


def _macos_usb_serial_family(port: str) -> str:
    name = _serial_device_name(port).lower()
    if name.startswith(_MACOS_USB_SERIAL_PREFIXES):
        return name
    return ""


def _is_macos_usb_serial_path(port: str) -> bool:
    return bool(_macos_usb_serial_family(port))


def _preferred_macos_serial_port(port: str) -> str:
    path = Path(port)
    name = path.name
    if name.startswith("tty."):
        cu_path = path.with_name("cu." + name[len("tty."):])
        if cu_path.exists():
            return str(cu_path)
    return port


def _needs_usb_bootloader_recovery(port: str) -> bool:
    return bool(_QA_USBMODEM_RE.match(_serial_device_name(port)))


def _looks_like_badge_device(dev: dict) -> bool:
    hwid = (dev.get("hwid") or "").upper()
    desc = (dev.get("description") or "").lower()
    port = (dev.get("port") or "").lower()
    return (
        "303A:1001" in hwid
        or "usb jtag/serial debug unit" in desc
        or _is_macos_usb_serial_path(port)
        or any(k in port for k in _BADGE_PORT_HINTS)
    )


# ── Build ─────────────────────────────────────────────────────────────────────

@activity.defn
async def build_firmware(env: str, firmware_dir: str = "") -> dict:
    """Build firmware by delegating to firmware/build.sh -n.

    Using build.sh as the single source of truth means ignition and direct
    builds can never drift — any flags or steps added to build.sh are
    automatically picked up here.

    Returns: success, env, output (last LOG_TAIL_LINES lines), error, duration_s.
    """
    if not _env_supported(env):
        return {"success": False, "env": env, "output": "",
                "error": _env_error(env), "duration_s": 0.0}

    fw = _fw(firmware_dir)
    build_script = fw / "build.sh"
    if not build_script.exists():
        return {"success": False, "env": env, "output": "",
                "error": f"build.sh not found in {fw}", "duration_s": 0.0}

    start = time.monotonic()
    activity.heartbeat("compiling")

    proc = await asyncio.create_subprocess_exec(
        "bash", str(build_script), env, "-n",
        cwd=str(fw),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )

    stdout = await _communicate_with_heartbeats(proc, "compiling")

    duration = time.monotonic() - start
    output = _tail(stdout, LOG_TAIL_LINES)

    if proc.returncode != 0:
        return {"success": False, "env": env, "output": output,
                "error": "Build failed — see output for details.", "duration_s": round(duration, 1)}

    return {"success": True, "env": env, "output": output, "error": "", "duration_s": round(duration, 1)}


@activity.defn
async def prepare_flash_artifacts(
    env: str,
    firmware_dir: str = "",
    rebuild_filesystem: bool = False,
) -> dict:
    """Prepare build artifacts used by the fast esptool flash path.

    Firmware is expected to have been built already. Filesystem images are
    generated once per worker process so parallel badge flashes do not each run
    PlatformIO's buildfs/uploadfs machinery.
    """
    if not _env_supported(env):
        return {"success": False, "env": env, "output": "",
                "error": _env_error(env), "duration_s": 0.0}

    pio = _resolve_pio()
    fw_dir = _fw(firmware_dir)
    build_dir = _build_dir(fw_dir, env)
    key = f"{fw_dir.resolve()}::{env}"
    start = time.monotonic()
    activity.heartbeat("preparing flash artifacts")

    required = [build_dir / "bootloader.bin", build_dir / "partitions.bin", build_dir / "firmware.bin"]
    missing = [str(p) for p in required if not p.exists()]
    if missing:
        return {
            "success": False,
            "env": env,
            "output": "",
            "error": "Missing firmware artifacts. Run with build enabled first: " + ", ".join(missing),
            "duration_s": 0.0,
        }

    if key in _PREPARED_FLASH_ARTIFACTS and _find_filesystem_image(build_dir):
        return {"success": True, "env": env, "output": "Flash artifacts already prepared.",
                "error": "", "duration_s": 0.0}

    lock = _PREPARE_LOCKS.setdefault(key, asyncio.Lock())
    async with lock:
        if key in _PREPARED_FLASH_ARTIFACTS and _find_filesystem_image(build_dir):
            return {"success": True, "env": env, "output": "Flash artifacts already prepared.",
                    "error": "", "duration_s": round(time.monotonic() - start, 1)}

        # If a Doom WAD is not available locally but a filesystem image
        # already exists, keep the previous image usable for --no-build
        # production runs instead of forcing an unnecessary rebuild.
        if (
            not rebuild_filesystem
            and env == ONLY_ENV
            and not _doom_wad_present(fw_dir)
            and _find_filesystem_image(build_dir)
        ):
            _PREPARED_FLASH_ARTIFACTS.add(key)
            return {
                "success": True,
                "env": env,
                "output": "Using existing filesystem image; data/doom1.wad is not present to rebuild it.",
                "error": "",
                "duration_s": round(time.monotonic() - start, 1),
            }

        if rebuild_filesystem:
            _remove_filesystem_images(build_dir)

        proc = await asyncio.create_subprocess_exec(
            pio, "run", "-e", env, "-t", "buildfs",
            cwd=str(fw_dir),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        output = _tail(
            await _communicate_with_heartbeats(proc, "building filesystem image"),
            LOG_TAIL_LINES,
        )
        duration = round(time.monotonic() - start, 1)

        if proc.returncode != 0:
            return {"success": False, "env": env, "output": output,
                    "error": "Filesystem image build failed.", "duration_s": duration}

        if not _find_filesystem_image(build_dir):
            return {"success": False, "env": env, "output": output,
                    "error": "Filesystem image build completed but no fs image was found.",
                    "duration_s": duration}

        _PREPARED_FLASH_ARTIFACTS.add(key)
        return {"success": True, "env": env, "output": output, "error": "", "duration_s": duration}


# ── Port detection ────────────────────────────────────────────────────────────

def _direct_macos_usb_serial_ports() -> list[str]:
    dev = Path("/dev")
    ports: set[str] = set()
    for pattern in _MACOS_USB_SERIAL_GLOBS:
        ports.update(str(path) for path in dev.glob(pattern))
    return sorted(
        ports,
        key=lambda port: (
            0 if Path(port).name.startswith("cu.") else 1,
            _serial_device_name(port).lower(),
            port,
        ),
    )


def _direct_macos_usb_serial_device(port: str) -> dict:
    return {
        "port": _preferred_macos_serial_port(port),
        "description": (
            "macOS QA-firmware USB CDC port"
            if _needs_usb_bootloader_recovery(port)
            else "macOS USB serial port"
        ),
        "hwid": "",
    }


def _select_devices_by_ports(devices: list[dict], ports: list[str] | None) -> list[dict]:
    if not ports:
        return devices
    wanted = set(ports)
    wanted_families = {
        family for family in (_macos_usb_serial_family(port) for port in ports) if family
    }
    selected = [
        d for d in devices
        if d.get("port") in wanted
        or (
            _macos_usb_serial_family(d.get("port", ""))
            and _macos_usb_serial_family(d.get("port", "")) in wanted_families
        )
    ]
    selected_ports = {d.get("port", "") for d in selected}
    selected_families = {
        family for family in (_macos_usb_serial_family(d.get("port", "")) for d in selected) if family
    }
    for port in ports:
        preferred = _preferred_macos_serial_port(port)
        family = _macos_usb_serial_family(preferred)
        if (
            Path(port).exists()
            and preferred not in selected_ports
            and not (family and family in selected_families)
        ):
            selected.append(_direct_macos_usb_serial_device(port))
            selected_ports.add(preferred)
            if family:
                selected_families.add(family)
    return selected


def _match_reenumerated_devices(
    before: list[dict],
    after: list[dict],
    selected_before: list[dict],
    ports: list[str] | None,
) -> list[dict]:
    if not ports:
        return after

    wanted = set(ports)
    wanted_families = {
        family for family in (_macos_usb_serial_family(port) for port in ports) if family
    }
    matched: list[dict] = []
    seen: set[str] = set()
    selected_by_port = {d.get("port", ""): d for d in selected_before}
    selected_by_family = {
        _macos_usb_serial_family(d.get("port", "")): d
        for d in selected_before
        if _macos_usb_serial_family(d.get("port", ""))
    }
    selected_by_location = {
        d.get("location", ""): d
        for d in selected_before
        if d.get("location")
    }

    def add(device: dict, source: dict | None = None) -> None:
        port = device.get("port", "")
        if port and port not in seen:
            device = dict(device)
            if source and source.get("port") and source.get("port") != port:
                device["initial_port"] = source.get("port", "")
                device["initial_usb_id"] = source.get("usb_id", "")
            matched.append(device)
            seen.add(port)

    for device in after:
        port = device.get("port", "")
        family = _macos_usb_serial_family(port)
        if port in wanted or (family and family in wanted_families):
            add(device, selected_by_port.get(port) or selected_by_family.get(family))

    if selected_by_location:
        for device in after:
            location = device.get("location", "")
            if location in selected_by_location:
                add(device, selected_by_location[location])

    before_ports = {d.get("port", "") for d in before}
    single_source = selected_before[0] if len(selected_before) == 1 else None
    for device in after:
        if device.get("port", "") not in before_ports:
            add(device, single_source)

    return matched


def _recovery_scan_ready(
    matched: list[dict],
    *,
    selected_count: int,
    recovery_source_ports: set[str],
) -> bool:
    if len(matched) < selected_count:
        return False
    matched_ports = {d.get("port", "") for d in matched}
    return not bool(matched_ports & recovery_source_ports)


def _pulse_bootloader_control_lines(port: str) -> dict:
    errors: list[str] = []
    for attempt in range(1, USB_BOOTLOADER_RECOVERY_ATTEMPTS + 1):
        try:
            import serial

            ser = serial.Serial()
            ser.port = port
            ser.baudrate = 115200
            ser.timeout = 0.1
            ser.write_timeout = 0
            ser.open()
            try:
                ser.dtr = False
                ser.rts = True
                time.sleep(USB_BOOTLOADER_RECOVERY_BOOT_HOLD_S)
                ser.dtr = True
                ser.rts = False
                time.sleep(USB_BOOTLOADER_RECOVERY_RELEASE_HOLD_S)
                ser.dtr = False
            finally:
                ser.close()
            time.sleep(USB_BOOTLOADER_RECOVERY_SETTLE_S)
            if not Path(port).exists():
                return {
                    "port": port,
                    "success": True,
                    "attempts": attempt,
                    "error": "port disappeared after reset pulse",
                }
        except FileNotFoundError:
            return {
                "port": port,
                "success": True,
                "attempts": attempt,
                "error": "port disappeared after reset pulse",
            }
        except Exception as e:
            errors.append(f"attempt {attempt}: {e}")
            time.sleep(0.25 * attempt)

    return {
        "port": port,
        "success": False,
        "attempts": USB_BOOTLOADER_RECOVERY_ATTEMPTS,
        "error": "; ".join(errors[-3:]) or "port still present after reset pulses",
    }


async def _recover_usb_bootloader_ports(devices: list[dict]) -> list[dict]:
    recovery_ports = [
        d["port"]
        for d in devices
        if d.get("port") and _needs_usb_bootloader_recovery(d["port"])
    ]
    if not recovery_ports:
        return []

    results: list[dict] = []
    semaphore = asyncio.Semaphore(USB_BOOTLOADER_RECOVERY_CONCURRENCY)

    async def recover_one(port: str) -> None:
        async with semaphore:
            _safe_activity_heartbeat(_heartbeat_payload("USB bootloader recovery", port))
            result = await asyncio.to_thread(_pulse_bootloader_control_lines, port)
            results.append(result)

    await asyncio.gather(*(recover_one(port) for port in recovery_ports))
    return sorted(results, key=lambda d: d.get("port", ""))


async def _detect_badge_devices_raw() -> list:
    devices = []
    try:
        pio = _resolve_pio()
        proc = await asyncio.create_subprocess_exec(
            pio, "device", "list", "--json-output",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await proc.communicate()
        try:
            devices = json.loads(stdout.decode())
        except json.JSONDecodeError:
            devices = []
    except Exception:
        devices = []

    devices.extend(
        _direct_macos_usb_serial_device(port)
        for port in _direct_macos_usb_serial_ports()
    )

    results = []
    seen_ports = set()
    seen_families = set()
    for dev in devices:
        hwid_raw = (dev.get("hwid") or "")
        port = _preferred_macos_serial_port(dev.get("port") or "")
        if not port:
            continue
        dev = {**dev, "port": port}
        family = _macos_usb_serial_family(port)
        if not _looks_like_badge_device(dev) or port in seen_ports or (family and family in seen_families):
            continue

        vid_pid_m = _VID_PID_RE.search(hwid_raw)
        ser_m = _SER_RE.search(hwid_raw)
        loc_m = _LOC_RE.search(hwid_raw)
        vid_pid = (vid_pid_m.group(1).upper() if vid_pid_m else "")
        serial = (ser_m.group(1) if ser_m else "")
        location = (loc_m.group(1) if loc_m else "")
        if vid_pid and serial:
            usb_id = f"{vid_pid}:SER:{serial}"
        elif serial:
            usb_id = f"SER:{serial}"
        elif vid_pid and location:
            usb_id = f"{vid_pid}:LOC:{location}"
        elif location:
            usb_id = f"LOC:{location}"
        else:
            usb_id = port

        results.append({
            "port": port,
            "description": dev.get("description") or "",
            "hwid": hwid_raw,
            "vid_pid": vid_pid,
            "serial": serial,
            "location": location,
            "usb_id": usb_id,
        })
        seen_ports.add(port)
        if family:
            seen_families.add(family)

    return sorted(results, key=lambda d: d["port"])


@activity.defn
async def detect_badge_ports() -> list:
    """Return sorted list of serial ports for connected badges."""
    devices = await _detect_badge_devices_raw()
    return sorted(d["port"] for d in devices)


@activity.defn
async def detect_badge_devices() -> list:
    """Return connected badge devices with USB identity metadata."""
    return await _detect_badge_devices_raw()


@activity.defn
async def prepare_badge_devices_for_flashing(ports: list[str] | None = None) -> list:
    """Detect badges and recover QA-firmware USB CDC ports into bootloader mode."""
    before = await _detect_badge_devices_raw()
    selected_before = _select_devices_by_ports(before, ports)
    _safe_activity_heartbeat(_heartbeat_payload(
        "USB badge detection",
        ", ".join(d.get("port", "") for d in selected_before) or "none",
    ))
    recovery_results = await _recover_usb_bootloader_ports(selected_before)
    if not recovery_results:
        return selected_before

    _safe_activity_heartbeat(_heartbeat_payload(
        "USB bootloader recovery",
        f"reset pulses sent to {len(recovery_results)} port(s)",
    ))

    after = before
    matched: list[dict] = []
    recovery_source_ports = {r.get("port", "") for r in recovery_results}
    for _ in range(USB_BOOTLOADER_RECOVERY_DETECT_TRIES):
        await asyncio.sleep(USB_BOOTLOADER_RECOVERY_DETECT_DELAY_S)
        after = await _detect_badge_devices_raw()
        matched = _match_reenumerated_devices(before, after, selected_before, ports)
        if _recovery_scan_ready(
            matched,
            selected_count=len(selected_before),
            recovery_source_ports=recovery_source_ports,
        ):
            break

    if not matched:
        matched = _match_reenumerated_devices(before, after, selected_before, ports)
    if not matched:
        matched = selected_before

    recovery_by_port = {r.get("port", ""): r for r in recovery_results}
    for device in matched:
        port = device.get("port", "")
        if port in recovery_by_port:
            device["usb_bootloader_recovery"] = recovery_by_port[port]
        else:
            device["usb_bootloader_recovery"] = {
                "success": all(r.get("success") for r in recovery_results),
                "source_ports": [r.get("port", "") for r in recovery_results],
            }
    return matched


@activity.defn
async def resolve_badge_port(device: dict) -> dict:
    """Resolve the current serial port for a previously discovered badge."""
    usb_id = device.get("usb_id") or ""
    initial_port = device.get("port") or ""
    current = await _detect_badge_devices_raw()

    for candidate in current:
        if usb_id and candidate.get("usb_id") == usb_id:
            return {"success": True, "port": candidate["port"], "device": candidate}

    if initial_port:
        for candidate in current:
            if candidate.get("port") == initial_port:
                return {"success": True, "port": candidate["port"], "device": candidate}
        initial_family = _macos_usb_serial_family(initial_port)
        if initial_family:
            for candidate in current:
                if _macos_usb_serial_family(candidate.get("port", "")) == initial_family:
                    return {"success": True, "port": candidate["port"], "device": candidate}

    return {
        "success": False,
        "port": "",
        "device": device,
        "error": f"Badge not found: {usb_id or initial_port or 'unknown USB identity'}",
    }


# ── Flash: firmware ───────────────────────────────────────────────────────────

@activity.defn
async def flash_badge_firmware(env: str, port: str, firmware_dir: str = "") -> dict:
    """Upload compiled firmware binary to one badge.

    Returns: success, port, output, error, duration_s.
    """
    if not _env_supported(env):
        return {"success": False, "port": port, "output": "",
                "error": _env_error(env), "duration_s": 0.0}

    fw_dir = _fw(firmware_dir)
    start = time.monotonic()
    sections: list[str] = []
    activity.heartbeat(_heartbeat_payload(f"writing firmware -> {port}"))

    if await _esptool_firmware(env, port, fw_dir, sections):
        return {"success": True, "port": port, "output": "\n\n".join(sections),
                "error": "", "duration_s": round(time.monotonic() - start, 1)}

    pio = _resolve_pio()
    sections.append("direct esptool firmware upload failed; falling back to PlatformIO upload")
    proc = await asyncio.create_subprocess_exec(
        pio, "run", "-e", env, "--upload-port", port, "-t", "upload",
        cwd=str(fw_dir),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    stdout, _ = await proc.communicate()
    duration = round(time.monotonic() - start, 1)
    sections.append(_tail(stdout.decode(errors="replace"), 80))
    output = "\n\n".join(sections)

    if proc.returncode != 0:
        return {"success": False, "port": port, "output": output,
                "error": "Firmware upload failed.", "duration_s": duration}

    return {"success": True, "port": port, "output": output, "error": "", "duration_s": duration}


@activity.defn
async def flash_badge_images(env: str, port: str, firmware_dir: str = "") -> dict:
    """Upload firmware and filesystem artifacts in a single esptool session."""
    if not _env_supported(env):
        return {"success": False, "port": port, "output": "",
                "error": _env_error(env), "duration_s": 0.0}

    fw_dir = _fw(firmware_dir)
    start = time.monotonic()
    sections: list[str] = []
    activity.heartbeat(_heartbeat_payload(f"writing firmware + apps -> {port}"))

    ok = await _esptool_all_images(env, port, fw_dir, sections)
    duration = round(time.monotonic() - start, 1)

    if not ok:
        return {"success": False, "port": port, "output": "\n\n".join(sections),
                "error": "Combined firmware/filesystem upload failed.", "duration_s": duration}

    return {"success": True, "port": port, "output": "\n\n".join(sections),
            "error": "", "duration_s": duration}


@activity.defn
async def flash_badge_factory_image(port: str, image_path: str, firmware_dir: str = "") -> dict:
    """Upload a complete 16 MB factory image to one badge."""
    image = Path(image_path).expanduser()
    if not image.exists():
        return {
            "success": False,
            "port": port,
            "output": "",
            "error": f"Factory image not found: {image}",
            "duration_s": 0.0,
        }
    if not image.is_file():
        return {
            "success": False,
            "port": port,
            "output": "",
            "error": f"Factory image is not a file: {image}",
            "duration_s": 0.0,
        }

    fw_dir = _fw(firmware_dir)
    penv_py = _resolve_python()
    start = time.monotonic()
    sections: list[str] = []
    activity.heartbeat(_heartbeat_payload(f"writing factory image -> {port}"))

    for extra in ([], ["--no-stub"]):
        write_args = ["write-flash"]
        if not extra:
            write_args.append("-z")
        args = [
            str(penv_py), "-m", "esptool",
            "--chip", "esp32s3", "--port", port, "--baud", _upload_baud(),
            "--before", "default-reset", "--after", "hard-reset",
            *extra, *write_args,
            "0x0", str(image),
        ]
        if await _run_esptool(
            args,
            port=port,
            fw_dir=fw_dir,
            sections=sections,
            tail_lines=120,
            heartbeat_label=f"writing factory image -> {port}",
        ):
            duration = round(time.monotonic() - start, 1)
            return {
                "success": True,
                "port": port,
                "output": "\n\n".join(sections),
                "error": "",
                "duration_s": duration,
            }

    duration = round(time.monotonic() - start, 1)
    return {
        "success": False,
        "port": port,
        "output": "\n\n".join(sections),
        "error": "Factory image upload failed.",
        "duration_s": duration,
    }


# ── Flash: filesystem ─────────────────────────────────────────────────────────

@activity.defn
async def flash_badge_filesystem(env: str, port: str, firmware_dir: str = "") -> dict:
    """Upload filesystem (apps) image to one badge.

    Tries direct esptool first; falls back to PlatformIO uploadfs if that fails.
    Returns: success, port, output, error, duration_s.
    """
    if not _env_supported(env):
        return {"success": False, "port": port, "output": "",
                "error": _env_error(env), "duration_s": 0.0}

    fw_dir = _fw(firmware_dir)
    start = time.monotonic()
    sections: list[str] = []
    activity.heartbeat(_heartbeat_payload(f"writing apps -> {port}"))

    if await _esptool_fs(env, port, fw_dir, sections):
        return {"success": True, "port": port, "output": "\n\n".join(sections),
                "error": "", "duration_s": round(time.monotonic() - start, 1)}

    pio = _resolve_pio()
    sections.append("direct esptool filesystem upload failed; falling back to PlatformIO uploadfs")
    proc = await asyncio.create_subprocess_exec(
        pio, "run", "-e", env, "--upload-port", port, "-t", "uploadfs",
        cwd=str(fw_dir),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    stdout, _ = await proc.communicate()
    sections.append(_tail(stdout.decode(errors="replace"), 60))

    if proc.returncode == 0:
        return {"success": True, "port": port, "output": "\n".join(sections),
                "error": "", "duration_s": round(time.monotonic() - start, 1)}

    return {"success": False, "port": port, "output": "\n\n".join(sections),
            "error": "Filesystem upload failed.", "duration_s": round(time.monotonic() - start, 1)}


# ── Verify: boot marker ───────────────────────────────────────────────────────

@activity.defn
async def verify_badge_boot_marker(port: str, timeout_s: int = 45) -> dict:
    """Confirm the flashed firmware boots by reading serial logs.

    Looks for the boot marker emitted early in setup:
      [boot] Temporal Replay 2026 - <build timestamp> - ...
    """
    start = time.monotonic()
    activity.heartbeat(_heartbeat_payload(f"verifying boot marker -> {port}"))

    penv_py = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if not penv_py.exists():
        penv_py = Path(shutil.which("python3") or "python3")

    timeout = max(5, int(timeout_s))
    probe = textwrap.dedent(
        r"""
        import re
        import sys
        import time

        port = sys.argv[1]
        timeout = int(sys.argv[2])

        success_pat = re.compile(r"Temporal Replay 2026\s+-\s+", re.IGNORECASE)
        fail_pat = re.compile(
            r"Guru Meditation|Backtrace:|stack canary|panic'ed|Brownout detector|"
            r"abort\(\) was called",
            re.IGNORECASE,
        )

        end = time.time() + timeout
        lines = []
        ser = None
        last_err = ""

        while time.time() < end:
            if ser is None:
                try:
                    import serial
                    ser = serial.Serial()
                    ser.port = port
                    ser.baudrate = 115200
                    ser.timeout = 0.3
                    ser.write_timeout = 0.3
                    ser.dtr = False
                    ser.rts = False
                    ser.open()
                    lines.append(f"[verify] opened {port}")
                except Exception as e:
                    last_err = str(e)
                    time.sleep(0.4)
                    continue

            try:
                raw = ser.readline()
            except Exception as e:
                lines.append(f"[verify] read error: {e}")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(0.3)
                continue

            if not raw:
                continue

            line = raw.decode(errors="replace").rstrip()
            if line:
                lines.append(line)
            if success_pat.search(line):
                print("\n".join(lines[-120:]))
                sys.exit(0)
            if fail_pat.search(line):
                print("\n".join(lines[-120:]))
                sys.exit(2)

        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if last_err:
            lines.append(f"[verify] serial open retries exhausted: {last_err}")
        else:
            lines.append("[verify] timeout waiting for Replay boot marker")
        print("\n".join(lines[-120:]))
        sys.exit(1)
        """
    ).strip()

    proc = await asyncio.create_subprocess_exec(
        str(penv_py), "-c", probe, port, str(timeout),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    stdout = await _communicate_with_heartbeats(proc, f"verifying boot marker -> {port}")
    duration = round(time.monotonic() - start, 1)
    output = _tail(stdout, 120)

    if proc.returncode == 0:
        return {
            "success": True,
            "port": port,
            "output": output,
            "boot_log": output,
            "error": "",
            "duration_s": duration,
        }
    if proc.returncode == 2:
        return {
            "success": False,
            "port": port,
            "output": output,
            "boot_log": output,
            "error": "Badge reported boot crash during verification.",
            "duration_s": duration,
        }
    return {
        "success": False,
        "port": port,
        "output": output,
        "boot_log": output,
        "error": f"Timed out waiting for Replay boot marker ({timeout}s).",
        "duration_s": duration,
    }


# ── Post-flash clock sync ───────────────────────────────────────────────────

@activity.defn
async def sync_badge_clock(port: str, timeout_s: int = 15) -> dict:
    """Set the badge wall clock from the host after firmware verification."""
    start = time.monotonic()
    activity.heartbeat(_heartbeat_payload(f"syncing clock -> {port}"))

    penv_py = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if not penv_py.exists():
        penv_py = Path(shutil.which("python3") or "python3")

    now = datetime.now(timezone.utc)
    expected_epoch = int(now.timestamp())
    timeout = max(5, int(timeout_s))

    probe = textwrap.dedent(
        r"""
        import re
        import sys
        import time

        port = sys.argv[1]
        timeout = int(sys.argv[2])
        expected = int(sys.argv[3])
        epoch_arg = sys.argv[4]

        marker_pat = re.compile(r"CLOCK_SYNC_OK:(\d+)")
        fail_pat = re.compile(
            r"Guru Meditation|Backtrace:|stack canary|panic'ed|Brownout detector|"
            r"abort\(\) was called|Traceback|ImportError|NameError|AttributeError|"
            r"SyntaxError|ValueError",
            re.IGNORECASE,
        )

        code = (
            "import badge\n"
            "print('CLOCK_SYNC_OK:%%d' %% badge.set_time(%s))\n"
        ) % epoch_arg

        end = time.time() + timeout
        lines = []
        buffer = ""
        sent = False
        raw_requested = False
        raw_ready = False
        friendly_fallback = False
        saw_reconnect = False
        prompt_seen = False
        boot_ready = False
        last_raw_request = 0.0
        ser = None
        last_err = ""

        while time.time() < end:
            if ser is None:
                try:
                    import serial
                    ser = serial.Serial()
                    ser.port = port
                    ser.baudrate = 115200
                    ser.timeout = 0.25
                    ser.write_timeout = 0.5
                    ser.dtr = False
                    ser.rts = False
                    ser.open()
                    lines.append(f"[clock] opened {port}")
                    try:
                        ser.reset_input_buffer()
                    except Exception:
                        pass
                except Exception as e:
                    last_err = str(e)
                    try:
                        if ser is not None:
                            ser.close()
                    except Exception:
                        pass
                    ser = None
                    time.sleep(0.4)
                    continue

            try:
                raw = ser.read(256)
            except Exception as e:
                lines.append(f"[clock] read error: {e}")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(0.3)
                continue

            if not raw:
                continue

            chunk = raw.decode(errors="replace")
            buffer += chunk
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.rstrip("\r")
                if line:
                    lines.append(line)
                    if "phase=ready" in line or "GUI: initialized" in line:
                        boot_ready = True
                    if "[mpy] USB reconnect" in line:
                        saw_reconnect = True
                    if saw_reconnect and ">>>" in line:
                        prompt_seen = True

            if "phase=ready" in buffer or "GUI: initialized" in buffer:
                boot_ready = True
            if "[mpy] USB reconnect" in buffer:
                saw_reconnect = True
            if saw_reconnect and ">>>" in buffer:
                prompt_seen = True

            transcript = "\n".join(lines[-12:]) + "\n" + buffer

            if not sent and not raw_requested and (boot_ready or prompt_seen):
                # Opening USB resets the badge; after the firmware prints its
                # reconnect marker it drains stale input for ~350 ms. Raw REPL
                # avoids friendly-REPL echo/prompt quirks from the fuller
                # MicroPython runtime, but enter it only after boot is far
                # enough along that mpy_poll() is servicing input.
                time.sleep(0.75)
                try:
                    ser.reset_input_buffer()
                except Exception:
                    pass
                ser.write(b"\x03\x03")
                ser.flush()
                time.sleep(0.08)
                ser.write(b"\x01")
                ser.flush()
                raw_requested = True
                last_raw_request = time.time()
                lines.append(f"[clock] requested raw REPL for epoch {expected}")

            if raw_requested and not raw_ready:
                if "raw REPL" in transcript or "raw paste mode" in transcript:
                    raw_ready = True
                elif time.time() - last_raw_request > 3.0 and prompt_seen:
                    # If raw mode is not available, fall back to the previous
                    # friendly REPL path, but keep the more conservative boot
                    # wait above.
                    ser.write(b"\x02")
                    ser.flush()
                    time.sleep(0.1)
                    ser.write(("exec(%r)\n" % code).encode())
                    ser.flush()
                    sent = True
                    friendly_fallback = True
                    lines.append(f"[clock] friendly fallback requested epoch {expected}")

            if raw_ready and not sent:
                ser.write(code.encode())
                ser.write(b"\x04")
                ser.flush()
                sent = True
                lines.append(f"[clock] raw REPL requested epoch {expected}")

            m = marker_pat.search(transcript)
            if m:
                actual = int(m.group(1))
                try:
                    if raw_requested and not friendly_fallback:
                        ser.write(b"\x02")
                        ser.flush()
                except Exception:
                    pass
                if abs(actual - expected) <= 5:
                    print("\n".join(lines[-80:]))
                    sys.exit(0)
                lines.append(f"[clock] epoch mismatch actual={actual} expected={expected}")
                print("\n".join(lines[-80:]))
                sys.exit(3)

            if fail_pat.search(transcript):
                print("\n".join(lines[-80:]))
                sys.exit(2)

        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if last_err:
            lines.append(f"[clock] serial open retries exhausted: {last_err}")
        else:
            lines.append("[clock] timeout waiting for CLOCK_SYNC_OK")
        print("\n".join(lines[-80:]))
        sys.exit(1)
        """
    ).strip()

    proc = await asyncio.create_subprocess_exec(
        str(penv_py),
        "-c",
        probe,
        port,
        str(timeout),
        str(expected_epoch),
        str(expected_epoch),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    stdout = await _communicate_with_heartbeats(proc, f"syncing clock -> {port}")
    duration = round(time.monotonic() - start, 1)
    output = _tail(stdout, 80)

    if proc.returncode == 0:
        return {
            "success": True,
            "port": port,
            "output": output,
            "clock_log": output,
            "epoch": expected_epoch,
            "error": "",
            "duration_s": duration,
        }
    if proc.returncode == 2:
        error = "Badge reported crash/error while syncing clock."
    elif proc.returncode == 3:
        error = "Badge clock did not match host time after sync."
    else:
        error = f"Timed out syncing badge clock ({timeout}s)."
    return {
        "success": False,
        "port": port,
        "output": output,
        "clock_log": output,
        "epoch": expected_epoch,
        "error": error,
        "duration_s": duration,
    }


# ── Verify: BLE came up cleanly ─────────────────────────────────────────────

@activity.defn
async def verify_badge_ble(port: str, timeout_s: int = 90) -> dict:
    """Confirm badge reaches BLE scan/advertise state by reading serial logs."""
    start = time.monotonic()
    activity.heartbeat(_heartbeat_payload(f"verifying ble -> {port}"))

    penv_py = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if not penv_py.exists():
        penv_py = Path(shutil.which("python3") or "python3")

    timeout = max(10, int(timeout_s))
    probe = textwrap.dedent(
        r"""
        import re
        import sys
        import time

        port = sys.argv[1]
        timeout = int(sys.argv[2])

        success_pat = re.compile(
            r"\[badgebeacon\]\s+broadcasting\b|\[map/ble\]|\[ble\]\s+scan started",
            re.IGNORECASE,
        )
        fail_pat = re.compile(
            r"Guru Meditation|panic'ed|reset_reason=(?:4|5|6|7|15)\b|"
            r"ESP_ERR_NO_MEM|BLE_INIT:\s+Malloc failed|setAdvertisementData\(\).*519",
            re.IGNORECASE,
        )

        end = time.time() + timeout
        lines = []
        ser = None
        last_err = ""

        while time.time() < end:
            if ser is None:
                try:
                    import serial
                    ser = serial.Serial()
                    ser.port = port
                    ser.baudrate = 115200
                    ser.timeout = 0.3
                    ser.write_timeout = 0.3
                    ser.dtr = False
                    ser.rts = False
                    ser.open()
                    lines.append(f"[verify] opened {port}")
                except Exception as e:
                    last_err = str(e)
                    time.sleep(0.4)
                    continue

            try:
                raw = ser.readline()
            except Exception as e:
                lines.append(f"[verify] read error: {e}")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(0.3)
                continue

            if not raw:
                continue

            line = raw.decode(errors="replace").rstrip()
            if line:
                lines.append(line)
            if fail_pat.search(line):
                print("\n".join(lines[-160:]))
                sys.exit(2)
            if success_pat.search(line):
                print("\n".join(lines[-160:]))
                sys.exit(0)

        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if last_err:
            lines.append(f"[verify] serial open retries exhausted: {last_err}")
        else:
            lines.append("[verify] timeout waiting for BLE scan/advertise log")
        print("\n".join(lines[-160:]))
        sys.exit(1)
        """
    ).strip()

    proc = await asyncio.create_subprocess_exec(
        str(penv_py), "-c", probe, port, str(timeout),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    stdout = await _communicate_with_heartbeats(proc, f"verifying ble -> {port}")
    duration = round(time.monotonic() - start, 1)
    output = _tail(stdout, 160)

    if proc.returncode == 0:
        return {
            "success": True,
            "port": port,
            "output": output,
            "ble_log": output,
            "error": "",
            "duration_s": duration,
        }
    if proc.returncode == 2:
        return {
            "success": False,
            "port": port,
            "output": output,
            "ble_log": output,
            "error": "Badge reported BLE panic/heap failure during verification.",
            "duration_s": duration,
        }
    return {
        "success": False,
        "port": port,
        "output": output,
        "ble_log": output,
        "error": f"Timed out waiting for BLE scan/advertise log ({timeout}s).",
        "duration_s": duration,
    }


async def _esptool_fs(env: str, port: str, fw_dir: Path, sections: list) -> bool:
    penv_py = _resolve_python()
    build_dir = _build_dir(fw_dir, env)
    fs_bin = _find_filesystem_image(build_dir)
    if not fs_bin:
        sections.append("direct esptool: no filesystem image found")
        return False

    FS_SUBTYPES = {0x81, 0x82, 0x83}  # fat=0x81, spiffs=0x82, littlefs=0x83
    partition = _partition_info(build_dir, subtypes=FS_SUBTYPES)
    if partition is None:
        sections.append("direct esptool: could not determine filesystem offset from partitions.bin")
        return False
    offset, partition_size = partition
    if fs_bin.stat().st_size > partition_size:
        sections.append(
            "direct esptool: filesystem image is larger than partition "
            f"({fs_bin.stat().st_size} > {partition_size}); rebuild the filesystem image"
        )
        return False

    for extra in ([], ["--no-stub"]):
        write_args = ["write-flash"]
        if not extra:
            write_args.append("-z")
        args = [
            str(penv_py), "-m", "esptool",
            "--chip", "esp32s3", "--port", port, "--baud", _upload_baud(),
            "--before", "default-reset", "--after", "hard-reset",
            *extra, *write_args, hex(offset), str(fs_bin),
        ]
        if await _run_esptool(
            args,
            port=port,
            fw_dir=fw_dir,
            sections=sections,
            tail_lines=40,
            heartbeat_label=f"writing apps -> {port}",
        ):
            return True

    return False


async def _esptool_firmware(env: str, port: str, fw_dir: Path, sections: list) -> bool:
    penv_py = _resolve_python()
    build_dir = _build_dir(fw_dir, env)
    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    firmware = build_dir / "firmware.bin"
    boot_app0 = _find_boot_app0()

    missing = [str(p) for p in (bootloader, partitions, firmware) if not p.exists()]
    if boot_app0 is None:
        missing.append("boot_app0.bin")
    if missing:
        sections.append("direct esptool: missing firmware artifacts: " + ", ".join(missing))
        return False

    app_offset = _partition_offset(build_dir, name="app0") or 0x10000
    assert boot_app0 is not None

    for extra in ([], ["--no-stub"]):
        write_args = ["write-flash"]
        if not extra:
            write_args.append("-z")
        args = [
            str(penv_py), "-m", "esptool",
            "--chip", "esp32s3", "--port", port, "--baud", _upload_baud(),
            "--before", "default-reset", "--after", "no-reset",
            *extra, *write_args,
            "0x0", str(bootloader),
            "0x8000", str(partitions),
            "0xe000", str(boot_app0),
            hex(app_offset), str(firmware),
        ]
        if await _run_esptool(
            args,
            port=port,
            fw_dir=fw_dir,
            sections=sections,
            tail_lines=60,
            heartbeat_label=f"writing firmware -> {port}",
        ):
            return True

    return False


async def _esptool_all_images(env: str, port: str, fw_dir: Path, sections: list) -> bool:
    penv_py = _resolve_python()
    build_dir = _build_dir(fw_dir, env)
    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    firmware = build_dir / "firmware.bin"
    fs_bin = _find_filesystem_image(build_dir)
    boot_app0 = _find_boot_app0()

    missing = [str(p) for p in (bootloader, partitions, firmware) if not p.exists()]
    if fs_bin is None:
        missing.append("filesystem image")
    if boot_app0 is None:
        missing.append("boot_app0.bin")
    if missing:
        sections.append("direct esptool: missing flash artifacts: " + ", ".join(missing))
        return False

    app_offset = _partition_offset(build_dir, name="app0") or 0x10000
    FS_SUBTYPES = {0x81, 0x82, 0x83}
    fs_partition = _partition_info(build_dir, subtypes=FS_SUBTYPES)
    if fs_partition is None:
        sections.append("direct esptool: could not determine filesystem offset from partitions.bin")
        return False
    fs_offset, fs_partition_size = fs_partition
    assert boot_app0 is not None
    assert fs_bin is not None

    if fs_bin.stat().st_size > fs_partition_size:
        sections.append(
            "direct esptool: filesystem image is larger than partition "
            f"({fs_bin.stat().st_size} > {fs_partition_size}); rebuild the filesystem image"
        )
        return False

    for extra in ([], ["--no-stub"]):
        write_args = ["write-flash"]
        if not extra:
            write_args.append("-z")
        args = [
            str(penv_py), "-m", "esptool",
            "--chip", "esp32s3", "--port", port, "--baud", _upload_baud(),
            "--before", "default-reset", "--after", "hard-reset",
            *extra, *write_args,
            "0x0", str(bootloader),
            "0x8000", str(partitions),
            "0xe000", str(boot_app0),
            hex(app_offset), str(firmware),
            hex(fs_offset), str(fs_bin),
        ]
        if await _run_esptool(
            args,
            port=port,
            fw_dir=fw_dir,
            sections=sections,
            tail_lines=120,
            heartbeat_label=f"writing firmware + apps -> {port}",
        ):
            return True

    return False
