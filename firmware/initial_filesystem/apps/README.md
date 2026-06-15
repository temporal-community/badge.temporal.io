# Badge Apps

Python apps for the Temporal Badge. Files under `initial_filesystem/` are
embedded into the firmware image and provisioned onto the badge's FatFS
`ffat` partition at boot.

In dev-menu builds, each `.py` file in this directory appears in the Apps menu.
A folder with a `main.py` entry point is also treated as an app. Normal firmware
does not expose the generic Apps menu; production apps need an explicit launcher
in the native menu.

## Creating an App

Create a `.py` file in this directory, or create a folder with a `main.py`
entry point. It runs top-to-bottom when selected from the menu. Call `exit()`
when done to return cleanly to the menu.

Prefer the folder form once an app is more than a small demo:

```text
initial_filesystem/apps/my_app/
  main.py
  icon.py
  state.py
  screens.py
```

`main.py` should be tiny. Add the app folder to `sys.path`, then import and run
the real entry point:

```python
import sys

APP_DIR = "/apps/my_app"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from engine import main

from badge_app import run_app

run_app("My App", main)
```

All badge API functions are auto-imported into the entry script's global scope,
so tiny one-file apps do not need `import badge` for hardware access. Modules
that are imported from a folder app should import what they use explicitly,
usually with `from badge import *`. See `../docs/API_REFERENCE.md` for the full
function list, and `../docs/MicroPythonDeveloperGuide.md` for the longer app
authoring guide.

Minimal example:

```python
import time

oled_clear()
oled_set_cursor(32, 28)
oled_print("Hello!")
oled_show()

time.sleep_ms(2000)
exit()
```

For app screens that should match the built-in firmware chrome, import
`badge_ui` and use its header/footer helpers. These call the native C++ UI
layout and button glyph code when the firmware provides it:

```python
import badge_ui as ui

ui.chrome("My App", "Score 0", "OK", "start", "BACK", "quit")
ui.line(0, "Menu item")
oled_show()
```

Use this helper before copying UI code into Python. It keeps MicroPython apps
aligned with the C++ menu chrome and avoids duplicating button glyph bitmaps.
The most common helpers are:

```python
ui.chrome("Title", "Right", "OK", "select", "BACK", "quit")
ui.chrome_tall("Title", "Right", (("X", "tool"), ("Y", "mode")), "BACK", "quit", "OK", "go")
ui.header("Title", "Score 120")
ui.action_bar("OK", "again", "BACK", "quit")
ui.center(28, "Centered text")
ui.line(0, "List row")
ui.selected_row(1, "Selected row")
ui.inline_hint(0, 53, "OK:start")
ui.hint_row((("X", "next"), ("Y", "mode"), ("BACK", "quit")), 43)
```

For apps that own both the OLED and LED matrix, import `badge_app` before
duplicating lifecycle and timing code. The shared helpers cover native-looking
crash screens, local high-score files, LED override cleanup, frame timers,
joystick thresholds, and button latches:

```python
from badge_app import ButtonLatch, DualScreenSession, GCTicker, read_stick_4way, with_led_override

def play_once():
    game = new_game()
    session = DualScreenSession(FRAME_MS)
    ok_latch = ButtonLatch(BTN_CONFIRM)

    def loop():
        while True:
            now = session.now()
            x_dir, y_dir = read_stick_4way()
            ok_edge = ok_latch.poll()
            if session.frame_due(game, now):
                tick_oled_game(game, ok_latch.consume(), now)
            tick_led_game(game, x_dir, y_dir, ok_edge, now)
            session.sleep()

    return with_led_override(loop)
```

Use `GCTicker()` directly for long-running apps that do not use
`DualScreenSession`. It sets the MicroPython GC allocation threshold when
available and runs a full collection every few seconds when `tick()` is called.

`run_app("My App", main, cleanup)` writes uncaught exceptions to
`/last_mpy_error.txt` and shows a badge-native crash screen instead of dropping
back to a blank or stale display. The optional cleanup callback runs on normal
return, crash, and `exit()`.

Dev firmware also includes a Crash Log app that reads `/last_mpy_error.txt` on
the badge. In normal firmware, use the Files screen or serial tooling to inspect
that file.

## Adding a Production App

Normal firmware should only surface apps that are intended for attendees. To
ship a MicroPython app in the main menu:

1. Put the app in `initial_filesystem/apps/<app>/main.py`.
2. Add an icon in `initial_filesystem/apps/<app>/icon.py` if the app also uses
   the LED matrix, and add the main-menu bitmap to `firmware/src/ui/AppIcons.h`.
3. Add a launcher action in `firmware/src/ui/GUI.cpp` that calls
   `mpy_gui_exec_file("/apps/<app>/main.py")`.
4. Add a `GridMenuItem` for the app in `kBadgeMenuItems`.

Keep one-off diagnostics and API demos in the dev Apps menu instead of adding
them to the normal firmware menu.

## Building and Validating App Changes

Run these from `firmware/` before committing an app change:

```sh
black initial_filesystem/apps/<app> initial_filesystem/lib/badge_app.py initial_filesystem/lib/badge_ui.py
python3 -m py_compile initial_filesystem/apps/<app>/*.py initial_filesystem/lib/badge_app.py initial_filesystem/lib/badge_ui.py
python3 scripts/generate_startup_files.py
pio run -e echo
```

The PlatformIO build also runs `scripts/generate_startup_files.py`, but running
it manually makes the generated diff visible before the build. Commit both
`src/micropython/StartupFilesData.h` and
`scripts/startup_hash_history.json` when app files change. Do not edit
`StartupFilesData.h` by hand.

### Forcing a refresh while iterating on a Python app

By default app files (`/apps/<app>/*.py`) ship via `fatfs.bin` — re-flashing a
~7 MB partition every iteration is too slow for the edit-build-flash loop.
The dev path bakes the app into the firmware binary itself and force-refreshes
it on boot. Two coordinated knobs:

1. **Compile-time `-DBADGE_DEV_FORCE_REFRESH=...`** in
   `firmware/platformio.ini`'s `build_flags_common` block. Comma-separated
   paths/prefixes; trailing `/` or `*` = prefix match.

   The whole flag MUST be wrapped in single quotes so SCons preserves
   the embedded double quotes — same convention as the existing
   `'-DFFCONF_H="ffconf.h"'` line in `platformio.ini`. Without the outer
   quotes the macro expands to bare tokens and the build fails with
   `expected primary-expression before '/' token`.

   ```ini
   '-DBADGE_DEV_FORCE_REFRESH="/apps/ir_remote/,/lib/badge_ui.py"'
   ```

   This single flag does **two** things automatically:

   - `scripts/generate_startup_files.py` reads the flag, treats every
     listed path as an additional bake prefix, and embeds the matching
     files in `StartupFilesData.h` (firmware app0). Look for the
     `[generate_startup_files] DEV BAKE: N extra files, N bytes` line
     in the build log.
   - At boot, `provisionStartupFiles()` overwrites the on-FAT version
     of every matching file with the freshly-baked content, even if
     the user has edited it through JumperIDE.

   Net effect: `pio run -e echo -t upload` (firmware-only, ~10 s) is
   enough to push new app code. **No `uploadfs` needed during iteration.**

2. **Runtime marker `/dev_force_refresh.txt`** — drop the file on the badge's
   FAT (via JumperIDE, `pio run -t uploadfs`, or serial). Same syntax as the
   build flag, one entry per line is fine, `#` starts a comment. The marker
   is auto-deleted after one boot so it's a one-shot. Useful when you don't
   want to rebuild firmware just to refresh one file (note: this only refreshes
   files that are actually in `kStartupFiles[]` — i.e. either under
   `lib/`/`matrixApps/` or covered by the build-time bake flag at last
   firmware build).

Boot log:

```text
[startup] dev force-refresh (build): /apps/ir_remote/,/lib/badge_ui.py
[startup] Dev force-refreshed /apps/ir_remote/main.py
[startup] Dev force-refreshed /apps/ir_remote/ir_lib.py
...
[startup] Provisioned: 0 created, 0 updated, 13 dev-forced, 5 unchanged
```

**REMOVE the build flag for production builds** — leaving it set bloats
app0 and forces overwrites users have made through JumperIDE.

Use `echo-dev` when you need the generic Apps menu or internal diagnostics:

```sh
pio run -e echo-dev
pio run -e echo-dev -t upload --upload-port /dev/cu.usbmodemXXXX
```

Dev firmware also exposes `badge.dev("fb")` for framebuffer captures. To render
a MicroPython app screen from the badge into a PNG, use:

```sh
python3 scripts/capture_oled_fb.py --port /dev/cu.usbmodemXXXX --screen synth-live --out /tmp/synth-live.png
python3 scripts/capture_oled_fb.py --port /dev/cu.usbmodemXXXX --screen synth-sounds --out /tmp/synth-sounds.png
```

Use normal `echo` builds for attendee-facing smoke tests:

```sh
pio run -e echo
pio run -e echo -t upload --upload-port /dev/cu.usbmodemXXXX
```

`black` and `py_compile` are quick host-side checks. They do not replace an
on-device smoke test for input, timing, display ownership, LED override, or
MicroPython heap behavior.

## Deploying

```sh
# Flash firmware + embedded startup files together
pio run -e echo -t upload --upload-port /dev/cu.usbmodemXXXX

# Upload the raw FatFS image from firmware/data/ when you need data files
# such as doom1.wad. This does not replace generated startup files.
pio run -e echo -t uploadfs --upload-port /dev/cu.usbmodemXXXX
```

App source is compiled into `src/micropython/StartupFilesData.h` and written to
the FatFS partition by the firmware's startup provisioning pass. App changes are
not saved in GitHub until the source files and generated startup files are both
committed.

## API Quick Reference

### Display (128×64 SSD1306 OLED)

```python
oled_clear()                      # clear buffer (optional: oled_clear(True) to refresh)
oled_set_cursor(x, y)             # set text cursor position
oled_print(text)                  # print text at cursor (does not refresh)
oled_println(text)                # print text + newline + auto refresh
oled_show()                       # flush buffer to screen
oled_set_text_size(size)          # text size 1-4
oled_set_font(name)               # set font by name
oled_text_width(text)             # pixel width of string
oled_text_height()                # pixel height of current font
oled_set_pixel(x, y, color)       # set pixel (0=black, 1=white)
oled_draw_box(x, y, w, h)         # filled rectangle
oled_set_draw_color(color)        # 0=black, 1=white, 2=XOR
oled_invert(enable)               # invert display
oled_get_framebuffer()            # raw framebuffer as bytes
oled_set_framebuffer(data)        # replace framebuffer
oled_get_framebuffer_size()       # (width, height, bytes)
```

### Buttons & Joystick

```python
button(BTN_CONFIRM)               # True if held down
button_pressed(BTN_CONFIRM)       # True once per press (edge-triggered)
button_held_ms(BTN_CONFIRM)       # ms button has been held
joy_x()                           # joystick X (0-4095)
joy_y()                           # joystick Y (0-4095)
```

Constants: `BTN_RIGHT` (0), `BTN_DOWN` (1), `BTN_LEFT` (2), `BTN_UP` (3).
Aliases: `BTN_CIRCLE`, `BTN_CROSS`, `BTN_SQUARE`, `BTN_TRIANGLE`,
`BTN_CONFIRM`, `BTN_SAVE`, `BTN_BACK`, `BTN_PRESETS`.
Semantic defaults: `BTN_CONFIRM`/`BTN_SAVE` use B/Circle, `BTN_BACK` uses
A/Cross, and `BTN_PRESETS` uses Y/Triangle. Set `swap_ok = 0` for A/Cross
confirm and B/Circle back. The physical and shape constants always refer to
the actual hardware button.

### LED Matrix (8×8 IS31FL3731)

```python
led_brightness(value)             # global brightness 0-255
led_clear()                       # all LEDs off
led_fill()                        # all LEDs on
led_set_pixel(x, y, brightness)   # single LED (0-7, 0-255)
led_get_pixel(x, y)               # read LED brightness
led_show_image(IMG_HEART)         # builtin image
led_set_frame([row0..row7], brt)  # 8 bitmask rows
led_start_animation(ANIM_SPINNER) # builtin animation
led_stop_animation()              # stop animation
led_override_begin()              # pause ambient LED mode
led_override_end()                # restore ambient LED mode
```

Images: `IMG_SMILEY`, `IMG_HEART`, `IMG_ARROW_UP`, `IMG_ARROW_DOWN`, `IMG_X_MARK`, `IMG_DOT`
Animations: `ANIM_SPINNER`, `ANIM_BLINK_SMILEY`, `ANIM_PULSE_HEART`

Use `led_override_begin()` before foreground LED drawing and
`led_override_end()` in cleanup so the saved LED app mode resumes after your app.

### IMU (LIS2DH12 Accelerometer)

```python
imu_ready()                       # True if IMU initialized
imu_tilt_x()                      # X tilt in milli-g
imu_tilt_y()                      # Y tilt in milli-g
imu_accel_z()                     # Z acceleration in milli-g
imu_face_down()                   # True if face-down
imu_motion()                      # consume motion event
```

### Haptics & Tone

```python
haptic_pulse()                    # default vibration pulse
haptic_pulse(strength, ms, hz)    # custom pulse
haptic_strength([value])          # get/set default strength (0-255)
haptic_off()                      # stop motor
tone(freq_hz, [ms], [duty])       # audible coil tone
no_tone()                         # stop tone
tone_playing()                    # True if tone active
```

### IR (NEC Protocol)

```python
ir_send(addr, cmd)                # transmit NEC frame
ir_start()                        # enable IR receive
ir_stop()                         # disable receive, flush queue
ir_available()                    # True if frame waiting
ir_read()                         # (addr, cmd) tuple or None
```

### Mouse Overlay

```python
mouse_overlay(True)               # enable cursor overlay
mouse_x()                         # cursor X (0-127)
mouse_y()                         # cursor Y (0-63)
mouse_set_pos(x, y)               # warp cursor
mouse_clicked()                   # last click button ID (-1 if none)
mouse_set_speed(speed)            # cursor speed 1-20
mouse_set_mode(MOUSE_RELATIVE)    # MOUSE_ABSOLUTE or MOUSE_RELATIVE
mouse_set_bitmap(data, w, h)      # custom cursor sprite (XBM, max 16x16)
```

### Control

```python
exit()                            # clean exit back to menu
```

Use `import gc; gc.collect()` for manual garbage collection.

## Standard Library

Available MicroPython modules:

| Module | Key functions |
|--------|---------------|
| `time` | `sleep_ms()`, `ticks_ms()`, `ticks_diff()`, `ticks_add()` |
| `math` / `cmath` | Standard math (single-precision float) |
| `json` | `dumps()`, `loads()` |
| `gc` | `collect()`, `mem_free()`, `mem_alloc()` |
| `random` | `randint()`, `choice()`, `random()` |
| `struct` | `pack()`, `unpack()` |
| `binascii` | `hexlify()`, `unhexlify()` |
| `sys` | `path`, `version`, `implementation` |
| `os` | `listdir()`, `mkdir()`, `remove()`, `rename()`, `stat()`, `statvfs()`, `uname()` |

## Escape Chord

Users can force-exit a running app by holding all four face buttons for about
1 second.
This raises `SystemExit` and returns to the menu. You don't need to
handle this in your app.

## Limitations

- **2 MB Python heap** (PSRAM) — use `with_led_override`, `DualScreenSession`,
  or `GCTicker` to collect before tight loops and periodically during long
  polling loops.
- **Large files compile slowly** — there is no firmware-enforced source size
  limit, but folder apps with smaller modules are easier to edit and test.
- **Core 1 only** — all Python execution happens on Core 1.

## Tips

- Call `gc.collect()` before entering a polling loop, or use `GCTicker` to keep
  collection cadence predictable in long-running loops.
- Use `exit()` rather than letting your script fall off the end.
- Keep display update loops at ~30-80ms intervals to balance responsiveness
  with CPU usage.

## Example Apps

| File | Demonstrates |
|------|-------------|
| `hello.py` | Display, button polling, timed exit |
| `api_test.py` | Interactive test menu for all badge API functions |
| `input_test.py` | All inputs: buttons, joystick, IMU |
| `mouse_demo.py` | Mouse overlay cursor with absolute/relative modes |
| `synth/` | Joystick synthesizer with loop recorder and loadable sounds |
| `tilt_ball.py` | IMU tilt → LED matrix dot position |
| `font_demo.py` | Cycle through available OLED fonts |
| `ir_test.py` | IR receive — display incoming NEC frames |
| `ir_poll_test.py` | IR receive polling at 50ms for 2 minutes |
| `gc_bench.py` | GC pause measurement benchmark |
| `loop_test.py` | Infinite loop (test escape chord: all four face buttons) |
| `crash_test.py` | Unhandled exception (test error display) |
| `oom_test.py` | Out-of-memory behavior |
| `import_block_test.py` | Verify blocked module imports |
| `syntax_error_test.py` | Syntax error handling |
| `testImports.py` | Module import verification |
| `http_test.py` | Explicit MicroPython HTTP GET smoke test against a public API |
