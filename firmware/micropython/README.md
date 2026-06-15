# Temporal Badge MicroPython (ESP32-S3)

This directory bootstraps an upstream-style MicroPython ESP32 port for the Temporal badge hardware.

## Why this structure

- Uses MicroPython's native `ports/esp32` build system (best-supported path).
- Defines a custom board (`TEMPORAL_BADGE_S3`) with pin aliases and board config.
- Allocates a FAT partition on flash for user scripts/data.
- Adds a `USER_C_MODULES` C extension (`temporalbadge`) so hardware control can be exposed safely to Python.

## 1) MicroPython source (git submodule)

The canonical upstream tree for the Arduino embed generator lives at
`firmware/micropython/micropython_repo` as a **git submodule** (pinned
commit; currently targets tag `v1.27.0`).

From the **Temporal-Badge** repo root (not `firmware/` alone):

```bash
git submodule update --init --recursive firmware/micropython/micropython_repo
```

Fresh clones:

```bash
git clone --recurse-submodules https://github.com/<org>/Temporal-Badge.git
# or after a clone without submodules:
git submodule update --init --recursive
```

Then run `./setup_arduino_embed.sh` (see below); it will run `submodule update`
for you if the directory is still empty.

If you maintain a standalone MicroPython tree instead, pass
`--mp-dir /path/to/micropython` to `setup_arduino_embed.sh`.

## 2) Install and export ESP-IDF (v5.2+)

Follow Espressif install steps, then in each shell:

```bash
source /path/to/esp-idf/export.sh
```

## 3) Build cross-compiler

```bash
cd firmware/micropython/micropython_repo   # from Temporal-Badge root
make -C mpy-cross
```

## 4) Build this board with user module

```bash
cd /path/to/micropython/ports/esp32
make BOARD=TEMPORAL_BADGE_S3 BOARD_DIR=/path/to/Replay-Badge/firmware/micropython/boards/TEMPORAL_BADGE_S3 USER_C_MODULES=/path/to/Replay-Badge/firmware/micropython/usermods/temporalbadge/micropython.cmake
```

## Arduino-native embed flow (no ESP-IDF required)

Replay firmware uses PlatformIO + Arduino. To embed MicroPython in this path:

```bash
cd /path/to/Replay-Badge/firmware/micropython
./setup_arduino_embed.sh
cd ..
pio run -e echo
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

## ESP-IDF build script (optional)

You can run the helper script from this directory:

```bash
./build_micropython_esp32.sh build --idf-export /path/to/esp-idf/export.sh
./build_micropython_esp32.sh deploy --idf-export /path/to/esp-idf/export.sh
./build_micropython_esp32.sh monitor --idf-export /path/to/esp-idf/export.sh
```

By default it uses:

- MicroPython repo: `firmware/micropython/micropython_repo` (git submodule)
- board: `TEMPORAL_BADGE_S3`
- user module: `usermods/temporalbadge`

## 5) Flash

```bash
make BOARD=TEMPORAL_BADGE_S3 BOARD_DIR=/path/to/Replay-Badge/firmware/micropython/boards/TEMPORAL_BADGE_S3 USER_C_MODULES=/path/to/Replay-Badge/firmware/micropython/usermods/temporalbadge/micropython.cmake deploy
```

## 6) Verify module on REPL

```python
import temporalbadge as tb
tb.init()
tb.button(tb.BTN_RIGHT)
tb.oled_println("Temporal Badge")
tb.led_brightness(16)
```

## Arduino main bridge symbols

`src/main.cpp` now calls the Arduino-embed bridge symbols:

- `mpy_start(Stream* stream)`
- `mpy_poll(void)`
- `mpy_gui_exec_file(const char* path)` for native menu launchers

Provide these from the ESP32 MicroPython integration layer to enable REPL
startup and foreground app launching.

## Notes on FAT and "mountable on computer"

- This board config uses a FAT data partition (`vfs.VfsFat`) on internal flash.
- That gives MicroPython-side FAT semantics and user script storage.
- Presenting the same FAT volume as USB MSC to a host computer is not enabled by default in upstream MicroPython ESP32 and requires additional USB MSC integration plus strict exclusive-access policy to avoid corruption.
- For now, use REPL + `mpremote`/WebREPL for file transfer, then add MSC in a controlled follow-up.
