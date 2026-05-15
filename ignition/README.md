# Ignition for the Temporal Replay 2026 Badge

Build and flash Temporal Replay 2026 Badge firmware to one connected badge or a
batch of badges. Ignition is the Temporal-powered system we built to bulk flash
the Replay 2026 badge fleet, with the same workflow still useful for a single
badge on a desk.

Uses a local [Temporal](https://temporal.io) server to orchestrate the build
and flash pipeline — no cloud required. Runs are browsable in the Temporal UI
at `http://localhost:8233` while the local dev server is running.

## What it does

1. Builds firmware via PlatformIO (`firmware/` by default)
2. Detects all connected badges over USB
3. Prepares a fresh default filesystem image when building, then starts one child workflow per detected badge
4. Flashes firmware + apps to all badges in parallel with one direct esptool upload per badge and auto-retry
5. Shows per-badge live status (Resolving USB -> Writing firmware + apps -> Verify boot -> Sync clock -> Done), with esptool progress in Temporal activity heartbeat details
6. Includes bounded boot and clock-sync logs in the verification activity results
7. Asks for confirmation before writing to connected badges (`-y` skips the prompt when you are ready)
8. Concurrency cap: up to 32 badges flashing at once

## First-time setup

Run once to install PlatformIO, Temporal CLI, and Python dependencies:

```bash
./setup.sh
./doctor.sh
```

Ignition currently builds and flashes the `replay2026` firmware environment only.

`setup.sh` creates `ignition/.venv` and installs `certifi` so Python HTTPS
requests use a bundled CA bundle when fetching MicroPython sources from GitHub.
If setup fails with certificate or Python import errors, re-run `./setup.sh` and
then `./doctor.sh`; the doctor checks the venv, Python imports, GitHub HTTPS,
PlatformIO, Temporal CLI, and connected USB serial ports.

## Usage

```bash
./start.sh                           # build replay2026 + flash all connected badges
./start.sh --build-and-flash         # explicit build + fresh default filesystem + flash
./start.sh -e replay2026             # explicit replay2026 environment
./start.sh -y                        # skip the pre-flash Enter prompt
./start.sh --no-build                # skip build, flash last compiled binary
./start.sh --build-only              # build only, do not flash
./start.sh --no-build --factory-image ~/Downloads/replay2026-factory-16MB.bin
./start.sh --firmware-dir /path      # target a different PlatformIO project
./start.sh --wifi-ssid NAME --wifi-pass PASS
```

Ignition looks for the PlatformIO project at `../firmware` by default. Developers
who prefer to drive PlatformIO directly can still use the
[`firmware/`](../firmware/README.md) project as a secondary path:

```bash
cd ../firmware
pio run -e replay2026
pio run -e replay2026 -t upload
```

## WiFi provisioning

The firmware can embed one build-time WiFi network. This is useful for event
flashing because the badge can connect after boot and use GitHub-backed OTA and
Community Apps.

Use Ignition CLI flags:

```bash
./start.sh -e replay2026 --wifi-ssid "YourNetwork" --wifi-pass "YourPassword"
```

Or create an ignored local file before building:

```bash
cp ../firmware/wifi.local.env.example ../firmware/wifi.local.env
$EDITOR ../firmware/wifi.local.env
./start.sh -e replay2026
```

Do not commit `wifi.local.env`; it is intentionally ignored.

## Badge upload mode

Badges are reset into bootloader automatically via USB serial (RTS/DTR).
For one badge, connect it before pressing Enter. For event flashing, connect a
powered hub batch before pressing Enter; after that batch finishes, unplug the
hub, connect the next hub, and run Ignition again.

The Enter prompt is the batch boundary. Ignition discovers whatever badges are
currently connected, starts child workflows for that set, and does not reuse
old child workflows for a newly swapped hub.

Ignition builds both firmware and filesystem artifacts. For the storage model
behind firmware-only flashes, filesystem uploads, and factory flashes, see
[`firmware/docs/STORAGE-MODEL.md`](../firmware/docs/STORAGE-MODEL.md). For the
public OTA and factory-image artifact names, see
[`release-assets/README.md`](../release-assets/README.md).

## How it works

```
start.sh
  ├── checks/starts temporal server  (localhost:7233)
  ├── checks/starts flash worker     (flash_worker/worker.py)
  └── runs flash.py
        ├── BuildFirmwareWorkflow    → build_firmware activity
        └── FlashBadgesWorkflow      → prepare_flash_artifacts
                                     → detect_badge_devices
                                     → BadgeFlashWorkflow child per badge
                                           → resolve_badge_port
                                           → flash_badge_images
                                           → verify_badge_boot_marker
                                           → sync_badge_clock
```

The Temporal worker (`flash_worker/`) handles all the actual work. `flash.py`
just starts workflows and polls for live status.

`FlashBadgesWorkflow.status` reports the batch phase, detected count, child
workflow IDs, and pass/fail totals. Each `BadgeFlashWorkflow.status` reports
that badge's stable USB identity, initial/current port, phase, attempts, and
boot log tail. Detailed esptool progress lives in activity heartbeat details.

Set `IGNITION_UPLOAD_BAUD` to override the direct esptool upload baud rate.
The default is `921600`, matching the board definitions.

## Logs

Build output, short flash tails, and bounded boot/clock verification logs are stored
in Temporal workflow history. Esptool progress is also visible while activities
run via heartbeat details. To browse:

```
http://localhost:8233/namespaces/default/workflows
```

Temporal keeps the useful operator history automatically without storing giant
serial/esptool logs in one event payload.

## Files

| File | Purpose |
|---|---|
| `start.sh` | One-command launcher — setup + build + flash |
| `setup.sh` | First-time install of all dependencies |
| `doctor.sh` | Preflight checks for Python, certificates, PlatformIO, Temporal, and USB |
| `flash.py` | CLI — starts Temporal workflows, shows live status |
| `flash_worker/workflows.py` | Temporal workflow definitions |
| `flash_worker/activities.py` | Build and flash implementations |
| `flash_worker/worker.py` | Worker process entry point |
| `flash_worker/requirements.txt` | Python deps (`temporalio`, `rich`) |
