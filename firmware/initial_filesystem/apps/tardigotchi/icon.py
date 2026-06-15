# Tardigotchi: Conway's Game of Life creatures
# 8x8 LED glider frames + OLED GoL simulation

# ---- 8x8 LED: Glider that actually translates diagonally ----
# A GoL glider repeats every 4 generations, shifted 1 right and 1 down.
# We pre-generate 16 frames (4 full cycles) covering the 8x8 grid with wrapping.
# Glider cells relative to origin: phase 0: (0,1),(1,2),(2,0),(2,1),(2,2)
# Correct GoL glider: 4 generations, 5 cells each, shifts +1,+1 per cycle
_GLIDER_PHASES = [
    [(0, 1), (1, 2), (2, 0), (2, 1), (2, 2)],
    [(0, 0), (0, 2), (1, 1), (1, 2), (2, 1)],
    [(0, 2), (1, 0), (1, 2), (2, 1), (2, 2)],
    [(0, 0), (1, 1), (1, 2), (2, 0), (2, 1)],
]

def _build_glider_frames():
    frames = []
    for step in range(16):
        phase = step % 4
        diag = step // 4  # shifts 1 right, 1 down per cycle
        frame = [0] * 8
        for dr, dc in _GLIDER_PHASES[phase]:
            r = (dr + diag) % 8
            c = (dc + diag) % 8
            frame[r] |= (1 << (7 - c))
        frames.append(frame)
    return frames

LED_GLIDER = _build_glider_frames()

LED_SLEEP = [
    0b00000000, 0b00111100, 0b01111110, 0b01111110,
    0b01111110, 0b00111100, 0b00011000, 0b00000000,
]

# ---- Conway patterns as (row, col) cell lists ----
# Placed relative to an origin; engine stamps them onto the grid

# Glider: the pet itself
PAT_GLIDER = [(0, 1), (1, 2), (2, 0), (2, 1), (2, 2)]

# Level 2: Lightweight spaceship (LWSS)
PAT_LWSS = [
    (0, 1), (0, 4),
    (1, 0),
    (2, 0), (2, 4),
    (3, 0), (3, 1), (3, 2), (3, 3),
]

# Level 3: R-pentomino (chaotic, short-lived fireworks)
PAT_RPENT = [(0, 1), (0, 2), (1, 0), (1, 1), (2, 1)]

# Level 4: Pulsar (period-3 oscillator)
PAT_PULSAR = []
for _dx in (-1, 1):
    for _dy in (-1, 1):
        for _i in (2, 3, 4):
            PAT_PULSAR.append((_i * _dy + 6, 1 * _dx + 6))
            PAT_PULSAR.append((1 * _dy + 6, _i * _dx + 6))
        for _i in (2, 3, 4):
            PAT_PULSAR.append((_i * _dy + 6, 6 * _dx + 6))
            PAT_PULSAR.append((6 * _dy + 6, _i * _dx + 6))

# Level 5: Gosper glider gun (fits in ~36x9)
PAT_GUN = [
    (4, 0), (4, 1), (5, 0), (5, 1),
    (4, 10), (5, 10), (6, 10), (3, 11), (7, 11),
    (2, 12), (8, 12), (2, 13), (8, 13),
    (5, 14),
    (3, 15), (7, 15), (4, 16), (5, 16), (6, 16),
    (5, 17),
    (2, 20), (3, 20), (4, 20), (2, 21), (3, 21), (4, 21),
    (1, 22), (5, 22),
    (0, 24), (1, 24), (5, 24), (6, 24),
    (2, 34), (3, 34), (2, 35), (3, 35),
]

# Level thresholds: (min_happiness_total, pattern, name)
# happiness_total = cumulative happiness earned over lifetime
# Levels: XP threshold, GoL pattern unlock, name, sprite
# Sprites: egg -> crack -> happy -> eating -> mad
LEVEL_UNLOCKS = [
    (0, PAT_GLIDER, "Egg"),         # level 0: egg sprite
    (25, PAT_GLIDER, "Hatchling"),   # level 1: cracking egg
    (100, PAT_LWSS, "Baby"),         # level 2: happy ziggy
    (250, PAT_RPENT, "Teen"),        # level 3: eating ziggy
    (500, PAT_PULSAR, "Adult"),      # level 4: mad ziggy
    (1000, PAT_GUN, "Elder"),        # level 5: mad ziggy (max)
]


def get_level(xp):
    lvl = 0
    for i, (threshold, _, _) in enumerate(LEVEL_UNLOCKS):
        if xp >= threshold:
            lvl = i
    return lvl


def get_unlocked_patterns(xp):
    return [(p, n) for (t, p, n) in LEVEL_UNLOCKS if xp >= t]


# ---- Conway's Game of Life engine ----
# Small grid: 30x24 cells, each drawn as 2x2 pixels = 60x48 OLED pixels

GW = 30
GH = 24


def make_grid():
    return [0] * GH


def stamp(grid, pattern, oy, ox):
    for dr, dc in pattern:
        r = (oy + dr) % GH
        c = (ox + dc) % GW
        grid[r] |= (1 << c)


def step(grid):
    new = [0] * GH
    for y in range(GH):
        above = grid[(y - 1) % GH]
        cur = grid[y]
        below = grid[(y + 1) % GH]
        # Shift rows to count neighbors fast
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
            alive = cur & bx
            if n == 3 or (n == 2 and alive):
                new[y] |= bx
    return new


def draw_grid(grid, ox, oy):
    from badge import oled_draw_box, oled_set_draw_color
    oled_set_draw_color(1)
    for y in range(GH):
        row = grid[y]
        if not row:
            continue
        for x in range(GW):
            if row & (1 << x):
                oled_draw_box(ox + x * 2, oy + y * 2, 2, 2)


# ---- Tardigrade OLED sprite (drawn with oled_draw_box for speed) ----

def get_sprite(mood="happy", level=0, action=None):
    """Pick the right sprite based on context."""
    from sprites import (ZIGGY_EGG, ZIGGY_CRACK, ZIGGY_HAPPY,
                         ZIGGY_SLEEPING, ZIGGY_SAD, ZIGGY_EATING, ZIGGY_MAD)
    # Egg stages always show egg/crack regardless of action
    if level == 0:
        return ZIGGY_EGG
    if level == 1:
        return ZIGGY_CRACK
    # Action override for hatched tardi
    if action == "feed":
        return ZIGGY_EATING
    if action == "play":
        if level >= 4:
            return ZIGGY_HAPPY
        if level == 3:
            return ZIGGY_EATING
        return ZIGGY_HAPPY
    if action == "pet":
        return ZIGGY_HAPPY
    # No action: use mood
    if mood == "sleeping":
        return ZIGGY_SLEEPING
    if mood in ("sad", "okay"):
        return ZIGGY_SAD
    return ZIGGY_HAPPY


def get_sprite_by_mood_hunger(mood, hunger, level=2):
    """For main screen: factor in hunger + level for sprite selection.
    Level 2 = Baby (happy/sad), Level 3 = Teen (eating), Level 4+ = Adult/Elder (mad when hungry)."""
    from sprites import ZIGGY_MAD, ZIGGY_HAPPY, ZIGGY_SLEEPING, ZIGGY_SAD, ZIGGY_EATING
    if mood == "sleeping":
        return ZIGGY_SLEEPING
    if mood in ("sad", "okay"):
        return ZIGGY_SAD
    # Level 4+: default to mad sprite, happy when well-fed
    if level >= 4:
        if hunger > 60:
            return ZIGGY_MAD
        return ZIGGY_HAPPY
    # Level 3: teen uses eating sprite as default look
    if level == 3:
        if hunger > 80:
            return ZIGGY_MAD
        return ZIGGY_EATING
    # Level 2: baby
    if hunger > 80:
        return ZIGGY_MAD
    return ZIGGY_HAPPY


def draw_oled_tardi(ox, oy, mood="happy", level=0, action=None, hunger=0, clip_top=0, small=False):
    from badge import oled_set_draw_color, oled_draw_box
    from sprites import ZIGGY_W, ZIGGY_H
    if action:
        spr = get_sprite(mood, level, action)
    elif level <= 1:
        spr = get_sprite(mood, level)
    else:
        spr = get_sprite_by_mood_hunger(mood, hunger, level)
    bpr = ZIGGY_W // 8
    oled_set_draw_color(1)
    if small:
        # 90% scale via nearest-neighbor sampling
        dw = ZIGGY_W * 9 // 10
        dh = ZIGGY_H * 9 // 10
        # Find leftmost content column to left-align the sprite
        left_col = ZIGGY_W
        for sy in range(ZIGGY_H):
            ro = sy * bpr
            for xb in range(bpr):
                if spr[ro + xb]:
                    col = xb * 8
                    b = spr[ro + xb]
                    while not (b & 0x80):
                        b <<= 1
                        col += 1
                    if col < left_col:
                        left_col = col
                    break
            if left_col == 0:
                break
        # Scale the left margin and subtract it so content starts at ox
        trim = left_col * 9 // 10
        for dy in range(dh):
            py = oy + dy
            if py < clip_top or py > 63:
                continue
            sy = dy * ZIGGY_H // dh
            row_off = sy * bpr
            run_start = -1
            for dx in range(dw):
                sx = dx * ZIGGY_W // dw
                px = ox + dx - trim
                if spr[row_off + (sx >> 3)] & (1 << (7 - (sx & 7))):
                    if run_start < 0:
                        run_start = px
                else:
                    if run_start >= 0:
                        oled_draw_box(run_start, py, px - run_start, 1)
                        run_start = -1
            if run_start >= 0:
                oled_draw_box(run_start, py, ox + dw - trim - run_start, 1)
    else:
        # Full-size: draw using horizontal runs
        for y in range(ZIGGY_H):
            py = oy + y
            if py < clip_top or py > 63:
                continue
            run_start = -1
            for xb in range(bpr):
                byte = spr[y * bpr + xb]
                for bit in range(8):
                    px = ox + xb * 8 + bit
                    if byte & (1 << (7 - bit)):
                        if run_start < 0:
                            run_start = px
                    else:
                        if run_start >= 0:
                            oled_draw_box(run_start, py, px - run_start, 1)
                            run_start = -1
            if run_start >= 0:
                oled_draw_box(run_start, py, ox + ZIGGY_W - run_start, 1)


def led_glider_frame(phase):
    return LED_GLIDER[phase % 16]


def led_frame_for_mood(m):
    if m == "sleeping":
        return LED_SLEEP
    return LED_GLIDER[0]


def led_frame_for_action(action):
    idx = {"feed": 4, "pet": 8, "play": 12}.get(action, 0)
    return LED_GLIDER[idx]
