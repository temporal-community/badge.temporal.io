"""Flappy Asteroids constants and state helpers."""

import random
import time

from badge_app import (
    active_until,
    clamp,
    elapsed_seconds,
    load_score,
    save_score,
    seed_random,
    ticks_add,
)

SCORE_PATH = "/flappy_asteroids_score.json"
SCORE_DEFAULTS = {"total": 0, "rocks": 0, "pipes": 0, "seconds": 0}

FRAME_MS = 34
BACK_QUIT_MS = 1000

OLED_W = 128
OLED_H = 64
PLAY_TOP = 10
SCALE = 16

SHIP_R = 3
MAX_SHIP_SPEED = 42
SHOT_SPEED = 52
SHOT_MS = 260
SHOT_TTL = 620
MAX_SHOTS = 3

ROCK_LIMIT = 6
ROCK_ADD_MS = 25000
ROCK_ADD_KILLS = 5
ROCK_RADII = (0, 2, 4, 7)
ROCK_SHAPES = (
    (),
    (
        ((-2, 0), (-1, -2), (1, -1), (2, 0), (1, 2), (-1, 1)),
        ((-2, -1), (0, -2), (2, -1), (1, 1), (2, 2), (-1, 2), (-2, 1)),
        ((-1, -2), (2, -1), (1, 1), (0, 2), (-2, 1), (-2, -1)),
    ),
    (
        (
            (-4, -1),
            (-3, -4),
            (0, -3),
            (2, -4),
            (4, -1),
            (3, 2),
            (1, 4),
            (-2, 3),
            (-4, 1),
        ),
        (
            (-4, -2),
            (-1, -4),
            (1, -3),
            (4, -4),
            (3, 0),
            (4, 3),
            (0, 4),
            (-2, 2),
            (-4, 3),
            (-3, 0),
        ),
        (
            (-3, -4),
            (0, -3),
            (3, -4),
            (4, -1),
            (2, 1),
            (3, 4),
            (-1, 3),
            (-4, 4),
            (-3, 1),
            (-4, -1),
        ),
    ),
    (
        (
            (-7, -2),
            (-5, -6),
            (-2, -5),
            (1, -7),
            (5, -5),
            (7, -2),
            (5, 1),
            (7, 4),
            (3, 7),
            (0, 5),
            (-4, 6),
            (-7, 2),
        ),
        (
            (-6, -5),
            (-2, -7),
            (0, -5),
            (4, -7),
            (7, -3),
            (5, 0),
            (7, 3),
            (4, 6),
            (1, 5),
            (-3, 7),
            (-7, 4),
            (-5, 0),
        ),
        (
            (-7, -3),
            (-3, -6),
            (0, -4),
            (3, -7),
            (6, -5),
            (7, -1),
            (4, 1),
            (6, 5),
            (2, 7),
            (-1, 5),
            (-5, 7),
            (-7, 3),
            (-5, 0),
        ),
        (
            (-6, -2),
            (-5, -6),
            (-1, -7),
            (2, -5),
            (6, -6),
            (7, -2),
            (5, 1),
            (7, 5),
            (3, 6),
            (-1, 4),
            (-4, 7),
            (-7, 3),
        ),
    ),
)
ROCK_SCORE = 10

BIRD_X = 2
BIRD_SCALE = 10
FLAP_V = -11
GRAVITY = 3
MAX_FALL_V = 12
FLAPPY_TICK_MS = 120
PIPE_START_MS = 430
PIPE_MIN_MS = 260
PIPE_SCORE = 25

BENEFIT_MS = 5500
TOAST_MS = 1700
PICKUP_CHANCE = 4
PIPE_BENEFIT_CHANCE = 5

DIR_X = (0, 3, 6, 8, 9, 8, 6, 3, 0, -3, -6, -8, -9, -8, -6, -3)
DIR_Y = (-9, -8, -6, -3, 0, 3, 6, 8, 9, 8, 6, 3, 0, -3, -6, -8)

TITLE_ICON = (0x24, 0x7E, 0xDB, 0x3C, 0x18, 0x66, 0x42, 0x42)
GAME_OVER_ICON = (0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81)

FLAPPY_BENEFITS = ("Wide Gap", "Slow Pipes", "Bird Shield")
ASTEROID_BENEFITS = ("Slow Rocks", "Ship Shield", "Power Shot")


def load_best():
    return load_score(SCORE_PATH, SCORE_DEFAULTS)


def save_best(best):
    save_score(SCORE_PATH, best)


def random_gap(gap_h):
    return random.randint(1, 7 - gap_h)


def make_rock(x=None, y=None, size=3):
    if x is None:
        x = random.choice((12, OLED_W - 12)) * SCALE
    if y is None:
        y = random.randint(PLAY_TOP + 8, OLED_H - 8) * SCALE

    vx = random.choice((-13, -10, -7, 7, 10, 13))
    vy = random.choice((-9, -6, 6, 9))
    shapes = ROCK_SHAPES[size]
    return {
        "x": x,
        "y": y,
        "vx": vx,
        "vy": vy,
        "size": size,
        "shape": random.randint(0, len(shapes) - 1),
    }


def new_game():
    seed_random()

    return {
        "ship_x": 64 * SCALE,
        "ship_y": 38 * SCALE,
        "ship_vx": 0,
        "ship_vy": 0,
        "ship_angle": 0,
        "shots": [],
        "shot_ready": 0,
        "rocks": [make_rock(size=3), make_rock(size=3), make_rock(size=3)],
        "next_rock_at": ticks_add(time.ticks_ms(), ROCK_ADD_MS),
        "next_rock_kills": ROCK_ADD_KILLS,
        "pickup": None,
        "bird_y": 3 * BIRD_SCALE,
        "bird_v": 0,
        "pipe_x": 8,
        "pipe_gap": random_gap(3),
        "pipe_scored": False,
        "rocks_destroyed": 0,
        "pipes_passed": 0,
        "wide_gap_until": 0,
        "slow_pipes_until": 0,
        "bird_shield": False,
        "slow_rocks_until": 0,
        "ship_shield": False,
        "power_shot": False,
        "toast": "",
        "toast_until": 0,
        "started": time.ticks_ms(),
        "last_frame": 0,
        "last_flappy": 0,
        "last_pipe": 0,
        "reason": "",
    }


def show_toast(game, now, text):
    game["toast"] = text
    game["toast_until"] = ticks_add(now, TOAST_MS)


def flappy_has_benefit(game, now):
    return (
        active_until(game["wide_gap_until"], now)
        or active_until(game["slow_pipes_until"], now)
        or game["bird_shield"]
    )


def asteroids_has_benefit(game, now):
    return (
        active_until(game["slow_rocks_until"], now)
        or game["ship_shield"]
        or game["power_shot"]
    )


def grant_flappy_benefit(game, now, benefit):
    if flappy_has_benefit(game, now):
        return False
    if benefit == "Wide Gap":
        game["wide_gap_until"] = ticks_add(now, BENEFIT_MS)
    elif benefit == "Slow Pipes":
        game["slow_pipes_until"] = ticks_add(now, BENEFIT_MS)
    else:
        game["bird_shield"] = True
    show_toast(game, now, benefit)
    return True


def grant_asteroids_benefit(game, now, benefit):
    if asteroids_has_benefit(game, now):
        return False
    if benefit == "Slow Rocks":
        game["slow_rocks_until"] = ticks_add(now, BENEFIT_MS)
    elif benefit == "Ship Shield":
        game["ship_shield"] = True
    else:
        game["power_shot"] = True
    show_toast(game, now, benefit)
    return True


def live_score(game, now):
    seconds = elapsed_seconds(game["started"], now)
    return (
        game["rocks_destroyed"] * ROCK_SCORE
        + game["pipes_passed"] * PIPE_SCORE
        + seconds
    )


def score_for(game, end_ms):
    seconds = elapsed_seconds(game["started"], end_ms)
    return {
        "total": live_score(game, end_ms),
        "rocks": game["rocks_destroyed"],
        "pipes": game["pipes_passed"],
        "seconds": seconds,
    }
