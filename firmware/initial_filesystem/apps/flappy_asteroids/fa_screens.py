"""Flappy Asteroids OLED and LED drawing."""

import time

from badge import *

from badge_app import wait_choice
import badge_ui as ui
from fa_data import (
    BIRD_X,
    BIRD_SCALE,
    DIR_X,
    DIR_Y,
    GAME_OVER_ICON,
    OLED_H,
    OLED_W,
    PLAY_TOP,
    ROCK_RADII,
    ROCK_SHAPES,
    SCALE,
    SHIP_R,
    TITLE_ICON,
    active_until,
    live_score,
)


def pixel(x, y):
    if 0 <= x < OLED_W and PLAY_TOP <= y < OLED_H:
        oled_set_pixel(int(x), int(y), 1)


def line(x0, y0, x1, y1):
    x0 = int(x0)
    y0 = int(y0)
    x1 = int(x1)
    y1 = int(y1)
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        pixel(x0, y0)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def draw_ship(game):
    cx = game["ship_x"] // SCALE
    cy = game["ship_y"] // SCALE
    angle = game["ship_angle"] & 15
    left = (angle - 5) & 15
    right = (angle + 5) & 15
    nose_x = cx + DIR_X[angle] // 2
    nose_y = cy + DIR_Y[angle] // 2
    left_x = cx + DIR_X[left] // 3
    left_y = cy + DIR_Y[left] // 3
    right_x = cx + DIR_X[right] // 3
    right_y = cy + DIR_Y[right] // 3
    line(nose_x, nose_y, left_x, left_y)
    line(nose_x, nose_y, right_x, right_y)
    line(left_x, left_y, right_x, right_y)


def draw_rock_outline(cx, cy, points):
    last = len(points) - 1
    for i in range(len(points)):
        x0, y0 = points[i]
        x1, y1 = points[0 if i == last else i + 1]
        line(cx + x0, cy + y0, cx + x1, cy + y1)


def draw_rock(rock):
    cx = rock["x"] // SCALE
    cy = rock["y"] // SCALE
    size = rock["size"]
    radius = ROCK_RADII[size]
    shapes = ROCK_SHAPES[size]
    points = shapes[rock.get("shape", 0) % len(shapes)]

    xs = [0]
    if cx - radius < 0:
        xs.append(OLED_W)
    if cx + radius >= OLED_W:
        xs.append(-OLED_W)

    play_h = OLED_H - PLAY_TOP
    ys = [0]
    if cy - radius < PLAY_TOP:
        ys.append(play_h)
    if cy + radius >= OLED_H:
        ys.append(-play_h)

    for dx in xs:
        for dy in ys:
            draw_rock_outline(cx + dx, cy + dy, points)


def draw_pickup(game, now):
    pickup = game["pickup"]
    if not pickup or ((now // 160) & 1):
        return
    x = pickup["x"] // SCALE
    y = pickup["y"] // SCALE
    ui.frame(x - 1, y - 1, 4, 4)


def active_label(game, now):
    if game["toast"] and time.ticks_diff(game["toast_until"], now) > 0:
        return game["toast"]
    if game["bird_shield"]:
        return "Bird Shield"
    if active_until(game["wide_gap_until"], now):
        return "Wide Gap"
    if active_until(game["slow_pipes_until"], now):
        return "Slow Pipes"
    if game["ship_shield"]:
        return "Ship Shield"
    if game["power_shot"]:
        return "Power Shot"
    if active_until(game["slow_rocks_until"], now):
        return "Slow Rocks"
    return "Asteroids"


def draw_asteroids(game, now):
    oled_clear()
    for rock in game["rocks"]:
        draw_rock(rock)
    for shot in game["shots"]:
        pixel(shot["x"] // SCALE, shot["y"] // SCALE)
    draw_pickup(game, now)
    draw_ship(game)
    ui.header(active_label(game, now), "S " + str(live_score(game, now)))
    oled_show()


def draw_flappy(game, now):
    rows = [0] * 8
    gap_h = 4 if active_until(game["wide_gap_until"], now) else 3
    pipe_x = game["pipe_x"]
    if 0 <= pipe_x < 8:
        for y in range(8):
            if y < game["pipe_gap"] or y >= game["pipe_gap"] + gap_h:
                rows[y] |= 0x80 >> pipe_x

    bird_y = (game["bird_y"] + BIRD_SCALE // 2) // BIRD_SCALE
    if 0 <= bird_y < 8:
        if not game["bird_shield"] or ((now // 140) & 1) == 0:
            rows[bird_y] |= 0x80 >> BIRD_X
    led_set_frame(rows, 55)


def title(best):
    led_set_frame(TITLE_ICON, 40)
    ui.chrome(
        "Flappy Asteroids",
        "Best " + str(best["total"]),
        "OK",
        "start",
        "BACK",
        "quit",
    )
    ui.text(4, 13, "Top OLED: Asteroids", 120)
    ui.text(4, 23, "Bottom LEDs: Flappy", 120)
    ui.text(4, 34, "Joy X: turn  Up: thrust", 120)
    ui.inline_hint(4, 42, "OK:fire+flap")
    oled_show()

    return wait_choice(True, False)


def draw_end_reason(reason):
    label = {
        "Bird Crash": "Your Bird Crashed",
        "Ship Crash": "Your Ship Exploded",
        "Quit": "Run Quit",
    }.get(reason, reason)
    try:
        oled_set_font("6x10")
    except Exception:
        pass
    label = ui.fit(label, 124)
    oled_set_cursor((128 - oled_text_width(label)) // 2, 13)
    oled_print(label)
    ui.font()


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
        "Rocks " + str(score["rocks"]) + "   Pipes " + str(score["pipes"]),
    )
    ui.center(46, "Time " + str(score["seconds"]) + "s")
    oled_show()

    return wait_choice(True, False)
