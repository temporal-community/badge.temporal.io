"""BreakSnake constants and shared state helpers."""

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

SCORE_PATH = "/breaksnake_score.json"
SCORE_DEFAULTS = {"total": 0, "bricks": 0, "food": 0, "seconds": 0}

FRAME_MS = 34
SNAKE_START_MS = 380
SNAKE_MIN_MS = 170
WIDE_MS = 6500
TOAST_MS = 1800

OLED_W = 128
OLED_H = 64
PLAY_TOP = 10
SCALE = 10

BALL = 2
PADDLE_W = 24
WIDE_PADDLE_W = 38
PADDLE_H = 3
PADDLE_Y = 60
PADDLE_STEP = 4

PICKUP_SIZE = 3
PICKUP_STEP = 1
PICKUP_CHANCE = 5
SPECIAL_FOOD_CHANCE = 4
SNAKE_MIN_LEN = 3
SNAKE_SHRINK = 3

BRICK_ROWS = 4
BRICK_COLS = 8
BRICK_X = 1
BRICK_Y = 13
BRICK_W = 14
BRICK_H = 4
BRICK_STEP_X = 16
BRICK_STEP_Y = 6

TITLE_ICON = [0x18, 0x3C, 0x7E, 0xDB, 0x18, 0x18, 0x24, 0x42]
GAME_OVER_ICON = [0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81]


def make_food(snake, avoid=None):
    for _i in range(80):
        point = (random.randint(0, 7), random.randint(0, 7))
        if point not in snake and (avoid is None or point not in avoid):
            return point
    return (0, 0)


def load_best():
    return load_score(SCORE_PATH, SCORE_DEFAULTS)


def save_best(best):
    save_score(SCORE_PATH, best)


def new_game():
    seed_random()

    snake = [(4, 4), (3, 4), (2, 4)]
    return {
        "paddle": 52,
        "ball_x": 64 * SCALE,
        "ball_y": 42 * SCALE,
        "ball_dx": 13,
        "ball_dy": -12,
        "bricks": [0xFF] * BRICK_ROWS,
        "brick_hits": 0,
        "pickup_x": -1,
        "pickup_y": 0,
        "snake": snake,
        "snake_dir": (1, 0),
        "next_dir": (1, 0),
        "food": make_food(snake),
        "special_food": None,
        "food_count": 0,
        "wide_until": 0,
        "toast": "",
        "toast_until": 0,
        "started": time.ticks_ms(),
        "last_frame": 0,
        "last_snake": 0,
        "reason": "",
    }


def show_toast(game, now, text):
    game["toast"] = text
    game["toast_until"] = ticks_add(now, TOAST_MS)


def wide_active(game, now):
    return active_until(game["wide_until"], now)


def paddle_width(game, now):
    if wide_active(game, now):
        return WIDE_PADDLE_W
    return PADDLE_W


def live_score(game, now):
    seconds = elapsed_seconds(game["started"], now)
    return game["brick_hits"] * 10 + game["food_count"] * 25 + seconds


def score_for(game, end_ms):
    seconds = elapsed_seconds(game["started"], end_ms)
    total = game["brick_hits"] * 10 + game["food_count"] * 25 + seconds
    return {
        "total": total,
        "bricks": game["brick_hits"],
        "food": game["food_count"],
        "seconds": seconds,
    }
