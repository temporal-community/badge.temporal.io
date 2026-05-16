"""dad_jokes.py — Fetch a random dad joke on demand.

Uses badge.http_get() (the only built-in HTTPS client on the badge — there is
no `requests` / `urequests` here) and prints the plain-text body word-wrapped
onto the OLED at a fixed readable size. Long jokes scroll with the joystick.
Press BTN_RIGHT to fetch a new joke, BTN_BACK to exit.
"""
import time
import gc
import json

import badge
import badge_ui as ui
from badge_app import read_axis, run_app

URL = "https://dadjokes736.com"
POLL_MS = 50
SCROLL_REPEAT_MS = 120

BODY_FONT = "6x10"

CONTENT_TOP = ui.HEADER_RULE_Y + 2
CONTENT_BOTTOM = ui.FOOTER_Y - 1
CONTENT_H = CONTENT_BOTTOM - CONTENT_TOP


def wrap_strict(joke, max_w):
    """Word-wrap with no mid-word splits. Words wider than max_w overflow
    horizontally but stay intact — readable trumps tidy."""
    words = joke.split()
    if not words:
        return [""]
    lines = []
    current = ""
    for word in words:
        candidate = word if not current else current + " " + word
        if badge.oled_text_width(candidate) <= max_w:
            current = candidate
        else:
            if current:
                lines.append(current)
            current = word
    if current:
        lines.append(current)
    return lines


def layout(joke):
    """Set BODY_FONT and return (lines, line_h, visible_lines, max_offset)."""
    badge.oled_set_font(BODY_FONT)
    line_h = badge.oled_text_height() + 1
    lines = wrap_strict(joke, ui.SCREEN_W - 4)
    visible_lines = max(1, CONTENT_H // line_h)
    max_offset = max(0, len(lines) - visible_lines)
    return lines, line_h, visible_lines, max_offset


def _draw_centered_line(line, y):
    w = badge.oled_text_width(line)
    x = (ui.SCREEN_W - w) // 2
    if x < 0:
        x = 0
    badge.oled_set_cursor(x, y)
    badge.oled_print(line)


def _draw_scroll_marker(y_top, up):
    """Tiny 3-px arrow on the right edge, pointing up or down."""
    x = ui.SCREEN_W - 3
    if up:
        badge.oled_set_pixel(x + 1, y_top, 1)
        badge.oled_set_pixel(x, y_top + 1, 1)
        badge.oled_set_pixel(x + 1, y_top + 1, 1)
        badge.oled_set_pixel(x + 2, y_top + 1, 1)
    else:
        badge.oled_set_pixel(x, y_top, 1)
        badge.oled_set_pixel(x + 1, y_top, 1)
        badge.oled_set_pixel(x + 2, y_top, 1)
        badge.oled_set_pixel(x + 1, y_top + 1, 1)


def draw(text, is_error, scroll_offset=0, status=None):
    badge.oled_clear()
    title = "Error" if is_error else "Dad Jokes"
    ui.header(title)
    if status:
        ui.center((CONTENT_TOP + CONTENT_BOTTOM) // 2 - 4, status)
    else:
        lines, line_h, visible_lines, max_offset = layout(text)
        if max_offset == 0:
            block_h = len(lines) * line_h
            y0 = CONTENT_TOP + (CONTENT_H - block_h) // 2
            for i, line in enumerate(lines):
                _draw_centered_line(line, y0 + i * line_h)
        else:
            for i in range(visible_lines):
                idx = scroll_offset + i
                if idx >= len(lines):
                    break
                _draw_centered_line(lines[idx], CONTENT_TOP + i * line_h)
            if scroll_offset > 0:
                _draw_scroll_marker(CONTENT_TOP, up=True)
            if scroll_offset < max_offset:
                _draw_scroll_marker(CONTENT_BOTTOM - 2, up=False)
    ui.footer("BACK:quit  RIGHT:joke")
    badge.oled_show()


def fetch():
    """Return (text, is_error). text is the joke body on success, or a
    human-readable diagnostic on failure."""
    gc.collect()
    try:
        body = badge.http_get(URL)
    except Exception as exc:
        return "Fetch raised: " + str(exc), True
    if not body:
        return "Server returned empty body", True
    # The C HTTP layer signals failure with {"ok":false,"error":"…"}. Anything
    # else is the upstream response body (plain text dad joke for this URL).
    if body[:1] == "{" and '"ok":false' in body:
        try:
            data = json.loads(body)
            reason = data.get("error") or "unknown"
        except Exception:
            reason = body
        return str(reason), True
    return body.strip(), False


def wait(text, is_error):
    """Return "exit" on BACK or "next" on RIGHT. Joystick up/down scrolls
    the text while waiting; the wait is otherwise indefinite."""
    _, _, _, max_offset = layout(text)
    scroll = 0
    last_shown_scroll = -1
    next_scroll_ms = 0
    while True:
        if badge.button_pressed(badge.BTN_BACK):
            return "exit"
        if badge.button_pressed(badge.BTN_RIGHT):
            return "next"
        now = time.ticks_ms()
        if max_offset > 0 and time.ticks_diff(now, next_scroll_ms) >= 0:
            y_dir = read_axis(badge.joy_y())
            if y_dir < 0 and scroll > 0:
                scroll -= 1
                next_scroll_ms = time.ticks_add(now, SCROLL_REPEAT_MS)
            elif y_dir > 0 and scroll < max_offset:
                scroll += 1
                next_scroll_ms = time.ticks_add(now, SCROLL_REPEAT_MS)
        if scroll != last_shown_scroll:
            draw(text, is_error, scroll_offset=scroll)
            last_shown_scroll = scroll
        time.sleep_ms(POLL_MS)


def main():
    text = "Press > for a joke"
    is_error = False
    draw(text, is_error)
    while True:
        if wait(text, is_error) == "exit":
            return
        draw(text, is_error, status="Fetching...")
        text, is_error = fetch()
        if not text:
            text = "(no content)"
            is_error = True
        draw(text, is_error)


run_app("Dad Jokes", main)
