"""Flappy Asteroids game loop and rules."""

import random
import time

from badge import *

from badge_app import ButtonLatch, DualScreenSession, read_axis, with_led_override
from fa_data import (
    ASTEROID_BENEFITS,
    BACK_QUIT_MS,
    BENEFIT_MS,
    BIRD_SCALE,
    BIRD_X,
    DIR_X,
    DIR_Y,
    FLAP_V,
    FLAPPY_BENEFITS,
    FLAPPY_TICK_MS,
    FRAME_MS,
    GRAVITY,
    MAX_FALL_V,
    MAX_SHIP_SPEED,
    MAX_SHOTS,
    OLED_H,
    OLED_W,
    PICKUP_CHANCE,
    PIPE_BENEFIT_CHANCE,
    PIPE_MIN_MS,
    PIPE_START_MS,
    PLAY_TOP,
    ROCK_ADD_KILLS,
    ROCK_ADD_MS,
    ROCK_LIMIT,
    ROCK_RADII,
    SCALE,
    SHIP_R,
    SHOT_MS,
    SHOT_TTL,
    active_until,
    asteroids_has_benefit,
    clamp,
    flappy_has_benefit,
    grant_asteroids_benefit,
    grant_flappy_benefit,
    load_best,
    make_rock,
    new_game,
    random_gap,
    save_best,
    score_for,
    show_toast,
    ticks_add,
)
from fa_screens import draw_asteroids, draw_flappy, game_over, title


def read_stick():
    return read_axis(joy_x()), read_axis(joy_y()) < 0


def wrap_point(value, limit):
    value %= limit * SCALE
    return value


def dist_sq(ax, ay, bx, by):
    dx = ax - bx
    dy = ay - by
    return dx * dx + dy * dy


def move_ship(game, x_dir, thrust):
    if x_dir:
        game["ship_angle"] = (game["ship_angle"] + x_dir) & 15
    if thrust:
        angle = game["ship_angle"]
        game["ship_vx"] += DIR_X[angle]
        game["ship_vy"] += DIR_Y[angle]

    game["ship_vx"] = clamp(game["ship_vx"], -MAX_SHIP_SPEED, MAX_SHIP_SPEED)
    game["ship_vy"] = clamp(game["ship_vy"], -MAX_SHIP_SPEED, MAX_SHIP_SPEED)
    game["ship_x"] = wrap_point(game["ship_x"] + game["ship_vx"], OLED_W)
    y = game["ship_y"] + game["ship_vy"]
    if y < PLAY_TOP * SCALE:
        y += (OLED_H - PLAY_TOP) * SCALE
    elif y >= OLED_H * SCALE:
        y -= (OLED_H - PLAY_TOP) * SCALE
    game["ship_y"] = y
    game["ship_vx"] = (game["ship_vx"] * 31) // 32
    game["ship_vy"] = (game["ship_vy"] * 31) // 32


def fire(game, now):
    if time.ticks_diff(now, game["shot_ready"]) < 0:
        return
    if len(game["shots"]) >= MAX_SHOTS:
        return
    angle = game["ship_angle"]
    power = game["power_shot"]
    game["power_shot"] = False
    game["shots"].append(
        {
            "x": game["ship_x"] + DIR_X[angle] * 2,
            "y": game["ship_y"] + DIR_Y[angle] * 2,
            "vx": DIR_X[angle] * 3 + game["ship_vx"],
            "vy": DIR_Y[angle] * 3 + game["ship_vy"],
            "until": ticks_add(now, SHOT_TTL),
            "power": power,
        }
    )
    game["shot_ready"] = ticks_add(now, SHOT_MS)


def move_shots(game, now):
    kept = []
    for shot in game["shots"]:
        if time.ticks_diff(shot["until"], now) <= 0:
            continue
        shot["x"] = wrap_point(shot["x"] + shot["vx"], OLED_W)
        y = shot["y"] + shot["vy"]
        if y < PLAY_TOP * SCALE:
            y += (OLED_H - PLAY_TOP) * SCALE
        elif y >= OLED_H * SCALE:
            y -= (OLED_H - PLAY_TOP) * SCALE
        shot["y"] = y
        kept.append(shot)
    game["shots"] = kept


def move_rocks(game, now):
    slow = active_until(game["slow_rocks_until"], now)
    for rock in game["rocks"]:
        div = 2 if slow else 1
        rock["x"] = wrap_point(rock["x"] + rock["vx"] // div, OLED_W)
        y = rock["y"] + rock["vy"] // div
        if y < PLAY_TOP * SCALE:
            y += (OLED_H - PLAY_TOP) * SCALE
        elif y >= OLED_H * SCALE:
            y -= (OLED_H - PLAY_TOP) * SCALE
        rock["y"] = y


def split_rock(game, rock):
    size = rock["size"]
    if size <= 1:
        return
    for turn in (-1, 1):
        child = make_rock(rock["x"], rock["y"], size - 1)
        child["vx"] = (rock["vy"] // 2 + turn * 8) or turn * 8
        child["vy"] = (-rock["vx"] // 2 + turn * 6) or turn * 6
        game["rocks"].append(child)


def maybe_spawn_pickup(game, rock, now):
    if game["pickup"] or flappy_has_benefit(game, now):
        return
    if rock["size"] <= 1:
        return
    if random.randint(0, PICKUP_CHANCE - 1) != 0:
        return
    game["pickup"] = {
        "x": rock["x"],
        "y": rock["y"],
        "kind": random.choice(FLAPPY_BENEFITS),
    }


def remove_rock(game, rock, split=True):
    try:
        game["rocks"].remove(rock)
    except ValueError:
        return
    game["rocks_destroyed"] += 1
    if split:
        split_rock(game, rock)


def collide_shots(game, now):
    remaining_shots = []
    for shot in game["shots"]:
        hit = None
        sx = shot["x"] // SCALE
        sy = shot["y"] // SCALE
        for rock in game["rocks"]:
            radius = ROCK_RADII[rock["size"]]
            rx = rock["x"] // SCALE
            ry = rock["y"] // SCALE
            if dist_sq(sx, sy, rx, ry) <= (radius + 1) * (radius + 1):
                hit = rock
                break
        if hit:
            maybe_spawn_pickup(game, hit, now)
            remove_rock(game, hit, not shot["power"])
            if shot["power"]:
                show_toast(game, now, "Power Shot")
        else:
            remaining_shots.append(shot)
    game["shots"] = remaining_shots


def maybe_add_rock(game, now):
    if len(game["rocks"]) >= ROCK_LIMIT:
        return
    due_time = time.ticks_diff(now, game["next_rock_at"]) >= 0
    due_kills = game["rocks_destroyed"] >= game["next_rock_kills"]
    if not due_time and not due_kills:
        return
    game["rocks"].append(make_rock(size=3))
    game["next_rock_at"] = ticks_add(now, ROCK_ADD_MS)
    game["next_rock_kills"] = game["rocks_destroyed"] + ROCK_ADD_KILLS


def collide_ship(game, now):
    sx = game["ship_x"] // SCALE
    sy = game["ship_y"] // SCALE
    for rock in list(game["rocks"]):
        radius = ROCK_RADII[rock["size"]]
        rx = rock["x"] // SCALE
        ry = rock["y"] // SCALE
        if dist_sq(sx, sy, rx, ry) <= (radius + SHIP_R) * (radius + SHIP_R):
            if game["ship_shield"]:
                game["ship_shield"] = False
                show_toast(game, now, "Ship Shield")
                remove_rock(game, rock, False)
                return True
            game["reason"] = "Ship Crash"
            return False
    return True


def collect_pickup(game, now):
    pickup = game["pickup"]
    if not pickup:
        return
    sx = game["ship_x"] // SCALE
    sy = game["ship_y"] // SCALE
    px = pickup["x"] // SCALE
    py = pickup["y"] // SCALE
    if dist_sq(sx, sy, px, py) <= 25:
        grant_flappy_benefit(game, now, pickup["kind"])
        game["pickup"] = None


def tick_asteroids(game, x_dir, thrust, shot_pressed, now):
    move_ship(game, x_dir, thrust)
    if shot_pressed:
        fire(game, now)
    move_shots(game, now)
    move_rocks(game, now)
    collide_shots(game, now)
    collect_pickup(game, now)
    maybe_add_rock(game, now)
    return collide_ship(game, now)


def pipe_interval(game, now):
    interval = max(PIPE_MIN_MS, PIPE_START_MS - game["pipes_passed"] * 8)
    if active_until(game["slow_pipes_until"], now):
        interval += 170
    return interval


def reset_pipe(game, now):
    gap_h = 4 if active_until(game["wide_gap_until"], now) else 3
    game["pipe_x"] = 8
    game["pipe_gap"] = random_gap(gap_h)
    game["pipe_scored"] = False


def maybe_grant_asteroids_from_pipe(game, now):
    if asteroids_has_benefit(game, now):
        return
    if random.randint(0, PIPE_BENEFIT_CHANCE - 1) != 0:
        return
    grant_asteroids_benefit(game, now, random.choice(ASTEROID_BENEFITS))


def check_pipe_collision(game, now):
    bird_y = (game["bird_y"] + BIRD_SCALE // 2) // BIRD_SCALE
    if bird_y < 0 or bird_y > 7:
        game["reason"] = "Bird Crash"
        return False

    gap_h = 4 if active_until(game["wide_gap_until"], now) else 3
    if game["pipe_x"] == BIRD_X:
        in_gap = game["pipe_gap"] <= bird_y < game["pipe_gap"] + gap_h
        if not in_gap:
            if game["bird_shield"]:
                game["bird_shield"] = False
                game["pipe_x"] = BIRD_X - 1
                game["pipe_scored"] = True
                show_toast(game, now, "Bird Shield")
                return True
            game["reason"] = "Bird Crash"
            return False
    return True


def tick_flappy(game, flap_pressed, now):
    if flap_pressed:
        game["bird_v"] = FLAP_V

    if time.ticks_diff(now, game["last_flappy"]) >= FLAPPY_TICK_MS:
        game["last_flappy"] = now
        game["bird_v"] = min(MAX_FALL_V, game["bird_v"] + GRAVITY)
        game["bird_y"] += game["bird_v"]

    if time.ticks_diff(now, game["last_pipe"]) >= pipe_interval(game, now):
        game["last_pipe"] = now
        game["pipe_x"] -= 1

    if game["pipe_x"] < BIRD_X and not game["pipe_scored"]:
        game["pipe_scored"] = True
        game["pipes_passed"] += 1
        maybe_grant_asteroids_from_pipe(game, now)

    if game["pipe_x"] < 0:
        reset_pipe(game, now)

    return check_pipe_collision(game, now)


def play_loop(game, session):
    fire_latch = ButtonLatch(BTN_CONFIRM)
    draw_flappy(game, time.ticks_ms())

    while True:
        now = session.now()
        x_dir, thrust = read_stick()
        fire_and_flap = fire_latch.poll()

        if session.frame_due(game, now):
            if not tick_asteroids(game, x_dir, thrust, fire_latch.consume(), now):
                return game, now
            draw_asteroids(game, now)

        if not tick_flappy(game, fire_and_flap, now):
            return game, now
        draw_flappy(game, now)

        if session.quit_held(BACK_QUIT_MS):
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
