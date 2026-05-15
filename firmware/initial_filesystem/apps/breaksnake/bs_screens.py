"""BreakSnake OLED and LED drawing."""

import time

from badge import *

from badge_app import wait_choice
import badge_ui as ui
from bs_data import (
    BALL,
    BRICK_COLS,
    BRICK_H,
    BRICK_ROWS,
    BRICK_STEP_X,
    BRICK_STEP_Y,
    BRICK_W,
    BRICK_X,
    BRICK_Y,
    GAME_OVER_ICON,
    PADDLE_H,
    PADDLE_Y,
    PICKUP_SIZE,
    SCALE,
    TITLE_ICON,
    live_score,
    paddle_width,
    wide_active,
)


def box(x, y, width, height):
    oled_draw_box(int(x), int(y), int(width), int(height))


def draw_joystick_icon(x, y):
    ui.frame(x, y, 18, 16)
    ui.hline(x + 4, y + 8, 10)
    ui.vline(x + 9, y + 3, 10)
    ui.fill(x + 7, y + 6, 5, 5)


def draw_axis_icon(x, y, horizontal):
    if horizontal:
        cy = y + 4
        ui.hline(x + 2, cy, 14)
        oled_set_pixel(x, cy, 1)
        oled_set_pixel(x + 1, cy - 1, 1)
        oled_set_pixel(x + 1, cy + 1, 1)
        oled_set_pixel(x + 17, cy, 1)
        oled_set_pixel(x + 16, cy - 1, 1)
        oled_set_pixel(x + 16, cy + 1, 1)
        ui.fill(x + 8, y + 2, 3, 5)
    else:
        cx = x + 9
        ui.vline(cx, y + 1, 7)
        oled_set_pixel(cx, y, 1)
        oled_set_pixel(cx - 1, y + 1, 1)
        oled_set_pixel(cx + 1, y + 1, 1)
        oled_set_pixel(cx, y + 8, 1)
        oled_set_pixel(cx - 1, y + 7, 1)
        oled_set_pixel(cx + 1, y + 7, 1)
        ui.fill(x + 7, y + 3, 5, 3)


def center_large(y, value):
    try:
        oled_set_font("6x10")
    except Exception:
        pass

    label = ui.fit(value, 124)
    width = oled_text_width(label)
    oled_set_cursor((128 - width) // 2, y)
    oled_print(label)
    ui.font()


def draw_end_reason(reason):
    label = {
        "Snake bite": "Your Snake Crashed",
        "Breakout miss": "Your Ball Escaped",
        "Quit": "Run Quit",
    }.get(reason, " ".join(part[:1].upper() + part[1:] for part in reason.split()))

    center_large(13, label)


def title(best):
    led_set_frame(TITLE_ICON, 40)
    ui.chrome("BreakSnake", "Best " + str(best["total"]), "OK", "start", "BACK", "quit")
    ui.text(4, 14, "Top OLED: Breakout", 120)
    ui.text(4, 24, "Bottom LEDs: Snake", 120)
    draw_joystick_icon(5, 34)
    draw_axis_icon(28, 35, True)
    ui.text(51, 37, "Joystick X: both", 72)
    draw_axis_icon(28, 43, False)
    ui.text(51, 46, "Joystick Y: snake", 72)
    oled_show()

    return wait_choice(True, False)


def draw_game_header(game, now):
    score_label = "S " + str(live_score(game, now))
    label = "BreakSnake"
    if game["toast"] and time.ticks_diff(game["toast_until"], now) > 0:
        label = game["toast"]
    elif wide_active(game, now):
        label = "Wide paddle"

    ui.header(label, score_label)


def draw_snake(game):
    rows = [0] * 8
    for x, y in game["snake"]:
        rows[y] |= 0x80 >> x

    food_x, food_y = game["food"]
    rows[food_y] |= 0x80 >> food_x

    if game["special_food"] is not None:
        special_x, special_y = game["special_food"]
        if (time.ticks_ms() // 180) & 1:
            rows[special_y] |= 0x80 >> special_x

    led_set_frame(rows, 55)


def draw_breakout(game, now):
    oled_clear()
    for row in range(BRICK_ROWS):
        mask = game["bricks"][row]
        y = BRICK_Y + row * BRICK_STEP_Y
        for col in range(BRICK_COLS):
            if mask & (1 << col):
                box(BRICK_X + col * BRICK_STEP_X, y, BRICK_W, BRICK_H)

    width = paddle_width(game, now)
    box(game["paddle"], PADDLE_Y, width, PADDLE_H)

    if game["pickup_x"] >= 0:
        box(game["pickup_x"], game["pickup_y"], PICKUP_SIZE, PICKUP_SIZE)

    box(game["ball_x"] // SCALE, game["ball_y"] // SCALE, BALL, BALL)
    draw_game_header(game, now)
    oled_show()


def game_over(score, best, new_best, reason):
    led_set_frame(GAME_OVER_ICON, 50)
    ui.chrome(
        "Game Over",
        "New Best" if new_best else "Best " + str(best["total"]),
        "OK",
        "again",
        "BACK",
        "quit",
    )
    draw_end_reason(reason)
    ui.center(28, "Score " + str(score["total"]))
    ui.center(
        37,
        "Bricks " + str(score["bricks"]) + "   Food " + str(score["food"]),
    )
    ui.center(46, "Time " + str(score["seconds"]) + "s")
    oled_show()

    return wait_choice(True, False)
