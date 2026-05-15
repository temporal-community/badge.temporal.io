# Temporal Replay 2026 Badge Firmware

Firmware for the Temporal Replay 2026 Badge. The public `replay2026` build is
PlatformIO Arduino firmware for the ESP32-S3 badge hardware, with native C++
screens, badge-to-badge IR flows, Doom, embedded MicroPython apps, and
developer-friendly app-building affordances.

![Badge synth screen](../docs/assets/screenshots/badge-synth-live.png)

If you only want to flash a badge, start with
[`../ignition/README.md`](../ignition/README.md). If you want to change the
firmware, start here, then use [`src/README.md`](src/README.md) for the C++
source map,
[`initial_filesystem/docs/README.md`](initial_filesystem/docs/README.md) for
badge-visible MicroPython app docs, and
[`docs/STORAGE-MODEL.md`](docs/STORAGE-MODEL.md) for the NVS / FATFS / app0
mental model and every supported flashing workflow.

Related docs:

- [`../ignition/README.md`](../ignition/README.md): default build and flashing
  tool for one badge or many badges.
- [`../release-assets/README.md`](../release-assets/README.md): OTA and factory
  image artifact names.
- [`../data/README.md`](../data/README.md): public schedule, speaker, and floor
  bundle embedded by the firmware.
- [`../registry/README.md`](../registry/README.md): Community Apps catalog
  fetched by badges over WiFi.

## Filesystem Source Of Truth

- `initial_filesystem/` is canonical, hand-edited, and committed.
- `data/` is a gitignored byte mirror produced by
  `scripts/generate_startup_files.py`. PlatformIO's `pio run -t buildfs` and
  `pio run -t uploadfs` read from here. Do not edit `data/` by hand; changes
  are overwritten on the next build.

The generator runs as a `pre:` script on every PlatformIO build, so you
normally do not need to invoke it explicitly. Manual run:

```bash
python3 scripts/generate_startup_files.py
```

## Common Commands

```bash
# Build public firmware.
~/.platformio/penv/bin/pio run -e replay2026

# Flash firmware only.
~/.platformio/penv/bin/pio run -e replay2026 -t upload

# Flash filesystem only: apps, docs, and doom1.wad.
~/.platformio/penv/bin/pio run -e replay2026 -t uploadfs

# Build a complete 16 MB factory image.
./make_factory.sh replay2026

```

Ignition wraps the build and flash path with device detection, verification,
and retries. Use direct PlatformIO commands when you are already working inside
the firmware project and need lower-level control.

## OLED Screenshots

The firmware exposes a dev framebuffer dump through `badge.dev("fb")`. The
helper script wraps that raw REPL call and writes a scaled PNG with the same
black background and white pixels as the physical OLED.

```bash
# List connected badge serial ports.
~/.platformio/penv/bin/python scripts/capture_oled_fb.py --list-ports

# Capture the current badge screen.
~/.platformio/penv/bin/python scripts/capture_oled_fb.py \
  --out ../docs/assets/screenshots/my-screen.png

# If more than one badge is connected, choose the port explicitly.
~/.platformio/penv/bin/python scripts/capture_oled_fb.py \
  --port /dev/cu.usbmodemXXX \
  --out ../docs/assets/screenshots/my-screen.png
```

Tips:

- The default output is `../docs/assets/screenshots/badge-screenshot.png`.
- Pass `--scale 4` or `--scale 6` to control PNG size.
- Pass `--screen synth-live` or `--screen synth-sounds` to render built-in
  app screenshot fixtures without manually navigating the badge UI.
- Close JumperIDE, serial monitors, and Ignition before capturing; only one
  process can own the badge serial port at a time.

## WiFi Diagnostics

The badge uses the ESP32-S3 radio, which can join 2.4 GHz WiFi networks only.
If a saved network will not connect, first confirm the SSID is visible to the
badge from the MicroPython REPL:

```python
import network
import time

wlan = network.WLAN(network.STA_IF)
wlan.active(True)
time.sleep(1)

for ssid, bssid, channel, rssi, authmode, hidden in wlan.scan():
    name = ssid.decode("utf-8", "ignore")
    print(name, "channel", channel, "rssi", rssi, "auth", authmode)

wlan.active(False)
```

The scan output lists visible access points, channels, RSSI, and auth mode, but
never prints passwords. If the target SSID is missing while nearby 2.4 GHz
networks appear, the network is probably 5 GHz only, hidden, out of range, or
blocked by venue configuration.
