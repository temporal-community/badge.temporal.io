# Temporal Badge — MicroPython Developer Guide

Write apps for your conference badge using Python. This guide covers
everything from connecting JumperIDE to building multi-file games with
LED matrix animations and badge-to-badge IR communication.

For the exhaustive function-by-function reference, see the
[API Reference](API_REFERENCE.md).

---

## 1. What Is This Badge?

The Temporal Badge is a wearable conference badge built on the
**ESP32-S3-WROOM-1 16N8** module (16 MB flash, 8 MB PSRAM). It runs Arduino
C++ firmware with an embedded **MicroPython v1.28 preview** runtime, so you can write
Python apps that control all the hardware directly.

### Hardware at a Glance

| Component | Spec |
|-----------|------|
| MCU | ESP32-S3-WROOM-1 16N8 (dual-core, 240 MHz, 8 MB PSRAM, 16 MB flash) |
| Display | 128×64 monochrome OLED (SSD1306) |
| LED Matrix | 8×8 red LEDs (IS31FL3731, PWM per pixel) |
| Input | 4 d-pad buttons + analog joystick |
| Motion | LIS2DH12 3-axis accelerometer |
| IR | NEC-protocol TX LED + TSOP receiver |
| Haptics | Vibration motor with coil-tone support |
| Storage | FatFS `ffat` partition (0x600000 bytes in `partitions_replay_16MB_doom.csv`), mounted as the MicroPython VFS root |
| Python heap | 2 MB from PSRAM |

### Physical Layout

The badge is held **upright** during use. When idle (walking around the
conference), it rests **upside down** on the lanyard, and the firmware
automatically flips the display to show a nametag.

```
        ╭───────────────────────────────────────────────╮
        │              ▀▀ IR TX/RX ▀▀                   │
        │                                               │
        │   ╭───────────────────────────────────────╮   │
        │   │                                       │   │
        │   │         128×64 OLED Display           │   │
        │   │         (0,0) ───► x                  │   │
        │   │          │                            │   │
        │   │          ▼ y                          │   │
        │   │                                       │   │
        │   ╰───────────────────────────────────────╯   │
        │                                               │
        │              ╭───────────╮                    │
        │              │ · · · · · │        [Y]         │
        │     ◉        │ · 8×8   · │     [X]   [B]      │
        │   Joystick   │ · LED   · │        [A]         │
        │              │ · Matrix· │                    │
        │              │ · · · · · │                    │
        │              ╰───────────╯                    │
        ╰───────────────────────────────────────────────╯

    Buttons:  [Y] = BTN_UP / BTN_TRIANGLE
              [X] = BTN_LEFT / BTN_SQUARE
              [B] = BTN_RIGHT / BTN_CIRCLE
              [A] = BTN_DOWN / BTN_CROSS
```

**Button mapping:**

- Physical: `BTN_UP` (Y), `BTN_DOWN` (A), `BTN_LEFT` (X), `BTN_RIGHT` (B)
- PlayStation aliases: `BTN_TRIANGLE`, `BTN_CROSS`, `BTN_SQUARE`, `BTN_CIRCLE`
- Semantic: `BTN_CONFIRM` (select/OK), `BTN_BACK` (cancel/back) — these follow
  the user's confirm/back swap setting

### Orientation and Nametag Mode

The IMU detects when the badge is flipped upside down. The firmware
automatically:

- Flips the OLED to show an idle display / nametag
- Rotates button and joystick input to match the new orientation
- Flips the LED matrix

Your app can detect this too — see
[Flip/Nametag Detection](#flipnametag-detection) in the Advanced Topics
section.

---

## 2. Getting Started with JumperIDE

The fastest way to write and test badge code is **JumperIDE**, a browser-based
MicroPython IDE that connects over WebSerial.

Go to [https://ide.jumperless.org/](https://ide.jumperless.org/) and press
the **Connect** button.

<img width="1303" height="1246" alt="JumperIDE connect button" src="https://github.com/user-attachments/assets/47edf213-8e91-4904-beb4-3a93d71538db" />

Select the badge serial port from the browser picker.

- On macOS this usually appears as USB/JTAG serial for the ESP32-S3.
- On Windows this appears as a COM port.
- If multiple ports appear, try the one that shows the MicroPython REPL prompt.

<img width="1304" height="1250" alt="Serial port picker" src="https://github.com/user-attachments/assets/a9ea53fa-86fd-46b0-839d-eeaa32606454" />

Open or create a script, then hit **Run / Stop** (or press `F5`).

<img width="1304" height="1250" alt="Run/Stop button" src="https://github.com/user-attachments/assets/0190af73-cd5f-49e9-b378-bb6d1c8a7bb4" />

Press it again to stop. If you make edits, hit the green **Save** button
(`Ctrl+S`) to write the file to the badge filesystem.

<img width="1306" height="1249" alt="Save button" src="https://github.com/user-attachments/assets/29413b36-1de1-478e-8d67-70cb4146fd60" />

The REPL terminal at the bottom shows `print()` output and exceptions.
JumperIDE uses MicroPython raw REPL under the hood, so anything compatible with
raw REPL workflows (including `mpremote`) also works with the badge.

### File Management

JumperIDE shows the badge filesystem in a tree view. You can:

- Browse `/apps/`, `/docs/`, `/lib/`, `/tests/`
- Open and edit files directly on the badge
- Create new files and directories
- Save changes with `Ctrl+S`


## 3. Your First App

### Hello World

```python
import time

oled_clear()
oled_set_cursor(32, 28)
oled_print("Hello Badge!")
oled_show()

led_show_image(IMG_HEART)
time.sleep_ms(3000)
led_clear()

exit()
```

This clears the OLED, prints centered text, shows a heart on the LED matrix for
3 seconds, then exits back to the menu.

### Adding Input

```python
import time

count = 0

while True:
    oled_clear()
    oled_set_cursor(0, 0)
    oled_print("Press buttons!")
    oled_set_cursor(0, 20)
    oled_print("Count: " + str(count))
    oled_show()

    if button_pressed(BTN_CONFIRM):
        count += 1
        haptic_pulse()
        led_show_image(IMG_SMILEY)

    if button_pressed(BTN_BACK):
        break

    time.sleep_ms(30)

led_clear()
exit()
```

**Key patterns:**

- `button_pressed()` is edge-triggered — it returns `True` once per press,
  then `False` until the button is released and pressed again. Use this for
  menu navigation.
- `button()` is level-triggered — it returns `True` as long as the button is
  held. Use this for continuous actions (shooting, accelerating).
- Always call `time.sleep_ms(20-30)` in your main loop to yield CPU time.
- Call `oled_show()` after drawing to make changes visible.

### Using the Native UI Chrome

For apps that should look like the built-in firmware screens, use `badge_ui`:

```python
import badge_ui as ui
import time

ui.chrome("My App", "v1.0", "OK", "action", "BACK", "quit")
ui.line(0, "Hello from Python!")
ui.line(1, "This matches the firmware style")
oled_show()

while True:
    if button_pressed(BTN_BACK):
        break
    time.sleep_ms(30)

exit()
```

`badge_ui` calls the native C++ UI layout code, so your app's header, footer,
and button glyph icons are pixel-identical to the firmware menus. See
`initial_filesystem/lib/badge_ui.py` for all available helpers.

For app lifecycle patterns, use `badge_app`. It provides crash reporting,
foreground LED cleanup, periodic garbage collection, joystick helpers, local
score helpers, and shared timing primitives for dual-screen games.

---

## 4. App Structure

### Single-File Apps

Good for quick experiments and small demos. Place a `.py` file in `/apps/`:

```
/apps/my_demo.py
```

All `badge` module functions are auto-imported into the global scope — no
`import badge` needed in single-file apps.

### Multi-File Apps

For anything beyond a simple demo, use a folder with a `main.py` entry point:

```
/apps/my_game/
    main.py          # Entry point (tiny — just imports and calls main)
    engine.py        # Game loop and logic
    data.py          # Constants, level data
    screens.py       # OLED rendering functions
    icon.py          # Optional: app icon bitmap for the menu
```

The `main.py` should be minimal:

```python
"""My Game app entry point."""

import sys

APP_DIR = "/apps/my_game"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge_app import run_app
from engine import main

run_app("My Game", main)
```

In your other modules, explicitly import what you need:

```python
from badge import *
import time
import gc
```

This pattern is used by BreakSnake, Flappy Asteroids, and Synth. The
`sys.path.insert` lets Python find sibling modules in the app directory.

### Shared App Helpers

Use `badge_app` before duplicating lifecycle or game-loop code. It centralizes
the patterns current production apps use:

```python
from badge_app import ButtonLatch, DualScreenSession, run_app, with_led_override

def play():
    game = new_game()
    session = DualScreenSession(frame_ms=34)
    fire = ButtonLatch(BTN_CONFIRM)

    def loop():
        while True:
            now = session.now()
            pressed_now = fire.poll()
            if session.frame_due(game, now):
                tick_oled(game, fire.consume(), now)
            tick_matrix(game, pressed_now, now)
            if session.quit_held(1000):
                break
            session.sleep()

    with_led_override(loop)

run_app("My Game", play)
```

Reach for:

- `run_app(app_name, main, cleanup=None)` for native-looking crash capture in
  `/last_mpy_error.txt`.
- `with_led_override(callback, *args)` when an app temporarily owns the LED
  matrix.
- `GCTicker` or `DualScreenSession` for predictable garbage collection in
  long-running loops.
- `read_axis()`, `read_stick_xy()`, and `read_stick_4way()` for common joystick
  threshold behavior.
- `load_score()` and `save_score()` for best-effort local JSON score files.

### Showing Up on the Main Menu

In dev builds (`echo-dev`), any folder under `/apps/` with a `main.py` is
automatically listed in the Apps menu. In production firmware, apps are
registered in the native menu grid with an icon in `AppIcons.h` and a
launcher entry in `GUI.cpp`.

A filesystem-based app registration system is planned — apps will be able to
declare their name, icon, and description to appear on the main screen without
firmware changes.

### Saving Data

Apps have full filesystem access through the standard `os` module and
`open()`:

```python
import json

SAVE_FILE = "/apps/my_game/save.json"

def save_state(score, level):
    data = {"score": score, "level": level}
    with open(SAVE_FILE, "w") as f:
        f.write(json.dumps(data))

def load_state():
    try:
        with open(SAVE_FILE, "r") as f:
            return json.loads(f.read())
    except:
        return {"score": 0, "level": 1}
```

---

## 5. MicroPython Cheat Sheet

### Available Modules

| Module | Notes |
|--------|-------|
| `sys` | `sys.path`, `sys.exit()` |
| `os` | Filesystem: `listdir`, `mkdir`, `remove`, `stat` |
| `time` | `sleep_ms()`, `ticks_ms()`, `ticks_diff()` |
| `random` | `randint()`, `choice()`, `uniform()` |
| `math` | `sin()`, `cos()`, `sqrt()`, `pi` |
| `cmath` | Complex math |
| `struct` | `pack()`, `unpack()` for binary data |
| `array` | Typed arrays |
| `binascii` | `hexlify()`, `unhexlify()` |
| `json` | `loads()`, `dumps()` |
| `collections` | `OrderedDict`, `namedtuple` |
| `errno` | Error constants |
| `gc` | `collect()`, `mem_free()`, `mem_alloc()` |
| `io` | `StringIO`, `BytesIO` |
| `micropython` | `mem_info()`, `stack_use()` |
| `uctypes` | C-compatible struct access |
| `badge` | All badge hardware (auto-imported) |

### Key Differences from CPython

**f-strings are supported.** You can use normal MicroPython string formatting:

```python
score = 42
print(f"Score: {score}")
print("Score: " + str(score))  # also fine
```

**Time functions use milliseconds:**

```python
import time

time.sleep_ms(100)      # 100 ms
time.sleep(1)           # 1 second (float OK)

start = time.ticks_ms()
# ... do work ...
elapsed = time.ticks_diff(time.ticks_ms(), start)
```

Use a cooperative main loop. Long-running computation in the foreground loop
can block input/render responsiveness, so keep per-frame work small and
use `sleep_ms()` to yield.

**Memory is limited.** The Python heap is 2 MB from PSRAM. Call
`gc.collect()` periodically in long-running apps, especially after releasing
large objects, or use `GCTicker` / `DualScreenSession` from `badge_app`:

```python
import gc

gc.collect()
print("Free:", gc.mem_free(), "bytes")
```

**No pip / external packages.** Only the modules listed above are available.
Shared code goes in `/lib/` (which is on `sys.path`).

For the full MicroPython language reference:
[docs.micropython.org](https://docs.micropython.org/en/latest/)

---

## 6. Hardware Guide

### OLED Display (128×64)

The coordinate system has **(0, 0) at the top-left corner**. X ranges from
0–127, Y from 0–63. The display is buffered — draw to the buffer, then call
`oled_show()` to push it to the screen.

**Text rendering:**

```python
oled_clear()
oled_set_cursor(0, 0)
oled_print("Top-left")

oled_set_cursor(0, 30)
oled_set_text_size(2)
oled_print("BIG")
oled_set_text_size(1)

oled_show()
```

**Centering text:**

```python
text = "Centered!"
w = oled_text_width(text)
x = (128 - w) // 2
oled_set_cursor(x, 28)
oled_print(text)
oled_show()
```

**Fonts:**

```python
fonts = oled_get_fonts().split(",")
for f in fonts:
    oled_set_font(f)
    oled_clear()
    oled_set_cursor(0, 20)
    oled_print(f)
    oled_show()
    time.sleep_ms(500)
```

**Drawing primitives:**

```python
oled_set_pixel(64, 32, 1)           # Single pixel
oled_draw_box(10, 10, 50, 20)       # Filled rectangle
oled_set_draw_color(2)              # XOR mode
oled_draw_box(20, 5, 30, 30)        # XOR overlay
oled_set_draw_color(1)              # Back to white
oled_show()
```

**Framebuffer access** for advanced effects:

```python
fb = oled_get_framebuffer()         # bytes object, 1024 bytes
w, h, size = oled_get_framebuffer_size()

buf = bytearray(fb)
# Modify buf...
oled_set_framebuffer(buf)
oled_show()
```

### LED Matrix (8×8)

An 8×8 red LED grid centered below the OLED. Each LED has individual PWM
brightness (0–255). The matrix has a separate global brightness control.

**Basic operations:**

```python
led_brightness(40)                  # Set global brightness
led_clear()                         # All off
led_fill()                          # All on
led_set_pixel(3, 3, 100)            # Single LED at (3,3)
val = led_get_pixel(3, 3)           # Read brightness back
```

**Built-in images and animations:**

```python
led_show_image(IMG_HEART)
time.sleep_ms(1000)

led_start_animation(ANIM_PULSE_HEART, 100)
time.sleep_ms(3000)
led_stop_animation()
```

**Custom bitmask frames:**

Each row is a byte where bit 7 is the leftmost pixel:

```python
# Checkerboard pattern
led_set_frame([
    0b10101010,
    0b01010101,
    0b10101010,
    0b01010101,
    0b10101010,
    0b01010101,
    0b10101010,
    0b01010101,
], 60)
```

**Foreground override** — prevent the ambient LED mode from clobbering your
drawing:

```python
led_override_begin()
# ... draw on the matrix ...
led_set_pixel(0, 0, 255)
# ... when done:
led_override_end()
```

**Background callbacks** — register a Python function that the firmware calls
periodically to animate the matrix while your main loop does other things:

```python
phase = 0

def spinner(now_ms):
    global phase
    led_clear()
    x = phase % 8
    for y in range(8):
        led_set_pixel(x, y, 80)
    phase += 1

matrix_app_start(spinner, 80, 30)

# ... main loop handles OLED and input ...
# The spinner runs in the background via the service pump

matrix_app_stop()   # When done
```

### Buttons

Four face buttons arranged in a d-pad layout. Use the semantic constants
(`BTN_CONFIRM`, `BTN_BACK`) for menu-style interaction, and physical constants
(`BTN_UP`, `BTN_DOWN`, `BTN_LEFT`, `BTN_RIGHT`) for game controls.

```python
# Edge-triggered — fires once per press
if button_pressed(BTN_CONFIRM):
    do_action()

# Level-triggered — true while held
if button(BTN_UP):
    move_up()

# Hold duration
ms = button_held_ms(BTN_DOWN)
if ms > 1000:
    long_press_action()
```

**Escape chord:** Holding all four face buttons for ~1 second force-exits any
running app. This is a firmware safety net — you don't need to implement it.

### Joystick

A 2-axis analog joystick returning raw ADC values (0–4095). Center position is
approximately 2048, but varies per unit.

```python
x = joy_x()  # 0 = full left, 4095 = full right
y = joy_y()  # 0 = full up, 4095 = full down

# Dead zone handling
CENTER = 2048
DEAD = 300

dx = x - CENTER
dy = y - CENTER
if abs(dx) < DEAD:
    dx = 0
if abs(dy) < DEAD:
    dy = 0
```

### IMU (Accelerometer)

The LIS2DH12 accelerometer provides tilt sensing and motion detection.

```python
if not imu_ready():
    oled_println("No IMU!")

# Tilt values in milli-g (±1000 typical)
tx = imu_tilt_x()
ty = imu_tilt_y()

# Map tilt to LED position
px = int((tx + 1000) * 7 / 2000)
py = int((ty + 1000) * 7 / 2000)
px = max(0, min(7, px))
py = max(0, min(7, py))

led_clear()
led_set_pixel(px, py, 80)
```

```python
# Motion detection (edge-triggered)
if imu_motion():
    oled_println("Shake detected!")

# Orientation detection
if imu_face_down():
    oled_println("Badge is flipped!")
```

### Haptics

The vibration motor supports haptic feedback pulses and audible coil tones.

```python
haptic_pulse()                      # Default pulse
haptic_pulse(200, 50)               # Stronger, 50ms
haptic_pulse(255, 100, 150)         # Full, 100ms, 150Hz carrier
haptic_off()                        # Stop immediately
```

**Coil tones** — at low duty cycles, the motor coil vibrates audibly:

```python
# Play a scale
notes = [262, 294, 330, 349, 392, 440, 494, 523]
for freq in notes:
    tone(freq, 200)
    time.sleep_ms(250)

# Continuous tone
tone(440)
time.sleep_ms(1000)
no_tone()

# Check if playing
if tone_playing():
    oled_println("Playing!")
```

### IR Send/Receive

Infrared communication using a modified NEC protocol. The IR LED is at the top
of the badge, and the TSOP receiver is next to it.

**You must call `ir_start()` before using any IR functions.** IR hardware is
shared with the Boop screen — Python gets exclusive access while IR mode is
active.

**Simple frames (1 byte address + 1 byte command):**

```python
ir_start()

# Send
ir_send(0x42, 0x01)

# Receive
time.sleep_ms(200)
if ir_available():
    frame = ir_read()
    if frame:
        addr, cmd = frame
        oled_println("Got: " + hex(addr) + " " + hex(cmd))

ir_stop()
```

**Multi-word frames (up to 8 × 32-bit words):**

```python
ir_start()

# Send a 3-word payload (CRC appended automatically)
ir_send_words([0xB0, 0xDEADBEEF, 0x12345678])

# Receive
words = ir_read_words()
if words:
    oled_println("Got " + str(len(words)) + " words")

ir_flush()   # Clear RX queue
ir_stop()
```

**Timing constraint:** Poll `ir_read()` or `ir_read_words()` within **50 ms**
or the IRremote buffer on Core 0 may overflow.

**TX power control:**

```python
ir_start()
print(ir_tx_power())    # Default: 50 (percent of 38kHz carrier)
ir_tx_power(10)          # Throttle down for close-range testing
```

---

## 7. Advanced Topics

### Mouse Overlay

A hardware-composited cursor overlay for building point-and-click GUIs. When
enabled, the joystick moves a cursor sprite and button presses become click
events.

```python
import time

mouse_overlay(True)
mouse_set_pos(64, 32)
mouse_set_speed(4)
mouse_set_mode(MOUSE_RELATIVE)

while True:
    oled_clear()
    oled_set_cursor(0, 0)
    oled_print("X:" + str(mouse_x()) + " Y:" + str(mouse_y()))
    oled_show()

    btn = mouse_clicked()
    if btn == BTN_CONFIRM:
        haptic_pulse()
    if btn == BTN_BACK:
        break

    time.sleep_ms(30)

mouse_overlay(False)
```

**Custom cursor sprites:**

```python
# 8×8 crosshair (MSB-first, row-major)
crosshair = bytes([
    0b00010000,
    0b00010000,
    0b00010000,
    0b11101110,
    0b00010000,
    0b00010000,
    0b00010000,
    0b00000000,
])
mouse_set_bitmap(crosshair, 8, 8)
```

The bitmap format is 1-bit-per-pixel, MSB-first within each byte, row-major.
Maximum size is 32×32 (128 bytes). The hot-spot is automatically set to the
sprite center.

### Flip/Nametag Detection

When the badge hangs upside down on its lanyard, the firmware automatically
enters nametag mode — flipping the OLED, LED matrix, and input orientation.
This happens transparently through the service pump.

Your app can detect and respond to orientation changes:

```python
import time

while True:
    if imu_face_down():
        oled_clear()
        oled_set_cursor(20, 28)
        oled_print("Walking mode!")
        oled_show()
    else:
        oled_clear()
        oled_set_cursor(20, 28)
        oled_print("Active mode!")
        oled_show()

    time.sleep_ms(200)
```

You can also use `imu_tilt_y()` for a continuous tilt value — negative values
mean the badge is tilted toward upside-down. The firmware uses a threshold on
this to trigger the nametag flip.

Possible use cases:

- Show a custom idle animation when the badge is face-down
- Display a QR code or name badge when inverted
- Pause a game when the badge is flipped

### Native UI Chrome

For apps that should look like built-in firmware screens, use the native UI
helpers directly or through `badge_ui`:

**Direct native calls:**

```python
oled_clear()
ui_header("My App", "v1.0")
ui_action_bar("OK", "start", "BACK", "quit")
# Draw content between header and footer (Y: 10–52)
oled_set_cursor(4, 20)
oled_print("Content here")
oled_show()
```

**Through badge_ui (recommended):**

```python
import badge_ui as ui

ui.chrome("My App", "v1.0", "OK", "start", "BACK", "quit")
ui.line(0, "First line")
ui.line(1, "Second line")
ui.selected_row(2, "Selected item")
oled_show()
```

The `badge_ui` module provides additional helpers: `ui.center()`,
`ui.fit()`, `ui.text()`, `ui.fill()`, `ui.hline()`, `ui.vline()`,
`ui.frame()`, `ui.status_box()`, `ui.spinner()`, and composable hint
functions (`ui.hint()`, `ui.hint_text()`, `ui.hint_row()`).

Button glyph names for `ui_action_bar` and inline hints: `OK`, `BACK`, `X`,
`Y`, `A`, `B`. These render as small button-shaped icons and respect the
badge's confirm/back swap setting.

---

## 8. Badge-to-Badge Communication

The IR hardware lets two badges exchange data when pointed at each other. This
enables multiplayer games, contact exchange, and collaborative apps.

### Simple Ping-Pong

Two badges take turns sending and receiving:

```python
import time

ir_start()
my_id = my_uuid()

oled_clear()
oled_println("IR Chat")
oled_println("CONFIRM = send")
oled_println("Waiting...")
oled_show()

while True:
    # Send on button press
    if button_pressed(BTN_CONFIRM):
        ir_send(0x42, 0x01)
        oled_println("Sent!")
        oled_show()
        haptic_pulse()

    # Check for incoming
    if ir_available():
        frame = ir_read()
        if frame:
            addr, cmd = frame
            oled_println("Got: " + hex(cmd))
            oled_show()
            led_show_image(IMG_HEART)
            time.sleep_ms(500)
            led_clear()

    if button_pressed(BTN_BACK):
        break

    time.sleep_ms(30)

ir_stop()
exit()
```

### Richer Data with Multi-Word Frames

For sending more than 2 bytes, use multi-word frames (up to 32 bytes of
payload):

```python
import time
import struct

ir_start()

def send_position(x, y, score):
    # Pack 3 values into 3 words
    ir_send_words([x, y, score])

def receive_position():
    words = ir_read_words()
    if words and len(words) >= 3:
        return words[0], words[1], words[2]
    return None

# Game loop
while True:
    if button_pressed(BTN_CONFIRM):
        send_position(joy_x(), joy_y(), 42)

    pos = receive_position()
    if pos:
        x, y, score = pos
        oled_clear()
        oled_println("Peer: " + str(x) + "," + str(y))
        oled_println("Score: " + str(score))
        oled_show()

    if button_pressed(BTN_BACK):
        break
    time.sleep_ms(30)

ir_stop()
```

### Tips for IR Communication

- Always call `ir_start()` first and `ir_stop()` when done
- Poll `ir_read()` within 50 ms to avoid buffer overflow
- A classic frame takes ~110 ms on wire; a 3-word frame takes ~230 ms
- Use `ir_flush()` to clear stale frames before starting a new exchange
- `ir_tx_power(10)` is useful for testing with two badges close together
- IR is line-of-sight — badges need to be roughly pointed at each other

---

## 9. Tips and Gotchas

### Memory Management

- **2 MB heap.** Call `gc.collect()` regularly, especially in game loops, or
  use `GCTicker` / `DualScreenSession` from `badge_app`.
- Avoid creating large temporary objects. Reuse buffers when possible.
- Check available memory: `gc.mem_free()` returns bytes free.
- There is no firmware-enforced source file size limit, but large single files
  are slower to read, compile, and edit over serial. Split large apps into
  multiple modules.

### Display

- `oled_show()` is required after any drawing operation. Nothing appears on
  screen until you call it.
- `oled_clear()` resets the cursor to (0,0). Pass `True` to also refresh:
  `oled_clear(True)`.
- The display is 128×64 — plan your layouts accordingly. The usable area with
  `badge_ui` chrome is roughly Y: 10–52 (between header and footer).

### LED Matrix

- Use `led_override_begin()` / `led_override_end()` when drawing directly on
  the matrix. Without it, the ambient LED mode may overwrite your pixels.
- `led_clear()` at the end of your app to be a good citizen.
- Matrix app callbacks (`matrix_app_start`) run from the service pump, not your
  main loop — keep them fast (no blocking, no heavy computation).

### IR

- **IR is mode-gated** — only works after `ir_start()`. Other screens (Boop)
  share the hardware.
- The RX buffer is 8 frames deep. If you don't read within ~50 ms per frame,
  frames get dropped.
- Always `ir_stop()` and `ir_flush()` in your cleanup code.

### Input

- `button_pressed()` consumes the event — calling it twice for the same button
  in the same loop iteration will miss the second call. Read it once and store
  the result.
- The joystick center varies per unit (~2048 typical). Always use a dead zone
  of at least 200–300.

### General

- **f-strings are available**, but string concatenation also works.
- **No `import badge` needed** in the entry script — all functions are
  auto-injected. In imported modules, use `from badge import *`.
- **Escape chord:** Hold all 4 face buttons for ~1 second to force-exit any
  stuck app.
- **Clean up** before exiting: `led_clear()`, `haptic_off()`, `no_tone()`,
  `ir_stop()`, `mouse_overlay(False)`.

---

## Quick Reference Card

```
┌──────────────────────────────────────────────────┐
│ OLED (128×64)                                    │
│   oled_clear() → oled_print() → oled_show()      │
│                                                  │
│ LED Matrix (8×8)                                 │
│   led_show_image(IMG_HEART)                      │
│   led_set_frame([row0..row7], brightness)        │
│                                                  │
│ Buttons                                          │
│   button_pressed(BTN_CONFIRM) → True once        │
│   button(BTN_UP) → True while held               │
│                                                  │
│ Joystick                                         │
│   joy_x() → 0–4095    joy_y() → 0–4095          │
│                                                  │
│ Haptics                                          │
│   haptic_pulse()    tone(440, 200)               │
│                                                  │
│ IR                                               │
│   ir_start() → ir_send(a,c) → ir_read() → tuple │
│                                                  │
│ IMU                                              │
│   imu_tilt_x()  imu_motion()  imu_face_down()   │
│                                                  │
│ Mouse                                            │
│   mouse_overlay(True) → mouse_clicked() → btn_id│
│                                                  │
│ Files                                            │
│   open("/apps/x/save.json","w").write(data)      │
│                                                  │
│ Exit                                             │
│   exit()  or hold all 4 buttons                  │
└──────────────────────────────────────────────────┘
```
