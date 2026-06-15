"""
PlatformIO extra_script: prepare data/ for uploadfs with doom WAD.

Before building the filesystem image, this script:
  1. Preserves a local doom1.wad from data/
  2. Rebuilds data/ from initial_filesystem/
  3. Restores the WAD as data/doom1.wad, falling back to
     initial_filesystem/doom1.wad

This ensures uploadfs produces a FAT image that has BOTH the WAD and
all the files the badge firmware expects at first boot, without making
data/ a stale copy of initial_filesystem/.

Usage:
  1. Place doom1.wad in firmware/data/
  2. Run: pio run -e echo -t uploadfs
"""

import os
import shutil
from SCons.Script import COMMAND_LINE_TARGETS
Import("env")  # noqa: F821

WAD_CANDIDATES = ("doom1.wad", "DOOM1.WAD", "Doom1.wad", "Doom1.WAD")
SKIP_DIR_NAMES = {"__pycache__"}
SKIP_EXTENSIONS = {".pyc", ".pyo", ".wad"}
ALLOW_MISSING_WAD = os.environ.get("BADGE_ALLOW_MISSING_DOOM_WAD", "").lower() in {
    "1",
    "true",
    "yes",
    "on",
}
prepared = False


def find_wad(data_dir):
    for name in WAD_CANDIDATES:
        path = os.path.join(data_dir, name)
        if os.path.isfile(path):
            return path
    return None


def reset_staging_dir(data_dir, cache_dir):
    cached_wad = None
    wad_path = find_wad(data_dir)
    if wad_path:
        os.makedirs(cache_dir, exist_ok=True)
        cached_wad = os.path.join(cache_dir, "doom1.wad")
        shutil.copy2(wad_path, cached_wad)

    if os.path.isdir(data_dir):
        shutil.rmtree(data_dir)
    os.makedirs(data_dir, exist_ok=True)

    return cached_wad


def prepare_data_dir(source, target, env):
    global prepared
    if prepared:
        return
    prepared = True

    project_dir = env.get("PROJECT_DIR", "")
    data_dir = os.path.join(project_dir, "data")
    initial_fs = os.path.join(project_dir, "initial_filesystem")
    cache_dir = os.path.join(project_dir, ".pio", "doom_wad_cache")
    bundled_wad = find_wad(initial_fs)

    cached_wad = reset_staging_dir(data_dir, cache_dir)

    if os.path.isdir(initial_fs):
        for root, dirs, files in os.walk(initial_fs):
            dirs[:] = [
                d for d in dirs
                if d not in SKIP_DIR_NAMES and not d.startswith(".")
            ]
            rel_root = os.path.relpath(root, initial_fs)
            dest_root = os.path.join(data_dir, rel_root) if rel_root != "." else data_dir

            for d in dirs:
                os.makedirs(os.path.join(dest_root, d), exist_ok=True)

            for f in files:
                if f.startswith(".") or os.path.splitext(f)[1].lower() in SKIP_EXTENSIONS:
                    print(f"  SKIP {os.path.relpath(os.path.join(root, f), initial_fs)}")
                    continue
                src = os.path.join(root, f)
                dst = os.path.join(dest_root, f)
                shutil.copy2(src, dst)
                print(f"  + {os.path.relpath(dst, data_dir)}")

        print("[doom] Rebuilt data/ from initial_filesystem/")

    if cached_wad:
        shutil.copy2(cached_wad, os.path.join(data_dir, "doom1.wad"))
    elif bundled_wad:
        shutil.copy2(bundled_wad, os.path.join(data_dir, "doom1.wad"))

    # Check for WAD
    found = find_wad(data_dir)

    if not found:
        if ALLOW_MISSING_WAD:
            print("[doom] WAD missing; continuing because BADGE_ALLOW_MISSING_DOOM_WAD=1")
            return

        print("\n" + "=" * 64)
        print("  WAD file not found in data/ directory!")
        print()
        print("  Place your DOOM WAD file at:")
        print(f"    {os.path.join(data_dir, 'doom1.wad')}")
        print()
        print("  DOOM1.WAD (shareware) is ~4 MB.")
        print("  Download from: https://doomwiki.org/wiki/DOOM1.WAD")
        print("=" * 64 + "\n")
        env.Exit(1)
        return

    wad_size = os.path.getsize(found)
    print(f"[doom] WAD: {os.path.basename(found)} ({wad_size / 1024 / 1024:.1f} MB)")


if {"buildfs", "uploadfs", "uploadfsota"} & set(COMMAND_LINE_TARGETS):
    prepare_data_dir(None, None, env)

env.AddPreAction("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}", prepare_data_dir)
env.AddPreAction("$BUILD_DIR/fatfs.bin", prepare_data_dir)
env.AddPreAction("buildfs", prepare_data_dir)
env.AddPreAction("uploadfs", prepare_data_dir)
