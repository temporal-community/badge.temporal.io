"""OLED and LED drawing for IR Block Battle."""

import time

from badge import *

import badge_ui as ui
from ibb_data import COLS, ROWS, SHAPES, add_event, hard_drop_y

BOARD_X = 1
BOARD_Y = 2
CELL_W = 3
CELL_H = 3
CELL_SIZE = 3
BOARD_FRAME_W = COLS * CELL_W + 2
BOARD_FRAME_H = ROWS * CELL_H + 2
HUD_X = 35
NEXT_X = 96
NEXT_Y = 1
NEXT_W = 31
NEXT_H = 25
LOG_X = HUD_X
LOG_Y = 38
LOG_W = 92
LOG_H = 25

TITLE_LED = (
    [0x18, 0x18, 0x3C, 0x18, 0x18, 0x00, 0x66, 0x66],
    [0x00, 0x18, 0x18, 0x3C, 0x18, 0x18, 0x66, 0x66],
    [0x00, 0x00, 0x18, 0x18, 0x3C, 0x18, 0x7E, 0x66],
    [0x00, 0x00, 0x00, 0x18, 0x18, 0x3C, 0x7E, 0x7E],
)

TETRIS_LED = (
    [0x00, 0x18, 0x3C, 0x7E, 0xFF, 0x7E, 0x3C, 0x18],
    [0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81],
    [0xFF, 0x81, 0xBD, 0xA5, 0xA5, 0xBD, 0x81, 0xFF],
)

INCOMING_LED = (
    [0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00],
    [0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF],
)


def _cell(x, y, color=1):
    ui.fill(BOARD_X + x * CELL_W, BOARD_Y + y * CELL_H, CELL_SIZE, CELL_SIZE, color)


def _garbage_cell(x, y):
    px = BOARD_X + x * CELL_W
    py = BOARD_Y + y * CELL_H
    ui.hline(px, py, CELL_SIZE)
    ui.fill(px + 1, py + 1, 1, 1)
    ui.hline(px, py + CELL_SIZE - 1, CELL_SIZE)


def _ghost_cell(x, y):
    px = BOARD_X + x * CELL_W
    py = BOARD_Y + y * CELL_H
    ui.frame(px, py, CELL_SIZE, CELL_SIZE)


def _piece_cells(piece, rot, px, py):
    for x, y in SHAPES[piece][rot & 3]:
        bx = px + x
        by = py + y
        if 0 <= bx < COLS and 0 <= by < ROWS:
            yield bx, by


def draw_piece_preview(piece, x, y, width=18, height=12, cell_size=3):
    if piece is None:
        return
    cells = SHAPES[piece][0]
    min_x = min(cell[0] for cell in cells)
    max_x = max(cell[0] for cell in cells)
    min_y = min(cell[1] for cell in cells)
    max_y = max(cell[1] for cell in cells)
    piece_w = (max_x - min_x + 1) * cell_size
    piece_h = (max_y - min_y + 1) * cell_size
    ox = x + max(0, (width - piece_w) // 2) - min_x * cell_size
    oy = y + max(0, (height - piece_h) // 2) - min_y * cell_size
    for bx, by in cells:
        ui.fill(ox + bx * cell_size, oy + by * cell_size, cell_size, cell_size)


def _draw_next(piece):
    ui.frame(NEXT_X, NEXT_Y, NEXT_W, NEXT_H)
    ui.text(NEXT_X + 4, NEXT_Y + 2, "NEXT", NEXT_W - 8)
    ui.hline(NEXT_X + 2, NEXT_Y + 10, NEXT_W - 4)
    draw_piece_preview(piece, NEXT_X + 3, NEXT_Y + 12, NEXT_W - 6, NEXT_H - 14, 4)


def _peer_label(link):
    if getattr(link, "linked", False):
        name = getattr(link, "peer_name", "")
        if name:
            return "VS " + name
        peer = getattr(link, "peer", -1)
        if peer >= 0:
            return "VS {:03X}".format(peer)
        return "VS PEER"
    if getattr(link, "ready", False):
        return "WAITING"
    return "SOLO"


def _draw_battle_stats(game, link):
    ui.frame(LOG_X, LOG_Y, LOG_W, LOG_H)
    ui.text(LOG_X + 3, LOG_Y + 2, _peer_label(link), LOG_W - 6)
    ui.text(
        LOG_X + 3,
        LOG_Y + 10,
        "SENT " + str(game.get("sent_garbage", 0)) + " GARB",
        LOG_W - 6,
    )
    ui.text(
        LOG_X + 3,
        LOG_Y + 18,
        "RECV " + str(game.get("received_garbage", 0)) + " GARB",
        LOG_W - 6,
    )


def draw_hold(piece):
    rows = [0] * 8
    if piece is not None:
        for bx, by in SHAPES[piece][0]:
            x = bx * 2
            y = by * 2
            if 0 <= x < 8 and 0 <= y < 8:
                rows[y] |= 0xC0 >> x
                rows[y + 1] |= 0xC0 >> x
    led_set_frame(rows, 45)


def _draw_title_well(phase):
    ui.frame(4, 13, 27, 36)
    base_y = 42
    for x in range(5):
        ui.fill(7 + x * 4, base_y, 3, 3)
    for x in (0, 3, 4):
        ui.fill(7 + x * 4, base_y - 3, 3, 3)
    drop_y = 16 + (phase % 17)
    for dx, dy in ((0, 0), (1, 0), (1, 1), (2, 1)):
        ui.fill(10 + dx * 4, drop_y + dy * 3, 3, 3)


def _draw_ir_badges(x, y, linked, phase):
    ui.frame(x, y + 5, 8, 8)
    ui.frame(x + 31, y + 5, 8, 8)
    for i in range(3):
        if linked or ((phase + i) & 1):
            ui.vline(x + 12 + i * 5, y + 7 - i, 6 + i * 2)
    if linked:
        ui.hline(x + 10, y + 9, 19)


def draw_title(best, link, phase=0):
    led_set_frame(TITLE_LED[phase % len(TITLE_LED)], 42)
    right = "IR wait"
    if link.linked:
        right = "IR on"
    elif not link.ready:
        right = "Solo"
    ui.chrome("IR Block Battle", right, "OK", "start", "BACK", "quit")
    _draw_title_well(phase)
    _draw_ir_badges(82, 14, link.linked, phase)
    ui.text(36, 15, "10x20", 44)
    ui.text(36, 25, "Rows -> IR", 44)
    ui.text(36, 35, "Up drop", 40)
    ui.inline_hint(75, 33, "X:hold")
    hint_w = ui.inline_hint(36, 43, "Y:help")
    ui.text(36 + hint_w + 4, 45, "Best " + str(best["total"]), 86 - hint_w)
    oled_show()


def _draw_tutorial_art(page, linked, phase):
    if page == 1:
        _draw_ir_badges(3, 17, linked, phase)
        return

    ui.frame(5, 14, 21, 35)
    for x in range(4):
        ui.fill(8 + x * 4, 43, 3, 3)
    if page == 2:
        for y in range(3):
            gap = (phase + y) % 4
            for x in range(4):
                if x != gap:
                    ui.fill(8 + x * 4, 40 - y * 3, 3, 3)
        return

    drop_y = 17 + (phase % 20)
    for dx, dy in ((1, 0), (0, 1), (1, 1), (2, 1)):
        ui.fill(8 + dx * 4, drop_y + dy * 3, 3, 3)


def draw_tutorial_page(page, link, phase=0):
    pages = (
        (
            "Controls",
            ("Stick L/R move", "Down soft drop", "Up hard drop", "OK/Y rotate X hold"),
        ),
        (
            "IR Battle",
            ("No lobby needed", "Run on both badges", "Face IR windows", "Clears send garbage"),
        ),
        (
            "Garbage",
            ("G = pending rows", "Clears cancel G", "No clear raises G", "Top out pings peer"),
        ),
    )
    title, lines = pages[page]
    ui.chrome(title, str(page + 1) + "/" + str(len(pages)), "OK", "next", "BACK", "done")
    _draw_tutorial_art(page, link.linked, phase)
    if page == 0:
        for i, line in enumerate(lines[:3]):
            ui.text(30, 13 + i * 9, line, 96)
        ui.text(30, 40, "Rot", 20)
        ui.inline_hint(51, 38, "OK")
        ui.inline_hint(65, 38, "Y")
        ui.text(82, 40, "Hold", 24)
        ui.inline_hint(108, 38, "X")
    else:
        for i, line in enumerate(lines):
            ui.text(30, 13 + i * 9, line, 96)
    oled_show()
    return len(pages)


def tutorial(link):
    page = 0
    while True:
        now = time.ticks_ms()
        link.service_rx(now)
        link.service_tx(now, 0)
        page_count = draw_tutorial_page(page, link, now // 45)

        if button_pressed(BTN_CONFIRM):
            page += 1
            if page >= page_count:
                return
        if button_pressed(BTN_BACK):
            return
        time.sleep_ms(35)


def draw_game(game, link, flash=""):
    board = game["board"]
    oled_clear()
    ui.frame(0, 1, BOARD_FRAME_W, BOARD_FRAME_H)

    for y in range(ROWS):
        for x in range(COLS):
            value = board[y][x]
            if value == 8:
                _garbage_cell(x, y)
            elif value:
                _cell(x, y)

    gy = hard_drop_y(board, game["piece"], game["rot"], game["x"], game["y"])
    if gy != game["y"]:
        for x, y in _piece_cells(game["piece"], game["rot"], game["x"], gy):
            _ghost_cell(x, y)

    for x, y in _piece_cells(game["piece"], game["rot"], game["x"], game["y"]):
        _cell(x, y)

    title = flash if flash else "IR Blocks"
    ui.text(HUD_X, 1, title, 58)
    ui.text(HUD_X, 10, "S " + str(game["score"]), 58)
    ui.text(HUD_X, 19, "LN " + str(game["lines"]) + " LV " + str(game["level"]), 58)
    _draw_next(game["next"])
    ui.text(HUD_X, 28, "G " + str(game["pending"]), 28)
    if link.linked:
        ui.text(65, 28, "IR on", 60)
    elif link.ready:
        ui.text(65, 28, "IR wait", 60)
    else:
        ui.text(65, 28, "Solo", 60)
    _draw_battle_stats(game, link)
    oled_show()


def line_clear_animation(game, link, rows, label):
    for step in range(4):
        now = time.ticks_ms()
        incoming = link.service_rx(now)
        if incoming:
            game["pending"] = min(99, game["pending"] + incoming)
            game["received_garbage"] = game.get("received_garbage", 0) + incoming
            add_event(game, "RX +" + str(incoming))
        link.service_tx(now, game["pending"])
        draw_game(game, link, label if step & 1 else "")
        oled_set_draw_color(2)
        for row in rows:
            ui.fill(BOARD_X, BOARD_Y + row * CELL_H, COLS * CELL_W, CELL_H)
        oled_set_draw_color(1)
        oled_show()
        time.sleep_ms(45)


def led_alert(kind, hold_piece):
    frames = TETRIS_LED if kind == "tetris" else INCOMING_LED
    for frame in frames:
        led_set_frame(frame, 60)
        time.sleep_ms(45)
    draw_hold(hold_piece)


def pause_screen(game, link):
    ui.chrome("Paused", "G " + str(game["pending"]), "OK", "resume", "BACK", "quit")
    ui.center(22, "IR Block Battle")
    if link.linked:
        ui.center(34, "IR linked")
    else:
        ui.center(34, "Solo / waiting")
    oled_show()

    while True:
        now = time.ticks_ms()
        game["pending"] += link.service_rx(now)
        link.service_tx(now, game["pending"])
        if button_pressed(BTN_CONFIRM):
            return True
        if button_pressed(BTN_BACK):
            return False
        time.sleep_ms(50)


def game_over(score, best, new_best, reason, peer_won):
    draw_hold(None)
    ui.chrome(
        "Game Over",
        "New Best" if new_best else "Best " + str(best["total"]),
        "OK",
        "again",
        "BACK",
        "quit",
    )
    if peer_won:
        message = "Peer topped out"
    else:
        message = reason
    ui.center(14, message)
    ui.frame(8, 25, 112, 24)
    ui.center(29, "Score " + str(score["total"]), 8, 112)
    ui.center(
        40,
        "Lines " + str(score["lines"]) + "  Lv " + str(score["level"]) + "  T4 " + str(score["tetrises"]),
        8,
        112,
    )
    oled_show()

    while True:
        if button_pressed(BTN_CONFIRM):
            return True
        if button_pressed(BTN_BACK):
            return False
        time.sleep_ms(30)
