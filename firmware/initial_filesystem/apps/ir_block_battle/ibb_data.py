"""IR Block Battle constants, pieces, and score helpers."""

import json
import random
import time

COLS = 10
ROWS = 20
SCORE_PATH = "/ir_block_battle_score.json"

JOY_LOW = 1100
JOY_HIGH = 3000
MOVE_REPEAT_MS = 120
SOFT_REPEAT_MS = 45
FRAME_MS = 40

LINE_ATTACK = [0, 0, 1, 2, 4]
LINE_SCORE = [0, 100, 300, 500, 800]
EVENT_COUNT = 3
GRAVITY_MS = [
    900,
    800,
    700,
    600,
    500,
    420,
    350,
    290,
    240,
    200,
    165,
    135,
    110,
    95,
    80,
]

PIECES = ("I", "J", "L", "O", "S", "T", "Z")
PIECE_VALUE = {"I": 1, "J": 2, "L": 3, "O": 4, "S": 5, "T": 6, "Z": 7}

SHAPES = {
    "I": (
        ((0, 1), (1, 1), (2, 1), (3, 1)),
        ((2, 0), (2, 1), (2, 2), (2, 3)),
        ((0, 2), (1, 2), (2, 2), (3, 2)),
        ((1, 0), (1, 1), (1, 2), (1, 3)),
    ),
    "J": (
        ((0, 0), (0, 1), (1, 1), (2, 1)),
        ((1, 0), (2, 0), (1, 1), (1, 2)),
        ((0, 1), (1, 1), (2, 1), (2, 2)),
        ((1, 0), (1, 1), (0, 2), (1, 2)),
    ),
    "L": (
        ((2, 0), (0, 1), (1, 1), (2, 1)),
        ((1, 0), (1, 1), (1, 2), (2, 2)),
        ((0, 1), (1, 1), (2, 1), (0, 2)),
        ((0, 0), (1, 0), (1, 1), (1, 2)),
    ),
    "O": (
        ((1, 0), (2, 0), (1, 1), (2, 1)),
        ((1, 0), (2, 0), (1, 1), (2, 1)),
        ((1, 0), (2, 0), (1, 1), (2, 1)),
        ((1, 0), (2, 0), (1, 1), (2, 1)),
    ),
    "S": (
        ((1, 0), (2, 0), (0, 1), (1, 1)),
        ((1, 0), (1, 1), (2, 1), (2, 2)),
        ((1, 1), (2, 1), (0, 2), (1, 2)),
        ((0, 0), (0, 1), (1, 1), (1, 2)),
    ),
    "T": (
        ((1, 0), (0, 1), (1, 1), (2, 1)),
        ((1, 0), (1, 1), (2, 1), (1, 2)),
        ((0, 1), (1, 1), (2, 1), (1, 2)),
        ((1, 0), (0, 1), (1, 1), (1, 2)),
    ),
    "Z": (
        ((0, 0), (1, 0), (1, 1), (2, 1)),
        ((2, 0), (1, 1), (2, 1), (1, 2)),
        ((0, 1), (1, 1), (1, 2), (2, 2)),
        ((1, 0), (0, 1), (1, 1), (0, 2)),
    ),
}


def ticks_add(now, delta):
    try:
        return time.ticks_add(now, delta)
    except AttributeError:
        return now + delta


def add_event(game, text):
    events = game.get("events")
    if events is None:
        events = []
        game["events"] = events
    events.insert(0, str(text))
    while len(events) > EVENT_COUNT:
        events.pop()


def gravity_ms(level):
    if level < len(GRAVITY_MS):
        return GRAVITY_MS[level]
    return GRAVITY_MS[-1]


def empty_board():
    return [[0 for _x in range(COLS)] for _y in range(ROWS)]


def valid(board, piece, rot, px, py):
    for x, y in SHAPES[piece][rot & 3]:
        bx = px + x
        by = py + y
        if bx < 0 or bx >= COLS or by >= ROWS:
            return False
        if by >= 0 and board[by][bx]:
            return False
    return True


def lock_piece(board, piece, rot, px, py):
    value = PIECE_VALUE[piece]
    topped = False
    for x, y in SHAPES[piece][rot & 3]:
        bx = px + x
        by = py + y
        if by < 0:
            topped = True
        elif 0 <= bx < COLS and by < ROWS:
            board[by][bx] = value
    return not topped


def clear_lines(board):
    kept = []
    cleared = 0
    for row in board:
        if all(row):
            cleared += 1
        else:
            kept.append(row)
    while len(kept) < ROWS:
        kept.insert(0, [0 for _x in range(COLS)])
    board[:] = kept
    return cleared


def add_garbage(board, count):
    ok = True
    for _i in range(count):
        if any(board[0]):
            ok = False
        for y in range(ROWS - 1):
            board[y] = board[y + 1]
        gap = random.randint(0, COLS - 1)
        row = [8 for _x in range(COLS)]
        row[gap] = 0
        board[ROWS - 1] = row
    return ok


def hard_drop_y(board, piece, rot, px, py):
    y = py
    while valid(board, piece, rot, px, y + 1):
        y += 1
    return y


def _shuffle(values):
    for i in range(len(values) - 1, 0, -1):
        j = random.randint(0, i)
        values[i], values[j] = values[j], values[i]


def new_bag():
    bag = list(PIECES)
    _shuffle(bag)
    return bag


def draw_from_bag(bag):
    if not bag:
        bag.extend(new_bag())
    return bag.pop()


def load_best():
    try:
        with open(SCORE_PATH, "r") as score_file:
            data = json.loads(score_file.read())
        return {
            "total": int(data.get("total", 0)),
            "lines": int(data.get("lines", 0)),
            "level": int(data.get("level", 0)),
            "tetrises": int(data.get("tetrises", 0)),
        }
    except Exception:
        return {"total": 0, "lines": 0, "level": 0, "tetrises": 0}


def save_best(best):
    try:
        with open(SCORE_PATH, "w") as score_file:
            score_file.write(json.dumps(best))
    except Exception:
        pass


def score_for(game):
    return {
        "total": game["score"],
        "lines": game["lines"],
        "level": game["level"],
        "tetrises": game["tetrises"],
    }
