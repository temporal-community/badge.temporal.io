#!/usr/bin/env python3
"""
inject_version.py — Single source of truth for the badge firmware version.

Reads firmware/VERSION (a one-line semver like "0.1.4") and injects:

  -DFIRMWARE_VERSION="<ver>"          (consumed by src/identity/BadgeVersion.h
                                       and any C++ TU that includes it)
  -DBADGE_FIRMWARE_VERSION="<ver>"    (consumed by lib/micropython_embed/src/
                                       mpconfigport.h to format the REPL banner
                                       "Replay Badge v<ver> with ESP32-S3")

Both header files keep their #ifndef fallbacks so direct `pio run` without
this script (or Arduino IDE builds) still compile — they just report the
hard-coded fallback string instead of the canonical VERSION.
"""

from __future__ import annotations

from pathlib import Path

try:
    Import("env")  # type: ignore[name-defined]
except NameError:
    env = None


def project_dir() -> Path:
    if env is not None:
        return Path(env.subst("$PROJECT_DIR"))
    return Path(__file__).resolve().parents[1]


def read_version(root: Path) -> str:
    path = root / "VERSION"
    if not path.exists():
        return "dev"
    raw = path.read_text(encoding="utf-8").strip()
    return raw or "dev"


def main() -> None:
    root = project_dir()
    version = read_version(root)

    print(f"[inject_version] firmware version = {version}")

    if env is None:
        return

    # Both names point at the same string. Two macros instead of one
    # because BadgeVersion.h and mpconfigport.h were already wired up to
    # different identifiers and changing either would break out-of-tree
    # consumers (BootSplash, DiagnosticsScreen, MICROPY_BANNER_MACHINE).
    flag_value = f'\\"{version}\\"'
    env.Append(BUILD_FLAGS=[
        f"-DFIRMWARE_VERSION={flag_value}",
        f"-DBADGE_FIRMWARE_VERSION={flag_value}",
    ])


main()
