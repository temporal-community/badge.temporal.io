## What kind of PR is this?

Check the best fit:

- [ ] Community App submission or update
- [ ] Bug fix
- [ ] Documentation update
- [ ] Firmware or MicroPython API change
- [ ] Ignition flashing/tooling change
- [ ] Hardware files or hardware docs
- [ ] Release, CI, or registry infrastructure

## What changed?

Briefly describe the change and who it helps.

## Community App details

Fill this out for Community App submissions or updates:

- App name:
- App folder: `community_apps/<slug>/`
- Contributor name:
- Short badge-screen description from `community.json`:
- License:
- Third-party assets or libraries:
- What does it do on the 128x64 OLED, 8x8 LED matrix, buttons, haptics, IR, WiFi, or filesystem?
- How does a user exit back to the badge menu?
- Does it use network access, persistent storage, or large assets? If yes, describe what it does and why.
- Does it use long-running or infinite loops? If yes, describe how it sleeps/yields and honors the exit chord.
- Does it write outside `/apps/<slug>/` or use unusual imports such as `eval`, `exec`, `subprocess`, or `ctypes`? If yes, explain why.
- Known limitations or reviewer questions:

Checklist:

- [ ] App has `main.py`
- [ ] App has `community.json`
- [ ] `community.json` includes `name`, `description`, and `contributors` or `author`
- [ ] Description is short enough for the badge UI, ideally 96 characters or fewer
- [ ] App has a README
- [ ] On-screen prompts use badge button names: `A`, `B`, `X`, `Y`, `Menu`, `Select`
- [ ] App provides a clear way back to the menu
- [ ] App does not permanently take over LEDs, files, WiFi, IR, or other badge resources
- [ ] Large files over 256 KiB, hidden files, and generated files are excluded or explained above
- [ ] Generated `community_apps.json` is not committed

## Testing

Tell us what you ran or tested:

- [ ] Docs links/pages reviewed
- [ ] `python3 firmware/scripts/generate_startup_files.py`
- [ ] Community app Python compile check
- [ ] `pio run -e replay2026`
- [ ] Ignition `./doctor.sh`
- [ ] Ignition flash or build-only path
- [ ] Tested on a real badge
- [ ] Not tested yet

Notes:

## Docs and release impact

- [ ] I updated docs or README files affected by this change
- [ ] No docs changes are needed
- [ ] This affects flashing, OTA, filesystem layout, public data, or release artifacts
- [ ] This does not affect flashing, OTA, filesystem layout, public data, or release artifacts

If this changes firmware, MicroPython APIs, Ignition, Community Apps, registry behavior, or badge app behavior, link the docs update or explain why docs are not needed:

- Docs update or explanation:

## Automated review context

The Community App review workflow checks metadata, README clarity, badge UI conventions, clean exit behavior, MicroPython compatibility, docs impact, network/filesystem behavior, asset size, and licensing. Pre-answer anything here that would help the automated reviewer and human maintainers:

- Why is this safe for badge storage, filesystem, network, and hardware resources?
- What should maintainers manually verify on a real badge?
- Any compatibility notes for the constrained badge MicroPython runtime?

## Safety check

- [ ] No secrets, local WiFi credentials, `.env` files, build output, caches, logs, or private event context are included
