#!/usr/bin/env python3
"""
Refresh compile_commands.json after upload so clangd tracks the uploaded env.
"""

from pathlib import Path
import subprocess
from typing import Any, cast


Import("env")  # type: ignore  # PlatformIO SCons global
env = cast(Any, globals()["env"])


def _refresh_compiledb(target=None, source=None, env=None, **_kwargs) -> None:
    if env is None:
        print("[refresh_compiledb] WARNING: missing SCons env; skipping compile db refresh")
        return

    project_dir = Path(env.subst("$PROJECT_DIR"))
    pio_env = env.subst("$PIOENV")
    python_exe = env.subst("$PYTHONEXE")

    cmd = [python_exe, "-m", "platformio", "run", "-e", pio_env, "-t", "compiledb"]
    print(f"[refresh_compiledb] Regenerating compile database for env '{pio_env}'")

    result = subprocess.run(cmd, cwd=project_dir, check=False)
    if result.returncode != 0:
        print(
            "[refresh_compiledb] WARNING: failed to regenerate compile_commands.json "
            f"(exit {result.returncode})"
        )


targets = set(COMMAND_LINE_TARGETS)  # type: ignore  # SCons global
if "compiledb" not in targets and "upload" in targets:
    env.AddPostAction("upload", _refresh_compiledb)
