# Agent Notes

This is the public Replay 2026 Badge repository.

## Public Repo Shape

- `docs/` contains the static documentation source. Vercel runs the docs build
  script and serves generated `docs-dist/` through the root `vercel.json`
  config.
- `firmware/` is the PlatformIO firmware project.
- `data/` contains the public schedule/speaker bundle consumed by firmware;
  keep `data/out/bundle.bin` available unless the firmware embed path changes.
- `ignition/` is the Temporal-powered flashing tool for one badge or many
  badges.
- `registry/` and `release-assets/` hold public release and app metadata.
- `hardware/` contains public KiCad source, fabrication outputs, mechanical
  references, and artwork/renders for the Replay 2026 Badge.
- Backend services and private operations tooling are intentionally out of
  scope for this repository.

## Public Defaults

- The public PlatformIO environment is `replay2026`.
- Ignition is the default flashing path.
- Direct PlatformIO commands are a secondary developer path.
- The public release is developer-friendly: it includes the badge UI, bundled
  apps, and development affordances needed for community app work.
- Public release downloads should describe `firmware.bin` for OTA and the full
  factory image `replay2026-factory-16MB.bin`.
- Full factory flashing wipes existing on-badge data.
- Firmware OTA checks GitHub Releases on
  `temporal-community/badge.temporal.io`.

## Documentation Rules

- Keep public docs focused on Replay 2026 Badge users and contributors.
- Keep docs site files in `docs/`; do not move the static HTML/CSS back to
  repo root.
- Do not add private machine paths, credentials, internal admin notes, or
  backend implementation details.
- Document WiFi provisioning with placeholders or ignored local files; never
  commit real SSIDs/passwords.
- Link [`JumperIDE`](https://ide.jumperless.org/) references.
- Avoid exposing old prototype target names as user-facing concepts.
- Keep hardware docs aligned with `hardware/README.md`; CAD and fabrication
  files are now part of the public repository.
- Mention that the bundled Doom WAD and any Doom engine code have separate
  terms from the MIT-licensed repository content.
- Keep public docs current-only. Do not keep migration notes, stale TODOs, or
  historical implementation writeups unless they describe behavior that still
  exists and are clearly useful to contributors.

## Hygiene

- Use the root `.gitignore` for local build output, caches, editor state, and
  sensitive local configuration.
- Do not commit generated PlatformIO output, Python virtualenvs, local logs, or
  machine-specific settings.
- Follow [`CONTRIBUTING.md`](CONTRIBUTING.md) for change-type checks before
  opening or updating a pull request.
