"""
Rename the absolute-path-derived symbols emitted by PlatformIO's
`board_build.embed_files` (e.g.
``_binary__home_alex_..._bundle_bin_start``) into the path-independent
short forms DataCache.cpp references (``_binary_bundle_bin_start`` /
``_binary_bundle_bin_end``).

PlatformIO's _embed_files.py invokes ``xtensa-esp-elf-objcopy --input-target
binary <abs-path-to-bundle.bin> bundle.bin.txt.o`` and objcopy mangles the
absolute path into the symbol names. That makes the .o non-portable —
every build host produces different symbols, so DataCache's
``asm("_binary_bundle_bin_start")`` only links on the original developer's
machine.

This post-action runs once, right after the .txt.o is built, and uses
``objcopy --redefine-sym`` to rename the long symbols to the canonical
short names. Idempotent — re-running with already-renamed symbols is a
no-op.
"""

from os.path import basename, isfile
from pathlib import Path
import shutil
import subprocess

Import("env")

board = env.BoardConfig()
mcu = board.get("build.mcu", "esp32")
is_xtensa = mcu in ("esp32", "esp32s2", "esp32s3")

# PlatformIO doesn't always put the toolchain on PATH for SCons subprocess
# calls, so use the toolchain bin dir reported by the platform package
# rather than relying on bare names.
_tool_pkg = "toolchain-xtensa-esp-elf" if is_xtensa else "toolchain-riscv32-esp-elf"
_tool_dir = env.PioPlatform().get_package_dir(_tool_pkg) or ""
_tool_bin = str(Path(_tool_dir) / "bin") if _tool_dir else ""
_short_tool = (
    f"xtensa-{mcu}-elf-objcopy" if is_xtensa else "riscv32-esp-elf-objcopy"
)
_resolved = (
    str(Path(_tool_bin) / _short_tool)
    if _tool_bin and isfile(str(Path(_tool_bin) / _short_tool))
    else (shutil.which(_short_tool) or _short_tool)
)
objcopy = _resolved

# Match the embed_files entries declared in platformio.ini. Each entry's
# basename becomes the desired short symbol prefix.
files = env.GetProjectOption("board_build.embed_files", "").splitlines()
files = [f.strip() for f in files if f.strip()]


def _short_name(filename):
    """objcopy's symbol-name mangler: replace anything that isn't [A-Za-z0-9_]
    with '_'."""
    out = []
    for ch in filename:
        out.append(ch if ch.isalnum() or ch == "_" else "_")
    return "".join(out)


def _long_name(abs_path):
    """The symbol name objcopy actually produced for the absolute path."""
    return _short_name(abs_path)


def _list_symbols(obj_path):
    nm = (
        objcopy.replace("objcopy", "nm")
        if objcopy and "objcopy" in objcopy
        else "nm"
    )
    res = subprocess.run([nm, str(obj_path)], capture_output=True, text=True)
    if res.returncode != 0:
        return []
    out = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if parts:
            out.append(parts[-1])
    return out


def _rename_symbols(target, source, env):
    for f in files:
        obj = Path(env.subst("$BUILD_DIR")) / (basename(f) + ".txt.o")
        if not obj.is_file():
            continue
        short = _short_name(basename(f))      # e.g. bundle_bin
        # Discover the actual long symbols emitted by objcopy. The path
        # gets resolved (".." segments collapsed) before mangling, so we
        # can't reliably reconstruct the long name from the platformio.ini
        # entry — read the symbol table directly instead.
        existing = _list_symbols(obj)
        for suffix in ("start", "end", "size"):
            new = f"_binary_{short}_{suffix}"
            if new in existing:
                continue  # already renamed on a prior incremental build
            old = next(
                (s for s in existing
                 if s.startswith("_binary_")
                 and s.endswith(f"_{short}_{suffix}")
                 and s != new),
                None,
            )
            if not old:
                continue
            cmd = [
                objcopy,
                "--redefine-sym", f"{old}={new}",
                str(obj), str(obj),
            ]
            print(f"[normalize_bundle_symbols] {old} -> {new}")
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode != 0:
                stderr = res.stderr.strip()
                print(f"[normalize_bundle_symbols] objcopy failed: {stderr}")


# Hook into the link step: by the time PIOMAINPROG is being assembled,
# every embed_files .txt.o has been generated. This action finalizes the
# rename before the linker pulls them in.
for f in files:
    obj = Path(env.subst("$BUILD_DIR")) / (basename(f) + ".txt.o")
    env.AddPostAction(str(obj), _rename_symbols)
