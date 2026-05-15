"""GAMES \u2014 IR mini-games for two badges.

Sub-launcher with four games:
  Echo       \u2014 send a nonce, peer must reflect within 500ms
  QuickDraw  \u2014 IR duel; first TX wins
  Beacon     \u2014 broadcast tag, see nearby badges live
  Disco      \u2014 LED matrix flashes on every received frame

All games stay in 'badge' mode and use ir_send_words / ir_read_words.
"""

import os
import random
import time

from badge import *
import ir_lib as L


# ── Echo ───────────────────────────────────────────────────────────────────


MAGIC_ECHO = 0xBADE0000


def _rand16():
    b = os.urandom(2)
    return (b[0] << 8) | b[1]


def echo():
    streak = 0
    best = 0
    state = "press Confirm"
    pending = None
    pending_t = 0
    with L.IrSession("badge", power=20):
        while True:
            now = time.ticks_ms()
            oled_clear()
            L.chrome("Echo")
            L.draw_hero(str(streak) if streak else "go")
            L.draw_subline("best %d   |   %s" % (best, state))
            L.footer(("Confirm", "ping"), ("Back", "back"))
            oled_show()

            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM) and pending is None:
                nonce = _rand16()
                pending = nonce
                pending_t = now
                try:
                    ir_send_words([MAGIC_ECHO | nonce])
                    L.set_tx_label("%04X" % nonce)
                except Exception:
                    pass
                state = "waiting"

            try:
                got = ir_read_words()
            except Exception:
                got = None
            if got is not None:
                w = int(got[0]) & 0xFFFFFFFF
                if (w & 0xFFFF0000) == MAGIC_ECHO:
                    seq = w & 0xFFFF
                    if pending is not None and seq == pending and \
                            time.ticks_diff(now, pending_t) <= 500:
                        streak += 1
                        if streak > best:
                            best = streak
                        L.auto_rx_label(words=got)
                        state = "ECHO"
                        pending = None
                    else:
                        # peer's ping \u2014 reflect it
                        try:
                            ir_send_words([MAGIC_ECHO | seq])
                            L.set_tx_label("%04X" % seq)
                        except Exception:
                            pass

            if pending is not None and time.ticks_diff(now, pending_t) > 500:
                pending = None
                streak = 0
                state = "miss"
            time.sleep_ms(25)


# ── Quick Draw ─────────────────────────────────────────────────────────────


MAGIC_DRAW = 0xC0FFEE00


def _qd_screen(big, sub):
    oled_clear()
    L.chrome("Quick Draw")
    L.draw_hero(big)
    L.draw_subline(sub)
    L.footer(("Confirm", "draw"), ("Back", "back"))
    oled_show()


def quickdraw():
    wins = 0
    losses = 0
    with L.IrSession("badge", power=20):
        while True:
            _qd_screen("ready", "W:%d  L:%d" % (wins, losses))
            while True:
                if button_pressed(BTN_BACK):
                    return
                if button_pressed(BTN_CONFIRM):
                    break
                time.sleep_ms(25)

            _qd_screen("steady", "...")
            ir_flush()
            wait_ms = 1500 + random.randint(0, 2500)
            t_end = time.ticks_add(time.ticks_ms(), wait_ms)
            jumped = False
            while time.ticks_diff(t_end, time.ticks_ms()) > 0:
                if button_pressed(BTN_CONFIRM):
                    jumped = True
                    break
                time.sleep_ms(15)
            if jumped:
                losses += 1
                _qd_screen("FOUL", "false start")
                time.sleep_ms(1300)
                continue

            _qd_screen("DRAW", "go!")
            t0 = time.ticks_ms()
            done = False
            while not done:
                if time.ticks_diff(time.ticks_ms(), t0) > 2000:
                    losses += 1
                    _qd_screen("slow", "too slow")
                    time.sleep_ms(1300)
                    break
                if button_pressed(BTN_CONFIRM):
                    try:
                        word = MAGIC_DRAW | (time.ticks_ms() & 0xFF)
                        ir_send_words([word])
                        L.set_tx_label("%08X" % word)
                    except Exception:
                        pass
                    rt = time.ticks_diff(time.ticks_ms(), t0)
                    wins += 1
                    _qd_screen("WIN", "%d ms" % rt)
                    time.sleep_ms(1500)
                    done = True
                    break
                try:
                    got = ir_read_words()
                except Exception:
                    got = None
                if got is not None:
                    w = int(got[0]) & 0xFFFFFFFF
                    if (w & 0xFFFFFF00) == MAGIC_DRAW:
                        losses += 1
                        L.auto_rx_label(words=got)
                        _qd_screen("LOSS", "beaten")
                        time.sleep_ms(1500)
                        done = True
                        break
                time.sleep_ms(5)


# ── Beacon Tag ─────────────────────────────────────────────────────────────


MAGIC_BEACON = 0xBEAC0000


def _self_tag():
    try:
        uid = my_uuid()
    except Exception:
        uid = "000000000000"
    val = 0
    for ch in uid[-6:]:
        try:
            val = (val << 4) | int(ch, 16)
        except Exception:
            pass
    return val & 0xFFFF


def beacon_tag():
    tag = _self_tag()
    last_tx = 0
    seen = {}  # peer -> last_seen ms
    with L.IrSession("badge", power=12):
        while True:
            now = time.ticks_ms()
            if time.ticks_diff(now, last_tx) > 600:
                try:
                    ir_send_words([MAGIC_BEACON | tag])
                    L.set_tx_label("%04X" % tag)
                except Exception:
                    pass
                last_tx = now

            try:
                got = ir_read_words()
            except Exception:
                got = None
            if got is not None:
                w = int(got[0]) & 0xFFFFFFFF
                if (w & 0xFFFF0000) == MAGIC_BEACON:
                    peer = w & 0xFFFF
                    if peer != tag:
                        seen[peer] = now
                        L.set_rx_label("%04X" % peer)

            for k in list(seen.keys()):
                if time.ticks_diff(now, seen[k]) > 6000:
                    del seen[k]

            oled_clear()
            L.chrome("Beacon")
            L.draw_hero("%d" % len(seen))
            L.draw_subline("nearby badges  |  me %04X" % tag)
            L.footer(("Back", "back"))
            oled_show()

            if button_pressed(BTN_BACK):
                return
            time.sleep_ms(40)


# ── Disco ──────────────────────────────────────────────────────────────────


PATTERNS = (
    (0b00011000, 0b00111100, 0b01111110, 0b11111111,
     0b11111111, 0b01111110, 0b00111100, 0b00011000),
    (0b10000001, 0b01000010, 0b00100100, 0b00011000,
     0b00011000, 0b00100100, 0b01000010, 0b10000001),
    (0b11111111, 0b10000001, 0b10000001, 0b10000001,
     0b10000001, 0b10000001, 0b10000001, 0b11111111),
)


def disco():
    pat = 0
    last_event_t = 0
    rx_count = 0
    led_override_begin()
    try:
        with L.IrSession("nec"):
            while True:
                now = time.ticks_ms()
                got = False
                try:
                    f = ir_nec_read()
                    if f is not None:
                        got = True
                        addr, cmd, is_repeat = f
                        L.auto_rx_label(addr=addr, cmd=cmd, repeat=is_repeat)
                except Exception:
                    pass
                if got:
                    rx_count += 1
                    last_event_t = now
                    pat = (pat + 1) % len(PATTERNS)
                    led_set_frame(PATTERNS[pat], 200)
                if last_event_t and time.ticks_diff(now, last_event_t) > 250:
                    led_clear()

                oled_clear()
                L.chrome("Disco")
                L.draw_hero(str(rx_count))
                L.draw_subline("frames received")
                L.footer(("Back", "back"))
                oled_show()

                if button_pressed(BTN_BACK):
                    return
                time.sleep_ms(40)
    finally:
        led_clear()
        led_override_end()


# ── Top-level launcher ─────────────────────────────────────────────────────


def run():
    items = (
        {"label": "Echo",      "icon": "ir_play", "desc": "Reflect a nonce game",        "fn": echo},
        {"label": "QuickDraw", "icon": "ir_play", "desc": "Fastest TX wins",             "fn": quickdraw},
        {"label": "Beacon",    "icon": "wifi",    "desc": "Spot nearby badges",          "fn": beacon_tag},
        {"label": "Disco",     "icon": "games",   "desc": "LED flashes on every RX",     "fn": disco},
    )
    while True:
        idx = L.grid_menu(list(items), title="GAMES")
        if idx < 0:
            return
        items[idx]["fn"]()
