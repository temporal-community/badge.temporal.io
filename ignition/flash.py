#!/usr/bin/env python3
"""flash.py — Build and flash Temporal Badge firmware.

Usage:
    python3 flash.py                         Build replay2026 + flash all badges
    python3 flash.py --build-and-flash       Explicit build + default filesystem + flash
    python3 flash.py --firmware-dir /path    Target a different firmware directory
    python3 flash.py --no-build              Skip build, flash only
    python3 flash.py --build-only            Build only, no flash
    python3 flash.py --factory-image PATH    Flash a downloaded factory image

Requires start.sh to have been run first (starts Temporal + worker).
"""
import argparse
import asyncio
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from rich.console import Console
    from rich.live import Live
    from rich.panel import Panel
    from rich.table import Table
    from rich.text import Text
except ImportError:
    print("error: 'rich' not installed. Run: ./setup.sh")
    sys.exit(1)

try:
    from temporalio.client import Client, WorkflowExecutionStatus
except ImportError:
    print("error: 'temporalio' not installed. Run: ./setup.sh")
    sys.exit(1)

from flash_worker.workflows import (
    TASK_QUEUE,
    BadgeFlashWorkflow,
    BuildFirmwareWorkflow,
    DetectBadgesWorkflow,
    FlashBadgesWorkflow,
)

TEMPORAL_UI = "http://localhost:8233"
POLL_INTERVAL = 0.35
ONLY_ENV = "replay2026"

console = Console()

_SPINNER = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]


def _spin(t: float) -> str:
    return _SPINNER[int(t * 8) % len(_SPINNER)]


def _s(seconds: float) -> str:
    return f"{int(seconds)}s"


# ── Status labels ─────────────────────────────────────────────────────────────

_BADGE_LABEL = {
    "waiting":    ("⠿",  "dim",         "Waiting..."),
    "starting":   (None,  "yellow",      "Starting workflow"),
    "running":    (None,  "yellow",      "Running"),
    "resolving":  (None,  "yellow",      "Resolving USB"),
    "preparing":  (None,  "yellow",      "Preparing images"),
    "flashing":   (None,  "yellow",      "Writing firmware + apps"),
    "firmware":   (None,  "yellow",      "Writing firmware"),
    "filesystem": (None,  "yellow",      "Writing apps"),
    "booting":    (None,  "cyan",        "Waiting for boot"),
    "verifying":  (None,  "cyan",        "Verifying boot"),
    "verifying_boot": (None, "cyan",     "Verifying boot"),
    "syncing_clock": (None, "cyan",      "Syncing clock"),
    "verifying_ble": (None, "cyan",      "Verifying BLE"),
    "done":       ("✓",  "bold green",  "Done"),
    "failed":     ("✗",  "bold red",    "Failed"),
}


def _badge_row(label: str, port: str, status: str, t: float, idx: int) -> tuple:
    icon_char, style, text = _BADGE_LABEL.get(status, ("?", "dim", status))
    if icon_char is None:
        icon = Text(_spin(t + idx * 0.3), style=style)
    else:
        icon = Text(icon_char, style=style)
    return icon, Text(label), Text(port, style="dim"), Text(text, style=style)


async def _query_child_statuses(client: Client, status: dict) -> dict:
    children = status.get("children") or []
    if not children:
        return status

    async def get_child(child: dict) -> dict:
        workflow_id = child.get("workflow_id")
        if not workflow_id:
            return child
        try:
            handle = client.get_workflow_handle(workflow_id)
            child_status = await handle.query(BadgeFlashWorkflow.status)
        except Exception:
            return child
        return {**child, **child_status}

    status = dict(status)
    status["children"] = await asyncio.gather(*(get_child(dict(c)) for c in children))
    return status


# ── Panels ────────────────────────────────────────────────────────────────────

def _build_panel(phase: str, elapsed: float, env: str, firmware_dir: str, error: str = "") -> Panel:
    t = Table.grid(padding=(0, 2))
    t.add_column(width=3)
    t.add_column(min_width=32)
    t.add_column()

    if phase == "building":
        t.add_row(Text(_spin(elapsed), style="bold yellow"), Text("Building firmware..."), Text(_s(elapsed), style="dim"))
    elif phase == "done":
        t.add_row(Text("✓", style="bold green"), Text("Build complete"), Text(_s(elapsed), style="dim green"))
    else:
        t.add_row(Text("✗", style="bold red"), Text(f"Build failed  {error}", style="red"), Text(_s(elapsed), style="dim"))

    subtitle = f"[dim]{firmware_dir}  •  {TEMPORAL_UI}[/dim]"
    return Panel(t, title=f"[bold]Temporal Badge[/bold]  [dim]{env}[/dim]",
                 subtitle=subtitle, border_style="bright_black", padding=(1, 2))


def _detect_panel(phase: str, elapsed: float, env: str, expected_count: int,
                  devices: list | None = None, error: str = "") -> Panel:
    devices = devices or []
    t = Table.grid(padding=(0, 2))
    t.add_column(width=3)
    t.add_column(min_width=20)
    t.add_column(min_width=28)
    t.add_column()

    expected = f"expected {expected_count}" if expected_count else "expected any"
    if phase == "detecting":
        t.add_row(
            Text(_spin(elapsed), style="bold yellow"),
            Text("Counting badges..."),
            Text(expected, style="dim"),
            Text(_s(elapsed), style="dim"),
        )
    elif phase == "done":
        t.add_row(
            Text("✓", style="bold green"),
            Text(f"Detected {len(devices)} badge(s)"),
            Text(expected, style="dim green"),
            Text(_s(elapsed), style="dim green"),
        )
    else:
        t.add_row(
            Text("✗", style="bold red"),
            Text(error or "Badge count failed", style="red"),
            Text(expected, style="dim"),
            Text(_s(elapsed), style="dim"),
        )

    if devices:
        t.add_row(Text(""), Text(""), Text(""), Text(""))
        for i, device in enumerate(devices):
            identifier = (
                device.get("badge_uid")
                or device.get("serial")
                or device.get("usb_id")
                or device.get("description")
                or ""
            )
            t.add_row(
                Text("•", style="dim"),
                Text(f"Badge {i + 1:02d}"),
                Text(device.get("port", ""), style="dim"),
                Text(identifier, style="dim"),
            )

    return Panel(t, title=f"[bold]Temporal Badge[/bold]  [dim]{env} preflight[/dim]",
                 subtitle=f"[dim]{TEMPORAL_UI}[/dim]",
                 border_style="bright_black", padding=(1, 2))


def _flash_panel(build_elapsed: float, env: str, firmware_dir: str,
                 phase: str, badges: dict, flash_elapsed: float, error: str = "") -> Panel:
    t = Table.grid(padding=(0, 2))
    t.add_column(width=3)
    t.add_column(min_width=10)
    t.add_column(min_width=28)
    t.add_column()

    # Build summary row
    t.add_row(Text("✓", style="bold green"), Text("Build"), Text(""), Text(_s(build_elapsed), style="dim green"))
    t.add_row(Text(""), Text(""), Text(""), Text(""))  # spacer

    if phase == "preparing":
        t.add_row(Text(_spin(flash_elapsed), style="bold yellow"), Text("Preparing flash images..."), Text(""), Text(_s(flash_elapsed), style="dim"))
    elif phase == "detecting":
        t.add_row(Text(_spin(flash_elapsed), style="bold yellow"), Text("Detecting badges..."), Text(""), Text(_s(flash_elapsed), style="dim"))
    elif not badges:
        t.add_row(Text("✗", style="bold red"), Text(error or "No badges found", style="red"), Text(""), Text(""))
    elif isinstance(badges, list):
        t.add_row(Text(""), Text(f"Detected {len(badges)}"), Text(""), Text(""))
        for i, child in enumerate(badges):
            label = child.get("label") or f"Badge {i + 1:02d}"
            port = child.get("current_port") or child.get("initial_port") or ""
            status = child.get("phase") or "running"
            t.add_row(*_badge_row(label, port, status, flash_elapsed, i))
    else:
        for i, port in enumerate(sorted(badges)):
            label = f"Badge {chr(65 + i)}"
            t.add_row(*_badge_row(label, port, badges[port], flash_elapsed, i))

    subtitle = f"[dim]{firmware_dir}  •  {TEMPORAL_UI}[/dim]"
    return Panel(t, title=f"[bold]Temporal Badge[/bold]  [dim]{env}[/dim]",
                 subtitle=subtitle, border_style="bright_black", padding=(1, 2))


# ── Workflow runner ───────────────────────────────────────────────────────────

async def _run_build(client: Client, env: str, firmware_dir: str, ts: str) -> tuple[dict, float]:
    with Live(console=console, refresh_per_second=12) as live:
        live.update(_build_panel("building", 0, env, firmware_dir))
        start = time.monotonic()

        handle = await client.start_workflow(
            BuildFirmwareWorkflow.run,
            args=[env, firmware_dir],
            id=f"build-{env}-{ts}",
            task_queue=TASK_QUEUE,
            memo={
                "ignition_kind": "build",
                "env": env,
                "firmware_dir": firmware_dir,
            },
            static_summary=f"Ignition build: {env}",
            static_details=f"Build firmware for `{env}` from `{firmware_dir}`.",
        )

        while True:
            desc = await handle.describe()
            if desc.status != WorkflowExecutionStatus.RUNNING:
                break
            live.update(_build_panel("building", time.monotonic() - start, env, firmware_dir))
            await asyncio.sleep(POLL_INTERVAL)

        elapsed = time.monotonic() - start
        result = await handle.result()

    return result, elapsed


async def _run_count_preflight(
    client: Client,
    env: str,
    ts: str,
    expected_count: int,
) -> tuple[dict, float]:
    with Live(console=console, refresh_per_second=12) as live:
        live.update(_detect_panel("detecting", 0, env, expected_count))
        start = time.monotonic()

        handle = await client.start_workflow(
            DetectBadgesWorkflow.run,
            args=[env, expected_count],
            id=f"detect-{env}-{ts}",
            task_queue=TASK_QUEUE,
            memo={
                "ignition_kind": "preflight_count",
                "env": env,
                "expected_count": expected_count,
            },
            static_summary=f"Ignition preflight count: {env}",
            static_details=(
                f"Detect connected badges before building or flashing "
                f"`{env}` (expected_count={expected_count or 'any'})."
            ),
        )

        while True:
            desc = await handle.describe()
            if desc.status != WorkflowExecutionStatus.RUNNING:
                break
            try:
                status = await handle.query(DetectBadgesWorkflow.status)
            except Exception:
                status = {"phase": "detecting", "devices": []}
            live.update(_detect_panel(
                status.get("phase", "detecting"),
                time.monotonic() - start,
                env,
                expected_count,
                status.get("devices", []),
            ))
            await asyncio.sleep(POLL_INTERVAL)

        elapsed = time.monotonic() - start
        result = await handle.result()

    return result, elapsed


async def _run_flash(
    client: Client,
    env: str,
    firmware_dir: str,
    build_elapsed: float,
    ts: str,
    rebuild_filesystem: bool,
    expected_count: int,
    factory_image: str,
) -> tuple[dict, float]:
    with Live(console=console, refresh_per_second=12) as live:
        start = time.monotonic()

        handle = await client.start_workflow(
            FlashBadgesWorkflow.run,
            args=[env, firmware_dir, rebuild_filesystem, expected_count, factory_image],
            id=f"flash-{env}-{ts}",
            task_queue=TASK_QUEUE,
            memo={
                "ignition_kind": "flash",
                "env": env,
                "firmware_dir": firmware_dir,
                "rebuild_filesystem": rebuild_filesystem,
                "expected_count": expected_count,
                "factory_image": factory_image,
            },
            static_summary=f"Ignition flash: {env}",
            static_details=(
                f"Flash `{env}` from `{firmware_dir}` "
                f"(rebuild_filesystem={rebuild_filesystem}, "
                f"expected_count={expected_count or 'any'}, "
                f"factory_image={factory_image or 'none'})."
            ),
        )

        while True:
            desc = await handle.describe()
            if desc.status != WorkflowExecutionStatus.RUNNING:
                break
            try:
                status = await handle.query(FlashBadgesWorkflow.status)
                status = await _query_child_statuses(client, status)
            except Exception:
                status = {"phase": "flashing", "badges": {}}
            live.update(_flash_panel(build_elapsed, env, firmware_dir,
                                     status.get("phase", "flashing"),
                                     status.get("children") or status.get("badges", {}),
                                     time.monotonic() - start))
            await asyncio.sleep(POLL_INTERVAL)

        elapsed = time.monotonic() - start
        result = await handle.result()

    return result, elapsed


# ── Main ──────────────────────────────────────────────────────────────────────

async def run(
    env: str,
    firmware_dir: str,
    do_build: bool,
    build_only: bool,
    rebuild_filesystem: bool,
    skip_ready_prompt: bool,
    expected_count: int,
    factory_image: str,
) -> int:
    try:
        client = await Client.connect("localhost:7233")
    except Exception:
        console.print()
        console.print("  [bold red]Cannot connect to Temporal at localhost:7233[/bold red]")
        console.print("  Run [bold]./start.sh[/bold] to start everything automatically.")
        console.print()
        return 1

    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    build_elapsed = 0.0

    # ── Count preflight ──────────────────────────────────────────────────────
    if expected_count and not build_only:
        count_result, count_elapsed = await _run_count_preflight(
            client, env, ts, expected_count
        )
        if not count_result["success"]:
            console.print(_detect_panel(
                "failed",
                count_elapsed,
                env,
                expected_count,
                count_result.get("devices", []),
                count_result.get("error", ""),
            ))
            console.print()
            console.print(f"  Preflight log: {TEMPORAL_UI}/namespaces/default/workflows")
            console.print()
            return 1

        console.print(_detect_panel(
            "done",
            count_elapsed,
            env,
            expected_count,
            count_result.get("devices", []),
        ))

    # ── Build ─────────────────────────────────────────────────────────────────
    if do_build:
        build_result, build_elapsed = await _run_build(client, env, firmware_dir, ts)

        if not build_result["success"]:
            console.print(_build_panel("failed", build_elapsed, env, firmware_dir, build_result.get("error", "")))
            console.print()
            console.print(f"  Full build log: {TEMPORAL_UI}/namespaces/default/workflows")
            console.print()
            return 1

        console.print(_build_panel("done", build_result["duration_s"], env, firmware_dir))

    if build_only:
        console.print()
        return 0

    # ── Pre-flash prompt ─────────────────────────────────────────────────────
    console.print()
    console.print("  Badges will be reset into bootloader automatically.")
    if factory_image:
        console.print("  Connected badges will receive the selected factory image.")
        console.print("  This wipes existing on-badge files.")
    elif rebuild_filesystem:
        console.print("  Connected badges will receive firmware and a fresh filesystem image.")
    else:
        console.print("  Connected badges will receive firmware and the prepared filesystem image.")
    console.print()

    if skip_ready_prompt:
        console.print("  [dim]Skipping readiness prompt.[/dim]")
    else:
        try:
            input("  Press Enter when the current hub batch is connected and ready... ")
        except (EOFError, KeyboardInterrupt):
            console.print()
            return 0
    console.print()

    # ── Flash ─────────────────────────────────────────────────────────────────
    flash_result, flash_elapsed = await _run_flash(
        client, env, firmware_dir, build_elapsed, ts, rebuild_filesystem,
        expected_count, factory_image
    )

    results = flash_result.get("results", [])
    n_ok = sum(1 for r in results if r["success"])
    n_total = len(results)

    # Final state panel
    console.print(_flash_panel(
        build_elapsed, env, firmware_dir,
        flash_result.get("phase") or ("done" if flash_result["success"] else "partial"),
        [
            {
                "label": r.get("label") or f"Badge {i + 1:02d}",
                "current_port": r.get("port", ""),
                "phase": "done" if r["success"] else "failed",
            }
            for i, r in enumerate(results)
        ],
        flash_elapsed,
        flash_result.get("error", "") if not flash_result["success"] else "",
    ))

    # Operator summary table — one row per badge
    console.print()
    tbl = Table.grid(padding=(0, 3))
    tbl.add_column(width=3)    # pass/fail icon
    tbl.add_column(min_width=8)  # label
    tbl.add_column(min_width=28)  # port
    tbl.add_column(min_width=14)  # serial / usb-id
    tbl.add_column()             # note / error

    for i, r in enumerate(results):
        d = r.get("device", {}) or {}
        serial_num = d.get("serial", "")
        usb_id = d.get("usb_id", "")
        badge_uid = d.get("badge_uid", "")
        port = r["port"]
        label = r.get("label") or f"Badge {i + 1:02d}"
        identifier = badge_uid or serial_num or usb_id or port
        note = "" if r["success"] else r.get("error", "failed")
        if r["success"]:
            tbl.add_row(
                Text("✓", style="bold green"),
                Text(label, style="bold"),
                Text(port, style="dim"),
                Text(identifier, style="dim"),
                Text("PASS", style="bold green"),
            )
        else:
            tbl.add_row(
                Text("✗", style="bold red"),
                Text(label, style="bold"),
                Text(port, style="dim"),
                Text(identifier, style="dim"),
                Text(note, style="red"),
            )

    console.print(tbl)
    console.print()

    if flash_result["success"]:
        console.print(f"  [bold green]✓  {n_ok}/{n_total} badges passed[/bold green]  ({_s(flash_elapsed)})")
    else:
        console.print(f"  [bold red]✗  {n_ok}/{n_total} badges passed[/bold red]  ({_s(flash_elapsed)})")

    console.print()
    console.print(f"  All logs: {TEMPORAL_UI}/namespaces/default/workflows")
    console.print()

    return 0 if flash_result["success"] else 1


def main() -> None:
    default_fw = str(Path(__file__).resolve().parent.parent / "firmware")

    parser = argparse.ArgumentParser(description="Build and flash Temporal Badge firmware")
    parser.add_argument("-e", "--env", default=ONLY_ENV,
                        help=f"PlatformIO environment (only supported: {ONLY_ENV})")
    parser.add_argument("--firmware-dir", default=default_fw, metavar="DIR",
                        help=f"Path to PlatformIO project (default: {default_fw})")
    build_mode = parser.add_mutually_exclusive_group()
    build_mode.add_argument("--build-and-flash", action="store_true",
                            help="Explicit full path: build firmware, rebuild filesystem defaults, then flash both")
    build_mode.add_argument("--no-build", action="store_true",
                            help="Skip firmware build, flash with last compiled binary")
    build_mode.add_argument("--build-only", action="store_true",
                            help="Build firmware only, do not flash")
    parser.add_argument("--factory-image", default="", metavar="PATH",
                        help="Flash a downloaded 16 MB factory image instead of building from source")
    parser.add_argument("-y", "--yes", "--skip-ready-prompt", dest="skip_ready_prompt",
                        action="store_true",
                        help="Skip the pre-flash Enter prompt for the current connected batch")
    parser.add_argument("--expected-count", type=int, default=0, metavar="N",
                        help="Fail before building/flashing unless exactly N badges are detected")
    args = parser.parse_args()

    if args.expected_count < 0:
        parser.error("--expected-count must be 0 or greater")
    if args.env != ONLY_ENV:
        parser.error(f"Ignition only builds/flashes {ONLY_ENV}.")

    do_build = not args.no_build
    build_only = args.build_only
    factory_image = str(Path(args.factory_image).expanduser()) if args.factory_image else ""
    if factory_image and (do_build or build_only):
        parser.error("--factory-image must be used with --no-build")
    # A build followed by a flash should refresh the filesystem image from
    # initial_filesystem/ so settings.txt and apps start from known defaults.
    rebuild_filesystem = do_build and not build_only

    sys.exit(asyncio.run(run(
        env=args.env,
        firmware_dir=args.firmware_dir,
        do_build=do_build,
        build_only=build_only,
        rebuild_filesystem=rebuild_filesystem,
        skip_ready_prompt=args.skip_ready_prompt,
        expected_count=args.expected_count,
        factory_image=factory_image,
    )))


if __name__ == "__main__":
    main()
