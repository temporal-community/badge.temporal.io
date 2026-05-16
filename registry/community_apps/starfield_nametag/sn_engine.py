"""Animated starfield with centered text on OLED, mirrored on the LED matrix.

Copyright (c) 2026 Alexandre Roman.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import random

from badge import *

from badge_app import DualScreenSession, with_led_override


OLED_W = 128
OLED_H = 64
CX = 64
CY = 32

STAR_COUNT = 110

FRAME_MS = 33

Z_NEAR = 0.5
Z_FAR = 4.0
Z_SPEED = 0.10
K = 64.0

MAX_BRIGHTNESS = 200

# "HI" on the 8x8 matrix, right-side-up: H at cols 0-2, gap at col 3,
# I at cols 4-6, col 7 blank, glyphs in rows 1-5.
_LED_HI = (
    0b00000000,
    0b10101110,
    0b10100100,
    0b11100100,
    0b10100100,
    0b10101110,
    0b00000000,
    0b00000000,
)

_text = "Hello, World!"
_text_size = 4

# tilt_y in milli-g. Measured on this badge: held in hand ≈ -75, hanging on a
# lanyard ≈ +950 (sign opposite to what the badge dev guide claims). Cross
# +500 to enter nametag/lanyard mode; recover at +300. Hysteresis keeps the
# flip from chattering as the badge swings on the cord.
_FLIP_ENTER_MG = 500
_FLIP_EXIT_MG = 300

_flipped = False


def _update_flip():
    global _flipped
    ty = imu_tilt_y()
    if _flipped:
        if ty < _FLIP_EXIT_MG:
            _flipped = False
    else:
        if ty > _FLIP_ENTER_MG:
            _flipped = True
    return _flipped


def set_text(value):
    global _text, _text_size
    _text = value if isinstance(value, str) and value else "Hello, World!"
    max_w = OLED_W - 8
    chosen = 1
    # WHY: firmware oled_text_width() reports the actual on-screen render width for the active
    # font (default Spleen 9px). The width does not follow the standard 6*size Adafruit GFX
    # formula, so we probe sizes via the firmware rather than computing arithmetically.
    for size in (4, 3, 2, 1):
        oled_set_text_size(size)
        if oled_text_width(_text) <= max_w:
            chosen = size
            break
    oled_set_text_size(1)
    _text_size = chosen


def _pick_xy():
    x = random.random() * 2.0 - 1.0
    y = random.random() * 2.0 - 1.0
    if abs(x) < 0.1 and abs(y) < 0.1:
        x = 0.1 if x >= 0 else -0.1
    return x, y


def _new_star(z):
    x, y = _pick_xy()
    return {"x": x, "y": y, "z": z}


def _new_game():
    # Seed initial z uniformly across the depth range so recycle events stay spread
    # out forever after; seeding all stars at Z_FAR causes a visible wave pulse.
    stars = [_new_star(Z_NEAR + random.random() * (Z_FAR - Z_NEAR)) for _ in range(STAR_COUNT)]
    return {
        "stars": stars,
        # Required by DualScreenSession.frame_due: it reads game["last_frame"] with no default.
        "last_frame": 0,
    }


def _advance_oled(game):
    for star in game["stars"]:
        star["z"] -= Z_SPEED
        recycle = star["z"] <= Z_NEAR
        if not recycle:
            scale = K / star["z"]
            sx = int(CX + star["x"] * scale)
            sy = int(CY + star["y"] * scale)
            if sx < 0 or sx >= OLED_W or sy < 0 or sy >= OLED_H:
                recycle = True
        if recycle:
            x, y = _pick_xy()
            star["x"] = x
            star["y"] = y
            star["z"] = Z_FAR


def _draw_stars(game):
    for star in game["stars"]:
        scale = K / star["z"]
        sx = int(CX + star["x"] * scale)
        sy = int(CY + star["y"] * scale)
        if sx < 0 or sx >= OLED_W or sy < 0 or sy >= OLED_H:
            continue
        if scale > 80:
            oled_draw_box(sx, sy, 2, 2)
        elif scale > 40:
            oled_draw_box(sx, sy, 2, 1)
        else:
            oled_draw_box(sx, sy, 1, 1)


def _draw_text(text):
    oled_set_text_size(_text_size)
    w = oled_text_width(text)
    h = oled_text_height()
    x = (OLED_W - w) // 2
    if x < 0:
        x = 0
    y = (OLED_H - h) // 2
    oled_set_draw_color(0)
    oled_draw_box(x - 2, y - 2, w + 4, h + 4)
    oled_set_draw_color(1)
    oled_set_cursor(x, y)
    oled_print(text)
    oled_set_text_size(1)


# WHY one-liner: the badge's oled_set_framebuffer() already applies a 180°
# rotation internally (internal[1023-i] = bit_reverse(input[i])) before
# storing — verified on hardware. So feeding the current framebuffer back
# through set_framebuffer flips the display 180° in one call. Do NOT add
# a manual rotation here: that would double-flip and become a no-op.
def _rotate_fb_180():
    oled_set_framebuffer(oled_get_framebuffer())


def _draw_led():
    for r in range(8):
        bits = _LED_HI[r]
        for c in range(8):
            on = (bits >> (7 - c)) & 1
            led_set_pixel(c, r, MAX_BRIGHTNESS if on else 0)


def _render(game):
    oled_clear()
    _draw_stars(game)
    _draw_text(_text)
    if _update_flip():
        _rotate_fb_180()
    else:
        oled_show()
    _draw_led()


def _run_loop(game, session):
    _render(game)
    while True:
        if session.quit_pressed():
            return
        now = session.now()
        if session.frame_due(game, now):
            _advance_oled(game)
            _render(game)
        session.sleep()


def main():
    game = _new_game()
    session = DualScreenSession(FRAME_MS)
    with_led_override(_run_loop, game, session)


def cleanup():
    global _flipped
    _flipped = False
    try:
        oled_invert(False)
    except Exception:
        pass
    try:
        oled_clear(True)
    except Exception:
        pass
    led_clear()
