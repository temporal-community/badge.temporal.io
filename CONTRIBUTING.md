# Contributing

Thanks for helping improve the Temporal Replay 2026 Badge. Pull requests are
welcome for docs, firmware, Ignition, badge apps, public data, registry
metadata, release automation, and hardware documentation.

## Before You Open A PR

- Keep the change focused.
- Explain what changed and why.
- List the commands or manual checks you ran.
- Link related README or docs pages when you add or change a workflow.
- Do not commit credentials, local WiFi settings, virtualenvs, PlatformIO build
  output, caches, logs, or private event operations context.
- Keep public docs current-only; remove stale TODOs or historical notes instead
  of publishing them as reference material.

## Change Checks

| Change type | Recommended checks |
|---|---|
| Docs site | Open `docs/index.html` locally and verify changed links. |
| Root or project README | Check links from the repo root and confirm the getting-started path is still clear. |
| Firmware | Run `pio run -e replay2026` from `firmware/`; flash hardware when the change touches boot, storage, input, display, IR, WiFi, OTA, or MicroPython behavior. |
| Ignition | Run `./doctor.sh` and the relevant `./start.sh` mode from `ignition/`; run Python tests when changing `flash_worker/` or `flash.py`. |
| Badge filesystem apps/docs | Update files under `firmware/initial_filesystem/`, then run `python3 firmware/scripts/generate_startup_files.py` from the repo root and commit generated registry changes when they change. |
| Public data bundle | Edit `data/in/`, run `python3 data/build-data.py`, and commit the generated `data/out/` files. |
| Community Apps registry | Prefer updating `firmware/initial_filesystem/` sources and regenerating. Hand-edit `registry/registry.json` only for legacy single-file entries. |
| Hardware docs/files | Update `hardware/README.md` or nearby README files when adding or replacing fabrication, CAD, artwork, or render assets. |
| Release workflow | Confirm `.github/workflows/release-firmware.yml`, `release-assets/README.md`, and firmware artifact names stay aligned. |

## Generated Files

Commit generated files only when they are part of the public source of truth:

- Commit `data/out/bundle.bin` and generated `data/out/*.json`.
- Commit `registry/community_apps.json` when app or filesystem sources change.
- Do not commit `firmware/data/`; it is a local build mirror generated from
  `firmware/initial_filesystem/`.
- Do not commit `firmware/.pio/`, `ignition/.venv/`, local logs, local WiFi
  files, or release binaries built on your machine.

## Pull Request Notes

A good PR description answers:

- What changed?
- Who does it help?
- What did you test?
- Does it affect flashing, OTA, the badge filesystem, public data, or hardware
  production files?

Use GitHub issues or PR discussion for questions, bug reports, and proposed
badge apps.
