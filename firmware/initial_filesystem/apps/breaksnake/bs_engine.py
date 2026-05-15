"""BreakSnake game loop and rules."""

import random
import time

from badge import *

from badge_app import DualScreenSession, read_stick_4way, with_led_override
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
    FRAME_MS,
    OLED_H,
    OLED_W,
    PADDLE_H,
    PADDLE_STEP,
    PADDLE_Y,
    PLAY_TOP,
    PICKUP_CHANCE,
    PICKUP_SIZE,
    PICKUP_STEP,
    SCALE,
    SNAKE_MIN_LEN,
    SNAKE_MIN_MS,
    SNAKE_SHRINK,
    SNAKE_START_MS,
    SPECIAL_FOOD_CHANCE,
    WIDE_MS,
    clamp,
    load_best,
    make_food,
    new_game,
    paddle_width,
    save_best,
    score_for,
    show_toast,
    ticks_add,
)
from bs_screens import draw_breakout, draw_snake, game_over, title


def read_stick():
    return read_stick_4way()


def turn_snake(game, x_dir, y_dir):
    next_dir = game["next_dir"]
    if x_dir:
        next_dir = (x_dir, 0)
    elif y_dir:
        next_dir = (0, y_dir)

    snake_dir = game["snake_dir"]
    if next_dir[0] == -snake_dir[0] and next_dir[1] == -snake_dir[1]:
        return

    game["next_dir"] = next_dir


def snake_interval(food_count):
    return max(SNAKE_MIN_MS, SNAKE_START_MS - food_count * 18)


def maybe_spawn_special_food(game):
    if game["special_food"] is not None:
        return
    if random.randint(0, SPECIAL_FOOD_CHANCE - 1) != 0:
        return
    game["special_food"] = make_food(game["snake"], [game["food"]])


def activate_wide_paddle(game, now):
    game["wide_until"] = ticks_add(now, WIDE_MS)
    show_toast(game, now, "Wide paddle")


def tick_snake(game, now):
    if time.ticks_diff(now, game["last_snake"]) < snake_interval(game["food_count"]):
        return True

    game["last_snake"] = now
    game["snake_dir"] = game["next_dir"]

    dx, dy = game["snake_dir"]
    head = game["snake"][0]
    new_head = ((head[0] + dx) & 7, (head[1] + dy) & 7)
    eating = new_head == game["food"]
    eating_special = (
        game["special_food"] is not None and new_head == game["special_food"]
    )
    body = game["snake"] if eating else game["snake"][:-1]

    if new_head in body:
        game["reason"] = "Snake bite"
        return False

    game["snake"].insert(0, new_head)
    if eating:
        game["food_count"] += 1
        avoid = None
        if game["special_food"] is not None:
            avoid = [game["special_food"]]
        game["food"] = make_food(game["snake"], avoid)
        maybe_spawn_special_food(game)
    elif eating_special:
        activate_wide_paddle(game, now)
        game["special_food"] = None
        game["snake"].pop()
    else:
        game["snake"].pop()

    draw_snake(game)
    return True


def reset_bricks(game):
    game["bricks"] = [0xFF] * BRICK_ROWS
    game["ball_dy"] = abs(game["ball_dy"])


def clear_pickup(game):
    game["pickup_x"] = -1
    game["pickup_y"] = 0


def maybe_spawn_pickup(game, x, y):
    if game["pickup_x"] >= 0:
        return
    if random.randint(0, PICKUP_CHANCE - 1) != 0:
        return
    game["pickup_x"] = clamp(x - PICKUP_SIZE // 2, 0, OLED_W - PICKUP_SIZE)
    game["pickup_y"] = y


def shrink_snake(game):
    keep = max(SNAKE_MIN_LEN, len(game["snake"]) - SNAKE_SHRINK)
    while len(game["snake"]) > keep:
        game["snake"].pop()
    draw_snake(game)


def tick_pickup(game, now):
    if game["pickup_x"] < 0:
        return

    game["pickup_y"] += PICKUP_STEP
    pickup_x = game["pickup_x"]
    pickup_y = game["pickup_y"]
    paddle = game["paddle"]
    width = paddle_width(game, now)

    if pickup_y + PICKUP_SIZE >= PADDLE_Y and pickup_y <= PADDLE_Y + PADDLE_H:
        if pickup_x + PICKUP_SIZE >= paddle and pickup_x <= paddle + width:
            shrink_snake(game)
            show_toast(game, now, "Snake shrinks")
            clear_pickup(game)
            return

    if pickup_y > OLED_H:
        clear_pickup(game)


def brick_at(x, y):
    if y < BRICK_Y or y >= BRICK_Y + BRICK_ROWS * BRICK_STEP_Y:
        return -1, -1

    row = (y - BRICK_Y) // BRICK_STEP_Y
    if row < 0 or row >= BRICK_ROWS:
        return -1, -1
    if y >= BRICK_Y + row * BRICK_STEP_Y + BRICK_H:
        return -1, -1

    col = (x - BRICK_X) // BRICK_STEP_X
    if col < 0 or col >= BRICK_COLS:
        return -1, -1
    if x >= BRICK_X + col * BRICK_STEP_X + BRICK_W:
        return -1, -1

    return row, col


def tick_breakout(game, x_dir, now):
    width = paddle_width(game, now)
    if x_dir:
        game["paddle"] = clamp(game["paddle"] + x_dir * PADDLE_STEP, 0, OLED_W - width)
    elif game["paddle"] > OLED_W - width:
        game["paddle"] = OLED_W - width

    previous_y = game["ball_y"] // SCALE
    game["ball_x"] += game["ball_dx"]
    game["ball_y"] += game["ball_dy"]
    ball_x = game["ball_x"] // SCALE
    ball_y = game["ball_y"] // SCALE

    if ball_x <= 0:
        game["ball_x"] = 0
        game["ball_dx"] = abs(game["ball_dx"])
    elif ball_x >= OLED_W - BALL:
        game["ball_x"] = (OLED_W - BALL) * SCALE
        game["ball_dx"] = -abs(game["ball_dx"])

    if ball_y <= PLAY_TOP:
        game["ball_y"] = PLAY_TOP * SCALE
        game["ball_dy"] = abs(game["ball_dy"])

    if ball_y > OLED_H:
        game["reason"] = "Breakout miss"
        return False

    paddle = game["paddle"]
    if (
        game["ball_dy"] > 0
        and ball_y + BALL >= PADDLE_Y
        and previous_y + BALL < PADDLE_Y
    ):
        if ball_x + BALL >= paddle and ball_x <= paddle + width:
            game["ball_y"] = (PADDLE_Y - BALL - 1) * SCALE
            game["ball_dy"] = -abs(game["ball_dy"])
            relative_hit = (ball_x + BALL // 2) - (paddle + width // 2)
            game["ball_dx"] = clamp(game["ball_dx"] + relative_hit // 3, -21, 21)
            if -7 < game["ball_dx"] < 0:
                game["ball_dx"] = -7
            elif 0 <= game["ball_dx"] < 7:
                game["ball_dx"] = 7

    row, col = brick_at(ball_x + BALL // 2, ball_y)
    if row >= 0 and (game["bricks"][row] & (1 << col)):
        game["bricks"][row] &= ~(1 << col)
        game["brick_hits"] += 1
        game["ball_dy"] = -game["ball_dy"]
        maybe_spawn_pickup(
            game,
            BRICK_X + col * BRICK_STEP_X + BRICK_W // 2,
            BRICK_Y + row * BRICK_STEP_Y + BRICK_H,
        )
        if game["brick_hits"] % 12 == 0:
            game["ball_dx"] += 1 if game["ball_dx"] > 0 else -1
            game["ball_dy"] += 1 if game["ball_dy"] > 0 else -1

    if not (
        game["bricks"][0] | game["bricks"][1] | game["bricks"][2] | game["bricks"][3]
    ):
        reset_bricks(game)

    tick_pickup(game, now)
    return True


def play_loop(game, session):
    draw_snake(game)

    while True:
        now = session.now()
        x_dir, y_dir = read_stick()
        turn_snake(game, x_dir, y_dir)

        if not tick_snake(game, now):
            return game, now

        if session.frame_due(game, now):
            if not tick_breakout(game, x_dir, now):
                return game, now
            draw_breakout(game, now)

        if session.quit_pressed():
            game["reason"] = "Quit"
            return game, now

        session.sleep()


def play_once():
    game = new_game()
    session = DualScreenSession(FRAME_MS)
    return with_led_override(play_loop, game, session)


def main():
    best = load_best()
    keep_going = with_led_override(title, best)

    if not keep_going:
        oled_clear(True)
        exit()

    while True:
        game, end_ms = play_once()
        score = score_for(game, end_ms)
        new_best = score["total"] > best["total"]
        if new_best:
            best = score
            save_best(best)

        again = with_led_override(game_over, score, best, new_best, game["reason"])

        if not again:
            oled_clear(True)
            exit()
