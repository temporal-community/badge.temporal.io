"""Reusable helpers for badge MicroPython apps."""

import gc
import json
import random
import sys
import time

from badge import *
import badge_ui as ui

LAST_ERROR_PATH = "/last_mpy_error.txt"
JOY_LOW = 1100
JOY_HIGH = 3000
GC_INTERVAL_MS = 2500
GC_THRESHOLD_BYTES = 32768


def clamp(value, low, high):
    if value < low:
        return low
    if value > high:
        return high
    return value


def ticks_add(now, delta):
    try:
        return time.ticks_add(now, delta)
    except AttributeError:
        return now + delta


def active_until(until, now):
    return bool(until) and time.ticks_diff(until, now) > 0


def elapsed_seconds(started, now):
    return max(0, time.ticks_diff(now, started) // 1000)


def seed_random():
    try:
        random.seed(time.ticks_ms())
    except Exception:
        pass


def configure_gc(threshold=GC_THRESHOLD_BYTES):
    gc.collect()
    try:
        gc.threshold(threshold)
    except Exception:
        pass


class GCTicker:
    def __init__(self, interval_ms=GC_INTERVAL_MS, threshold=GC_THRESHOLD_BYTES):
        self.interval_ms = interval_ms
        self.last_gc = time.ticks_ms()
        configure_gc(threshold)

    def tick(self, now=None):
        if not self.interval_ms:
            return False
        if now is None:
            now = time.ticks_ms()
        if time.ticks_diff(now, self.last_gc) < self.interval_ms:
            return False
        self.last_gc = now
        gc.collect()
        return True


def read_axis(value, low=JOY_LOW, high=JOY_HIGH):
    if value < low:
        return -1
    if value > high:
        return 1
    return 0


def read_stick_xy(low=JOY_LOW, high=JOY_HIGH):
    return read_axis(joy_x(), low, high), read_axis(joy_y(), low, high)


def read_stick_4way(low=JOY_LOW, high=JOY_HIGH, no_diagonal=True):
    x_dir, y_dir = read_stick_xy(low, high)
    if no_diagonal and x_dir and y_dir:
        return 0, 0
    return x_dir, y_dir


def load_score(path, defaults):
    score = {}
    for key in defaults:
        score[key] = defaults[key]

    try:
        with open(path, "r") as score_file:
            data = json.loads(score_file.read())
        for key in defaults:
            score[key] = int(data.get(key, defaults[key]))
    except Exception:
        pass

    return score


def save_score(path, score):
    try:
        with open(path, "w") as score_file:
            score_file.write(json.dumps(score))
    except Exception:
        pass


def wait_choice(confirm=True, back=False, delay_ms=30):
    while True:
        if button_pressed(BTN_CONFIRM):
            return confirm
        if button_pressed(BTN_BACK):
            return back
        time.sleep_ms(delay_ms)


def with_led_override(callback, *args):
    gc.collect()
    led_override_begin()
    try:
        return callback(*args)
    finally:
        led_override_end()
        gc.collect()


class ButtonLatch:
    def __init__(self, button_id):
        self.button_id = button_id
        self.pending = False

    def poll(self):
        pressed = button_pressed(self.button_id)
        if pressed:
            self.pending = True
        return pressed

    def consume(self):
        pressed = self.pending
        self.pending = False
        return pressed


class DualScreenSession:
    def __init__(self, frame_ms, sleep_ms=4, gc_ms=GC_INTERVAL_MS):
        self.frame_ms = frame_ms
        self.sleep_ms = sleep_ms
        self.gc = GCTicker(gc_ms)

    def now(self):
        return time.ticks_ms()

    def frame_due(self, game, now, key="last_frame", frame_ms=None):
        interval = self.frame_ms if frame_ms is None else frame_ms
        if time.ticks_diff(now, game[key]) < interval:
            return False
        game[key] = now
        return True

    def quit_pressed(self):
        return button_pressed(BTN_BACK)

    def quit_held(self, hold_ms):
        return button(BTN_BACK) and button_held_ms(BTN_BACK) >= hold_ms

    def sleep(self):
        self.gc.tick()
        time.sleep_ms(self.sleep_ms)


def _safe_stop_outputs():
    for name in ("no_tone", "haptic_off", "led_clear"):
        try:
            globals()[name]()
        except Exception:
            pass


def _run_cleanup(cleanup):
    if cleanup is None:
        return None
    try:
        cleanup()
    except Exception as exc:
        return exc
    return None


def _error_label(exc):
    try:
        return exc.__class__.__name__
    except Exception:
        return "Exception"


def _write_error(app_name, exc):
    try:
        with open(LAST_ERROR_PATH, "w") as error_file:
            error_file.write(app_name + "\n")
            try:
                sys.print_exception(exc, error_file)
            except Exception:
                error_file.write(_error_label(exc) + ": " + str(exc) + "\n")
    except Exception:
        pass


def _show_error(app_name, exc):
    led_override_begin()
    led_set_frame((0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81), 45)
    ui.chrome("App crashed", app_name, "BACK", "exit", "OK", "exit")
    ui.center(17, ui.fit(app_name, 118))
    ui.center(30, ui.fit(_error_label(exc), 118))
    detail = str(exc)
    if detail:
        ui.center(41, ui.fit(detail, 118))
    else:
        ui.center(41, "Saved crash log")
    oled_show()


def _wait_error_exit():
    while True:
        if button_pressed(BTN_BACK) or button_pressed(BTN_CONFIRM):
            break
        time.sleep_ms(40)
    led_clear()
    led_override_end()
    oled_clear(True)


def run_app(app_name, callback, cleanup=None):
    app_exc = None
    try:
        callback()
    except Exception as exc:
        app_exc = exc
    finally:
        cleanup_exc = _run_cleanup(cleanup)

    exc = app_exc or cleanup_exc
    if exc is not None:
        _write_error(app_name, exc)
        _safe_stop_outputs()
        _show_error(app_name, exc)
        _wait_error_exit()
        exit()
