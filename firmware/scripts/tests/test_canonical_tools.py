"""Regression tests for public-repository host tooling."""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest.mock import patch


SCRIPTS_DIR = Path(__file__).resolve().parents[1]


def load_script_module(script_name: str) -> ModuleType:
    """Load a host-tool script without adding its directory to ``sys.path``.

    :param script_name: Filename within ``firmware/scripts``.
    :return: Loaded Python module.
    """
    script_path = SCRIPTS_DIR / script_name
    spec = importlib.util.spec_from_file_location(script_path.stem, script_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {script_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GenerateStartupFilesTests(unittest.TestCase):
    """Verify deterministic filesystem-manifest generation."""

    def test_manifest_does_not_include_itself(self) -> None:
        """A regenerated manifest must be stable and omit its own path."""
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = Path(temp_dir)
            scripts_dir = repo_root / "firmware" / "scripts"
            source_dir = repo_root / "firmware" / "initial_filesystem"
            scripts_dir.mkdir(parents=True)
            source_dir.mkdir(parents=True)
            script_path = scripts_dir / "generate_startup_files.py"
            shutil.copy2(SCRIPTS_DIR / script_path.name, script_path)
            (source_dir / "hello.txt").write_text("hello\n", encoding="utf-8")
            (source_dir / "manifest.json").write_text("{}\n", encoding="utf-8")

            command = [sys.executable, str(script_path)]
            environment = {
                **os.environ,
                "TEMPORAL_BADGE_REGEN_MANIFEST": "1",
            }
            subprocess.run(command, env=environment, check=True, capture_output=True)
            manifest_path = source_dir / "manifest.json"
            first_content = manifest_path.read_bytes()
            manifest = json.loads(first_content)

            self.assertEqual(
                [entry["path"] for entry in manifest["files"]],
                ["/hello.txt"],
            )

            subprocess.run(command, env=environment, check=True, capture_output=True)
            self.assertEqual(manifest_path.read_bytes(), first_content)


class RecoveryImageTests(unittest.TestCase):
    """Verify recovery-image defaults match the public repository."""

    def test_stages_apps_from_community_apps(self) -> None:
        """Community submissions are copied from ``community_apps``."""
        module = load_script_module("build_recovery_image.py")
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = Path(temp_dir)
            app_dir = repo_root / "community_apps" / "demo"
            app_dir.mkdir(parents=True)
            (app_dir / "main.py").write_text("print('demo')\n", encoding="utf-8")
            (app_dir / "manifest.toml").write_text("ignored = true\n", encoding="utf-8")

            staged_count = module.stage_community_apps(repo_root)

            staged_dir = repo_root / "firmware" / "data" / "apps" / "demo"
            self.assertEqual(staged_count, 1)
            self.assertEqual(
                (staged_dir / "main.py").read_text(encoding="utf-8"),
                "print('demo')\n",
            )
            self.assertFalse((staged_dir / "manifest.toml").exists())

    def test_default_environment_is_replay2026(self) -> None:
        """A default invocation must target the public PlatformIO environment."""
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = Path(temp_dir)
            scripts_dir = repo_root / "firmware" / "scripts"
            scripts_dir.mkdir(parents=True)
            script_path = scripts_dir / "build_recovery_image.py"
            shutil.copy2(SCRIPTS_DIR / script_path.name, script_path)
            output_path = repo_root / "recovery.bin"

            result = subprocess.run(
                [
                    sys.executable,
                    str(script_path),
                    "--out",
                    str(output_path),
                    "--skip-buildfs",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("/.pio/build/replay2026/fatfs.bin", result.stderr)

    def test_buildfs_requests_community_app_staging(self) -> None:
        """Recovery filesystems must stage apps after data is regenerated."""
        module = load_script_module("build_recovery_image.py")
        with tempfile.TemporaryDirectory() as temp_dir:
            firmware_dir = Path(temp_dir)
            fake_pio = firmware_dir / "pio"
            fake_pio.touch()
            fatfs = firmware_dir / ".pio" / "build" / "replay2026" / "fatfs.bin"
            fatfs.parent.mkdir(parents=True)
            fatfs.touch()
            module.PIO_BIN = fake_pio

            with patch.object(module.subprocess, "run") as run:
                run.return_value.returncode = 0
                result = module.build_fatfs(firmware_dir, "replay2026")

            self.assertEqual(result, fatfs)
            self.assertEqual(
                run.call_args.kwargs["env"]["BADGE_STAGE_COMMUNITY_APPS"],
                "1",
            )


if __name__ == "__main__":
    unittest.main()
