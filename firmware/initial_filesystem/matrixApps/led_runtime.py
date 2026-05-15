"""Persistent ambient LED matrix runtime."""

import gc
import json
import os
import time

import badge


STATE_PATH = "/led_state.json"

MODE_TEMPORAL = "temporal"
MODE_REPLAY = "replay"
MODE_SPARKLE = "sparkle"
MODE_RAIN = "rain"
MODE_WAVE = "wave"
MODE_LIFE = "life"
MODE_LIFE_RANDOM = "life_random"
MODE_CUSTOM = "custom"
MODE_OFF = "off"

MODES = (
    MODE_TEMPORAL,
    MODE_REPLAY,
    MODE_SPARKLE,
    MODE_RAIN,
    MODE_WAVE,
    MODE_LIFE,
    MODE_LIFE_RANDOM,
    MODE_CUSTOM,
    MODE_OFF,
)

BLANK = [0, 0, 0, 0, 0, 0, 0, 0]
HEART = [0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00]
SMILEY = [0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C]
GLIDER = [0x00, 0x00, 0x20, 0x10, 0x70, 0x00, 0x00, 0x00]
BLINKER = [0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00]
PULSAR = [0x00, 0x38, 0x24, 0x38, 0x38, 0x24, 0x38, 0x00]
RPENT = [0x00, 0x18, 0x30, 0x10, 0x00, 0x00, 0x00, 0x00]

STALE_GRACE = 8

MIN_DELAY = 5
MAX_DELAY = 10000
DEFAULT_DELAY = 120
MIN_BRIGHTNESS = 0
MAX_BRIGHTNESS = 255
DEFAULT_BRIGHTNESS = 40

REPLAY_COLUMNS = (
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xDF, 0xDF, 0xD8, 0xDE, 0xFF, 0xFF, 0x71, 0xFF,
    0xFF, 0xDB, 0xDB, 0xDB, 0xC3, 0xC3, 0xDF, 0xDF,
    0xD8, 0xD8, 0xD8, 0xF8, 0xF8, 0xFF, 0xFF, 0x07,
    0x03, 0x03, 0x03, 0x03, 0xFF, 0xFF, 0xF8, 0xD8,
    0xD8, 0xFF, 0xFF, 0xF8, 0xF8, 0xFF, 0x1F, 0x1F,
    0xF8, 0xF8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
)

WAVE_FRAMES = (
    [0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01],
    [0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x80],
    [0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x80, 0x40],
    [0x10, 0x08, 0x04, 0x02, 0x01, 0x80, 0x40, 0x20],
    [0x08, 0x04, 0x02, 0x01, 0x80, 0x40, 0x20, 0x10],
    [0x04, 0x02, 0x01, 0x80, 0x40, 0x20, 0x10, 0x08],
    [0x02, 0x01, 0x80, 0x40, 0x20, 0x10, 0x08, 0x04],
    [0x01, 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02],
)

TEMPORAL_32 = (
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00010000,
    0x00028000, 0x00028000, 0x000FE000, 0x00121000,
    0x000E6000, 0x00028000, 0x00028000, 0x00010000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
)

_rng = [(time.ticks_ms() ^ 0x43C9A1D7) & 0x7FFFFFFF]
_state = None
_runner = None


def _collect():
    gc.collect()


def _rand8():
    _rng[0] = ((_rng[0] * 1103515245) + 12345) & 0x7FFFFFFF
    return (_rng[0] >> 8) & 0xFF


def _random_frame():
    try:
        return list(os.urandom(8))
    except Exception:
        return [_rand8() for _i in range(8)]


def _copy_frame(frame):
    if not frame or len(frame) != 8:
        return list(BLANK)
    return [int(row) & 0xFF for row in frame]


def _clamp(val, lo, hi, default):
    v = int(val) if isinstance(val, (int, float)) else default
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


def _default_state():
    return {
        "mode": MODE_TEMPORAL,
        "delay": DEFAULT_DELAY,
        "brightness": DEFAULT_BRIGHTNESS,
        "life_seed": list(GLIDER),
        "life_randomize": False,
        "custom": list(HEART),
    }


def _clean_state(raw):
    state = _default_state()
    if isinstance(raw, dict):
        mode = raw.get("mode", MODE_TEMPORAL)
        if mode in MODES:
            state["mode"] = mode
        state["delay"] = _clamp(raw.get("delay", DEFAULT_DELAY),
                               MIN_DELAY, MAX_DELAY, DEFAULT_DELAY)
        state["brightness"] = _clamp(raw.get("brightness", DEFAULT_BRIGHTNESS),
                                     MIN_BRIGHTNESS, MAX_BRIGHTNESS,
                                     DEFAULT_BRIGHTNESS)
        state["life_seed"] = _copy_frame(raw.get("life_seed", GLIDER))
        state["life_randomize"] = bool(raw.get("life_randomize", False))
        state["custom"] = _copy_frame(raw.get("custom", HEART))
    return state


def load_state():
    global _state
    try:
        with open(STATE_PATH, "r") as f:
            _state = _clean_state(json.loads(f.read()))
    except Exception:
        _state = _default_state()
    gc.collect()
    return _state


def save_state(state):
    global _state
    _state = _clean_state(state)
    try:
        with open(STATE_PATH, "w") as f:
            f.write(json.dumps(_state))
    finally:
        gc.collect()
    return _state


def current_state():
    return _state if _state is not None else load_state()


def save_mode(mode, delay=None, brightness=None):
    state = _clean_state(current_state())
    state["mode"] = mode if mode in MODES else MODE_TEMPORAL
    if delay is not None:
        state["delay"] = _clamp(delay, MIN_DELAY, MAX_DELAY, DEFAULT_DELAY)
    if brightness is not None:
        state["brightness"] = _clamp(brightness, MIN_BRIGHTNESS,
                                     MAX_BRIGHTNESS, DEFAULT_BRIGHTNESS)
    return save_state(state)


def save_life_seed(seed, randomize=False):
    state = _clean_state(current_state())
    state["mode"] = MODE_LIFE
    state["life_seed"] = _copy_frame(seed)
    state["life_randomize"] = bool(randomize)
    return save_state(state)


def save_custom(pattern):
    state = _clean_state(current_state())
    state["mode"] = MODE_CUSTOM
    state["custom"] = _copy_frame(pattern)
    return save_state(state)


def display_name(mode):
    if mode == MODE_TEMPORAL:
        return "Temporal"
    if mode == MODE_REPLAY:
        return "Replay"
    if mode == MODE_SPARKLE:
        return "Sparkle"
    if mode == MODE_RAIN:
        return "Rain"
    if mode == MODE_WAVE:
        return "Wave"
    if mode == MODE_LIFE:
        return "Game of Life"
    if mode == MODE_LIFE_RANDOM:
        return "Random Life"
    if mode == MODE_CUSTOM:
        return "Custom"
    if mode == MODE_OFF:
        return "Off"
    return "LED"


def mode_index(mode):
    for i in range(len(MODES)):
        if MODES[i] == mode:
            return i
    return 0



def _frame_from_columns(columns, start):
    frame = [0] * 8
    for x in range(8):
        col = columns[(start + x) % len(columns)]
        bit = 0x80 >> x
        for y in range(8):
            if col & (0x80 >> y):
                frame[y] |= bit
    return frame


def _frame_from_32(rows, xoff, yoff):
    out = [0] * 8
    if xoff < 0:
        xoff = 0
    if xoff > 24:
        xoff = 24
    if yoff < 0:
        yoff = 0
    if yoff > 24:
        yoff = 24
    shift = 24 - xoff
    for row in range(8):
        out[row] = (rows[yoff + row] >> shift) & 0xFF
    return out


def poster(mode, state=None):
    state = state or current_state()
    if mode == MODE_TEMPORAL:
        return _frame_from_32(TEMPORAL_32, 12, 12)
    if mode == MODE_REPLAY:
        return _frame_from_columns(REPLAY_COLUMNS, 8)
    if mode == MODE_SPARKLE:
        return [0x81, 0x24, 0x00, 0x5A, 0x18, 0x00, 0x42, 0x18]
    if mode == MODE_RAIN:
        return [0x80, 0x00, 0x24, 0x00, 0x08, 0x40, 0x02, 0x00]
    if mode == MODE_WAVE:
        return list(WAVE_FRAMES[0])
    if mode == MODE_LIFE:
        return _copy_frame(state.get("life_seed", GLIDER))
    if mode == MODE_LIFE_RANDOM:
        return _random_frame()
    if mode == MODE_CUSTOM:
        return _copy_frame(state.get("custom", HEART))
    return list(BLANK)


def life_preset_names():
    return ("Glider", "Blinker", "Pulsar", "R-Pent", "Randomize", "Clear")


def custom_preset_names():
    return ("Heart", "Smiley", "Randomize", "Clear")


def life_preset_frame(name):
    if name == "Glider":
        return list(GLIDER)
    if name == "Blinker":
        return list(BLINKER)
    if name == "Pulsar":
        return list(PULSAR)
    if name == "R-Pent":
        return list(RPENT)
    if name == "Randomize":
        return _random_frame()
    return list(BLANK)


def custom_preset_frame(name):
    if name == "Heart":
        return list(HEART)
    if name == "Smiley":
        return list(SMILEY)
    if name == "Randomize":
        return _random_frame()
    return list(BLANK)


class Runner:
    def __init__(self, mode, state=None, draft=None):
        self.mode = mode
        self.state = state or current_state()
        self.frame = 0
        self.first = True
        self.interval = self.state.get("delay", DEFAULT_DELAY)
        self.brightness = self.state.get("brightness", DEFAULT_BRIGHTNESS)
        self._is_life = mode == MODE_LIFE or mode == MODE_LIFE_RANDOM
        if mode == MODE_LIFE_RANDOM and draft is None:
            self.life = _random_frame()
        else:
            self.life = _copy_frame(
                draft if draft is not None else self.state.get("life_seed", GLIDER))
        self.life_next = [0] * 8
        self.life_prev = list(self.life) if self._is_life else None
        self.life_prev2 = list(self.life) if self._is_life else None
        self.stale_count = 0
        self.custom = _copy_frame(
            draft if draft is not None else self.state.get("custom", HEART))
        self.rain = [(_rand8() + i) & 7 for i in range(8)]

    def close(self):
        self.state = None
        self.life = None
        self.life_next = None
        self.life_prev = None
        self.life_prev2 = None
        self.custom = None
        self.rain = None

    def _draw(self, frame):
        badge.led_set_frame(frame, self.brightness)

    def _temporal_frame(self):
        if badge.imu_ready():
            tx = badge.imu_tilt_x()
            ty = badge.imu_tilt_y()
            xoff = int((tx + 480) * 24 / 960)
            yoff = int((ty + 480) * 24 / 960)
        else:
            sweep = self.frame % 48
            xoff = sweep if sweep <= 24 else 48 - sweep
            yraw = (self.frame * 2) % 48
            yoff = yraw if yraw <= 24 else 48 - yraw
        return _frame_from_32(TEMPORAL_32, xoff, yoff)

    def _sparkle_frame(self):
        return [(_rand8() & _rand8()) for _i in range(8)]

    def _rain_frame(self):
        frame = [0] * 8
        for x in range(8):
            y = self.rain[x]
            bit = 0x80 >> x
            frame[y] |= bit
            frame[(y - 1) & 7] |= bit
            if (_rand8() & 3) == 0:
                self.rain[x] = (y + 1) & 7
        return frame

    def _life_frame(self):
        if self.first:
            self.first = False
            for i in range(8):
                self.life_prev[i] = self.life[i]
                self.life_prev2[i] = self.life[i]
            self.stale_count = 0
            return list(self.life)
        for i in range(8):
            self.life_prev2[i] = self.life_prev[i]
            self.life_prev[i] = self.life[i]
        for y in range(8):
            row = 0
            for x in range(8):
                neighbors = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        if self.life[(y + dy) & 7] & (0x80 >> ((x + dx) & 7)):
                            neighbors += 1
                alive = self.life[y] & (0x80 >> x)
                if (alive and neighbors in (2, 3)) or (
                        not alive and neighbors == 3):
                    row |= 0x80 >> x
            self.life_next[y] = row
        for i in range(8):
            self.life[i] = self.life_next[i]
        empty = not any(self.life)
        still = self.life == self.life_prev
        period2 = self.life == self.life_prev2
        if empty or still or period2:
            self.stale_count += 1
            if self.stale_count >= STALE_GRACE:
                seed = _random_frame()
                for i in range(8):
                    self.life[i] = seed[i]
                self.first = True
        else:
            self.stale_count = 0
        return list(self.life)

    def tick(self, now_ms=None):
        if self.mode == MODE_TEMPORAL:
            frame = self._temporal_frame()
        elif self.mode == MODE_REPLAY:
            frame = _frame_from_columns(REPLAY_COLUMNS, self.frame)
        elif self.mode == MODE_SPARKLE:
            frame = self._sparkle_frame()
        elif self.mode == MODE_RAIN:
            frame = self._rain_frame()
        elif self.mode == MODE_WAVE:
            frame = WAVE_FRAMES[self.frame & 7]
        elif self._is_life:
            frame = self._life_frame()
        elif self.mode == MODE_CUSTOM:
            frame = self.custom
        else:
            frame = BLANK
        self._draw(frame)
        self.frame += 1


def start_preview(mode, draft=None):
    runner = Runner(mode, current_state(), draft)
    runner.tick(time.ticks_ms())
    return runner


def release_runner(runner):
    if runner is not None:
        try:
            runner.close()
        except Exception:
            pass
    _collect()
    return None


def _ambient_tick(now_ms):
    if _runner is not None:
        _runner.tick(now_ms)


def restore_ambient():
    global _runner
    old = _runner
    _runner = None
    badge.matrix_app_stop()
    release_runner(old)

    state = load_state()
    mode = state.get("mode", MODE_TEMPORAL)
    if mode == MODE_OFF:
        badge.led_clear()
        _collect()
        return
    _runner = Runner(mode, state)
    badge.matrix_app_start(_ambient_tick, state.get("delay", DEFAULT_DELAY))
    _ambient_tick(time.ticks_ms())
    _collect()
