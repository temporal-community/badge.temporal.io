# Temporal Badge MicroPython API Reference

All functions and constants from the `badge` module are automatically imported
into the global namespace. You can call them directly (e.g. `button(BTN_CONFIRM)`)
without needing the `badge.` prefix.

---

[Init](#init):

* `init()` — Initialize the badge hardware bridge (called automatically)

[OLED Display](#oled-display):

* `oled_print(text)` — Print text at cursor
* `oled_println(text)` — Print text + newline + show
* `oled_clear([show])` — Clear display
* `oled_show()` — Refresh display
* `oled_set_cursor(x, y)` — Set text cursor
* `oled_set_text_size(size)` — Set text size (1–4)
* `oled_get_text_size()` — Get text size
* `oled_invert(enable)` — Invert display colors
* `oled_text_width(text)` — Get pixel width of a string
* `oled_text_height()` — Get pixel height of current font
* `oled_set_font(name)` — Set font by name
* `oled_get_fonts()` — Get available font names
* `oled_get_current_font()` — Get current font name
* `oled_set_pixel(x, y, color)` — Set single pixel
* `oled_get_pixel(x, y)` — Read single pixel
* `oled_draw_box(x, y, w, h)` — Draw filled rectangle
* `oled_set_draw_color(color)` — Set draw color (0=black, 1=white, 2=XOR)
* `oled_get_framebuffer()` — Get framebuffer as bytes
* `oled_set_framebuffer(data)` — Set framebuffer from bytes
* `oled_get_framebuffer_size()` — Get (width, height, bytes)

[Native UI Chrome](#native-ui-chrome):

* `ui_header(title, [right])` — Draw the standard header and rule
* `ui_action_bar([left_button], [left_label], [right_button], [right_label])` — Draw footer actions with native button glyphs
* `ui_chrome(title, [right], [left_button], [left_label], [right_button], [right_label])` — Clear and draw standard header/footer chrome
* `ui_inline_hint(x, y, hint)` — Draw an inline hint with native button glyphs
* `ui_inline_hint_right(right_x, y, hint)` — Right-align an inline hint
* `ui_measure_hint(hint)` — Return an inline hint's pixel width

[App Helper Library](#app-helper-library-badge_apppy):

* `run_app(app_name, callback, [cleanup])` — Show/save a native-looking crash report for uncaught app exceptions
* `with_led_override(callback, *args)` — Run foreground LED drawing with guaranteed cleanup
* `ButtonLatch(button_id)` — Latch edge-triggered button presses until a slower tick consumes them
* `GCTicker([interval_ms], [threshold])` — Periodic garbage collection for long-running loops
* `DualScreenSession(frame_ms, [sleep_ms], [gc_ms])` — Shared timing helpers for OLED + LED games
* `read_axis(value, [low], [high])` / `read_stick_xy()` / `read_stick_4way()` — Shared joystick threshold helpers
* `load_score(path, defaults)` / `save_score(path, score)` — Local JSON score persistence

[Mouse Overlay](#mouse-overlay):

* `mouse_overlay(enable)` — Enable/disable cursor overlay
* `mouse_set_bitmap(data, w, h)` — Set cursor sprite (row-major bitmap, max 32×32)
* `mouse_x()` — Current cursor X position
* `mouse_y()` — Current cursor Y position
* `mouse_set_pos(x, y)` — Warp cursor to position
* `mouse_clicked()` — Read last click button ID (-1 if none)
* `mouse_set_speed(speed)` — Set cursor speed (1–20, default 3)
* `mouse_set_mode(mode)` — Set positioning mode (`MOUSE_ABSOLUTE` or `MOUSE_RELATIVE`, default `MOUSE_RELATIVE`)

[Buttons & Joystick](#buttons--joystick):

* `button(id)` — Read button state (True if held)
* `button_pressed(id)` — Edge-triggered press (True once per press)
* `button_held_ms(id)` — Milliseconds button has been held
* `joy_x()` — Joystick X axis (0–4095)
* `joy_y()` — Joystick Y axis (0–4095)

[LED Matrix](#led-matrix-8x8) (8×8 IS31FL3731):

* `led_brightness(value)` — Set global brightness (0–255)
* `led_clear()` — Turn off all LEDs
* `led_fill([brightness])` — Turn on all LEDs
* `led_set_pixel(x, y, brightness)` — Set single LED
* `led_get_pixel(x, y)` — Read single LED brightness
* `led_show_image(name)` — Show builtin image by name
* `led_set_frame(rows, [brightness])` — Draw 8×8 bitmask pattern
* `led_start_animation(name, [interval_ms])` — Start builtin animation
* `led_stop_animation()` — Stop current animation
* `led_override_begin()` — Pause ambient LED mode for foreground drawing
* `led_override_end()` — Release foreground drawing and restore ambient LEDs

[Matrix App Host](#matrix-app-host):

* `matrix_app_start(callback, [interval_ms], [brightness])` — Register a Python callback for background LED matrix rendering
* `matrix_app_set_speed(interval_ms)` — Change the callback tick interval
* `matrix_app_set_brightness(brightness)` — Change the LED brightness
* `matrix_app_stop()` — Unregister the callback
* `matrix_app_active()` — Check if a callback is registered
* `matrix_app_info()` — Return diagnostic state

[IMU](#imu-accelerometer) (LIS2DH12 Accelerometer):

* `imu_ready()` — Check if IMU is initialized
* `imu_tilt_x()` — X-axis tilt in milli-g
* `imu_tilt_y()` — Y-axis tilt in milli-g
* `imu_accel_z()` — Z-axis acceleration in milli-g
* `imu_face_down()` — True if badge is face-down
* `imu_motion()` — Consume motion event (True if motion detected since last call)

[Haptics](#haptics) (Vibration Motor + Coil Tone):

* `haptic_pulse([strength], [duration_ms], [freq_hz])` — Fire vibration pulse
* `haptic_strength([value])` — Get or set motor strength (0–255)
* `haptic_off()` — Stop motor
* `tone(freq_hz, [duration_ms], [duty])` — Play audible tone from motor coil
* `no_tone()` — Stop tone
* `tone_playing()` — Check if tone is active

[IR Send/Receive](#ir-sendreceive) (NEC Protocol):

* `ir_send(addr, cmd)` — Transmit one 1-byte-addr / 1-byte-cmd NEC frame
* `ir_start()` — Start IR receive mode
* `ir_stop()` — Stop IR receive, flush queue
* `ir_available()` — Check if a received frame is waiting
* `ir_read()` — Read received (addr, cmd) as a tuple
* `ir_send_words(words)` — Transmit a multi-word NEC frame (1–8 × 32-bit)
* `ir_read_words()` — Read a received multi-word NEC frame as a tuple
* `ir_flush()` — Drop every pending RX frame
* `ir_tx_power([percent])` — Get/set IR carrier duty (1–50%)
* `ir_set_mode(name)` — Switch routing: `"badge"`, `"nec"`, or `"raw"`
* `ir_get_mode()` — Returns the active mode name
* `ir_nec_send(addr, cmd, repeats=0)` — Send a consumer NEC frame
* `ir_nec_read()` — Returns `(addr, cmd, is_repeat)` or `None`
* `ir_raw_capture()` — Returns captured `bytes` of `(mark_us, space_us)` pairs
* `ir_raw_send(buf, carrier_hz=38000)` — Replay arbitrary IR symbols

[Badge Identity & Boops](#badge-identity--boops):

* `my_uuid()` — Return this badge's 12-char hex UID
* `boops()` — Return `/boops.json` contents as a string

[Script Control](#script-control):

* `exit()` — Raise `SystemExit` to cleanly stop the running app
* `dev(*args)` — Test harness dispatcher *(dev builds only)*

[Filesystem Access](#filesystem-access):

* Standard `os` module — `listdir`, `mkdir`, `remove`, `rename`, `stat`, etc.
* Standard `open()` / `read()` / `write()` / `close()`

---

## Init

### `init()`

Initialize the badge hardware bridge. This is called automatically when the
`badge` module loads, so app code should not normally need to call it directly.
Returns `0` on success; raises `OSError` on failure.

---

## OLED Display

128×64 monochrome SSD1306 OLED display controlled via U8G2.

### `oled_print(text)`

Print text at the current cursor position. Does not refresh the display
automatically — call `oled_show()` to make it visible.

* `text`: String to display.

### `oled_println(text)`

Print text followed by a newline, then automatically refresh the display.

* `text`: String to display.

**Example:**

```python
oled_clear()
oled_println("Hello, badge!")
oled_println("Line 2")
```

### `oled_clear([show])`

Clear the display and reset cursor to (0, 0).

* `show` (optional): If `True`, refresh display immediately. Default `False`.

**Example:**

```python
oled_clear()          # Clear buffer only
oled_clear(True)      # Clear and refresh immediately
```

### `oled_show()`

Refresh the display to show buffered changes. Required after `oled_print()`,
`oled_set_pixel()`, or `oled_set_framebuffer()` to make changes visible.

### `oled_set_cursor(x, y)`

Move the text cursor to pixel coordinates.

* `x`: X position (0–127).
* `y`: Y position (0–63).

### `oled_set_text_size(size)`

Set the text rendering size.

* `size`: Text size multiplier (1–4). Returns `True` on success.

### `oled_get_text_size()`

Returns the current text size (1–4).

### `oled_invert(enable)`

Invert the display colors.

* `enable`: `True` to invert, `False` for normal.

### `oled_text_width(text)`

Get the pixel width of a string in the current font.

* `text`: The string to measure.
* Returns the width in pixels.

**Example:**

```python
w = oled_text_width("Hello")
x = (128 - w) // 2
oled_set_cursor(x, 30)
oled_print("Hello")
oled_show()
```

### `oled_text_height()`

Get the maximum character height of the current font.

* Returns the height in pixels.

### `oled_set_font(name)`

Set the font by name.

* `name`: Font family name (case-sensitive).
* Returns `True` if font was found, `False` otherwise.

### `oled_get_fonts()`

Returns a comma-separated string of available font names.

**Example:**

```python
fonts = oled_get_fonts().split(",")
for f in fonts:
    oled_set_font(f)
    oled_clear()
    oled_println(f)
    import time; time.sleep(1)
```

### `oled_get_current_font()`

Returns the name of the currently active font.

### `oled_set_pixel(x, y, color)`

Set a single pixel in the framebuffer.

* `x`: X coordinate (0–127).
* `y`: Y coordinate (0–63).
* `color`: `1` for white/on, `0` for black/off.

Call `oled_show()` after setting pixels to make changes visible.

### `oled_get_pixel(x, y)`

Read the color of a single pixel.

* Returns `1` (white/on) or `0` (black/off).

### `oled_draw_box(x, y, w, h)`

Draw a filled rectangle in the framebuffer using the current draw color.

* `x`: Left edge (0–127).
* `y`: Top edge (0–63).
* `w`: Width in pixels.
* `h`: Height in pixels.

Call `oled_show()` after drawing to make changes visible.

**Example:**

```python
oled_clear()
oled_draw_box(10, 10, 50, 20)
oled_set_draw_color(0)
oled_set_cursor(12, 12)
oled_print("Hello")
oled_set_draw_color(1)
oled_show()
```

### `oled_set_draw_color(color)`

Set the drawing color for subsequent draw operations.

* `color`: `0` = black/off, `1` = white/on (default), `2` = XOR (inverts
  existing pixels).

### `oled_get_framebuffer()`

Returns the entire display framebuffer as a `bytes` object. Format: 1 bit per
pixel, organized in vertical bytes (Adafruit SSD1306 format).

### `oled_set_framebuffer(data)`

Replace the entire display framebuffer and refresh.

* `data`: `bytes` or `bytearray` of the correct size (typically 1024 bytes for
  128×64).
* Returns `True` on success, `False` on size mismatch.

### `oled_get_framebuffer_size()`

Returns a tuple `(width, height, buffer_size_bytes)`.

**Example:**

```python
w, h, size = oled_get_framebuffer_size()
print(f"Display: {w}x{h}, {size} bytes")
```

---

## Native UI Chrome

These helpers draw the same header, footer, and button glyph style used by
the firmware screens. App code should usually import `badge_ui`, which wraps
these native functions with Python conveniences like `ui.chrome(...)`.
`badge_ui` also includes `ui.hint(...)`, `ui.hint_text(...)`, and
`ui.hint_row(...)` helpers for composing multiple glyph-backed action hints
without hand-building strings throughout an app. Use `ui.chrome_tall(...)` or
`ui.tall_action_bar(...)` when a screen needs two footer rows; keep content
above the taller footer so controls never cover app text.

### `ui_header(title, [right])`

Draw the standard small header and divider. `right` is optional top-right text.

### `ui_action_bar([left_button], [left_label], [right_button], [right_label])`

Draw footer actions with native button glyphs. Button names include `OK`,
`BACK`, `X`, `Y`, `A`, and `B`; semantic names respect the badge's confirm/back
swap setting.

### `ui_chrome(title, [right], [left_button], [left_label], [right_button], [right_label])`

Clear the OLED buffer, draw the standard header, then draw the footer action
bar. Call `oled_show()` after drawing your screen content.

### `ui_inline_hint(x, y, hint)`

Draw inline text with native button glyph replacement, such as `"OK:start"` or
`"BACK quit"`. Returns the drawn width in pixels.

### `ui_inline_hint_right(right_x, y, hint)`

Right-align an inline hint to `right_x`. Returns the drawn width in pixels.

### `ui_measure_hint(hint)`

Return the pixel width an inline hint will use.

---

## App Helper Library (`badge_app.py`)

`badge_app` is an embedded Python helper library for app-level patterns that
are not raw hardware APIs. Import from it explicitly:

```python
from badge_app import run_app, with_led_override
```

### `run_app(app_name, callback, [cleanup])`

Run an app entry point with a native-looking crash wrapper. Uncaught exceptions
are written to `/last_mpy_error.txt`, the OLED shows an "App crashed" screen,
the LED matrix shows an X pattern, and OK or BACK returns to the firmware menu.
If `cleanup` is provided, it is called after normal returns, uncaught
exceptions, and `exit()` / `SystemExit`.

Use it from folder-app `main.py` files:

```python
import sys

APP_DIR = "/apps/my_app"
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge_app import run_app
from engine import main

run_app("My App", main)
```

`SystemExit` raised by `exit()` is not caught by this wrapper, so normal app
exits still return directly to the host runtime after cleanup runs.
For apps with hardware state to release, pass a cleanup callback:

```python
run_app("Synth", run, cleanup)
```

### `with_led_override(callback, *args)`

Run `callback(*args)` between `led_override_begin()` and `led_override_end()`.
It also performs a `gc.collect()` before and after the callback. Use it for apps
that foreground-own the 8×8 matrix so the ambient LED mode is restored even when
the callback returns early.

```python
def title(best):
    led_set_frame(TITLE_ICON, 40)
    draw_title(best)
    return wait_choice(True, False)

start = with_led_override(title, best_score)
```

### `ButtonLatch(button_id)`

Latch a `button_pressed()` edge until a slower game tick is ready to consume it.
This prevents fast button taps from being missed when the OLED game updates less
often than the main input loop.

```python
fire = ButtonLatch(BTN_CONFIRM)

while True:
    flap_now = fire.poll()
    if oled_frame_due:
        tick_oled_game(fire.consume())
    tick_led_game(flap_now)
```

Methods:

* `poll()` — Read `button_pressed(button_id)`, remember the edge, and return the
  immediate edge state.
* `consume()` — Return and clear the remembered edge.

### `GCTicker([interval_ms], [threshold])`

Configure MicroPython garbage collection for an app loop. Construction performs
an initial `gc.collect()` and, when supported by the runtime, sets
`gc.threshold(threshold)`. Call `tick()` from a polling loop to run a full
collection every `interval_ms` without scattering `gc.collect()` calls through
game logic.

```python
gc_tick = GCTicker()

while True:
    update()
    draw()
    gc_tick.tick()
    time.sleep_ms(18)
```

Methods:

* `tick([now])` — Collect when the interval has elapsed, and return `True` only
  when collection ran.

### `DualScreenSession(frame_ms, [sleep_ms], [gc_ms])`

Small loop helper for apps that drive the OLED and LED matrix at different
rates. It owns a `GCTicker` and calls it from `sleep()` so long-running
dual-screen games get periodic garbage collection by default.

```python
session = DualScreenSession(34)

while True:
    now = session.now()
    if session.frame_due(game, now):
        draw_oled_frame(game, now)
    if session.quit_held(1000):
        break
    session.sleep()
```

Methods:

* `now()` — Return `time.ticks_ms()`.
* `frame_due(game, now, [key], [frame_ms])` — Check `game[key]` (default
  `"last_frame"`), update it when due, and return `True` only for due frames.
* `quit_pressed()` — Return `button_pressed(BTN_BACK)`.
* `quit_held(hold_ms)` — Return `True` once BACK has been held for `hold_ms`.
* `sleep()` — Sleep for the configured loop delay.

### Joystick Helpers

The badge joystick reports raw ADC values. These helpers centralize the usual
threshold logic so apps do not each hard-code their own version.

```python
from badge_app import read_axis, read_stick_4way

x_dir = read_axis(joy_x())        # -1, 0, or 1
x_dir, y_dir = read_stick_4way()  # diagonal input returns (0, 0)
```

* `read_axis(value, [low], [high])` — Convert a raw joystick value to `-1`, `0`,
  or `1`. Defaults are `1100` and `3000`.
* `read_stick_xy([low], [high])` — Return `(x_dir, y_dir)` using the default
  thresholds.
* `read_stick_4way([low], [high], [no_diagonal])` — Return `(x_dir, y_dir)`.
  When `no_diagonal` is `True` (default), diagonal input returns `(0, 0)`.

### Score and Time Helpers

```python
from badge_app import load_score, save_score, elapsed_seconds

defaults = {"total": 0, "seconds": 0}
best = load_score("/my_app_score.json", defaults)
best["total"] = 120
save_score("/my_app_score.json", best)
seconds = elapsed_seconds(game["started"], time.ticks_ms())
```

* `load_score(path, defaults)` — Load JSON from `path`, returning integer values
  for the keys in `defaults`. Missing or invalid files return a copy of
  `defaults`.
* `save_score(path, score)` — Best-effort JSON write. Storage errors are ignored
  so score saves do not crash games.
* `elapsed_seconds(started, now)` — Return non-negative elapsed seconds using
  `time.ticks_diff()`.

### Utility Helpers

* `clamp(value, low, high)` — Clamp a numeric value.
* `ticks_add(now, delta)` — Use `time.ticks_add()` when available, with a host
  Python fallback for smoke tests.
* `active_until(until, now)` — Return whether a ticks-based deadline is still
  active.
* `seed_random()` — Best-effort `random.seed(time.ticks_ms())`.
* `wait_choice([confirm], [back], [delay_ms])` — Block until OK or BACK is
  pressed and return the configured value (`True` for OK, `False` for BACK by
  default).

---

## Buttons & Joystick

Four face buttons and a 2-axis analog joystick. The directional constants are
kept for compatibility; new apps should prefer the semantic aliases where they
fit the interaction.

### Button Constants

```python
BTN_RIGHT = 0
BTN_DOWN  = 1
BTN_LEFT  = 2
BTN_UP    = 3

BTN_CIRCLE   = BTN_RIGHT
BTN_CROSS    = BTN_DOWN
BTN_SQUARE   = BTN_LEFT
BTN_TRIANGLE = BTN_UP

BTN_CONFIRM = 4
BTN_SAVE    = BTN_CONFIRM
BTN_BACK    = 5
BTN_PRESETS = BTN_TRIANGLE
```

`BTN_CONFIRM`/`BTN_SAVE` and `BTN_BACK` follow the firmware
`swap_ok` setting: the default is B/Circle confirm and A/Cross back; setting
`swap_ok = 0` uses A/Cross confirm and B/Circle back. The physical constants and
PlayStation-style shape constants always refer to the actual hardware button.

### `button(id)`

Read the current state of a button.

* `id`: Button constant (`BTN_CONFIRM`, `BTN_BACK`, `BTN_PRESETS`, etc.).
* Returns `True` if the button is currently held down.

**Example:**

```python
if button(BTN_CONFIRM):
    oled_println("confirm held!")
```

### `button_pressed(id)`

Edge-triggered button press detection. Returns `True` **once** per physical
press, then `False` until the button is released and pressed again. Consumes
the press event on read.

* `id`: Button constant.
* Returns `True` if a new press was detected since the last call.

**Example:**

```python
import time
count = 0
while True:
    if button_pressed(BTN_CONFIRM):
        count += 1
        oled_clear()
        oled_println(f"Presses: {count}")
    time.sleep(0.02)
```

### `button_held_ms(id)`

Get how long a button has been continuously held.

* `id`: Button constant.
* Returns milliseconds the button has been held, or `0` if not pressed.

**Example:**

```python
ms = button_held_ms(BTN_DOWN)
if ms > 1000:
    oled_println("Long press!")
```

### `joy_x()`

Read the joystick X axis.

* Returns an integer 0–4095. Center is approximately 2047.

### `joy_y()`

Read the joystick Y axis.

* Returns an integer 0–4095. Center is approximately 2047.

**Example:**

```python
x = joy_x()
y = joy_y()
oled_clear()
oled_println(f"X:{x} Y:{y}")
```

---

## LED Matrix (8×8)

8×8 LED matrix driven by the IS31FL3731 with per-pixel PWM brightness control.

### Image Constants

Builtin image names for use with `led_show_image()`:

```python
IMG_SMILEY    = "smiley"
IMG_HEART     = "heart"
IMG_ARROW_UP  = "arrow_up"
IMG_ARROW_DOWN = "arrow_down"
IMG_X_MARK    = "x_mark"
IMG_DOT       = "dot"
```

### Animation Constants

Builtin animation names for use with `led_start_animation()`:

```python
ANIM_SPINNER      = "spinner"
ANIM_BLINK_SMILEY = "blink_smiley"
ANIM_PULSE_HEART  = "pulse_heart"
```

### `led_brightness(value)`

Set the global LED brightness.

* `value`: Brightness level (0–255). `0` is off, `255` is maximum.

### `led_clear()`

Turn off all LEDs on the matrix.

### `led_fill([brightness])`

Turn on all LEDs. If `brightness` is omitted, uses the current global brightness.

* `brightness` (optional): Per-pixel brightness (0–255).

### `led_set_pixel(x, y, brightness)`

Set a single LED brightness.

* `x`: Column (0–7).
* `y`: Row (0–7).
* `brightness`: LED brightness (0–255).

**Example:**

```python
led_clear()
led_set_pixel(3, 3, 100)
led_set_pixel(4, 4, 100)
```

### `led_get_pixel(x, y)`

Read the brightness of a single LED.

* Returns brightness value (0–255).

### `led_show_image(name)`

Display a builtin image on the matrix.

* `name`: Image name string (see Image Constants above).
* Returns `True` if the image was found.

**Example:**

```python
led_show_image(IMG_HEART)

import time
time.sleep(2)
led_show_image("smiley")    # string name works too
```

### `led_set_frame(rows, [brightness])`

Draw an arbitrary 8×8 pattern from a list of row bitmasks. Each row is a uint8
where the MSB is the leftmost pixel.

* `rows`: List or tuple of exactly 8 integers (0–255), one per row.
* `brightness` (optional): On-pixel brightness (0–255). Defaults to current
  global brightness.

**Example:**

```python
# Draw a smiley face
led_set_frame([
    0b00111100,
    0b01000010,
    0b10100101,
    0b10000001,
    0b10100101,
    0b10011001,
    0b01000010,
    0b00111100,
])

# Draw an X with explicit brightness
led_set_frame([
    0b10000001,
    0b01000010,
    0b00100100,
    0b00011000,
    0b00011000,
    0b00100100,
    0b01000010,
    0b10000001,
], 50)  # dim
```

### `led_start_animation(name, [interval_ms])`

Start a builtin animation on the matrix.

* `name`: Animation name string (see Animation Constants above).
* `interval_ms` (optional): Frame interval in milliseconds. Default 120ms.
* Returns `True` on success.

**Example:**

```python
led_start_animation(ANIM_PULSE_HEART)

import time
time.sleep(5)
led_stop_animation()
```

### `led_stop_animation()`

Stop the currently running animation.

### `led_override_begin()`

Pause the saved ambient LED mode so a foreground app can draw on the matrix.
Call this before direct LED drawing when you want to temporarily override the
LED app.

### `led_override_end()`

Release a foreground LED override and restore the saved ambient LED mode.
Call this from cleanup paths after `led_override_begin()`.

---

## Matrix App Host

Register a Python callback that the firmware calls periodically to render on
the 8×8 LED matrix in the background. This is useful for ambient LED animations
that should continue while the main Python loop handles OLED and input.

### `matrix_app_start(callback, [interval_ms], [brightness])`

Register a callable to be invoked every `interval_ms` milliseconds.

* `callback`: Callable that accepts one argument, the current `millis()`
  timestamp.
* `interval_ms` (optional): Tick interval in milliseconds. Minimum 16 ms.
  Defaults to the firmware ambient interval.
* `brightness` (optional): LED brightness (0–255). Defaults to the firmware
  ambient brightness.

**Example:**

```python
frame = 0

def led_tick(now_ms):
    global frame
    led_clear()
    col = frame % 8
    for row in range(8):
        led_set_pixel(col, row, 80)
    frame += 1

matrix_app_start(led_tick, 100, 40)
```

### `matrix_app_set_speed(interval_ms)`

Change the tick interval for the active callback.

* `interval_ms`: New interval in milliseconds. Minimum 16 ms.
* Returns the clamped interval.

### `matrix_app_set_brightness(brightness)`

Change the LED brightness for the active callback.

* `brightness`: New brightness (0–255).
* Returns the clamped brightness.

### `matrix_app_stop()`

Unregister the callback and restore the default ambient LED mode.

### `matrix_app_active()`

Check whether a background callback is currently registered.

* Returns `True` if a callback is active.

### `matrix_app_info()`

Return a diagnostic tuple with the current matrix app state.

* Returns `(active, saved, interval_ms, brightness, overridden, invocations)`.

---

## IMU (Accelerometer)

LIS2DH12 3-axis accelerometer for tilt sensing and motion detection.

### `imu_ready()`

Check if the IMU is initialized and taking readings.

* Returns `True` if the IMU is ready.

### `imu_tilt_x()`

Read the smoothed X-axis tilt.

* Returns a float in milli-g (mG). Typical range ±1000 mG.

### `imu_tilt_y()`

Read the smoothed Y-axis tilt.

* Returns a float in milli-g (mG). Typical range ±1000 mG.

### `imu_accel_z()`

Read the Z-axis acceleration.

* Returns a float in milli-g (mG). ~1000 mG when stationary (1g gravity).

### `imu_face_down()`

Check if the badge is face-down.

* Returns `True` if the badge is face-down (Z-axis below threshold).

### `imu_motion()`

Check for a motion event. **Consumes** the event on read — calling again
returns `False` until new motion is detected.

* Returns `True` if motion was detected since the last call.

**Example:**

```python
import time

while True:
    if imu_motion():
        oled_clear()
        oled_println("Motion!")
        haptic_pulse()

    x = imu_tilt_x()
    y = imu_tilt_y()

    # Map tilt to LED matrix pixel
    px = int((x + 1000) / 250)
    py = int((y + 1000) / 250)
    px = max(0, min(7, px))
    py = max(0, min(7, py))

    led_clear()
    led_set_pixel(px, py, 100)
    time.sleep(0.05)
```

---

## Haptics

Vibration motor with PWM control and audible coil tone support. The motor can
produce haptic feedback pulses or, at very low duty cycles, audible tones from
coil vibration.

### `haptic_pulse([strength], [duration_ms], [freq_hz])`

Fire a haptic vibration pulse. All parameters are optional — omitted values use
the configured defaults (strength ~155, duration ~35ms, frequency ~80Hz).

* `strength` (optional): Motor intensity (0–255).
* `duration_ms` (optional): Pulse duration in milliseconds.
* `freq_hz` (optional): PWM carrier frequency in Hz.

**Example:**

```python
haptic_pulse()              # Default pulse
haptic_pulse(200)           # Stronger
haptic_pulse(100, 50)       # Medium strength, 50ms
haptic_pulse(255, 100, 150) # Full strength, 100ms, 150Hz carrier
```

### `haptic_strength([value])`

Get or set the default motor strength used by `haptic_pulse()`.

* `value` (optional): New strength (0–255). If omitted, just returns current.
* Returns the current strength (0–255).

**Example:**

```python
print(haptic_strength())    # Read current
haptic_strength(200)        # Set to 200
```

### `haptic_off()`

Immediately stop the motor and cancel any active pulse.

### `tone(freq_hz, [duration_ms], [duty])`

Play an audible tone from the motor coil. At very low duty cycles (~30/255),
the coil doesn't spin but vibrates audibly at the PWM frequency.

* `freq_hz`: Tone frequency in Hz.
* `duration_ms` (optional): Duration in milliseconds. `0` or omitted = play
  until `no_tone()`.
* `duty` (optional): Duty cycle (0–255). Default 30.

**Example:**

```python
import time

# Play a scale
for freq in [262, 294, 330, 349, 392, 440, 494, 523]:
    tone(freq, 200)
    time.sleep(0.25)

# Continuous tone until stopped
tone(440)
time.sleep(2)
no_tone()
```

### `no_tone()`

Stop the currently playing tone.

### `tone_playing()`

Check if a tone is currently playing.

* Returns `True` if a tone is active.

---

## IR Send/Receive

Infrared communication over a modified NEC protocol using the onboard IR LED
(TX) and TSOP receiver (RX). The RMT-driven encoder automatically prepends an
NEC leader, packs each payload word, appends a CRC32, and emits a trailing
pulse — MicroPython just picks the payload.

Two payload shapes are available:

* **Classic 1-byte/1-byte frames** via `ir_send(addr, cmd)` / `ir_read()`.
  A single 32-bit NEC word `(~addr, addr, ~cmd, cmd)`, ~110 ms on wire.
* **Multi-word frames** via `ir_send_words(words)` / `ir_read_words()`.
  1–8 raw 32-bit words (up to 32 bytes of payload) with an appended CRC.
  Each data word adds ~54 ms, so a 3-word frame ≈ 230 ms on wire.

The Boop screen and MicroPython share the same RMT hardware; `ir_start()`
brings the radio up for Python use and waits for it to be ready before
returning, so the first `ir_send*()` after `ir_start()` will not race.

### `ir_send(addr, cmd)`

Transmit a single classic NEC frame.

* `addr`: NEC address byte (0–255).
* `cmd`: NEC command byte (0–255).

Blocks briefly while the RMT hardware streams the frame. Returns `0` on
success; raises `OSError` if the IR hardware is down (e.g. called before
`ir_start()`).

### `ir_start()`

Bring the IR hardware up and start receiving in Python mode. Incoming frames
are queued in an 8-slot ring buffer for retrieval with `ir_read()` or
`ir_read_words()`. `ir_start()` blocks up to ~500 ms while Core 0 powers the
RMT channel, and drains any stale frames left over from the Boop screen.

### `ir_stop()`

Stop Python IR RX and drop any queued frames. Does **not** power down the
hardware — the Boop screen may still use it.

### `ir_available()`

Check if at least one classic NEC frame is waiting in the RX queue.

* Returns `True` if `ir_read()` would return a frame.

### `ir_read()`

Pop one classic NEC frame from the queue.

* Returns `(addr, cmd)` if a frame is available, or `None` if the queue
  is empty.

**Example:**

```python
import time

ir_start()
oled_println("Listening for IR...")

while True:
    if ir_available():
        frame = ir_read()
        if frame:
            addr, cmd = frame
            oled_clear()
            oled_println(f"IR: {addr:#x} {cmd:#x}")
            haptic_pulse()
    time.sleep(0.05)
```

### `ir_send_words(words)`

Transmit a multi-word NEC frame. The encoder automatically emits a leader,
then each word LSB-first, then a CRC32 over the payload.

* `words`: A list, tuple or other sequence of 1–8 integers. Each element is
  converted to an unsigned 32-bit value.
* Raises `ValueError` if the sequence is empty or longer than 8 words.
* Raises `OSError(1)` if the IR hardware is not up (e.g. before `ir_start()`).

**Example:**

```python
ir_start()
ir_send_words([0xB0, 0xDEADBEEF, 0x12345678])
```

### `ir_read_words()`

Read one received multi-word NEC frame.

* Returns a `tuple` of up to 64 integers (the payload words, CRC-validated
  and stripped by the decoder) if a frame is available.
* Returns `None` if the RX queue is empty.

### `ir_flush()`

Drop every pending RX frame (both classic and multi-word queues). Safe to
call at any time; a no-op if the IR hardware is down.

### `ir_tx_power([percent])`

Get or set the IR carrier duty cycle. A higher duty drives the IR LED harder
and extends range at the cost of current and LED stress.

* `percent` (optional): New duty cycle in the range **1–50** (percent of the
  38 kHz carrier period). Call with no argument to just read the current
  value. Power-on default is **50**.
* Returns the current duty as an integer percent after the (optional) set.
* Raises `ValueError` if `percent` is outside 1–50.

**Example:**

```python
ir_start()
print("Default duty:", ir_tx_power())   # 50
ir_tx_power(10)                          # throttle down for self-loopback
```

### IR Playground modes (consumer NEC + raw symbols)

`ir_set_mode(name)` swaps the alternate-routing for the same RMT channel.
The legacy multi-word path keeps decoding badge frames in the background;
`ir_set_mode` only changes which alternate stream MicroPython sees.

| Mode    | Outbound API                  | Inbound API           |
|---------|-------------------------------|-----------------------|
| `badge` | `ir_send_words(words)`        | `ir_read_words()`     |
| `nec`   | `ir_nec_send(addr, cmd, n=0)` | `ir_nec_read()`       |
| `raw`   | `ir_raw_send(buf, carrier=…)` | `ir_raw_capture()`    |

* `ir_set_mode(name)` raises `ValueError` if `name` is not one of those three
  strings, or if a Boop is currently in flight (refuse to leave `"badge"`
  mid-pairing). On `ir_stop()` the mode resets to `"badge"`.
* `ir_get_mode()` returns the current mode name.

#### `ir_nec_send(addr, cmd, repeats=0)`

Transmit a canonical 32-bit consumer NEC frame
(`addr | ~addr | cmd | ~cmd`). `repeats` schedules that many leader-only
"button-held" frames at the standard ~110 ms interval.

#### `ir_nec_read()`

Returns `(addr, cmd, is_repeat)` or `None`. On a repeat frame `addr` and
`cmd` are carried forward from the previous full frame.

#### `ir_raw_capture()`

Returns a `bytes` object of `(mark_us, space_us)` pairs (4 bytes per pair,
little-endian `uint16`), or `None` if no frame is queued. Use this for
non-NEC protocols or to inspect raw timings.

#### `ir_raw_send(buf, carrier_hz=38000)`

Replay an arbitrary symbol buffer at the given carrier frequency
(3000–60000 Hz). Use the same shape `ir_raw_capture()` returns, or build
one from `struct.pack("<HH", mark, space)` pairs.

---

## Badge Identity & Boops

### `my_uuid()`

Return this badge's globally unique identifier, derived from the ESP32-S3
eFuse `OPTIONAL_UNIQUE_ID` (first 6 bytes, hex-encoded).

* Returns a 12-character lowercase hex string, e.g. `"a1b2c3d4e5f6"`.

### `boops()`

Return the on-flash `/boops.json` contents as a string. This is the same
document that the Boop screen maintains — each completed boop is appended
with the peer UID, peer name / ticket (if the server backfilled them), and
a `status` field (`"ok"`, `"local"`, etc.).

* Returns a JSON string. When no boops have been recorded yet, returns
  `'{"pairings":[]}'` so callers can always `json.loads()` the result.

**Example:**

```python
import json

data = json.loads(boops())
for p in data.get("pairings", []):
    oled_println(p.get("peer_badge_uid", "?"))
oled_show()
```

---

## Script Control

### `exit()`

Cleanly stop the currently running MicroPython app by raising `SystemExit`.
Prefer this over `sys.exit()` so the host runtime sees the same exception
type the rest of the firmware expects.

Holding all four face buttons for about 1 second also force-exits the running
app from outside.

### `dev(*args)` *(dev builds only)*

Variadic string-argument dispatcher for the firmware test harness. This is only
available in builds with `BADGE_ENABLE_MP_DEV`, such as the `echo-dev`
PlatformIO environment. Each argument is coerced to a string and forwarded to
the C++ runtime. Returns a string result.

---

## Filesystem Access

The badge's VFS is mounted and accessible through the standard `os` module.
Apps can read and write files for saving state, high scores, or configuration.

```python
import os

os.listdir("/apps")
os.listdir("/")

with open("/apps/my_app/save.json", "w") as f:
    f.write('{"score": 42}')

with open("/apps/my_app/save.json", "r") as f:
    data = f.read()
```

Available `os` operations include `listdir`, `mkdir`, `remove`, `rename`,
`stat`, `getcwd`, `chdir`, `ilistdir`, and `statvfs`. Standard `open()`,
`read()`, `write()`, and `close()` work as expected.

---

## Mouse Overlay

Hardware-composited cursor overlay for building GUIs and games. When enabled,
a cursor sprite is automatically drawn on top of the OLED framebuffer at every
display refresh. The joystick controls cursor position and face buttons
generate click events — all handled asynchronously in the service pump so
Python code only needs to query position and clicks.

### `mouse_overlay(enable)`

Enable or disable the cursor overlay.

* `enable`: `True` to enable, `False` to disable.

When enabled, the joystick moves the cursor and button presses are captured
as click events (instead of being consumed by `button_pressed()`).

**Example:**

```python
mouse_overlay(True)
mouse_set_pos(64, 32)

while True:
    oled_clear()
    oled_set_cursor(0, 0)
    oled_print("x:" + str(mouse_x()) + " y:" + str(mouse_y()))
    oled_show()

    btn = mouse_clicked()
    if btn == BTN_RIGHT:
        oled_println("Clicked!")
    if btn == BTN_LEFT:
        break
    time.sleep_ms(30)

mouse_overlay(False)
```

### `mouse_set_bitmap(data, w, h)`

Set a custom cursor sprite. Format is a packed 1-bit-per-pixel bitmap,
row-major, **MSB-first within each byte** (the leftmost pixel of a row is
bit 7 of its first byte). Each row is padded to a whole number of bytes, so
`ceil(w / 8) * h` bytes are read from `data`.

* `data`: `bytes` or `bytearray` containing the bitmap.
* `w`: Width in pixels (1–32). Values larger than 32 are clamped to 32.
* `h`: Height in pixels (1–32). Values larger than 32 are clamped to 32.

The internal cursor buffer is 128 bytes, so any `(w, h)` with
`ceil(w / 8) * h ≤ 128` is accepted. A 32×32 sprite uses the entire buffer.
The hot-spot is automatically set to the sprite's center. The default cursor
is an 8×8 arrow pointer.

**Example:**

```python
# 8x8 crosshair cursor
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

### `mouse_x()`

Returns the current cursor X position (0–127).

### `mouse_y()`

Returns the current cursor Y position (0–63).

### `mouse_set_pos(x, y)`

Warp the cursor to an absolute position.

* `x`: X position (clamped to 0–127).
* `y`: Y position (clamped to 0–63).

### `mouse_clicked()`

Read the last click event. Returns the button ID (`BTN_RIGHT`, `BTN_DOWN`,
`BTN_LEFT`, `BTN_UP`) or `-1` if no click is pending. **Consumes** the event
on read — calling again returns `-1` until a new button is pressed.

### `mouse_set_speed(speed)`

Set the cursor movement speed (only affects relative mode).

* `speed`: Pixels per joystick poll at full deflection (1–20). Default is 3.

**Example:**

```python
mouse_set_speed(5)   # faster cursor
mouse_set_speed(1)   # very precise
```

### `mouse_set_mode(mode)`

Switch between absolute and relative positioning.

* `mode`: `MOUSE_ABSOLUTE` (joystick position = cursor position) or
  `MOUSE_RELATIVE` (joystick deflection = cursor velocity). Default is
  `MOUSE_RELATIVE`.

In **absolute** mode the cursor tracks the joystick 1:1 — stick center is
screen center. In **relative** mode the joystick acts like a mouse — deflect
to move, release to stop. Use `mouse_set_speed()` and `mouse_set_pos()` to
tune relative mode.

**Example:**

```python
mouse_set_mode(MOUSE_RELATIVE)
mouse_set_pos(64, 32)
mouse_set_speed(4)
```

---

## Constants Reference

### Buttons

| Constant | Value | Description |
|----------|-------|-------------|
| `BTN_RIGHT` | 0 | Right button |
| `BTN_DOWN` | 1 | Down button |
| `BTN_LEFT` | 2 | Left button |
| `BTN_UP` | 3 | Up button |
| `BTN_CIRCLE` | 0 | PlayStation-style alias for right |
| `BTN_CROSS` | 1 | PlayStation-style alias for down |
| `BTN_SQUARE` | 2 | PlayStation-style alias for left |
| `BTN_TRIANGLE` | 3 | PlayStation-style alias for up |
| `BTN_CONFIRM` | 4 | Semantic confirm/select, follows `swap_ok` |
| `BTN_SAVE` | 4 | Semantic save/apply, follows `swap_ok` |
| `BTN_BACK` | 5 | Semantic back/cancel, follows `swap_ok` |
| `BTN_PRESETS` | 3 | Semantic alias for preset/actions |

### LED Matrix Images

| Constant | Value | Description |
|----------|-------|-------------|
| `IMG_SMILEY` | `"smiley"` | Smiley face |
| `IMG_HEART` | `"heart"` | Heart shape |
| `IMG_ARROW_UP` | `"arrow_up"` | Upward arrow |
| `IMG_ARROW_DOWN` | `"arrow_down"` | Downward arrow |
| `IMG_X_MARK` | `"x_mark"` | X mark |
| `IMG_DOT` | `"dot"` | Center dot |

### LED Matrix Animations

| Constant | Value | Description |
|----------|-------|-------------|
| `ANIM_SPINNER` | `"spinner"` | Rotating spinner |
| `ANIM_BLINK_SMILEY` | `"blink_smiley"` | Blinking smiley face |
| `ANIM_PULSE_HEART` | `"pulse_heart"` | Pulsing heart |

### Mouse Overlay Modes

| Constant | Value | Description |
|----------|-------|-------------|
| `MOUSE_ABSOLUTE` | 1 | Joystick position = cursor position |
| `MOUSE_RELATIVE` | 0 | Joystick deflection = cursor velocity (default) |
