import gc
import math
import time

from badge import *
from badge_app import GCTicker, run_app
import badge_ui as ui

FREQS = (
    65,
    69,
    73,
    78,
    82,
    87,
    92,
    98,
    104,
    110,
    117,
    123,
    131,
    139,
    147,
    156,
    165,
    175,
    185,
    196,
    208,
    220,
    233,
    247,
    262,
    277,
    294,
    311,
    330,
    349,
    370,
    392,
    415,
    440,
    466,
    494,
    523,
    554,
    587,
    622,
    659,
    698,
    740,
    784,
    831,
    880,
    932,
    988,
    1047,
    1109,
    1175,
    1245,
    1319,
    1397,
    1480,
    1568,
    1661,
    1760,
    1865,
    1976,
    2093,
)
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
SCALES = (
    ("Penta", (0, 2, 4, 7, 9)),
    ("Major", (0, 2, 4, 5, 7, 9, 11)),
    ("Minor", (0, 2, 3, 5, 7, 8, 10)),
    ("Blues", (0, 3, 5, 6, 7, 10)),
    ("Chrom", (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)),
)

SCALE_RIFF = (262, 294, 330, 349, 392, 440, 494, 523)
TWINKLE = (
    262,
    262,
    392,
    392,
    440,
    440,
    392,
    0,
    349,
    349,
    330,
    330,
    294,
    294,
    262,
    0,
    392,
    392,
    349,
    349,
    330,
    330,
    294,
    0,
    262,
    262,
    392,
    392,
    440,
    440,
    392,
    0,
    349,
    349,
    330,
    330,
    294,
    294,
    262,
)
CHARGE = (392, 523, 659, 784, 659, 784)
RIFFS = (
    ("C Scale", "melody", SCALE_RIFF, 150),
    ("Twinkle", "melody", TWINKLE, 180),
    ("Charge", "melody", CHARGE, 130),
    ("Chirp Up", "up", None, 0),
    ("Chirp Down", "down", None, 0),
    ("Siren", "siren", None, 0),
)

MODE_LIVE = 0
MODE_SOUNDS = 1
PI2 = 6.2831853
DEADZONE = 350
MAX_RADIUS = 1700
BASE_OCT = 2
MAX_EVENTS = 420
PLAY_RES = 32
NAV_MS = 170
JOY_LOW = 1100
JOY_HIGH = 3000
LONG_PRESS_MS = 650
LOG_VOL = (1, 3, 9, 17, 22, 28, 34, 40, 44, 50, 60)

gc.collect()

jcx = joy_x()
jcy = joy_y()
scale_i = 0
mode = MODE_LIVE
riff_i = 0
last_nav = 0
y_down = False
y_long = False

recording = False
looping = False
loop = []
loop_len = 0
rec_t0 = 0
play_t0 = 0

prev_freq = -1
prev_vol = 0
rec_rows = [0] * 8
rec_tick = 0
play_trace = None
play_min = 0
play_max = 0


def note_name(semi):
    return NOTE_NAMES[semi % 12] + str(semi // 12 + 2)


def freq_to_semi(freq):
    if freq <= 0:
        return -1
    for i in range(len(FREQS)):
        if FREQS[i] >= freq:
            return i
    return len(FREQS) - 1


def live_note():
    dx = joy_x() - jcx
    dy = joy_y() - jcy
    r2 = dx * dx + dy * dy
    if r2 < DEADZONE * DEADZONE:
        return 0, "---", 0, dx, dy, -1

    r = int(math.sqrt(r2))
    angle = math.atan2(dx, -dy)
    if angle < 0:
        angle += PI2

    vol_i = (r - DEADZONE) * 10 // (MAX_RADIUS - DEADZONE)
    if vol_i < 0:
        vol_i = 0
    if vol_i > 10:
        vol_i = 10
    vol = LOG_VOL[vol_i]

    intervals = SCALES[scale_i][1]
    count = len(intervals)
    idx = int((angle / PI2) * count * 2) % (count * 2)
    semi = (BASE_OCT + idx // count) * 12 + intervals[idx % count]
    if semi >= len(FREQS):
        semi = len(FREQS) - 1
    return FREQS[semi], note_name(semi), vol, dx, dy, idx


def vol_mask(vol):
    width = vol * 7 // 60 + 1
    if width > 8:
        width = 8
    left = (8 - width) // 2
    mask = 0
    for i in range(width):
        mask |= 1 << (7 - left - i)
    return mask


def draw_live_led(note_idx, note_count, vol):
    if note_idx < 0:
        led_clear()
        return
    row = note_idx * 7 // max(note_count - 1, 1)
    rows = [0] * 8
    rows[row] = vol_mask(vol)
    led_set_frame(rows, 40)


def draw_record_led(note_idx, note_count, vol):
    global rec_tick
    now = time.ticks_ms()
    if time.ticks_diff(now, rec_tick) >= 180:
        rec_tick = now
        for row in range(8):
            rec_rows[row] = (rec_rows[row] << 1) & 0xFF
        if note_idx >= 0:
            center = note_idx * 7 // max(note_count - 1, 1)
            spread = vol * 5 // 60 + 1
            if spread > 4:
                spread = 4
            top = max(0, center - spread // 2)
            bot = min(8, center + (spread + 1) // 2)
            for row in range(top, bot):
                rec_rows[row] |= 1
    led_set_frame(rec_rows, 40)


def build_play_trace():
    global play_trace, play_min, play_max
    play_trace = [-1] * PLAY_RES
    if not loop or loop_len <= 0:
        play_min = 0
        play_max = 0
        return

    mn = 61
    mx = 0
    for _, freq, _ in loop:
        if freq > 0:
            semi = freq_to_semi(freq)
            if semi < mn:
                mn = semi
            if semi > mx:
                mx = semi
    play_min = mn
    play_max = mx

    cur_freq = 0
    event_i = 0
    for col in range(PLAY_RES):
        end_t = (col + 1) * loop_len // PLAY_RES
        while event_i < len(loop) and loop[event_i][0] <= end_t:
            cur_freq = loop[event_i][1]
            event_i += 1
        if cur_freq > 0:
            play_trace[col] = freq_to_semi(cur_freq)


def trace_row(semi):
    if semi < 0:
        return -1
    span = play_max - play_min
    if span <= 0:
        return 3
    return 7 - (semi - play_min) * 7 // span


def draw_play_led(pos):
    if loop_len <= 0 or play_trace is None:
        led_clear()
        return
    center = pos * PLAY_RES // loop_len
    rows = [0] * 8
    for dc in range(8):
        src = (center + dc - 4) % PLAY_RES
        row = trace_row(play_trace[src])
        if row >= 0:
            rows[row] |= 1 << (7 - dc)
    for row in range(8):
        rows[row] |= 1 << 3
    led_set_frame(rows, 45)


def apply_tone(freq, vol):
    global prev_freq, prev_vol
    if freq == prev_freq and (freq <= 0 or vol == prev_vol):
        return
    if freq > 0:
        tone(freq, 0, vol)
    else:
        no_tone()
    prev_freq = freq
    prev_vol = vol


def silence():
    global prev_freq, prev_vol
    no_tone()
    prev_freq = -1
    prev_vol = 0


def toggle_recording():
    global recording, looping, loop, loop_len, rec_t0, rec_tick, rec_rows
    if not recording:
        recording = True
        looping = False
        loop = []
        loop_len = 0
        rec_rows = [0] * 8
        rec_t0 = time.ticks_ms()
        rec_tick = rec_t0
        haptic_pulse(90, 25)
        return

    recording = False
    loop_len = time.ticks_diff(time.ticks_ms(), rec_t0)
    if loop_len < 200:
        loop = []
        loop_len = 0
        led_clear()
    else:
        build_play_trace()
    haptic_pulse(90, 25)


def toggle_looping():
    global looping, play_t0
    if recording or not loop:
        haptic_pulse(20, 10)
        return
    looping = not looping
    if looping:
        play_t0 = time.ticks_ms()
    else:
        silence()
    haptic_pulse(40, 15)


def open_sounds():
    global mode, looping, recording
    mode = MODE_SOUNDS
    looping = False
    recording = False
    silence()
    haptic_pulse(50, 15)


def handle_y_button():
    global y_down, y_long, scale_i
    held = button(BTN_PRESETS)
    if held:
        if not y_down:
            y_down = True
            y_long = False
        elif not y_long and button_held_ms(BTN_PRESETS) >= LONG_PRESS_MS:
            y_long = True
            open_sounds()
    elif y_down:
        if not y_long and mode == MODE_LIVE:
            scale_i = (scale_i + 1) % len(SCALES)
            haptic_pulse(35, 12)
        y_down = False


def wait_or_abort(ms):
    start = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), start) < ms:
        if button_pressed(BTN_BACK):
            silence()
            return False
        time.sleep_ms(18)
    return True


def draw_riff_led(freq, step):
    if freq <= 0:
        led_clear()
        return
    row = 7 - freq_to_semi(freq) * 7 // (len(FREQS) - 1)
    rows = [0] * 8
    rows[row] = 0x18 | (1 << (step & 7))
    led_set_frame(rows, 38)


def play_notes(notes, delay_ms):
    for i, freq in enumerate(notes):
        draw_riff_led(freq, i)
        if freq > 0:
            tone(freq, max(25, delay_ms - 25), 32)
        else:
            silence()
        if not wait_or_abort(delay_ms):
            return False
    silence()
    return True


def play_chirp(start, stop, step):
    i = 0
    freq = start
    while (step > 0 and freq < stop) or (step < 0 and freq > stop):
        draw_riff_led(freq, i)
        tone(freq, 25, 30)
        if not wait_or_abort(30):
            return False
        freq += step
        i += 1
    silence()
    return True


def play_siren():
    i = 0
    for _ in range(3):
        for freq in range(400, 900, 25):
            draw_riff_led(freq, i)
            tone(freq, 15, 30)
            if not wait_or_abort(18):
                return False
            i += 1
        for freq in range(900, 400, -25):
            draw_riff_led(freq, i)
            tone(freq, 15, 30)
            if not wait_or_abort(18):
                return False
            i += 1
    silence()
    return True


def play_riff():
    name, kind, notes, delay_ms = RIFFS[riff_i]
    draw_sound_screen("Loading")
    oled_show()
    haptic_pulse(35, 12)
    ok = True
    if kind == "melody":
        ok = play_notes(notes, delay_ms)
    elif kind == "up":
        ok = play_chirp(220, 1800, 55)
    elif kind == "down":
        ok = play_chirp(1800, 220, -55)
    else:
        ok = play_siren()
    silence()
    draw_sound_screen("Loaded" if ok else "Stopped")
    oled_show()
    time.sleep_ms(350)


def draw_joy_box(dx, dy):
    bx = 0
    by = 13
    size = 24
    edge = size - 1
    chamfer = 5
    oled_draw_box(bx + chamfer, by, size - chamfer * 2, 1)
    oled_draw_box(bx + chamfer, by + edge, size - chamfer * 2, 1)
    oled_draw_box(bx, by + chamfer, 1, size - chamfer * 2)
    oled_draw_box(bx + edge, by + chamfer, 1, size - chamfer * 2)
    for i in range(chamfer):
        c = chamfer - 1 - i
        oled_set_pixel(bx + c, by + i, 1)
        oled_set_pixel(bx + edge - c, by + i, 1)
        oled_set_pixel(bx + c, by + edge - i, 1)
        oled_set_pixel(bx + edge - c, by + edge - i, 1)

    cx = bx + size // 2
    cy = by + size // 2
    oled_set_pixel(cx - 1, cy, 1)
    oled_set_pixel(cx + 1, cy, 1)
    oled_set_pixel(cx, cy - 1, 1)
    oled_set_pixel(cx, cy + 1, 1)

    sx = cx + dx * (size // 2 - 4) // max(jcx, 1) - 2
    sy = cy + dy * (size // 2 - 4) // max(jcy, 1) - 1
    if sx < bx + 1:
        sx = bx + 1
    if sx > bx + size - 6:
        sx = bx + size - 6
    if sy < by + 1:
        sy = by + 1
    if sy > by + size - 4:
        sy = by + size - 4
    oled_draw_box(sx + 1, sy, 3, 1)
    oled_draw_box(sx, sy + 1, 5, 1)
    oled_draw_box(sx + 1, sy + 2, 3, 1)


def draw_live_screen(freq, name, vol, dx, dy):
    right = SCALES[scale_i][0]
    rec_label = "stop" if recording else "rec"
    x_label = "stop" if looping else "loop"
    ui.chrome_tall(
        "Synth",
        right,
        (("X", x_label), ("Y", "scale"), ("Y", "hold sounds")),
        "BACK",
        "exit",
        "OK",
        rec_label,
    )
    draw_joy_box(dx, dy)

    if freq > 0:
        ui.text(30, 14, name + " " + str(freq) + "Hz", 96)
        ui.text(30, 26, "Vol", 22)
        fill_w = vol
        if fill_w > 60:
            fill_w = 60
        ui.frame(52, 27, 62, 6)
        ui.fill(53, 28, fill_w, 4)
    else:
        ui.center(20, "---", 30, 96)

    if recording:
        status = "REC " + str(len(loop)) + " evts"
    elif loop:
        sec = str(loop_len // 1000) + "." + str(loop_len % 1000 // 100) + "s"
        status = ("Play " if looping else "Loop ") + sec
    else:
        status = "Ready"
    ui.text(0, 37, status, 124)
    oled_show()


def draw_sound_screen(status=None):
    name, _, _, _ = RIFFS[riff_i]
    ui.chrome_tall(
        "Sounds",
        str(riff_i + 1) + "/" + str(len(RIFFS)),
        (),
        "BACK",
        "close",
        "OK",
        "load",
    )
    ui.center(17, name)
    if status:
        ui.center(31, status)
    else:
        ui.center(31, "Joystick X selects")


def handle_sounds():
    global mode, riff_i, last_nav
    if button_pressed(BTN_BACK):
        mode = MODE_LIVE
        silence()
        haptic_pulse(35, 12)
        return
    if button_pressed(BTN_CONFIRM):
        play_riff()
        return

    now = time.ticks_ms()
    if time.ticks_diff(now, last_nav) >= NAV_MS:
        x = joy_x()
        if x < JOY_LOW:
            riff_i = (riff_i - 1) % len(RIFFS)
            last_nav = now
            haptic_pulse(20, 8)
        elif x > JOY_HIGH:
            riff_i = (riff_i + 1) % len(RIFFS)
            last_nav = now
            haptic_pulse(20, 8)


def splash():
    oled_clear()
    ui.header("Synth")
    ui.center(19, "Joystick tones")
    ui.center(33, "Loop + sounds")
    ui.action_bar("BACK", "exit", "OK", "start")
    oled_show()
    led_brightness(32)
    led_show_image(IMG_HEART)
    time.sleep_ms(900)
    led_clear()


def run():
    global prev_freq, prev_vol
    frame = 0
    gc_ticker = GCTicker()
    led_override_begin()
    splash()
    led_brightness(36)
    while True:
        if mode == MODE_SOUNDS:
            handle_sounds()
            draw_sound_screen()
            oled_show()
            time.sleep_ms(25)
            continue

        if button_pressed(BTN_BACK):
            return

        handle_y_button()
        if button_pressed(BTN_CONFIRM):
            toggle_recording()
        if button_pressed(BTN_SQUARE):
            toggle_looping()

        freq, name, vol, dx, dy, note_idx = live_note()
        note_count = len(SCALES[scale_i][1]) * 2
        live = freq > 0
        play_freq = freq
        play_vol = vol

        if looping and loop and not recording:
            elapsed = time.ticks_diff(time.ticks_ms(), play_t0)
            pos = elapsed % loop_len if loop_len > 0 else 0
            if not live:
                play_freq = 0
                play_vol = 0
                for event_t, event_freq, event_vol in loop:
                    if event_t <= pos:
                        play_freq = event_freq
                        play_vol = event_vol
                    else:
                        break
            draw_play_led(pos)
        elif recording:
            t = time.ticks_diff(time.ticks_ms(), rec_t0)
            if len(loop) < MAX_EVENTS:
                if (
                    not loop
                    or loop[-1][1] != freq
                    or (freq > 0 and abs(loop[-1][2] - vol) > 8)
                ):
                    loop.append((t, freq, vol))
            draw_record_led(note_idx, note_count, vol)
        else:
            draw_live_led(note_idx, note_count, vol)

        apply_tone(play_freq, play_vol)

        frame += 1
        if (frame & 1) == 0:
            draw_live_screen(play_freq, name, play_vol, dx, dy)

        gc_ticker.tick()
        time.sleep_ms(18)


def cleanup():
    silence()
    haptic_off()
    led_clear()
    led_override_end()
    oled_clear(True)


def main():
    run()


run_app("Synth", main, cleanup)
exit()
