#!/usr/bin/env python3
"""
Build a single-file 16 MB recovery image for the Temporal Replay 2026
badge.

The output is a flat dump of the entire SPI flash chip with everything
a fresh badge needs pre-positioned at its production offset:

  0x000000  bootloader.bin       (PlatformIO output)
  0x008000  partitions.bin       (the `_doom` layout — same one OTA targets)
  0x00E000  boot_app0.bin        (framework — points OTA pointer at app0)
  0x010000  firmware.bin         (the production app — built by `pio run -e echo`)
  0x7D0000  fatfs.bin            (initial_filesystem + DOOM WAD + every
                                  in-repo community app preloaded into
                                  /apps/<id>/)

End-user recovery (no source checkout, no PlatformIO):

  esptool.py --chip esp32s3 --port <PORT> write_flash 0x0 \
      temporal-badge-full-flash-16mb.bin

The merge target uses `--flash_size 16MB --fill-flash-size 16MB` so the
output is exactly 16 MiB and `write_flash 0x0 …` reproduces the badge's
factory state byte-for-byte. (esptool writes nothing for 0xFF-only
sectors, so the cost of flashing the gap regions is essentially free
in wall-clock terms.)

Inputs are read from `firmware/.pio/build/<env>/`. The `fatfs.bin` step
runs from this script — see `stage_community_apps()` — so callers don't
have to remember to run `pio run -t buildfs` after dropping community
files into `firmware/data/apps/`.

CI: `release-firmware.yml` calls this with `--env echo --out
artifacts/release/temporal-badge-full-flash-16mb.bin`. The resulting
file is attached to every GitHub release alongside `firmware.bin`.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# All `pio` invocations explicitly use the bundled binary so we don't
# trip over a pyenv / micromamba shim that resolves to a Python without
# PlatformIO installed. Same convention as ignition/start.sh.
PIO_BIN = Path.home() / ".platformio" / "penv" / "bin" / "pio"
# The PlatformIO virtualenv's Python interpreter is the canonical
# source of `esptool` on dev machines — pio depends on it and installs
# it into the same venv. Falling back to `sys.executable` only works
# when the user invoked us with the system Python (which CI does, but
# a pyenv / micromamba shell does not). See `find_esptool_python()`.
PIO_VENV_PYTHON = Path.home() / ".platformio" / "penv" / "bin" / "python"

# Default flash offsets for partitions_replay_16MB_doom.csv. The
# bootloader + boot_app0 offsets are framework constants. The ffat
# offset MUST match the partition table that ships at 0x8000 — if you
# change the partition CSV, update this too.
PART_DOOM_FFAT_OFFSET = 0x7D0000
PART_BOOTLOADER_OFFSET = 0x0
PART_TABLE_OFFSET = 0x8000
PART_BOOT_APP0_OFFSET = 0xE000
PART_FIRMWARE_OFFSET = 0x10000

FLASH_SIZE_BYTES = 16 * 1024 * 1024


class BuildError(RuntimeError):
    pass


def repo_root_from(firmware_dir: Path) -> Path:
    return firmware_dir.parent


def discover_boot_app0(framework_root: Path) -> Path:
    """Locate boot_app0.bin inside the active arduino-esp32 framework
    package. Path layout has been stable across framework versions but
    we walk the tree instead of hardcoding it so a future package
    layout change just works."""
    candidates = list(framework_root.rglob("boot_app0.bin"))
    if not candidates:
        raise BuildError(
            f"boot_app0.bin not found under {framework_root}. "
            "Is framework-arduinoespressif32 installed?"
        )
    # Prefer the partitions/ folder — that's where the framework's
    # own flasher scripts pull it from.
    for c in candidates:
        if c.parent.name == "partitions":
            return c
    return candidates[0]


def discover_framework_root() -> Path:
    """Best-effort: ask PlatformIO where the framework lives. Falls
    back to ~/.platformio/packages so manual invocations still work."""
    if PIO_BIN.is_file():
        try:
            out = subprocess.check_output(
                [str(PIO_BIN), "pkg", "show", "framework-arduinoespressif32"],
                stderr=subprocess.STDOUT,
                text=True,
            )
            for line in out.splitlines():
                line = line.strip()
                # `pio pkg show` prints "Location: <path>" in its
                # default text mode.
                if line.lower().startswith("location:"):
                    p = Path(line.split(":", 1)[1].strip())
                    if p.is_dir():
                        return p
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
    # Fallback — most CI runners and dev boxes use the default path.
    default = Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32"
    if default.is_dir():
        return default
    raise BuildError(
        "Could not locate framework-arduinoespressif32. "
        "Run `pio pkg install -e echo` first."
    )


def stage_community_apps(repo: Path) -> int:
    """Mirror community/<id>/ into firmware/data/apps/<id>/ so the next
    `pio run -t buildfs` bakes them into fatfs.bin. Returns the number
    of apps staged so the caller can log it.

    Skips `manifest.toml` (contributor metadata, not runtime) and any
    dotfile / __pycache__ noise. Existing `data/apps/<id>/` from a
    previous run is replaced — the directory belongs to this script
    while it's running, and `upload_doom_wad.py` already wipes the
    whole `data/` tree on `buildfs` anyway."""
    community = repo / "community"
    data_apps = repo / "firmware" / "data" / "apps"
    if not community.is_dir():
        return 0
    data_apps.mkdir(parents=True, exist_ok=True)

    count = 0
    for app_dir in sorted(community.iterdir()):
        if not app_dir.is_dir() or app_dir.name.startswith("."):
            continue
        dest = data_apps / app_dir.name
        if dest.exists():
            shutil.rmtree(dest)
        # copytree with our own ignore so we don't drag in
        # manifest.toml, .DS_Store, __pycache__, etc.
        def _ignore(_src: str, names: list[str]) -> list[str]:
            drop: list[str] = []
            for n in names:
                if n in ("manifest.toml", ".DS_Store", "__pycache__"):
                    drop.append(n)
                elif n.startswith("."):
                    drop.append(n)
                elif n.endswith((".pyc", ".pyo")):
                    drop.append(n)
            return drop
        shutil.copytree(app_dir, dest, ignore=_ignore)
        count += 1
        print(f"[recovery-image] staged community/{app_dir.name}/ "
              f"-> data/apps/{app_dir.name}/")
    return count


def build_fatfs(firmware_dir: Path, env: str) -> Path:
    """Build fatfs.bin via PlatformIO. Returns the on-disk path."""
    if not PIO_BIN.is_file():
        raise BuildError(
            f"PlatformIO not found at {PIO_BIN}. "
            "Install with: python -m pip install platformio"
        )
    print(f"[recovery-image] pio run -e {env} -t buildfs")
    proc = subprocess.run(
        [str(PIO_BIN), "run", "-e", env, "-t", "buildfs"],
        cwd=str(firmware_dir),
        env={**os.environ, "BADGE_ALLOW_MISSING_DOOM_WAD": "1"},
    )
    if proc.returncode != 0:
        raise BuildError(f"`pio run -e {env} -t buildfs` failed "
                         f"(rc={proc.returncode})")
    fatfs = firmware_dir / ".pio" / "build" / env / "fatfs.bin"
    if not fatfs.is_file():
        raise BuildError(f"buildfs succeeded but {fatfs} missing")
    return fatfs


def python_has_module(py: Path, module: str) -> bool:
    """True iff `<py> -c 'import <module>'` exits 0. Cheap probe used
    to pick an interpreter that actually has esptool installed."""
    if not py.is_file():
        return False
    try:
        proc = subprocess.run(
            [str(py), "-c", f"import {module}"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return proc.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def find_esptool_python() -> Path:
    """Locate a Python interpreter that has esptool importable. Tries
    PlatformIO's bundled venv first (it always carries esptool because
    pio uses it for flashing), then falls back to `sys.executable`.

    Catches the common dev-machine failure where the script is invoked
    via a pyenv / micromamba shell whose Python doesn't have esptool
    — pio's penv does and is the right choice in that case."""
    candidates: list[Path] = []
    if PIO_VENV_PYTHON.is_file():
        candidates.append(PIO_VENV_PYTHON)
    candidates.append(Path(sys.executable))

    for py in candidates:
        if python_has_module(py, "esptool"):
            return py

    tried = ", ".join(str(p) for p in candidates)
    raise BuildError(
        f"No Python interpreter with `esptool` was found. Tried: {tried}. "
        f"Install esptool via PlatformIO (`{PIO_BIN} pkg install -e echo`) "
        f"or directly: `python3 -m pip install --user esptool`."
    )


def merge_bin(out_path: Path, parts: list[tuple[int, Path]]) -> None:
    """Run `esptool.py merge_bin --fill-flash-size 16MB` against the
    given (offset, file) tuples. Uses `find_esptool_python()` so the
    invocation works regardless of which shell launched the script."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    py = find_esptool_python()
    argv = [
        str(py), "-m", "esptool",
        "--chip", "esp32s3",
        "merge_bin",
        "--output", str(out_path),
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "16MB",
        "--fill-flash-size", "16MB",
    ]
    for offset, path in parts:
        if not path.is_file():
            raise BuildError(f"merge_bin input missing: {path}")
        argv.extend([f"0x{offset:X}", str(path)])
    print(f"[recovery-image] {' '.join(argv)}")
    proc = subprocess.run(argv)
    if proc.returncode != 0:
        raise BuildError(f"esptool merge_bin failed (rc={proc.returncode})")
    actual = out_path.stat().st_size
    if actual != FLASH_SIZE_BYTES:
        raise BuildError(
            f"merged image is {actual} bytes, expected {FLASH_SIZE_BYTES} "
            f"({FLASH_SIZE_BYTES / 1024 / 1024:.0f} MB). Check "
            f"--fill-flash-size argument and partition offsets."
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--env", default="echo",
                    help="PlatformIO env to source firmware.bin / "
                         "bootloader.bin / partitions.bin / fatfs.bin "
                         "from (default: echo)")
    ap.add_argument("--out", required=True, type=Path,
                    help="Output path for the merged 16 MB image.")
    ap.add_argument("--ffat-offset", type=lambda s: int(s, 0),
                    default=PART_DOOM_FFAT_OFFSET,
                    help="ffat partition offset (default: 0x7D0000 — _doom)")
    ap.add_argument("--skip-buildfs", action="store_true",
                    help="Don't run `pio run -t buildfs` — assume the "
                         "fatfs.bin under .pio/build/<env>/ is already "
                         "up to date.")
    args = ap.parse_args()

    firmware_dir = Path(__file__).resolve().parents[1]
    repo = repo_root_from(firmware_dir)
    build_dir = firmware_dir / ".pio" / "build" / args.env

    try:
        n_apps = stage_community_apps(repo)
        if n_apps:
            print(f"[recovery-image] {n_apps} community app(s) staged")
        else:
            print("[recovery-image] no community apps to stage "
                  "(community/ is empty)")

        if args.skip_buildfs:
            fatfs = build_dir / "fatfs.bin"
            if not fatfs.is_file():
                raise BuildError(
                    f"--skip-buildfs given but {fatfs} doesn't exist. "
                    f"Run without --skip-buildfs first.")
        else:
            fatfs = build_fatfs(firmware_dir, args.env)

        framework_root = discover_framework_root()
        boot_app0 = discover_boot_app0(framework_root)
        print(f"[recovery-image] boot_app0.bin -> {boot_app0}")

        merge_bin(args.out, [
            (PART_BOOTLOADER_OFFSET, build_dir / "bootloader.bin"),
            (PART_TABLE_OFFSET,      build_dir / "partitions.bin"),
            (PART_BOOT_APP0_OFFSET,  boot_app0),
            (PART_FIRMWARE_OFFSET,   build_dir / "firmware.bin"),
            (args.ffat_offset,       fatfs),
        ])

        print(f"[recovery-image] wrote {args.out} "
              f"({args.out.stat().st_size / 1024 / 1024:.2f} MB)")
        return 0
    except BuildError as exc:
        print(f"[recovery-image] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
