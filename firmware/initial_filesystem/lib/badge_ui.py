"""Small OLED UI helpers that mirror the badge's native menu chrome."""

import badge as _badge
from badge import *

SCREEN_W = 128
SCREEN_H = 64
HEADER_RULE_Y = 8
CONTENT_Y = 10
ROW_H = 9
FOOTER_Y = 53
FOOTER_GLYPH_Y = 53
TALL_FOOTER_Y = 43
TALL_FOOTER_UPPER_Y = 44
TALL_FOOTER_LOWER_Y = 54
FONT_UI = "Smallsimple"


def _native(name):
    try:
        return getattr(_badge, name)
    except Exception:
        return None


def font():
    try:
        oled_set_font(FONT_UI)
        oled_set_text_size(1)
    except Exception:
        pass


def fit(value, max_w):
    s = str(value)
    while s and oled_text_width(s) > max_w:
        s = s[:-1]
    return s


def fill(x, y, w, h, color=1):
    oled_set_draw_color(color)
    oled_draw_box(int(x), int(y), int(w), int(h))
    oled_set_draw_color(1)


def hline(x, y, w, color=1):
    fill(x, y, w, 1, color)


def vline(x, y, h, color=1):
    fill(x, y, 1, h, color)


def frame(x, y, w, h):
    hline(x, y, w)
    hline(x, y + h - 1, w)
    vline(x, y, h)
    vline(x + w - 1, y, h)


def text(x, y, value, max_w=None, color=1):
    font()
    s = str(value)
    if max_w is not None:
        s = fit(s, max_w)
    oled_set_draw_color(color)
    oled_set_cursor(int(x), int(y))
    oled_print(s)
    oled_set_draw_color(1)
    return oled_text_width(s)


def center(y, value, x=0, w=SCREEN_W, color=1):
    font()
    s = fit(value, w)
    return text(x + (w - oled_text_width(s)) // 2, y, s, None, color)


def row_baseline(row):
    return CONTENT_Y + row * ROW_H + 1


def line(row, value, x=3, max_w=122, color=1):
    return text(x, row_baseline(row), value, max_w, color)


def selected_row(row, value):
    y = CONTENT_Y + row * ROW_H
    fill(0, y, SCREEN_W, ROW_H, 1)
    vline(1, y + 1, ROW_H - 2, 0)
    line(row, value, 3, 122, 0)
    oled_set_draw_color(1)


def header(title, right=None):
    draw = _native("ui_header")
    if draw:
        draw(str(title), None if right is None else str(right))
        return

    font()
    if right is not None:
        r = fit(right, 42)
        rw = oled_text_width(r)
        text(SCREEN_W - rw, 1, r)
        text(0, 1, fit(title, SCREEN_W - rw - 4))
    else:
        text(0, 1, fit(title, SCREEN_W))
    hline(0, HEADER_RULE_Y, SCREEN_W)


def footer(hint=None):
    hline(0, FOOTER_Y, SCREEN_W)
    if hint:
        inline_hint(0, FOOTER_GLYPH_Y, hint)


def _action_hint(button, label):
    if not button:
        return ""
    if label:
        return str(button) + ":" + str(label)
    return str(button)


def hint(button, label=None):
    return _action_hint(button, label)


def _hint_item(item):
    if isinstance(item, (tuple, list)):
        if len(item) > 1:
            return hint(item[0], item[1])
        if len(item) == 1:
            return hint(item[0])
        return ""
    return str(item)


def hint_text(actions, sep=" "):
    parts = []
    for item in actions:
        value = _hint_item(item)
        if value:
            parts.append(value)
    return sep.join(parts)


def hint_row(actions, y, x=0, gap=4, max_w=SCREEN_W):
    cursor = int(x)
    start = cursor
    limit = start + int(max_w)
    for item in actions:
        value = _hint_item(item)
        if not value:
            continue
        w = measure_hint(value)
        if cursor > start and cursor + w > limit:
            break
        inline_hint(cursor, y, value)
        cursor += w + gap
    return cursor - start


def action_width(button, label):
    hint = _action_hint(button, label)
    measure = _native("ui_measure_hint")
    if measure:
        return measure(hint)

    font()
    return oled_text_width(hint)


def action(x, y, button, label):
    return inline_hint(x, y, _action_hint(button, label))


def action_bar(left_button=None, left_label=None, right_button=None, right_label=None):
    draw = _native("ui_action_bar")
    if draw:
        draw(left_button, left_label, right_button, right_label)
        return

    hline(0, FOOTER_Y, SCREEN_W)
    if left_button:
        action(0, FOOTER_GLYPH_Y, left_button, left_label)
    if right_button:
        w = action_width(right_button, right_label)
        action(SCREEN_W - w, FOOTER_GLYPH_Y, right_button, right_label)


def tall_action_bar(
    upper_actions=(),
    left_button=None,
    left_label=None,
    right_button=None,
    right_label=None,
):
    hline(0, TALL_FOOTER_Y, SCREEN_W)
    if upper_actions:
        hint_row(upper_actions, TALL_FOOTER_UPPER_Y)
    if left_button:
        action(0, TALL_FOOTER_LOWER_Y, left_button, left_label)
    if right_button:
        w = action_width(right_button, right_label)
        action(SCREEN_W - w, TALL_FOOTER_LOWER_Y, right_button, right_label)


def screen(title, right=None, hint=None):
    oled_clear()
    font()
    header(title, right)
    footer(hint)


def chrome(
    title,
    right=None,
    left_button=None,
    left_label=None,
    right_button=None,
    right_label=None,
):
    draw = _native("ui_chrome")
    if draw:
        draw(title, right, left_button, left_label, right_button, right_label)
        return

    oled_clear()
    font()
    header(title, right)
    action_bar(left_button, left_label, right_button, right_label)


def chrome_tall(
    title,
    right=None,
    upper_actions=(),
    left_button=None,
    left_label=None,
    right_button=None,
    right_label=None,
):
    oled_clear()
    font()
    header(title, right)
    tall_action_bar(
        upper_actions,
        left_button,
        left_label,
        right_button,
        right_label,
    )


def status_box(y, title, detail=None, busy=False):
    frame(4, y, 120, 24)
    if busy:
        spinner(12, y + 12, 0)
        x = 22
        w = 96
    else:
        x = 8
        w = 112
    text(x, y + 4, title, w)
    if detail:
        text(x, y + 14, detail, w)


def measure_hint(hint):
    measure = _native("ui_measure_hint")
    if measure:
        return measure(str(hint))

    font()
    return oled_text_width(str(hint))


def inline_hint(x, y, hint):
    draw = _native("ui_inline_hint")
    if draw:
        return draw(int(x), int(y), str(hint))

    return text(x, y + 3, hint)


def inline_hint_right(right_x, y, hint):
    draw = _native("ui_inline_hint_right")
    if draw:
        return draw(int(right_x), int(y), str(hint))

    return inline_hint(right_x - measure_hint(hint), y, hint)


def spinner(cx, cy, phase):
    pts = ((0, -3), (2, -2), (3, 0), (2, 2), (0, 3), (-2, 2), (-3, 0), (-2, -2))
    phase &= 7
    for i, p in enumerate(pts):
        x = cx + p[0]
        y = cy + p[1]
        if i == phase:
            fill(x - 1, y - 1, 3, 3)
        elif ((i + 1) & 7) == phase or ((i + 7) & 7) == phase:
            fill(x, y, 2, 2)
        else:
            oled_set_pixel(x, y, 1)
