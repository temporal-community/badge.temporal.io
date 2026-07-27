# Release assets

Public firmware releases upload two firmware artifacts plus the generated
Community Apps catalog:

```text
firmware.bin
replay2026-factory-16MB.bin
community_apps.json
```

These files are built by
[`release-firmware.yml`](../.github/workflows/release-firmware.yml) from the
`replay2026` PlatformIO environment.

`firmware.bin` is the OTA application image. The badge checks GitHub Releases
for this asset when firmware OTA is enabled.

`replay2026-factory-16MB.bin` is a complete 16 MB factory image. It includes
the bootloader, partition table, application firmware, FAT filesystem image, and
`firmware/initial_filesystem/doom1.wad`.

Recommended Ignition flash command:

```sh
cd ignition
./start.sh --latest-release
```

Ignition downloads the latest GitHub Release factory image, caches it under
`~/.cache/replay2026-badge/releases/`, and flashes it through the normal
Temporal workflow. To pin a release, use `./start.sh --release-tag v1.0.0`.
To flash a manually downloaded image, use
`./start.sh --no-build --factory-image ~/Downloads/replay2026-factory-16MB.bin`.

## Local checks

```sh
cd firmware
pio run -e replay2026
pio run -e replay2026 -t buildfs
./make_factory.sh replay2026 --no-build
```

The GitHub release workflow performs the same build and uploads `firmware.bin`,
`replay2026-factory-16MB.bin`, and generated `community_apps.json` to the
release.

## Publishing a release

`firmware/VERSION` is the source of truth for the public firmware version.
To publish a release, bump that file to a stable semantic version such as
`1.2.3` in a pull request. When the version bump lands on `main`, the release
workflow derives the matching `v1.2.3` tag, builds and verifies every artifact,
then creates the tag and GitHub Release at that exact merge commit.

Pushing a matching `v*` tag remains available as a recovery or backfill path:

```sh
git tag v$(tr -d '[:space:]' < firmware/VERSION)
git push origin v$(tr -d '[:space:]' < firmware/VERSION)
```

Publishing a GitHub Release also runs the workflow. A manual workflow run with
the tag field left blank derives `v<firmware/VERSION>` and can bootstrap a
version that was bumped before this automation existed; supplying a tag can
rebuild an existing release. The workflow verifies the version and tag, refuses
to move a conflicting existing tag during automatic publishing, builds
`firmware.bin`, builds `replay2026-factory-16MB.bin`, generates
`community_apps.json`, creates or updates the GitHub Release, and uploads all
release assets.

Release builds may skip when firmware, embedded data, Community Apps,
release-assets docs, and the release workflow have not changed since the
previous release tag. Use the manual `force_rebuild` option when an existing
release needs fresh assets.
