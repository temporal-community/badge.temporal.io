"""LED matrix carousel and editors."""

import gc
import time

import badge
import led_runtime as led


JOY_LOW = 1100
JOY_HIGH = 3000
NAV_MS = 170
GRID_X = 40
GRID_Y = 12
CELL = 6


def _print_at(x, y, text):
    badge.oled_set_cursor(x, y)
    badge.oled_print(text)


def _box(x, y, w, h, color=1):
    badge.oled_set_draw_color(color)
    badge.oled_draw_box(x, y, w, h)
    badge.oled_set_draw_color(1)


def _outline(x, y, w, h, color=1):
    _box(x, y, w, 1, color)
    _box(x, y + h - 1, w, 1, color)
    _box(x, y, 1, h, color)
    _box(x + w - 1, y, 1, h, color)


def _draw_grid(frame, cursor=None):
    for y in range(8):
        row = frame[y]
        for x in range(8):
            px = GRID_X + x * CELL
            py = GRID_Y + y * CELL
            _outline(px, py, CELL, CELL, 1)
            if row & (0x80 >> x):
                _box(px + 1, py + 1, CELL - 2, CELL - 2, 1)
    if cursor is not None:
        x, y = cursor
        px = GRID_X + x * CELL
        py = GRID_Y + y * CELL
        _outline(px, py, CELL, CELL, 2)


ADJ_DELAY = 0
ADJ_BRIGHT = 1


def _delay_step(val):
    if val >= 1000:
        return 250
    if val >= 200:
        return 50
    if val >= 50:
        return 10
    return 5


def _render_carousel(index, delay, brightness, adj):
    mode = led.MODES[index]
    badge.oled_clear(False)
    _print_at(0, 0, led.display_name(mode) + " " +
              str(index + 1) + "/" + str(len(led.MODES)))
    _draw_grid(led.poster(mode))
    dp = ">" if adj == ADJ_DELAY else " "
    bp = ">" if adj == ADJ_BRIGHT else " "
    _print_at(0, 20, dp + str(delay) + "ms")
    _print_at(0, 30, bp + "brt " + str(brightness))
    if mode == led.MODE_LIFE:
        _print_at(0, 56, "X swp Y lab B bk A sv")
    elif mode == led.MODE_CUSTOM:
        _print_at(0, 56, "X swp Y drw B bk A sv")
    else:
        _print_at(0, 56, "X swap B back A save")
    badge.oled_show()


def _render_editor(title, frame, cursor):
    badge.oled_clear(False)
    _print_at(0, 0, title)
    _draw_grid(frame, cursor)
    _print_at(0, 56, "X pix Y pre B bk A sv")
    badge.oled_show()


def _render_presets(title, names, index):
    badge.oled_clear(False)
    _print_at(0, 0, title)
    first = index - 2
    if first < 0:
        first = 0
    if first > len(names) - 5:
        first = max(0, len(names) - 5)
    y = 12
    for row in range(5):
        i = first + row
        if i >= len(names):
            break
        prefix = ">" if i == index else " "
        _print_at(0, y, prefix + names[i])
        y += 9
    _print_at(0, 56, "B back A load")
    badge.oled_show()


def _joy_step(axis, last_nav):
    now = time.ticks_ms()
    if time.ticks_diff(now, last_nav) < NAV_MS:
        return 0, last_nav
    val = badge.joy_x() if axis == "x" else badge.joy_y()
    if val < JOY_LOW:
        return -1, now
    if val > JOY_HIGH:
        return 1, now
    return 0, last_nav


def _tick_runner(runner, last_tick):
    if runner is None:
        return last_tick
    now = time.ticks_ms()
    if time.ticks_diff(now, last_tick) >= runner.interval:
        runner.tick(now)
        return now
    return last_tick


def _replace_runner(runner, mode, draft=None):
    led.release_runner(runner)
    return led.start_preview(mode, draft)


def _toggle(frame, x, y):
    frame[y] ^= 0x80 >> x


def _preset_menu(kind, runner, last_tick):
    if kind == led.MODE_LIFE:
        names = led.life_preset_names()
        title = "Life Preset"
    else:
        names = led.custom_preset_names()
        title = "Custom Preset"
    index = 0
    last_nav = 0
    _render_presets(title, names, index)
    while True:
        last_tick = _tick_runner(runner, last_tick)
        step, last_nav = _joy_step("y", last_nav)
        if step:
            index = (index + step) % len(names)
            _render_presets(title, names, index)
        if badge.button_pressed(badge.BTN_BACK):
            return None, None
        if badge.button_pressed(badge.BTN_CONFIRM):
            name = names[index]
            if kind == led.MODE_LIFE:
                return led.life_preset_frame(name), name
            return led.custom_preset_frame(name), name
        time.sleep_ms(20)


def _edit_pattern(kind):
    state = led.current_state()
    if kind == led.MODE_LIFE:
        title = "Life Lab"
        frame = list(state.get("life_seed", led.GLIDER))
    else:
        title = "Custom"
        frame = list(state.get("custom", led.HEART))
    x = 0
    y = 0
    is_randomize = state.get("life_randomize", False) if kind == led.MODE_LIFE else False
    runner = _replace_runner(None, kind, frame)
    last_tick = 0
    last_nav_x = 0
    last_nav_y = 0
    _render_editor(title, frame, (x, y))

    try:
        while True:
            last_tick = _tick_runner(runner, last_tick)
            xstep, last_nav_x = _joy_step("x", last_nav_x)
            ystep, last_nav_y = _joy_step("y", last_nav_y)
            if xstep or ystep:
                x = (x + xstep) & 7
                y = (y + ystep) & 7
                _render_editor(title, frame, (x, y))

            if badge.button_pressed(badge.BTN_SQUARE):
                _toggle(frame, x, y)
                runner = _replace_runner(runner, kind, frame)
                last_tick = 0
                _render_editor(title, frame, (x, y))

            if badge.button_pressed(badge.BTN_PRESETS):
                loaded, preset_name = _preset_menu(kind, runner, last_tick)
                if loaded is not None:
                    frame = loaded
                    if kind == led.MODE_LIFE:
                        is_randomize = preset_name == "Randomize"
                    runner = _replace_runner(runner, kind, frame)
                    last_tick = 0
                _render_editor(title, frame, (x, y))
                gc.collect()

            if badge.button_pressed(badge.BTN_SAVE):
                if kind == led.MODE_LIFE:
                    led.save_life_seed(frame, randomize=is_randomize)
                else:
                    led.save_custom(frame)
                return True

            if badge.button_pressed(badge.BTN_BACK):
                return False

            time.sleep_ms(20)
    finally:
        led.release_runner(runner)
        gc.collect()


def main():
    gc.collect()
    state = led.load_state()
    index = led.mode_index(state.get("mode", led.MODE_TEMPORAL))
    delay = state.get("delay", led.DEFAULT_DELAY)
    brightness = state.get("brightness", led.DEFAULT_BRIGHTNESS)
    adj = ADJ_DELAY
    runner = None
    last_tick = 0
    last_nav_x = 0
    last_nav_y = 0

    badge.led_override_begin()
    try:
        runner = _replace_runner(None, led.MODES[index])
        _render_carousel(index, delay, brightness, adj)
        while True:
            last_tick = _tick_runner(runner, last_tick)
            xstep, last_nav_x = _joy_step("x", last_nav_x)
            if xstep:
                index = (index + xstep) % len(led.MODES)
                runner = _replace_runner(runner, led.MODES[index])
                last_tick = 0
                _render_carousel(index, delay, brightness, adj)

            ystep, last_nav_y = _joy_step("y", last_nav_y)
            if ystep:
                if adj == ADJ_DELAY:
                    step = _delay_step(delay)
                    delay = led._clamp(delay - ystep * step,
                                       led.MIN_DELAY, led.MAX_DELAY,
                                       led.DEFAULT_DELAY)
                    runner.interval = delay
                else:
                    brightness = led._clamp(brightness + ystep,
                                            led.MIN_BRIGHTNESS,
                                            led.MAX_BRIGHTNESS,
                                            led.DEFAULT_BRIGHTNESS)
                    runner.brightness = brightness
                _render_carousel(index, delay, brightness, adj)

            if badge.button_pressed(badge.BTN_SQUARE):
                adj = ADJ_BRIGHT if adj == ADJ_DELAY else ADJ_DELAY
                _render_carousel(index, delay, brightness, adj)

            mode = led.MODES[index]
            if badge.button_pressed(badge.BTN_PRESETS) and (
                    mode == led.MODE_LIFE or mode == led.MODE_CUSTOM):
                runner = led.release_runner(runner)
                if _edit_pattern(mode):
                    return
                runner = _replace_runner(None, mode)
                last_tick = 0
                _render_carousel(index, delay, brightness, adj)

            if badge.button_pressed(badge.BTN_CONFIRM):
                runner = led.release_runner(runner)
                led.save_mode(mode, delay=delay, brightness=brightness)
                return

            if badge.button_pressed(badge.BTN_BACK):
                return

            time.sleep_ms(20)
    finally:
        led.release_runner(runner)
        badge.led_override_end()
        badge.oled_clear(True)
        gc.collect()


main()
