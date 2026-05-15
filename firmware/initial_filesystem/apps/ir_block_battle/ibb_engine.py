"""IR Block Battle loop and rules."""

import gc
import random
import time

from badge import *

from ibb_data import (
    COLS,
    FRAME_MS,
    JOY_HIGH,
    JOY_LOW,
    LINE_ATTACK,
    LINE_SCORE,
    MOVE_REPEAT_MS,
    ROWS,
    SOFT_REPEAT_MS,
    add_event,
    add_garbage,
    clear_lines,
    draw_from_bag,
    empty_board,
    gravity_ms,
    hard_drop_y,
    load_best,
    lock_piece,
    new_bag,
    save_best,
    score_for,
    ticks_add,
    valid,
)
from ibb_net import DuelLink
from ibb_serial import HELP, SerialControls, normalize
from ibb_screens import (
    draw_game,
    draw_hold,
    draw_title,
    game_over,
    led_alert,
    line_clear_animation,
    pause_screen,
    tutorial,
)


def _new_game():
    bag = new_bag()
    piece = draw_from_bag(bag)
    next_piece = draw_from_bag(bag)
    now = time.ticks_ms()
    return {
        "board": empty_board(),
        "bag": bag,
        "piece": piece,
        "next": next_piece,
        "hold": None,
        "hold_used": False,
        "x": 3,
        "y": 0,
        "rot": 0,
        "score": 0,
        "lines": 0,
        "level": 0,
        "tetrises": 0,
        "pending": 0,
        "last_fall": now,
        "last_move": 0,
        "last_soft": 0,
        "last_frame": 0,
        "hard_ready": True,
        "flash": "",
        "flash_until": 0,
        "reason": "Top out",
        "over_sent": False,
        "peer_won": False,
        "events": [],
        "sent_garbage": 0,
        "received_garbage": 0,
    }


def _flash(game, now, text, ms=900):
    game["flash"] = text
    game["flash_until"] = ticks_add(now, ms)


def _log_clear(cleared, pending_before, attack):
    print(
        "[IBB] clear={} pending_before={} attack_out={}".format(
            cleared, pending_before, attack
        )
    )


def _spawn(game):
    game["piece"] = game["next"]
    game["next"] = draw_from_bag(game["bag"])
    game["x"] = 3
    game["y"] = 0
    game["rot"] = 0
    game["hold_used"] = False
    return valid(game["board"], game["piece"], game["rot"], game["x"], game["y"])


def _try_move(game, dx, dy):
    nx = game["x"] + dx
    ny = game["y"] + dy
    if valid(game["board"], game["piece"], game["rot"], nx, ny):
        game["x"] = nx
        game["y"] = ny
        return True
    return False


def _try_rotate(game, direction):
    new_rot = (game["rot"] + direction) & 3
    for kick in (0, -1, 1, -2, 2):
        nx = game["x"] + kick
        if valid(game["board"], game["piece"], new_rot, nx, game["y"]):
            game["x"] = nx
            game["rot"] = new_rot
            try:
                tone(740, 18)
            except Exception:
                pass
            return True
    return False


def _hold(game):
    if game["hold_used"]:
        return True
    current = game["piece"]
    if game["hold"] is None:
        game["hold"] = current
        if not _spawn(game):
            return False
    else:
        game["piece"] = game["hold"]
        game["hold"] = current
        game["x"] = 3
        game["y"] = 0
        game["rot"] = 0
        if not valid(game["board"], game["piece"], game["rot"], game["x"], game["y"]):
            return False
    game["hold_used"] = True
    draw_hold(game["hold"])
    return True


def _apply_clear(game, link, cleared, now):
    game["lines"] += cleared
    game["level"] = game["lines"] // 10
    game["score"] += LINE_SCORE[cleared] * (game["level"] + 1)
    attack = LINE_ATTACK[cleared]
    if cleared == 4:
        game["tetrises"] += 1
        _flash(game, now, "TETRIS!")
        led_alert("tetris", game["hold"])
        try:
            haptic_pulse(230, 70, 140)
        except Exception:
            pass
    elif cleared:
        _flash(game, now, "+" + str(cleared) + " lines")

    pending_before = game["pending"]
    if attack:
        if game["pending"] > 0:
            blocked = min(game["pending"], attack)
            game["pending"] -= blocked
            attack -= blocked
            add_event(game, "CAN " + str(blocked))
            print("[IBB] cancel garbage blocked={} remaining={}".format(blocked, attack))
        if attack > 0:
            link.send_attack(attack)
            game["sent_garbage"] += attack
            add_event(game, "TX +" + str(attack))
    elif cleared:
        add_event(game, "CLR " + str(cleared))
    if cleared:
        _log_clear(cleared, pending_before, attack)


def _fill_rows_for_clear(game, rows, gaps, value=8):
    if isinstance(gaps, int):
        gaps = (gaps,)
    board = game["board"]
    for y in range(ROWS - rows, ROWS):
        for x in range(COLS):
            board[y][x] = 0 if x in gaps else value
    game["hold_used"] = False


def _setup_single(game, now):
    _fill_rows_for_clear(game, 1, (4, 5))
    game["piece"] = "O"
    game["x"] = 3
    game["y"] = 0
    game["rot"] = 0
    game["last_fall"] = now
    _flash(game, now, "Single ready")
    print("[IBB] serial setup single")


def _setup_double(game, now):
    _fill_rows_for_clear(game, 2, (4, 5))
    game["piece"] = "O"
    game["x"] = 3
    game["y"] = 0
    game["rot"] = 0
    game["last_fall"] = now
    _flash(game, now, "Double ready")
    print("[IBB] serial setup double")


def _setup_triple(game, now):
    _fill_rows_for_clear(game, 3, 5)
    game["piece"] = "I"
    game["x"] = 3
    game["y"] = 0
    game["rot"] = 1
    game["last_fall"] = now
    _flash(game, now, "Triple ready")
    print("[IBB] serial setup triple")


def _setup_tetris(game, now):
    _fill_rows_for_clear(game, 4, 5)
    game["piece"] = "I"
    game["x"] = 3
    game["y"] = 0
    game["rot"] = 1
    game["last_fall"] = now
    _flash(game, now, "Tetris ready")
    print("[IBB] serial setup tetris")


def _serial_attack(game, link, now, lines, label):
    if lines > 0:
        link.send_attack(lines)
        game["sent_garbage"] += lines
        add_event(game, "TX +" + str(lines))
        _flash(game, now, "TX " + label)
        print("[IBB] serial attack {} lines".format(lines))


def _lock_and_continue(game, link, now):
    if not lock_piece(game["board"], game["piece"], game["rot"], game["x"], game["y"]):
        game["reason"] = "Top out"
        return False

    full_rows = []
    for row_i, row in enumerate(game["board"]):
        if all(row):
            full_rows.append(row_i)
    if full_rows:
        label = "TETRIS!" if len(full_rows) == 4 else "+" + str(len(full_rows)) + " lines"
        line_clear_animation(game, link, full_rows, label)

    cleared = clear_lines(game["board"])
    if cleared:
        _apply_clear(game, link, cleared, now)
    elif game["pending"] > 0:
        incoming = game["pending"]
        game["pending"] = 0
        if not add_garbage(game["board"], incoming):
            game["reason"] = "Garbage KO"
            return False
        add_event(game, "RISE " + str(incoming))
        _flash(game, now, "Garbage " + str(incoming))
        print("[IBB] garbage applied lines={}".format(incoming))

    return _spawn(game)


def _handle_stick(game, link, now):
    x = joy_x()
    y = joy_y()
    dx = -1 if x < JOY_LOW else (1 if x > JOY_HIGH else 0)

    if dx:
        if game["last_move"] == 0 or time.ticks_diff(now, game["last_move"]) >= MOVE_REPEAT_MS:
            _try_move(game, dx, 0)
            game["last_move"] = now
    else:
        game["last_move"] = 0

    if y > JOY_HIGH:
        if game["last_soft"] == 0 or time.ticks_diff(now, game["last_soft"]) >= SOFT_REPEAT_MS:
            if _try_move(game, 0, 1):
                game["score"] += 1
            else:
                return _lock_and_continue(game, link, now)
            game["last_soft"] = now
    else:
        game["last_soft"] = 0

    if y < JOY_LOW:
        if game["hard_ready"]:
            drop_y = hard_drop_y(game["board"], game["piece"], game["rot"], game["x"], game["y"])
            game["score"] += (drop_y - game["y"]) * 2
            game["y"] = drop_y
            game["hard_ready"] = False
            return _lock_and_continue(game, link, now)
    else:
        game["hard_ready"] = True

    return True


def _handle_buttons(game, link, now):
    if button_pressed(BTN_CONFIRM):
        _try_rotate(game, 1)
    if button_pressed(BTN_PRESETS):
        _try_rotate(game, -1)
    if button_pressed(BTN_SQUARE):
        if not _hold(game):
            game["reason"] = "Top out"
            return False
    if button_pressed(BTN_BACK):
        if not pause_screen(game, link):
            game["reason"] = "Quit"
            return False
        game["last_fall"] = now
        game["last_move"] = 0
        game["last_soft"] = 0
    return True


def _handle_serial(game, link, serial, now):
    for raw in serial.poll():
        ch = normalize(raw)
        if ch == "a":
            _try_move(game, -1, 0)
        elif ch == "d":
            _try_move(game, 1, 0)
        elif ch == "s":
            if _try_move(game, 0, 1):
                game["score"] += 1
            elif not _lock_and_continue(game, link, now):
                return False
        elif ch == "w" or ch == " ":
            drop_y = hard_drop_y(
                game["board"], game["piece"], game["rot"], game["x"], game["y"]
            )
            game["score"] += (drop_y - game["y"]) * 2
            game["y"] = drop_y
            if not _lock_and_continue(game, link, now):
                return False
        elif ch == "k":
            _try_rotate(game, 1)
        elif ch == "j":
            _try_rotate(game, -1)
        elif ch == "h":
            if not _hold(game):
                game["reason"] = "Top out"
                return False
        elif ch == "g":
            _setup_single(game, now)
        elif ch == "f":
            _setup_double(game, now)
        elif ch == "r":
            _setup_triple(game, now)
        elif ch == "t":
            _setup_tetris(game, now)
        elif ch == "2":
            _serial_attack(game, link, now, LINE_ATTACK[2], "double")
        elif ch == "3":
            _serial_attack(game, link, now, LINE_ATTACK[3], "triple")
        elif ch == "4":
            _serial_attack(game, link, now, LINE_ATTACK[4], "tetris")
        elif ch == "?":
            print(HELP)
    return True


def _service_link(game, link, now):
    incoming = link.service_rx(now)
    if incoming:
        game["pending"] = min(99, game["pending"] + incoming)
        game["received_garbage"] += incoming
        add_event(game, "RX +" + str(incoming))
        _flash(game, now, "Incoming " + str(incoming))
        print("[IBB] pending add={} total={}".format(incoming, game["pending"]))
        led_alert("incoming", game["hold"])
        try:
            haptic_pulse(190, 50, 110)
        except Exception:
            pass
    if link.peer_over and not game["peer_won"]:
        game["peer_won"] = True
        add_event(game, "RX OUT")
        _flash(game, now, "Peer topped out", 1500)
    link.service_tx(now, game["pending"])


def _play_once(link, serial):
    game = _new_game()
    draw_hold(None)
    gc.collect()

    while True:
        now = time.ticks_ms()
        _service_link(game, link, now)

        if not _handle_buttons(game, link, now):
            return game
        if not _handle_serial(game, link, serial, now):
            return game
        if not _handle_stick(game, link, now):
            return game

        if time.ticks_diff(now, game["last_fall"]) >= gravity_ms(game["level"]):
            game["last_fall"] = now
            if not _try_move(game, 0, 1):
                if not _lock_and_continue(game, link, now):
                    return game

        if time.ticks_diff(now, game["last_frame"]) >= FRAME_MS:
            game["last_frame"] = now
            flash = ""
            if game["flash"] and time.ticks_diff(game["flash_until"], now) > 0:
                flash = game["flash"]
            draw_game(game, link, flash)

        time.sleep_ms(5)


def _title(best, link, serial):
    while True:
        now = time.ticks_ms()
        link.service_rx(now)
        link.service_tx(now, 0)
        draw_title(best, link, now // 40)
        for raw in serial.poll():
            ch = normalize(raw)
            if ch == "\n":
                return True
            if ch == "?":
                print(HELP)
            if ch == "y":
                tutorial(link)
        if button_pressed(BTN_CONFIRM):
            return True
        if button_pressed(BTN_PRESETS):
            tutorial(link)
        if button_pressed(BTN_BACK):
            return False
        time.sleep_ms(35)


def main():
    try:
        random.seed(time.ticks_ms())
    except Exception:
        pass

    best = load_best()
    link = DuelLink()
    serial = SerialControls()
    link.start()
    led_override_begin()
    try:
        if not _title(best, link, serial):
            oled_clear(True)
            exit()

        while True:
            game = _play_once(link, serial)
            if game["reason"] != "Quit" and not game["over_sent"]:
                link.send_over()
                game["over_sent"] = True
                now = time.ticks_ms()
                for _i in range(24):
                    link.service_tx(now, game["pending"])
                    time.sleep_ms(55)
                    now = time.ticks_ms()

            score = score_for(game)
            new_best = score["total"] > best["total"]
            if new_best:
                best = score
                save_best(best)

            again = game_over(score, best, new_best, game["reason"], game["peer_won"])
            link.peer_over = False
            if not again:
                break
    finally:
        link.stop()
        led_override_end()
        oled_clear(True)
        gc.collect()

    exit()
