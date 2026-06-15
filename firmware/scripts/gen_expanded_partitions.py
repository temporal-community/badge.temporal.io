"""
PlatformIO pre-script: compile the OPT-IN expanded partition table
(partitions_replay_16MB_ver2.csv) into a tiny .bin and stage it at a
stable path so the regular `board_build.embed_files` machinery can
fold it into the firmware image.

Why this exists
---------------
We want one production firmware binary that can in-place migrate a
badge from the default 6.0 MB ffat layout (`_doom`) to the opt-in
6.875 MB layout (`_ver2`) without USB reflashing. The migration code
in BadgeOTA needs the *bytes* of the new partition table at runtime
so it can rewrite flash @ 0x8000, but we don't want to ship the .csv
and reimplement gen_esp32part.py inside the firmware.

So: at build time, run the framework's gen_esp32part.py against the
expanded CSV and drop the resulting .bin in
`firmware/build_assets/partitions_ver2.bin`. platformio.ini lists
that path under `[env:echo].board_build.embed_files`, which makes
the linker emit:

    _binary_partitions_ver2_bin_start
    _binary_partitions_ver2_bin_end
    _binary_partitions_ver2_bin_size

(normalize_bundle_symbols.py canonicalises the long absolute-path
symbol names into those short forms, same as it does for bundle.bin.)

The generated .bin is gitignored; it is fully derived from the CSV
and the framework's generator script.

Idempotent — only rewrites the .bin when the source CSV or the
generator script is newer than the existing output.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

try:
    Import("env")  # type: ignore[name-defined]
except NameError:
    env = None  # allow `python3 gen_expanded_partitions.py` for testing


PARTITIONS_CSV_NAME = "partitions_replay_16MB_ver2.csv"
OUT_REL = Path("build_assets") / "partitions_ver2.bin"


def project_dir() -> Path:
    if env is not None:
        return Path(env.subst("$PROJECT_DIR"))
    return Path(__file__).resolve().parents[1]


def find_gen_script() -> Path | None:
    """Locate gen_esp32part.py inside the active PlatformIO framework."""
    candidates: list[Path] = []
    if env is not None:
        try:
            platform = env.PioPlatform()
            for pkg in ("framework-arduinoespressif32", "framework-espidf"):
                pkg_dir = platform.get_package_dir(pkg)
                if not pkg_dir:
                    continue
                base = Path(pkg_dir)
                candidates.extend(base.rglob("gen_esp32part.py"))
        except Exception as exc:  # pragma: no cover — defensive
            print(f"[gen_expanded_partitions] platform probe failed: {exc}")

    # Fallback search inside ~/.platformio for plain CLI invocations.
    home = Path.home() / ".platformio" / "packages"
    if home.is_dir():
        candidates.extend(home.rglob("gen_esp32part.py"))

    for c in candidates:
        if c.is_file():
            return c
    return None


def csv_path(root: Path) -> Path:
    return root / PARTITIONS_CSV_NAME


def out_path(root: Path) -> Path:
    return root / OUT_REL


def newer_than(src: Path, dst: Path) -> bool:
    if not dst.exists():
        return True
    try:
        return src.stat().st_mtime > dst.stat().st_mtime
    except OSError:
        return True


def regenerate(root: Path) -> Path:
    csv = csv_path(root)
    out = out_path(root)
    gen = find_gen_script()
    if not csv.is_file():
        raise SystemExit(f"[gen_expanded_partitions] missing CSV: {csv}")
    if gen is None:
        raise SystemExit(
            "[gen_expanded_partitions] could not locate gen_esp32part.py "
            "in the PlatformIO framework packages"
        )

    out.parent.mkdir(parents=True, exist_ok=True)

    if not newer_than(csv, out) and not newer_than(gen, out):
        # Up to date.
        return out

    # gen_esp32part.py CSV → BIN. We rely on the default MD5 trailer so
    # the bootloader accepts the new table without --disable-md5sum.
    cmd = [sys.executable, str(gen), str(csv), str(out)]
    print(f"[gen_expanded_partitions] {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(
            f"[gen_expanded_partitions] gen_esp32part.py failed "
            f"({proc.returncode})"
        )

    size = out.stat().st_size
    print(
        f"[gen_expanded_partitions] wrote {out.relative_to(root)} ({size} bytes)"
    )
    return out


def main() -> None:
    root = project_dir()
    out = regenerate(root)

    if env is None:
        return

    # Tie a relink to source-of-truth changes. embed_files is processed
    # outside this script, but a stale .bin can otherwise survive an
    # incremental build if the CSV alone changed.
    env.Depends("$BUILD_DIR/${PROGNAME}.elf", str(out))
    env.Depends("$BUILD_DIR/${PROGNAME}.elf", str(csv_path(root)))


main()
