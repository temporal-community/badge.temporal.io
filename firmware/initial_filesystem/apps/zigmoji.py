"""
zigmoji.py - Zig-moji animation viewer

Browse Zig-moji characters and sponsor logos.

Controls:
  LEFT / RIGHT  previous / next animation
  UP            exit
"""
import time
import gc

JOY_LOW = 1100
JOY_HIGH = 3000
NAV_MS = 170

BASE = "/apps/zigmoji/"

# (display name, [frame filenames], ms-per-frame)
CHANNELS = [
    ("Ziggy Fly",   ["fly_%02d.fb" % i for i in range(1, 7)],   180),
    ("Ziggy Blink", ["blink_%02d.fb" % i for i in range(1, 3)],  600),
    ("Ziggy Heart", ["heart_%02d.fb" % i for i in range(1, 4)],  250),
    ("Ziggy Sleep", ["sleep_%02d.fb" % i for i in range(1, 4)],  500),
    ("Ziggy Wow",   ["wow_%02d.fb" % i for i in range(1, 3)],    400),
    ("T-Heart",     ["theart_%02d.fb" % i for i in range(1, 4)], 300),
    ("Starry",      ["starry_%02d.fb" % i for i in range(1, 4)], 300),
    ("Vulcan",      ["vulcan_%02d.fb" % i for i in range(1, 4)], 250),
    ("Sponsors",    [
        "sp_apartment.fb", "sp_augment.fb",   "sp_bitovi.fb",
        "sp_braintrust.fb","sp_google.fb",     "sp_grid.fb",
        "sp_liatro.fb",    "sp_tailscale.fb",  "sp_techno.fb",
    ], 2000),
]


def load_frame(filename):
    try:
        with open(BASE + filename, "rb") as f:
            return f.read()
    except OSError:
        return None


def load_channel_frames(filenames):
    """Load all frames for a channel. Returns list of bytes objects."""
    frames = []
    for fn in filenames:
        data = load_frame(fn)
        if data is not None:
            frames.append(data)
    return frames


def show_label(text):
    """Show channel name as text overlay for a moment."""
    oled_clear()
    w = oled_text_width(text)
    oled_set_cursor((128 - w) // 2, 28)
    oled_print(text)
    oled_show()
    time.sleep_ms(700)


def build_valid_channels():
    """Filter CHANNELS to those with at least one loadable frame."""
    valid = []
    for name, filenames, ms in CHANNELS:
        if load_frame(filenames[0]) is not None:
            valid.append((name, filenames, ms))
    return valid


# ── Startup ────────────────────────────────────────────────────────────────────

valid = build_valid_channels()

if not valid:
    oled_clear()
    oled_set_cursor(8, 24)
    oled_print("No zigmoji files!")
    oled_set_cursor(8, 38)
    oled_print("Run convert script")
    oled_show()
    time.sleep_ms(2000)
    exit()

# Brief title screen
oled_clear()
oled_set_cursor(28, 20)
oled_print("Zig-moji")
oled_set_cursor(12, 38)
oled_print("Joy browse B exit")
oled_show()
time.sleep_ms(1200)

# ── Main loop ──────────────────────────────────────────────────────────────────

ch_idx = 0
frame_idx = 0

name, filenames, frame_ms = valid[ch_idx]
cached = load_channel_frames(filenames)

if not cached:
    oled_clear()
    oled_set_cursor(0, 28)
    oled_print("Load error")
    oled_show()
    time.sleep_ms(1000)
    exit()

oled_set_framebuffer(cached[0])
last_ms = time.ticks_ms()
last_nav_ms = 0

while True:
    now = time.ticks_ms()

    # Advance animation frame
    if len(cached) > 1 and time.ticks_diff(now, last_ms) >= frame_ms:
        last_ms = now
        frame_idx = (frame_idx + 1) % len(cached)
        oled_set_framebuffer(cached[frame_idx])

    if button_pressed(BTN_BACK):
        break

    # Navigation
    if time.ticks_diff(now, last_nav_ms) >= NAV_MS:
        jx = joy_x()
        step = 0
        if jx > JOY_HIGH:
            step = 1
        elif jx < JOY_LOW:
            step = -1
        if step:
            ch_idx = (ch_idx + step) % len(valid)
            name, filenames, frame_ms = valid[ch_idx]
            show_label(name)
            gc.collect()
            cached = load_channel_frames(filenames)
            frame_idx = 0
            last_ms = time.ticks_ms()
            last_nav_ms = now
            if cached:
                oled_set_framebuffer(cached[0])

    time.sleep_ms(20)

oled_clear(True)
exit()
