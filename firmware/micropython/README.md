# Temporal Badge MicroPython (ESP32-S3)

This directory bootstraps an upstream-style MicroPython ESP32 port for the Temporal badge hardware.

## Why this structure

- Uses MicroPython's native `ports/esp32` build system (best-supported path).
- Defines a custom board (`TEMPORAL_BADGE_S3`) with pin aliases and board config.
- Allocates a FAT partition on flash for user scripts/data.
- Adds a `USER_C_MODULES` C extension (`temporalbadge`) so hardware control can be exposed safely to Python.

## 1) MicroPython source

The public repo does not require a checked-in MicroPython submodule. The
maintained PlatformIO/Arduino embed flow fetches the upstream sources it needs
from MicroPython `v1.27.0` and vendors generated embed files under
`firmware/lib/micropython_embed/`.

For normal setup, use Ignition:

```sh
cd ignition
./setup.sh
```

That prepares Python dependencies and fetches the MicroPython files needed by
the firmware build. To refresh the vendored upstream source files directly:

```sh
python3 firmware/scripts/fetch_micropython_sources.py --tag v1.27.0
```

If you maintain a standalone MicroPython checkout, pass
`--mp-dir /path/to/micropython` to `setup_arduino_embed.sh`; otherwise the
script shallow-clones the pinned upstream tag into the ignored
`firmware/micropython/micropython_repo/` work directory as needed.

## 2) Arduino-native embed flow

Replay firmware uses PlatformIO + Arduino. This is the maintained public build
path and does not require a separate ESP-IDF shell:

```sh
cd firmware/micropython
./setup_arduino_embed.sh
cd ..
pio run -e replay2026
```

This generates a local PlatformIO library at `lib/micropython_embed`.
`src/micropython/MicroPythonBridge.cpp` uses it to provide:

- `mpy_start(Stream*)`
- `mpy_poll(void)`
- `mpy_gui_exec_file(const char*)`

If the embed library is not generated yet, the bridge safely returns disabled.

### REPL compatibility (JumperIDE/mpremote)

- Friendly REPL is available on serial (`>>>`).
- Raw REPL control chars are supported on the same stream:
  - `Ctrl-A` enter raw REPL (`raw REPL; CTRL-B to exit` + `>` prompt)
  - `Ctrl-B` exit raw REPL
  - `Ctrl-C` cancel pending raw input
  - `Ctrl-D` execute buffered raw script with `OK...<0x04><0x04>` framing

### Filesystem/import/open/os support

- MicroPython mounts the badge's `ffat` data partition through VFS FatFS during
  `mpy_start`.
- Standard MicroPython `open()` and `os` APIs use that mounted FatFS volume.
- `sys.path` is extended with `'/lib'`, `'/matrixApps'`, and `'/'`.
- Startup files from `firmware/initial_filesystem/` are generated into
  `src/micropython/StartupFilesData.h` and provisioned onto FatFS at firmware
  boot.

Quick test on serial REPL:

```python
import sys
print(sys.path)
import myapp   # from /apps/myapp.py on FatFS
with open("/apps/test.txt", "w") as f:
    f.write("ok\n")
import os
print(os.listdir("/apps"))
print(os.statvfs("/apps"))
print(os.path.exists("/apps/test.txt"))
```

## 3) Verify module on REPL

```python
import badge
badge.init()
badge.button(badge.BTN_RIGHT)
badge.oled_println("Temporal Badge")
badge.led_brightness(16)
```

## Arduino main bridge symbols

`src/main.cpp` now calls the Arduino-embed bridge symbols:

- `mpy_start(Stream* stream)`
- `mpy_poll(void)`
- `mpy_gui_exec_file(const char* path)` for native menu launchers

Provide these from the ESP32 MicroPython integration layer to enable REPL
startup and foreground app launching.

## Community Apps

Installable community apps live in `../../community_apps/`. They
are not part of the factory filesystem, but `scripts/generate_startup_files.py`
generates the release-hosted `community_apps.json` catalog so badges can
install them over WiFi.

Current community app submissions:

| App | Contributor | Description |
|---|---|---|
| Tardigotchi | aask42 | Hatch and care for a tiny tardigrade. |
| Durable Snake | Alexandre Roman | Snake game with three retries. |
| Starfield Nametag | Alexandre Roman | Animated starfield with a personalized nametag. |

## Notes on FAT and "mountable on computer"

- This board config uses a FAT data partition (`vfs.VfsFat`) on internal flash.
- That gives MicroPython-side FAT semantics and user script storage.
- Presenting the same FAT volume as USB MSC to a host computer is not enabled by default in upstream MicroPython ESP32 and requires additional USB MSC integration plus strict exclusive-access policy to avoid corruption.
- For now, use REPL + `mpremote`/WebREPL for file transfer, then add MSC in a controlled follow-up.
