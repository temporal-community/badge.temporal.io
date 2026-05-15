"""IR Playground shared UI + helpers.

Owns:
  - IrSession: ir_start / mode-set / restore context manager.
  - chrome(title): a custom 9 px top status bar that replaces the native
    chrome on every IR Playground screen. Shows TX label on the left,
    a 2-segment activity bar in the centre (driven by C-side
    ir_activity() so it flashes in lock-step with the actual carrier),
    RX label on the right, and a 3-letter mode pill in the lower-right
    corner.
  - footer(): semantic-token hint row pinned to the bottom 9 px.
  - grid_menu(items, ...): blocking 2x2 grid menu that visually mirrors
    the native GridMenuScreen (62x18 cells, rounded corners, icon left,
    label right) but lives entirely in MicroPython.
  - list_menu(...): big-text vertical picker for sub-mode pickers.
  - Text-size hierarchy (small/big/huge) saved in NVS via kv_put.
  - Codebook accessor (load TV/Audio/Projector vendor tables on demand).
  - NEC + raw send wrappers that auto-update the TX label.

Every sub-app should:
  - import this module;
  - wrap its main loop with `IrSession(mode)`;
  - call `chrome(title)` + `footer(...)` once per render to draw the
    standard frame around its body;
  - use `set_tx_label(...)` / `set_rx_label(...)` (or the convenience
    `nec_send_with_label(...)` helper) so the status bar always reflects
    the latest action.
"""

import gc
import os
import struct
import sys
import time

from badge import *


# ── Constants ───────────────────────────────────────────────────────────────

SCREEN_W = 128
SCREEN_H = 64

# Top status bar (8 px tall, rule pixels sit on y=8 — matches the native
# header so cells land at the canonical kGridY = 11).
BAR_TOP = 0
BAR_H = 8
BAR_BOTTOM = BAR_TOP + BAR_H  # = 8
BAR_RULE_Y = BAR_BOTTOM        # rule lives on y=8

# Footer (rule on y=54, action chips baseline y=63) — native GridMenuScreen
# convention from firmware/src/ui/OLEDLayout.h.
FOOTER_RULE_Y = 54
FOOTER_BASE_Y = 54

# Body region — matches the native screens' kContentY = 10. List rows use
# the native primitive (which adds its own row math); other body content
# (big_message hero text, sub-app diagnostic readouts) starts here.
BODY_TOP = 10
BODY_BOTTOM = FOOTER_RULE_Y - 1  # = 53
BODY_H = BODY_BOTTOM - BODY_TOP + 1
LIST_ROW_H = 11                  # kRowHeight in SettingsScreen.cpp /
                                  # WifiScreen.cpp / native pattern

# Activity bar geometry inside the top bar.
# Layout:  [ TX:label   ][TX seg|RX seg][   RX:label  mod ]
#          left text     centre 16 px   right text + mode pill
ACTIVITY_W = 16
ACTIVITY_X = (SCREEN_W - ACTIVITY_W) // 2
ACTIVITY_Y = 1
ACTIVITY_H = 5
ACTIVITY_GAP = 2  # pixels between the two halves
ACTIVITY_HALF = (ACTIVITY_W - ACTIVITY_GAP) // 2

# How long an event is considered "active" for the bar (ms).
ACTIVITY_HOLD_MS = 120

# Two-tier font system, fixed sizes:
#   - LABEL_FONT (Smallsimple, 5x7) for the chrome / footer hint row.
#     Matches the native UIFonts::kText so the chrome text aligns
#     with everything else the badge draws.
#   - BODY_FONT (6x10) for sub-app body text. Slightly larger than
#     Smallsimple but still tight; same font breaksnake / flappy use.
#   - HERO_FONT (9x15) for the one big readout per screen (RTT,
#     captured hex, AC mode).
#
# Hardcoded line heights so body/hero placement doesn't depend on
# oled_text_height() being available — every screen lands at the
# same place regardless of where it is in the call stack.
#
# IMPORTANT: the badge's oled_set_cursor(x, y) treats `y` as the TOP of
# the glyph, not the baseline (see firmware/src/hardware/oled.cpp:240,
# which adds the font ascender internally). Every coordinate below is a
# glyph-top, never a baseline.
LABEL_FONT = "Smallsimple"
BODY_FONT = "6x10"
HERO_FONT = "9x15"

LABEL_LINE_H = 9   # Smallsimple row pitch (5x7 + 2 leading)
BODY_LINE_H = 12   # 6x10 row pitch
HERO_LINE_H = 16   # 9x15 row pitch

LABEL_TOP_Y = 1        # chrome label text top — glyph y=1..7, rule at 8
BODY_FIRST_TOP_Y = 11  # first body row top — glyph y=11..20
HERO_TOP_Y = 24        # hero text top — 9x15 glyph y=24..38, centred in body
SUBLINE_TOP_Y = 43     # subline text top — 6x10 glyph y=43..52, just above rule

# Backward-compat aliases — earlier rev used *_BASE_Y names. Sub-apps
# referencing these still work; new code should use the *_TOP_Y names.
LABEL_BASE_Y = LABEL_TOP_Y
BODY_FIRST_BASE_Y = BODY_FIRST_TOP_Y
HERO_BASE_Y = HERO_TOP_Y

# Storage
SLOTS_DIR = "/ir_remotes"


# ── Mode session ────────────────────────────────────────────────────────────


class IrSession:
    """Bring IR up, switch mode, restore on exit. Context manager."""

    def __init__(self, mode="badge", power=None):
        self.mode = mode
        self.power = power
        self._prev_mode = None
        self._prev_power = None
        self._started = False

    def __enter__(self):
        ir_start()
        self._started = True
        try:
            self._prev_mode = ir_get_mode()
        except Exception:
            self._prev_mode = None
        try:
            self._prev_power = ir_tx_power()
        except Exception:
            self._prev_power = None
        try:
            ir_set_mode(self.mode)
        except Exception:
            pass
        if self.power is not None:
            try:
                ir_tx_power(int(self.power))
            except Exception:
                pass
        ir_flush()
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            ir_flush()
        except Exception:
            pass
        if self._prev_power is not None:
            try:
                ir_tx_power(self._prev_power)
            except Exception:
                pass
        if self._prev_mode is not None:
            try:
                ir_set_mode(self._prev_mode)
            except Exception:
                pass
        if self._started:
            try:
                ir_stop()
            except Exception:
                pass
        return False


# ── Status-bar label state ──────────────────────────────────────────────────

_tx_label = ""
_tx_label_t = 0
_rx_label = ""
_rx_label_t = 0
_LABEL_FADE_MS = 4000


def set_tx_label(text):
    global _tx_label, _tx_label_t
    _tx_label = str(text)[:14]
    _tx_label_t = time.ticks_ms()


def set_rx_label(text):
    global _rx_label, _rx_label_t
    _rx_label = str(text)[:14]
    _rx_label_t = time.ticks_ms()


def auto_rx_label(addr=None, cmd=None, repeat=False, raw_pairs=None, words=None):
    """Format an RX label from typical frame payloads.

    Sub-apps can call this from their RX path to get a uniform display
    instead of hand-formatting every time."""
    # Keep labels short — chrome bar's RX zone only fits ~8 chars at
    # the standard label font.
    if raw_pairs is not None:
        set_rx_label("%dp" % raw_pairs)
        return
    if words is not None:
        # Just the leading hex word — pair count + first word would
        # overflow the chrome bar.
        set_rx_label("%08X" % (words[0] & 0xFFFFFFFF))
        return
    if addr is not None and cmd is not None:
        suffix = "R" if repeat else ""
        set_rx_label("%02X/%02X%s" % (addr & 0xFF, cmd & 0xFF, suffix))


def clear_labels():
    global _tx_label, _rx_label, _tx_label_t, _rx_label_t
    _tx_label = ""
    _rx_label = ""
    _tx_label_t = 0
    _rx_label_t = 0


# ── Font selection ──────────────────────────────────────────────────────────


def use_font(role):
    """Set the OLED font for one of the three semantic roles.
    `role` is one of 'label' (chrome), 'body' (most text), 'hero' (big
    one-liner readout). Falls back to Smallsimple if the named font
    isn't registered (older firmware)."""
    name = LABEL_FONT
    if role == "body":
        name = BODY_FONT
    elif role == "hero":
        name = HERO_FONT
    try:
        oled_set_font(name)
    except Exception:
        try:
            oled_set_font(LABEL_FONT)
        except Exception:
            pass


def line_height(role):
    if role == "hero":
        return HERO_LINE_H
    if role == "body":
        return BODY_LINE_H
    return LABEL_LINE_H


# ── Pixel helpers ───────────────────────────────────────────────────────────


def _hline(x, y, w):
    oled_draw_box(int(x), int(y), int(w), 1)


def _vline(x, y, h):
    oled_draw_box(int(x), int(y), 1, int(h))


def _fill(x, y, w, h, color=1):
    if color == 0:
        oled_set_draw_color(0)
        oled_draw_box(int(x), int(y), int(w), int(h))
        oled_set_draw_color(1)
    else:
        oled_draw_box(int(x), int(y), int(w), int(h))


def _frame(x, y, w, h):
    _hline(x, y, w)
    _hline(x, y + h - 1, w)
    _vline(x, y, h)
    _vline(x + w - 1, y, h)


def _rounded_frame(x, y, w, h, r=2):
    """Draws a rounded rectangle by clipping the corner pixels."""
    _frame(x, y, w, h)
    if r <= 0:
        return
    # Erase the four corners with black pixels so rounded look survives
    # when the cell is selected (filled). Cheap and good enough at r=2.
    oled_set_draw_color(0)
    for c in range(r):
        for k in range(r - c):
            oled_set_pixel(x + c, y + k, 0)
            oled_set_pixel(x + w - 1 - c, y + k, 0)
            oled_set_pixel(x + c, y + h - 1 - k, 0)
            oled_set_pixel(x + w - 1 - c, y + h - 1 - k, 0)
    oled_set_draw_color(1)


def _text_w(s):
    try:
        return oled_text_width(str(s))
    except Exception:
        return len(str(s)) * 6


def _print_at(x, y, s, color=1):
    if color == 0:
        oled_set_draw_color(0)
    oled_set_cursor(int(x), int(y))
    oled_print(str(s))
    if color == 0:
        oled_set_draw_color(1)


def _fit(s, max_w):
    s = str(s)
    while s and _text_w(s) > max_w:
        s = s[:-1]
    return s


# ── Top status bar ──────────────────────────────────────────────────────────


def _activity_state():
    """Returns (tx_active, rx_active). C-side ir_activity returns
    (ms_since_tx_or_None, ms_since_rx_or_None)."""
    try:
        tx_ms, rx_ms = ir_activity()
    except Exception:
        return (False, False)
    tx = tx_ms is not None and tx_ms < ACTIVITY_HOLD_MS
    rx = rx_ms is not None and rx_ms < ACTIVITY_HOLD_MS
    return (tx, rx)


def _draw_activity():
    tx, rx = _activity_state()
    # frames around each half
    _frame(ACTIVITY_X, ACTIVITY_Y, ACTIVITY_HALF, ACTIVITY_H)
    _frame(ACTIVITY_X + ACTIVITY_HALF + ACTIVITY_GAP,
           ACTIVITY_Y, ACTIVITY_HALF, ACTIVITY_H)
    if tx:
        _fill(ACTIVITY_X + 1, ACTIVITY_Y + 1,
              ACTIVITY_HALF - 2, ACTIVITY_H - 2)
    if rx:
        _fill(ACTIVITY_X + ACTIVITY_HALF + ACTIVITY_GAP + 1,
              ACTIVITY_Y + 1, ACTIVITY_HALF - 2, ACTIVITY_H - 2)


def _mode_glyph():
    try:
        m = ir_get_mode()
    except Exception:
        m = "badge"
    return {"badge": "BAD", "nec": "NEC", "raw": "RAW"}.get(m, "?")


def _draw_status_bar(title=None):
    use_font("label")

    now = time.ticks_ms()
    tx_text = _tx_label if (_tx_label and time.ticks_diff(now, _tx_label_t) < _LABEL_FADE_MS) else ""
    rx_text = _rx_label if (_rx_label and time.ticks_diff(now, _rx_label_t) < _LABEL_FADE_MS) else ""

    # Left zone — TX label or fallback to the screen title.
    left = "TX:" + tx_text if tx_text else (title or "")
    left_max = ACTIVITY_X - 2
    _print_at(0, 1, _fit(left, left_max))

    # Centre — activity bar.
    _draw_activity()

    # Right zone — RX label, then mode pill flush right.
    mode = _mode_glyph()
    mode_w = _text_w(mode)
    mode_x = SCREEN_W - mode_w
    _print_at(mode_x, 1, mode)
    rx_zone_x = ACTIVITY_X + ACTIVITY_W + 2
    rx_zone_w = mode_x - rx_zone_x - 2
    if rx_zone_w > 0:
        text = "RX:" + rx_text if rx_text else ""
        text = _fit(text, rx_zone_w)
        _print_at(rx_zone_x, 1, text)

    # Hairline rule under the bar.
    _hline(0, BAR_RULE_Y, SCREEN_W)


def chrome(title=None):
    """Draw the standard IR Playground top bar. Call once per render."""
    _draw_status_bar(title)


# ── Footer (semantic-token hint row) ────────────────────────────────────────


def footer(*hints):
    """Bottom hint row using semantic tokens. Each hint is either a
    'token:label' string or a (token, label) tuple. Tokens auto-glyph
    (Confirm/Cancel/Back/Up/Down/^/v/</>/...). Project rule: NEVER write
    the bare letters X / Y / A / B in user-facing copy."""
    _hline(0, FOOTER_RULE_Y, SCREEN_W)
    use_font("label")

    parts = []
    for h in hints:
        if h is None:
            continue
        if isinstance(h, (tuple, list)):
            parts.append("%s:%s" % (h[0], h[1]) if len(h) > 1 else str(h[0]))
        else:
            parts.append(str(h))
    if not parts:
        return
    text = "  ".join(parts)
    try:
        ui_inline_hint(0, FOOTER_BASE_Y, text)
    except Exception:
        # Fall back to plain text if the native glyph hint isn't there.
        _print_at(0, FOOTER_BASE_Y + 3, text)


# ── Grid menu (native renderer via ui_grid_cell + ui_grid_footer) ───────────

GRID_COLS = 2
GRID_ROWS = 2


def _grid_render(items, cursor, title):
    """Draw the 2x2 menu with the native cell + footer primitives. The
    only Python-side rendering is our custom TX/RX status bar at the top
    (which lives in the same 8 px band the native chrome would use)."""
    oled_clear()
    chrome(title)
    visible = items[:GRID_COLS * GRID_ROWS]
    for i, item in enumerate(visible):
        col = i % GRID_COLS
        row = i // GRID_COLS
        ui_grid_cell(col, row, item.get("label", "?"),
                     i == cursor, item.get("icon", "apps"))
    desc = ""
    if 0 <= cursor < len(visible):
        desc = visible[cursor].get("desc", "")
    ui_grid_footer(desc)
    oled_show()


def grid_menu_with_cursor(items, title, initial_cursor=0):
    """Same as grid_menu but accepts a starting cursor and returns
    (chosen_index, cursor_after) so callers can preserve cursor across
    re-entries (e.g. send-press-rinse-repeat workflows)."""
    items = items[:GRID_COLS * GRID_ROWS]
    cursor = max(0, min(initial_cursor, len(items) - 1))
    last_dir = (0, 0)
    last_step = time.ticks_ms()
    last_render_t = 0
    _grid_render(items, cursor, title)

    while True:
        if button_pressed(BTN_BACK):
            return -1, cursor
        if button_pressed(BTN_CONFIRM):
            return cursor, cursor

        now = time.ticks_ms()
        if time.ticks_diff(now, last_render_t) > 80:
            _grid_render(items, cursor, title)
            last_render_t = now

        dx = _stick_x()
        dy = _stick_y()
        if (dx, dy) != (0, 0) and ((dx, dy) != last_dir or
                                    time.ticks_diff(now, last_step) > 220):
            new_cursor = cursor
            if dx == -1 and (cursor % GRID_COLS) > 0:
                new_cursor = cursor - 1
            elif dx == 1 and (cursor % GRID_COLS) < (GRID_COLS - 1) and cursor + 1 < len(items):
                new_cursor = cursor + 1
            elif dy == -1 and cursor >= GRID_COLS:
                new_cursor = cursor - GRID_COLS
            elif dy == 1 and cursor + GRID_COLS < len(items):
                new_cursor = cursor + GRID_COLS
            if new_cursor != cursor:
                cursor = new_cursor
                _grid_render(items, cursor, title)
                last_render_t = now
            last_dir = (dx, dy)
            last_step = now
        if (dx, dy) == (0, 0):
            last_dir = (0, 0)
        time.sleep_ms(30)


def grid_menu(items, title="IR Playground"):
    """Convenience wrapper for grid_menu_with_cursor that drops the
    follow-up cursor — for one-shot menus that don't repeat."""
    idx, _ = grid_menu_with_cursor(items, title, initial_cursor=0)
    return idx


# ── List menu (native renderer via ui_list_row) ────────────────────────────


def list_menu_with_cursor(items, title=None, hint_label="open",
                            initial_cursor=0):
    """Stateful single-column picker. Returns (chosen_index, cursor_after)
    where chosen_index is -1 on Back. Pass cursor_after as initial_cursor
    on the next call to keep the user's position across repeated picks
    (essential for IR send-and-repeat workflows)."""
    if not items:
        return -1, 0

    try:
        rows_visible = max(1, ui_list_rows_visible())
    except Exception:
        rows_visible = 4

    cursor = max(0, min(initial_cursor, len(items) - 1))
    last_dir = 0
    last_step = time.ticks_ms()

    def _draw():
        oled_clear()
        chrome(title)
        # Window the list around the cursor — keep the cursor on-screen,
        # showing one row of context above when possible.
        if len(items) <= rows_visible:
            top = 0
        else:
            top = cursor - 1
            if top < 0:
                top = 0
            if top + rows_visible > len(items):
                top = len(items) - rows_visible
        for i in range(rows_visible):
            idx = top + i
            if idx >= len(items):
                break
            label = items[idx].get("label", "")
            ui_list_row(i, str(label), idx == cursor)
        footer(("Up/Down", "pick"), ("Confirm", hint_label),
                ("Back", "back"))
        oled_show()

    last_render_t = 0
    _draw()
    while True:
        now = time.ticks_ms()
        if button_pressed(BTN_BACK):
            return -1, cursor
        if button_pressed(BTN_CONFIRM):
            return cursor, cursor

        dy = 0
        if button_pressed(BTN_UP):
            dy = -1
        elif button_pressed(BTN_DOWN):
            dy = 1
        else:
            dy = _stick_y()

        if dy != 0 and (dy != last_dir or
                          time.ticks_diff(now, last_step) > 200):
            cursor = (cursor + dy) % len(items)
            _draw()
            last_render_t = now
            last_dir = dy
            last_step = now
        if dy == 0:
            last_dir = 0

        # Keep the activity bar alive even when the cursor isn't moving.
        if time.ticks_diff(now, last_render_t) > 80:
            _draw()
            last_render_t = now

        time.sleep_ms(30)


def list_menu(items, title=None, hint_label="open"):
    """One-shot list menu — convenience wrapper that drops the cursor
    (for callers that don't need the keep-cursor behavior)."""
    idx, _ = list_menu_with_cursor(items, title, hint_label,
                                     initial_cursor=0)
    return idx


# ── Stick helpers ───────────────────────────────────────────────────────────


def _stick_x():
    x = joy_x()
    if x < 1100:
        return -1
    if x > 3000:
        return 1
    return 0


def _stick_y():
    y = joy_y()
    if y < 1100:
        return -1
    if y > 3000:
        return 1
    return 0


# ── Big-message helper ──────────────────────────────────────────────────────


def wait_button(buttons, timeout_ms=None):
    """Block until any of `buttons` is pressed (iterable of BTN_* ids).
    Returns the pressed button id, or None on timeout."""
    if not isinstance(buttons, (tuple, list)):
        buttons = (buttons,)
    start = time.ticks_ms()
    while True:
        for b in buttons:
            if button_pressed(b):
                return b
        if timeout_ms is not None and time.ticks_diff(time.ticks_ms(), start) > timeout_ms:
            return None
        time.sleep_ms(25)


def wait_yes_no():
    """Block on Confirm (yes) / Back (no). Returns True/False."""
    while True:
        if button_pressed(BTN_CONFIRM):
            return True
        if button_pressed(BTN_BACK):
            return False
        time.sleep_ms(25)


def draw_hero(text):
    """Draw the screen's one big readout, centered horizontally, glyph
    top at HERO_TOP_Y. Use for the dominant element on any sub-app
    screen so every hero lands at the same vertical position."""
    use_font("hero")
    s = _fit(str(text), SCREEN_W - 4)
    w = _text_w(s)
    x = max(0, (SCREEN_W - w) // 2)
    _print_at(x, HERO_TOP_Y, s)


def draw_subline(text, y=None):
    """Draw a single supporting line under the hero, centered, in the
    body font. Glyph top defaults to SUBLINE_TOP_Y (= 43), so the 10 px
    glyph sits at y=43..52 with a 1 px gap before the footer rule."""
    if not text:
        return
    use_font("body")
    s = _fit(str(text), SCREEN_W - 4)
    w = _text_w(s)
    x = max(0, (SCREEN_W - w) // 2)
    if y is None:
        y = SUBLINE_TOP_Y
    _print_at(x, y, s)


def draw_body_line(row, text, x=2):
    """Draw a body-font row at the given 0-based body row index. row=0
    is the first line below the chrome rule. Up to 3 rows fit (0..2)
    above the subline; row 3 will collide with draw_subline / footer."""
    use_font("body")
    s = _fit(str(text), SCREEN_W - x - 2)
    _print_at(x, BODY_FIRST_TOP_Y + row * BODY_LINE_H, s)


def big_message(title, body=None, hero=None, hint_actions=None):
    """One-shot screen with a single hero line + optional body subline.
    Identical geometry to draw_hero / draw_subline so any screen that
    composes those primitives reads the same as one rendered through
    big_message()."""
    oled_clear()
    chrome(title)
    if hero is not None:
        draw_hero(hero)
    if body:
        draw_subline(body)
    if hint_actions:
        footer(*hint_actions)
    else:
        footer(("Confirm", "ok"), ("Back", "back"))
    oled_show()


# ── NEC convenience helpers ────────────────────────────────────────────────


def nec_send(addr, cmd, repeats=0, label=None):
    """Send a NEC frame and update the TX status label.
    Returns True on success.

    The default label is just the raw "addr/cmd" hex (e.g. "07/02"),
    which fits the chrome bar's TX zone. Callers should pass `label=`
    only when the data is too cryptic on its own — and even then keep
    the value short (≤ 7 chars) so it doesn't overflow into the
    activity indicator."""
    if label is not None:
        set_tx_label(label)
    else:
        set_tx_label("%02X/%02X" % (addr & 0xFF, cmd & 0xFF))
    try:
        ir_nec_send(int(addr) & 0xFF, int(cmd) & 0xFF, int(repeats))
        return True
    except Exception:
        return False


def raw_send(buf, carrier_hz=38000, label=None):
    """Send a raw symbol buffer. Default label = pair count + carrier."""
    if label is not None:
        set_tx_label(label)
    else:
        set_tx_label("%dp" % (len(buf) // 4))
    try:
        ir_raw_send(buf, int(carrier_hz))
        return True
    except Exception:
        return False


def nec_read():
    try:
        return ir_nec_read()
    except Exception:
        return None


def raw_capture_blocking(timeout_ms=2000):
    deadline = time.ticks_add(time.ticks_ms(), int(timeout_ms))
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        try:
            buf = ir_raw_capture()
        except Exception:
            buf = None
        if buf:
            return buf
        time.sleep_ms(20)
    return None


# ── Slot persistence (universal remote layouts) ─────────────────────────────


def ensure_slots_dir():
    try:
        os.stat(SLOTS_DIR)
    except OSError:
        try:
            os.mkdir(SLOTS_DIR)
        except OSError:
            pass


def _slot_path(name):
    return SLOTS_DIR + "/" + name + ".txt"


def list_layouts():
    ensure_slots_dir()
    out = []
    try:
        for entry in os.listdir(SLOTS_DIR):
            if entry.endswith(".txt"):
                out.append(entry[:-4])
    except OSError:
        pass
    out.sort()
    return out


def _hex(buf):
    return "".join("{:02x}".format(b) for b in buf)


def _unhex(s):
    out = bytearray()
    for i in range(0, len(s), 2):
        out.append(int(s[i:i + 2], 16))
    return bytes(out)


def save_layout(name, slots):
    ensure_slots_dir()
    lines = []
    for slot_id, value in slots.items():
        kind = value.get("kind", "")
        if kind == "nec":
            lines.append("%s|nec|%d|%d|%d" % (
                slot_id,
                int(value.get("addr", 0)) & 0xFF,
                int(value.get("cmd", 0)) & 0xFF,
                int(value.get("repeats", 0)),
            ))
        elif kind == "raw":
            blob = value.get("data") or b""
            carrier = int(value.get("carrier_hz", 38000))
            lines.append("%s|raw|%d|%s" % (slot_id, carrier, _hex(blob)))
    try:
        with open(_slot_path(name), "w") as fh:
            fh.write("\n".join(lines) + "\n")
        return True
    except OSError:
        return False


def load_layout(name):
    out = {}
    try:
        with open(_slot_path(name), "r") as fh:
            data = fh.read()
    except OSError:
        return out
    for line in data.split("\n"):
        line = line.strip()
        if not line:
            continue
        parts = line.split("|")
        slot = parts[0]
        kind = parts[1] if len(parts) > 1 else ""
        if kind == "nec" and len(parts) >= 5:
            out[slot] = {
                "kind": "nec",
                "addr": int(parts[2]),
                "cmd": int(parts[3]),
                "repeats": int(parts[4]),
            }
        elif kind == "raw" and len(parts) >= 4:
            out[slot] = {
                "kind": "raw",
                "carrier_hz": int(parts[2]),
                "data": _unhex(parts[3]),
            }
    return out


def delete_layout(name):
    try:
        os.remove(_slot_path(name))
    except OSError:
        pass


# ── Codebook accessor (lazy import — saves ~3 KB heap until used) ──────────


def codebook(category):
    """Return the imported vendor table for `category` (tv/audio/projector/ac).
    Returns the module's VENDORS tuple or () if missing."""
    try:
        if category == "tv":
            from data import tv_codes
            return tv_codes.VENDORS
        if category == "audio":
            from data import audio_codes
            return audio_codes.VENDORS
        if category == "projector":
            from data import projector_codes
            return projector_codes.VENDORS
        if category == "ac":
            from data import ac_codes
            return ac_codes.VENDORS
        if category == "tvbgone":
            from data import tvbgone_codes
            return tvbgone_codes.CODES
    except Exception:
        return ()
    return ()


def emit_remote_button(payload, label=None):
    """Transmit one button entry from a codebook (kind|payload tuple).

    payload formats:
      ('nec', addr, cmd, repeats)
      ('raw', bytes, carrier_hz)

    Returns True on success."""
    if not payload:
        return False
    kind = payload[0]
    if kind == "nec":
        return nec_send(payload[1], payload[2], payload[3] if len(payload) > 3 else 0,
                        label=label)
    if kind == "raw":
        carrier = payload[2] if len(payload) > 2 else 38000
        return raw_send(payload[1], carrier_hz=carrier, label=label)
    return False
