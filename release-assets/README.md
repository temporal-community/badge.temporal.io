# Release assets

Public firmware releases upload two firmware artifacts:

```text
firmware.bin
replay2026-factory-16MB.bin
```

These files are built by
[`release-firmware.yml`](../.github/workflows/release-firmware.yml) from the
`replay2026` PlatformIO environment.

`firmware.bin` is the OTA application image. The badge checks GitHub Releases
for this asset when firmware OTA is enabled.

`replay2026-factory-16MB.bin` is a complete 16 MB factory image. It includes
the bootloader, partition table, application firmware, FAT filesystem image, and
`firmware/initial_filesystem/doom1.wad`.

## Local checks

```sh
cd firmware
pio run -e replay2026
pio run -e replay2026 -t buildfs
./make_factory.sh replay2026 --no-build --no-fs
```

The GitHub release workflow performs the same build and uploads
`firmware.bin` plus `replay2026-factory-16MB.bin` to the release.
