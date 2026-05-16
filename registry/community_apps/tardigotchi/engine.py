import time
import random
import json
import badge_ui as ui
from badge import (
    oled_clear, oled_show, oled_print, oled_println,
    oled_set_cursor, oled_set_pixel, oled_draw_box,
    oled_set_draw_color, oled_text_width,
    ui_header, ui_action_bar, ui_chrome,
    button_pressed, BTN_CONFIRM, BTN_BACK, BTN_CROSS,
    BTN_RIGHT, BTN_DOWN, BTN_LEFT, BTN_UP,
    led_set_frame, led_clear, led_override_begin, led_override_end,
    haptic_pulse, joy_x, joy_y,
)
from badge_app import (
    ButtonLatch,
    DualScreenSession,
    GCTicker,
    load_score,
    save_score,
    read_stick_4way,
    wait_choice,
    with_led_override,
    clamp,
)

# Tardigotchi state lives in NVS via the badge.kv API so it survives
# every flash type (firmware OTA, factory fatfs.bin reflash, Community
# Apps install). The legacy /tardigrade_save.json file is migrated on
# first load_state() and then deleted. See firmware/docs/STORAGE-MODEL.md.
SAVE_KEY = "tardi_save"
LEGACY_SAVE_PATH = "/tardigrade_save.json"

from icon import (
    led_frame_for_mood, led_frame_for_action, led_glider_frame,
    get_level, get_unlocked_patterns,
    PAT_GLIDER, LEVEL_UNLOCKS,
    draw_oled_tardi,
)

# ---- Inline Conway GoL (uses already-imported badge funcs) ----
GW = 32
GH = 28


def make_grid():
    return [0] * GH


def gol_stamp(grid, pattern, oy, ox):
    for dr, dc in pattern:
        r = (oy + dr) % GH
        c = (ox + dc) % GW
        grid[r] |= (1 << c)


def gol_step(grid):
    new = [0] * GH
    for y in range(GH):
        above = grid[(y - 1) % GH]
        cur = grid[y]
        below = grid[(y + 1) % GH]
        for x in range(GW):
            xl = (x - 1) % GW
            xr = (x + 1) % GW
            bxl = 1 << xl
            bx = 1 << x
            bxr = 1 << xr
            n = 0
            if above & bxl: n += 1
            if above & bx: n += 1
            if above & bxr: n += 1
            if cur & bxl: n += 1
            if cur & bxr: n += 1
            if below & bxl: n += 1
            if below & bx: n += 1
            if below & bxr: n += 1
            if n == 3 or (n == 2 and (cur & bx)):
                new[y] |= bx
    return new


def gol_draw(grid, ox, oy):
    oled_set_draw_color(1)
    for y in range(GH):
        row = grid[y]
        if not row:
            continue
        py = oy + y * 2
        run = -1
        for x in range(GW):
            if row & (1 << x):
                if run < 0:
                    run = x
            else:
                if run >= 0:
                    oled_draw_box(ox + run * 2, py, (x - run) * 2, 2)
                    run = -1
        if run >= 0:
            oled_draw_box(ox + run * 2, py, (GW - run) * 2, 2)


MENU_ITEMS = ["Feed", "Pet", "Play", "Field", "Stats", "Name"]
MENU_COUNT = len(MENU_ITEMS)


def flush_buttons():
    """Consume stale edge-triggered presses so they don't ghost."""
    button_pressed(BTN_CONFIRM)
    button_pressed(BTN_BACK)
    button_pressed(BTN_UP)
    button_pressed(BTN_DOWN)
    button_pressed(BTN_LEFT)
    button_pressed(BTN_RIGHT)
    button_pressed(BTN_CROSS)

FEED_MSGS = [
    "Munches on moss!",
    "Yum! Microbes!",
    "Nom nom nom...",
    "Tasty algae!",
]

PET_MSGS = [
    "Wiggles happily!",
    "Tiny dance!",
    "Curls up content",
    "Legs wiggle!",
]

PLAY_MSGS = [
    "Zero-G float!",
    "Barrel roll!",
    "Space trip!",
    "Hide & seek!",
    "Frozen prank!",
]


def default_state():
    return {
        "name": "Ziggy",
        "happiness": 50,
        "hunger": 50,
        "age_secs": 0,
        "last_tick_ms": time.ticks_ms(),
        "last_frame": 0,
        "xp": 0,
    }


def _load_legacy():
    """One-shot migration from the old FATFS save file."""
    try:
        import os, badge
        with open(LEGACY_SAVE_PATH, "r") as f:
            data = json.load(f)
        try:
            badge.kv_put(SAVE_KEY, json.dumps(data))
        except Exception:
            pass
        try:
            os.remove(LEGACY_SAVE_PATH)
        except OSError:
            pass
        return data
    except (OSError, ValueError):
        return None


def load_state():
    defaults = default_state()
    data = None
    try:
        import badge
        raw = badge.kv_get(SAVE_KEY, None)
        if raw:
            data = json.loads(raw)
    except (ValueError, AttributeError):
        data = None
    if data is None:
        data = _load_legacy()
    if data is None:
        d = defaults
        d["_session_ms"] = time.ticks_ms()
        return d

    for k in defaults:
        if k not in data:
            data[k] = defaults[k]
    if "birth_ms" in data and "age_secs" not in data:
        data["age_secs"] = 0
    data.pop("birth_ms", None)
    data["happiness"] = clamp(data["happiness"], 0, 100)
    data["hunger"] = clamp(data["hunger"], 0, 100)
    data["_session_ms"] = time.ticks_ms()
    return data


def save_state(state):
    # Update cumulative age from session elapsed time
    now = time.ticks_ms()
    session_elapsed = time.ticks_diff(now, state.get("_session_ms", now))
    state["age_secs"] = state.get("age_secs", 0) + max(0, session_elapsed) // 1000
    state["_session_ms"] = now  # reset session counter
    try:
        import badge
        save = {k: v for k, v in state.items() if not k.startswith("_")}
        badge.kv_put(SAVE_KEY, json.dumps(save))
    except (OSError, TypeError):
        pass


def age_display(state):
    # Cumulative saved age + current session elapsed
    saved = state.get("age_secs", 0)
    session_ms = time.ticks_diff(time.ticks_ms(), state.get("_session_ms", time.ticks_ms()))
    secs = saved + max(0, session_ms) // 1000
    if secs < 60:
        return str(secs) + "s"
    mins = secs // 60
    if mins < 60:
        return str(mins) + "m"
    hours = mins // 60
    rm = mins % 60
    if hours < 24:
        return str(hours) + "h" + str(rm) + "m"
    days = hours // 24
    rh = hours % 24
    return str(days) + "d" + str(rh) + "h"


def mood(state):
    score = state["happiness"] - state["hunger"] // 2
    if score > 60:
        return "ecstatic"
    elif score > 30:
        return "happy"
    elif score > 0:
        return "okay"
    elif score > -20:
        return "sad"
    else:
        return "sleeping"


def mood_led(m):
    return led_frame_for_mood(m)


def draw_bar(x, y, w, value, max_val=100):
    oled_set_draw_color(1)
    oled_draw_box(x, y, w, 5)
    oled_set_draw_color(0)
    oled_draw_box(x + 1, y + 1, w - 2, 3)
    oled_set_draw_color(1)
    fill_w = max(0, (value * (w - 2)) // max_val)
    if fill_w > 0:
        oled_draw_box(x + 1, y + 1, fill_w, 3)


def init_gol(state):
    grid = make_grid()
    gol_stamp(grid, PAT_GLIDER, 1, 1)
    xp = state.get("xp", 0)
    patterns = get_unlocked_patterns(xp)
    for pat, name in patterns:
        if pat is PAT_GLIDER:
            continue
        gol_stamp(grid, pat, random.randint(2, GH - 8), random.randint(4, GW - 8))
    return grid


MENU_H = 7
CONTENT_Y = MENU_H + 8


def _menu_x(i):
    """Pixel-perfect left edge for menu item i (0..MENU_COUNT)."""
    return (128 * i) // MENU_COUNT


def draw_menu_bar(sel):
    """Draw menu bar at y=0..MENU_H-1. Selected = white box + black text."""
    ui.font()
    oled_set_draw_color(0)
    oled_draw_box(0, 0, 128, MENU_H)
    for i in range(MENU_COUNT):
        x0 = _menu_x(i)
        x1 = _menu_x(i + 1)
        w = x1 - x0
        tw = oled_text_width(MENU_ITEMS[i])
        tx = x0 + (w - tw) // 2
        if i == sel:
            oled_set_draw_color(1)
            oled_draw_box(x0, 0, w, MENU_H)
            oled_set_draw_color(0)
            oled_set_cursor(tx, MENU_H - 1)
            oled_print(MENU_ITEMS[i])
        else:
            oled_set_draw_color(1)
            oled_set_cursor(tx, MENU_H - 1)
            oled_print(MENU_ITEMS[i])
    oled_set_draw_color(1)


def draw_main_screen(state, sel, grid):
    oled_clear()
    ui.font()

    # Draw 1:1 pixel GoL background across full OLED below menu
    gol_draw(grid, 0, MENU_H + 1)

    lvl = get_level(state.get("xp", 0))

    # Draw tardigrade sprite below menu (skip top MENU_H rows)
    draw_oled_tardi(0, MENU_H, mood(state), lvl, hunger=state["hunger"], small=True)

    # Menu bar drawn AFTER sprite so it's never overwritten
    draw_menu_bar(sel)
    oled_set_draw_color(1)
    oled_set_cursor(64, CONTENT_Y + 2)
    oled_print(state["name"])
    oled_set_cursor(64, CONTENT_Y + 11)
    oled_print("Lv" + str(lvl + 1) + " " + mood(state))

    oled_set_cursor(64, CONTENT_Y + 20)
    oled_print("Joy")
    draw_bar(84, CONTENT_Y + 19, 42, state["happiness"])
    oled_set_cursor(64, CONTENT_Y + 29)
    oled_print("Hun")
    draw_bar(84, CONTENT_Y + 28, 42, state["hunger"])
    oled_set_cursor(64, CONTENT_Y + 38)
    oled_print(age_display(state))

    oled_set_draw_color(1)
    oled_show()


def draw_action_screen(state, msg, action=None):
    oled_clear()
    lvl = get_level(state.get("xp", 0))
    if action:
        draw_oled_tardi(0, 0, mood(state), lvl, action=action)
    ui.font()
    ui_header(state["name"])
    ui.center(28, msg)
    ui_action_bar("BACK", "done")
    oled_show()


# Grid keyboard layout: 10 columns x 4 rows
KB_ROWS = [
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZabcd",
    "efghij0123",
]
KB_COLS = 10
KB_ROW_COUNT = len(KB_ROWS)


def draw_name_screen(state, name_buf, kx, ky):
    oled_clear()
    ui_header("Name")

    # Current name with cursor
    name_str = "".join(name_buf) + "_"
    ui.center(12, name_str)

    # Draw keyboard grid
    cell_w = 12
    cell_h = 8
    grid_x = (128 - KB_COLS * cell_w) // 2
    grid_y = 22

    oled_set_draw_color(1)
    for ry in range(KB_ROW_COUNT):
        for cx in range(KB_COLS):
            px = grid_x + cx * cell_w
            py = grid_y + ry * cell_h
            ch = KB_ROWS[ry][cx]
            tw = oled_text_width(ch)
            tx = px + (cell_w - tw) // 2
            ty = py + cell_h
            if cx == kx and ry == ky:
                oled_draw_box(px, py + 1, cell_w, cell_h)
                oled_set_draw_color(0)
                oled_set_cursor(tx, ty)
                oled_print(ch)
                oled_set_draw_color(1)
            else:
                oled_set_cursor(tx, ty)
                oled_print(ch)

    ui_action_bar("BACK", "done", "RIGHT", "add")
    oled_show()


def draw_stats_screen(state):
    oled_clear()
    xp = state.get("xp", 0)
    lvl = get_level(xp)
    unlocked = get_unlocked_patterns(xp)
    ui_chrome(state["name"], "Lv" + str(lvl + 1), "BACK", "back")

    oled_set_cursor(4, 18)
    oled_print("Happiness: " + str(state["happiness"]) + "%")
    draw_bar(4, 24, 120, state["happiness"])

    oled_set_cursor(4, 32)
    oled_print("Hunger:    " + str(state["hunger"]) + "%")
    draw_bar(4, 38, 120, state["hunger"])

    oled_set_cursor(4, 46)
    oled_print("Age:" + age_display(state) + "  XP:" + str(xp))

    # Show unlocked creatures
    names = " ".join(n for _, n in unlocked)
    oled_set_cursor(4, 56)
    oled_print(names[:22])

    oled_show()


def do_feed(state):
    state["hunger"] = max(0, state["hunger"] - 20)
    state["happiness"] = min(100, state["happiness"] + 5)
    state["xp"] = state.get("xp", 0) + 5
    haptic_pulse(180, 50)
    save_state(state)
    return random.choice(FEED_MSGS)


def do_pet(state):
    state["happiness"] = min(100, state["happiness"] + 15)
    state["xp"] = state.get("xp", 0) + 15
    haptic_pulse(100, 80)
    save_state(state)
    return random.choice(PET_MSGS)


def do_play(state):
    state["happiness"] = min(100, state["happiness"] + 25)
    state["hunger"] = min(100, state["hunger"] + 10)
    state["xp"] = state.get("xp", 0) + 25
    haptic_pulse(200, 100)
    save_state(state)
    return random.choice(PLAY_MSGS)


def passive_tick(state):
    now = time.ticks_ms()
    elapsed = time.ticks_diff(now, state["last_tick_ms"])
    if elapsed >= 60000:
        ticks = elapsed // 60000
        state["hunger"] = min(100, state["hunger"] + ticks * 2)
        state["happiness"] = max(0, state["happiness"] - ticks)
        state["last_tick_ms"] = now
        save_state(state)


def name_entry(state):
    name_buf = list(state["name"])
    kx, ky = 0, 0
    last_nav = 0
    NAV_DELAY = 150

    while True:
        draw_name_screen(state, name_buf, kx, ky)

        now = time.ticks_ms()
        if time.ticks_diff(now, last_nav) > NAV_DELAY:
            dx, dy = read_stick_4way()
            if dx != 0 or dy != 0:
                kx = (kx + dx) % KB_COLS
                ky = (ky + dy) % KB_ROW_COUNT
                last_nav = now
                haptic_pulse(30, 15)

        if button_pressed(BTN_RIGHT):
            if len(name_buf) < 12:
                name_buf.append(KB_ROWS[ky][kx])
                haptic_pulse(60, 30)

        if button_pressed(BTN_LEFT):
            if name_buf:
                name_buf.pop()
                haptic_pulse(60, 30)

        if button_pressed(BTN_BACK):
            if name_buf:
                state["name"] = "".join(name_buf)
                save_state(state)
            return

        time.sleep_ms(30)


# ---- LED 8x8 GoL for Field mode ----
LED_TARDI = [
    0b00011000,
    0b00111100,
    0b01111110,
    0b00111100,
    0b01011010,
]


def led_gol_step(grid):
    new = [0] * 8
    for y in range(8):
        above = grid[(y - 1) % 8]
        cur = grid[y]
        below = grid[(y + 1) % 8]
        for x in range(8):
            xl = (x - 1) % 8
            xr = (x + 1) % 8
            bx = 1 << x
            bxl = 1 << xl
            bxr = 1 << xr
            n = 0
            if above & bxl: n += 1
            if above & bx: n += 1
            if above & bxr: n += 1
            if cur & bxl: n += 1
            if cur & bxr: n += 1
            if below & bxl: n += 1
            if below & bx: n += 1
            if below & bxr: n += 1
            if n == 3 or (n == 2 and (cur & bx)):
                new[y] |= bx
    return new


def led_gol_init():
    grid = [0] * 8
    grid[1] = 0b00100000
    grid[2] = 0b00010000
    grid[3] = 0b01110000
    return grid


def do_field(state, sel):
    """Ziggy walks up off OLED onto LED matrix field."""
    m = mood(state)
    lvl = get_level(state.get("xp", 0))
    start_y = CONTENT_Y + 16

    # Phase 1: Walk tardi up off OLED, land at bottom-middle of LED
    TARDI_LED_ROW = 3  # rows 3-7 on LED (bottom-middle)
    for tardi_y in range(start_y, -12, -2):
        oled_clear()
        oled_set_draw_color(1)
        draw_oled_tardi(2, tardi_y, m, lvl, hunger=state["hunger"], clip_top=MENU_H)
        draw_menu_bar(sel)
        oled_show()
        # As tardi exits top of OLED, reveal on LED from bottom up
        if tardi_y < 0:
            reveal = min(5, (-tardi_y) // 3 + 1)
            frame = [0] * 8
            for r in range(reveal):
                src = 4 - r
                if 0 <= src < 5:
                    frame[7 - r] = LED_TARDI[src]
            led_set_frame(frame, 60)
        time.sleep_ms(35)

    # Animate tardi sliding from bottom of LED up to final position
    for offset in range(8, TARDI_LED_ROW, -1):
        frame = [0] * 8
        for i in range(5):
            row = offset + i
            if 0 <= row < 8:
                frame[row] = LED_TARDI[i]
        led_set_frame(frame, 60)
        time.sleep_ms(60)

    # Show tardi at bottom-middle of LED
    frame = [0] * 8
    for i in range(5):
        frame[TARDI_LED_ROW + i] = LED_TARDI[i]
    led_set_frame(frame, 60)
    time.sleep_ms(300)

    # Phase 2: Unified field — one 8-col grid spanning both screens
    # Rows 0-7 = LED matrix (1:1), rows 8-15 = OLED (16px per cell)
    # Real GoL runs on the full 8x16 grid, wrapping top-bottom
    # Glider is immune, overlaid on top
    FH = 16  # total field height
    _GP = [
        [(0, 1), (1, 2), (2, 0), (2, 1), (2, 2)],
        [(0, 0), (0, 2), (1, 1), (1, 2), (2, 1)],
        [(0, 2), (1, 0), (1, 2), (2, 1), (2, 2)],
        [(0, 0), (1, 1), (1, 2), (2, 0), (2, 1)],
    ]
    glider_step = 0

    # The unified GoL grid
    field = [0] * FH
    # Seed life across both screens
    field[3] = 0b00100000
    field[4] = 0b00010000
    field[5] = 0b01110000
    field[10] = 0b00001100
    field[11] = 0b00001010
    field[12] = 0b00001100
    ftick = 0

    # Placed cells (user drops, separate until collision)
    placed = [0] * FH
    cx, cy = 4, 4
    last_stick = 0
    CPX = 16  # OLED cell size: 128/8 = 16px

    while True:
        now = time.ticks_ms()

        # Joystick cursor across unified field
        if time.ticks_diff(now, last_stick) > 180:
            dx, dy = read_stick_4way()
            if dx or dy:
                cx = (cx + dx) % 8
                cy = (cy + dy) % FH
                last_stick = now
                haptic_pulse(20, 10)

        if button_pressed(BTN_CONFIRM):
            placed[cy] |= (1 << cx)
            haptic_pulse(60, 30)

        # Glider cells (immune, wraps on full 16-row field)
        phase = glider_step % 4
        diag = (glider_step // 4) % FH
        gl = set()
        for dr, dc in _GP[phase]:
            gl.add(((dr + diag) % FH, (dc + diag) % 8))

        # Collision: glider hits placed → merge placed into field
        for r, c in gl:
            if placed[r] & (1 << c):
                for i in range(FH):
                    field[i] |= placed[i]
                placed = [0] * FH
                haptic_pulse(200, 100)
                state["xp"] = state.get("xp", 0) + 10
                save_state(state)
                break

        # Step real GoL on unified 8x16 field
        nf = [0] * FH
        for y in range(FH):
            a = field[(y - 1) % FH]
            c_ = field[y]
            b = field[(y + 1) % FH]
            for x in range(8):
                xl = (x - 1) % 8
                xr = (x + 1) % 8
                bx = 1 << x
                n = 0
                if a & (1 << xl): n += 1
                if a & bx: n += 1
                if a & (1 << xr): n += 1
                if c_ & (1 << xl): n += 1
                if c_ & (1 << xr): n += 1
                if b & (1 << xl): n += 1
                if b & bx: n += 1
                if b & (1 << xr): n += 1
                if n == 3 or (n == 2 and (c_ & bx)):
                    nf[y] |= bx
        field = nf
        ftick += 1
        if not any(field) or ftick > 300:
            field = [0] * FH
            field[3] = 0b00100000
            field[4] = 0b00010000
            field[5] = 0b01110000
            ftick = 0

        # --- LED: rows 0-7 of field + placed + glider + cursor ---
        # field/placed use bit N = col N (LSB), LED uses bit 7 = leftmost
        # Reverse bits for display
        led_frame = [0] * 8
        for i in range(8):
            v = field[i] | placed[i]
            # Flip: bit 0->7, 1->6, etc
            rv = 0
            for b in range(8):
                if v & (1 << b):
                    rv |= (1 << (7 - b))
            led_frame[i] = rv
        for r, c in gl:
            if r < 8:
                led_frame[r] |= (1 << (7 - c))
        # Cursor on LED (only when cy < 8)
        if cy < 8:
            cb = 1 << (7 - cx)
            if (now // 300) % 2:
                led_frame[cy] |= cb
            else:
                led_frame[cy] ^= cb
            # Crosshair lines on LED
            for i in range(8):
                if i != cx:
                    led_frame[cy] |= (1 << (7 - i))  # horizontal
                if i != cy:
                    led_frame[i] |= (1 << (7 - cx))  # vertical
        glider_step += 1
        led_set_frame(led_frame, 60)

        # --- OLED: rows 8-15 as full-width cells + glider + placed + cursor ---
        oled_clear()
        oled_set_draw_color(1)
        CW = 16  # cell width
        CH = 7   # cell height (7*8=56, leaves 8px for text)
        for r in range(8):
            row = field[8 + r] | placed[8 + r]
            for c in range(8):
                if row & (1 << c):
                    oled_draw_box(c * CW + 1, r * CH + 1, CW - 2, CH - 2)
            for gr, gc in gl:
                if gr == 8 + r:
                    oled_draw_box(gc * CW, r * CH, CW, CH)
        # Cursor on OLED (when cy >= 8)
        if cy >= 8:
            cr = cy - 8
            if (now // 300) % 2:
                oled_draw_box(cx * CW, cr * CH, CW, CH)
            # Crosshair lines on OLED
            oled_set_draw_color(1)
            for i in range(8):
                if i != cx:
                    oled_set_pixel(i * CW + CW // 2, cr * CH + CH // 2)
                if i != cr:
                    oled_set_pixel(cx * CW + CW // 2, i * CH + CH // 2)
        ui.font()
        oled_set_cursor(2, 63)
        oled_print("Joy:aim B:drop A:back")
        oled_show()

        if button_pressed(BTN_BACK):
            break
        time.sleep_ms(100)

    # Phase 3: Quick walk back down
    for tardi_y in range(-20, start_y + 1, 4):
        oled_clear()
        draw_oled_tardi(2, tardi_y, m, lvl, hunger=state["hunger"], clip_top=MENU_H)
        draw_menu_bar(sel)
        oled_show()
        if tardi_y < 0:
            reveal = min(5, (-tardi_y) // 4 + 1)
            frame = [0] * 8
            for r in range(reveal):
                src = 4 - r
                if 0 <= src < 5:
                    frame[7 - r] = LED_TARDI[src]
            led_set_frame(frame, 60)
        else:
            led_clear()
        time.sleep_ms(25)


def main():
    state = load_state()
    sel = 0
    session = DualScreenSession(150)
    last_nav = 0
    NAV_DELAY = 150
    led_phase = 0
    last_led = 0
    gol_grid = init_gol(state)
    gol_tick = 0
    prev_level = get_level(state.get("xp", 0))

    led_override_begin()

    # Draw immediately on launch so screen isn't blank
    draw_main_screen(state, sel, gol_grid)

    try:
        while True:
            now = session.now()
            passive_tick(state)

            # Animate LED glider
            if time.ticks_diff(now, last_led) > 350:
                m = mood(state)
                if m == "sleeping":
                    led_set_frame(mood_led(m), 20)
                else:
                    led_set_frame(led_glider_frame(led_phase), 50)
                    led_phase = (led_phase + 1) % 16
                last_led = now

            # Step GoL and draw
            if session.frame_due(state, now):
                gol_grid = gol_step(gol_grid)
                gol_tick += 1
                alive = sum(1 for r in gol_grid if r)
                if alive == 0 or gol_tick > 200:
                    gol_grid = init_gol(state)
                    gol_tick = 0
                draw_main_screen(state, sel, gol_grid)

            # Check for level up
            cur_level = get_level(state.get("xp", 0))
            if cur_level > prev_level:
                prev_level = cur_level
                gol_grid = init_gol(state)
                gol_tick = 0
                haptic_pulse(255, 200)

            # Navigation — redraw menu immediately on change
            if time.ticks_diff(now, last_nav) > NAV_DELAY:
                dx, _ = read_stick_4way()
                if dx != 0:
                    sel = (sel + dx) % MENU_COUNT
                    last_nav = now
                    haptic_pulse(40, 20)
                    draw_menu_bar(sel)
                    oled_show()

            # Confirm action
            if button_pressed(BTN_CONFIRM):
                item = MENU_ITEMS[sel]
                if item == "Feed":
                    msg = do_feed(state)
                    led_set_frame(led_frame_for_action("feed"), 60)
                    draw_action_screen(state, msg, "feed")
                    wait_choice(back=True, delay_ms=30)
                elif item == "Pet":
                    msg = do_pet(state)
                    led_set_frame(led_frame_for_action("pet"), 60)
                    draw_action_screen(state, msg, "pet")
                    wait_choice(back=True, delay_ms=30)
                elif item == "Play":
                    do_play(state)
                    draw_action_screen(state, random.choice(PLAY_MSGS), "play")
                    wait_choice(back=True, delay_ms=30)
                elif item == "Field":
                    do_field(state, sel)
                elif item == "Stats":
                    draw_stats_screen(state)
                    wait_choice(back=True, delay_ms=30)
                elif item == "Name":
                    name_entry(state)
                flush_buttons()
                gol_grid = init_gol(state)
                gol_tick = 0
                draw_main_screen(state, sel, gol_grid)

            # Exit
            if session.quit_held(1000):
                save_state(state)
                break

            session.sleep()
    finally:
        led_clear()
        led_override_end()
