"""SCAN \u2014 IR signal inspector.

Three views, switch with <left> / <right>:
  Live   \u2014 last decoded frame, big text, frames/sec counter
  Detail \u2014 single-shot raw capture, leader timing + protocol guess
  Scope  \u2014 frames-per-second time graph

Default mode is NEC; left/right cycles between nec/raw to compare what
each path decodes from the same source. Useful for figuring out 'why
isn't my remote decoded?' \u2014 if NEC sees nothing but raw sees pulses,
your remote is non-NEC (Sony / RC5 / RC6).
"""

import struct
import time

from badge import *
import ir_lib as L


VIEWS = ("Live", "Detail", "Scope")
MODES = ("nec", "raw", "badge")


# ── Live view ──────────────────────────────────────────────────────────────


def _live(mode_idx):
    last_frame_text = "\u2014"
    last_frame_t = 0
    count = 0
    last_count_t = time.ticks_ms()
    fps = 0

    while True:
        now = time.ticks_ms()
        mode = MODES[mode_idx]

        try:
            if mode == "nec":
                f = ir_nec_read()
                if f is not None:
                    addr, cmd, is_repeat = f
                    count += 1
                    last_frame_t = now
                    last_frame_text = "%02X/%02X%s" % (
                        addr, cmd, "  (repeat)" if is_repeat else "")
                    L.auto_rx_label(addr=addr, cmd=cmd, repeat=is_repeat)
            elif mode == "raw":
                buf = ir_raw_capture()
                if buf:
                    n = len(buf) // 4
                    count += 1
                    last_frame_t = now
                    last_frame_text = "%d pairs" % n
                    L.auto_rx_label(raw_pairs=n)
            else:
                f = ir_read_words()
                if f is not None:
                    count += 1
                    last_frame_t = now
                    last_frame_text = "%dw  %08X" % (len(f), f[0] & 0xFFFFFFFF)
                    L.auto_rx_label(words=f)
        except Exception:
            pass

        if time.ticks_diff(now, last_count_t) >= 1000:
            fps = count
            count = 0
            last_count_t = now

        oled_clear()
        L.chrome("Scan/Live")
        L.draw_hero(last_frame_text)
        sub = "%s  -  %d / sec" % (mode, fps)
        if last_frame_t:
            sub += "   %dms ago" % time.ticks_diff(now, last_frame_t)
        L.draw_subline(sub)
        L.footer(("Left/Right", "view"), ("Up", "mode"), ("Back", "back"))
        oled_show()

        if button_pressed(BTN_BACK):
            return None
        if button_pressed(BTN_LEFT):
            return ("view", -1, mode_idx)
        if button_pressed(BTN_RIGHT):
            return ("view", 1, mode_idx)
        if button_pressed(BTN_UP):
            mode_idx = (mode_idx + 1) % len(MODES)
            try:
                ir_set_mode(MODES[mode_idx])
                ir_flush()
            except Exception:
                pass
            last_frame_text = "\u2014"
            last_frame_t = 0
            count = 0
            fps = 0
        time.sleep_ms(25)


# ── Detail view ────────────────────────────────────────────────────────────


def _guess(leader_mark, leader_space, n_pairs):
    if leader_mark > 8000 and 4000 <= leader_space <= 5000 and n_pairs >= 33:
        return "NEC"
    if leader_mark > 8000 and 2000 <= leader_space <= 2500:
        return "NEC repeat"
    if 2000 <= leader_mark <= 2700 and 400 <= leader_space <= 700:
        return "Sony SIRC"
    if leader_mark < 1000 and n_pairs > 10:
        return "RC5/RC6"
    return "?"


def _detail():
    """Single-shot raw capture + analysis. Drops to raw mode while in this
    view; restores caller's mode on return via the IrSession upstream."""
    try:
        ir_set_mode("raw")
        ir_flush()
    except Exception:
        pass

    last_buf = None

    while True:
        oled_clear()
        L.chrome("Scan/Detail")
        if last_buf is None:
            L.draw_body_line(0, "Press Confirm,")
            L.draw_body_line(1, "then a remote button.")
        else:
            n = len(last_buf) // 4
            lm, ls = struct.unpack_from("<HH", last_buf, 0)
            guess = _guess(lm, ls, n)
            L.draw_hero(guess)
            L.draw_subline("leader %d/%d us  -  %d pairs" % (lm, ls, n))
        L.footer(("Confirm", "scan"), ("Left/Right", "view"), ("Back", "back"))
        oled_show()

        if button_pressed(BTN_BACK):
            return None
        if button_pressed(BTN_LEFT):
            return ("view", -1, MODES.index("raw"))
        if button_pressed(BTN_RIGHT):
            return ("view", 1, MODES.index("raw"))
        if button_pressed(BTN_CONFIRM):
            ir_flush()
            L.big_message("Scan/Detail", body="listening 4s\u2026",
                           hero="...",
                           hint_actions=(("Back", "cancel"),))
            buf = L.raw_capture_blocking(4000)
            last_buf = buf if buf else None
            if buf:
                L.auto_rx_label(raw_pairs=len(buf) // 4)
        time.sleep_ms(40)


# ── Scope view ─────────────────────────────────────────────────────────────


def _scope(mode_idx):
    HISTORY = 96
    GRAPH_TOP = L.BODY_TOP + 2
    GRAPH_H = L.FOOTER_RULE_Y - GRAPH_TOP - 12  # leave room for axis label
    history = [0] * HISTORY
    bucket = 0
    bucket_t = time.ticks_ms()
    peak = 1

    while True:
        now = time.ticks_ms()
        mode = MODES[mode_idx]

        try:
            if mode == "nec":
                while ir_nec_read() is not None:
                    bucket += 1
            elif mode == "raw":
                while ir_raw_capture() is not None:
                    bucket += 1
            else:
                while ir_read_words() is not None:
                    bucket += 1
        except Exception:
            pass

        if time.ticks_diff(now, bucket_t) >= 250:
            history.pop(0)
            history.append(bucket)
            if bucket > peak:
                peak = bucket
            else:
                peak = max(1, peak - 1)
            bucket = 0
            bucket_t = now

        oled_clear()
        L.chrome("Scan/Scope")
        # bars
        x0 = (L.SCREEN_W - HISTORY) // 2
        for i in range(HISTORY):
            v = history[i]
            if v <= 0:
                continue
            bar_h = max(1, min(GRAPH_H, (v * GRAPH_H) // max(1, peak)))
            for k in range(bar_h):
                oled_set_pixel(x0 + i, GRAPH_TOP + GRAPH_H - 1 - k, 1)
        L.draw_subline("%s  peak %d/250ms" % (mode, peak))
        L.footer(("Left/Right", "view"), ("Up", "mode"), ("Back", "back"))
        oled_show()

        if button_pressed(BTN_BACK):
            return None
        if button_pressed(BTN_LEFT):
            return ("view", -1, mode_idx)
        if button_pressed(BTN_RIGHT):
            return ("view", 1, mode_idx)
        if button_pressed(BTN_UP):
            mode_idx = (mode_idx + 1) % len(MODES)
            try:
                ir_set_mode(MODES[mode_idx])
                ir_flush()
            except Exception:
                pass
            history = [0] * HISTORY
            peak = 1
            bucket = 0
        time.sleep_ms(25)


# ── Top-level ──────────────────────────────────────────────────────────────


def run():
    view_idx = 0
    mode_idx = MODES.index("nec")

    with L.IrSession(MODES[mode_idx]):
        while True:
            view = VIEWS[view_idx]
            if view == "Live":
                ev = _live(mode_idx)
            elif view == "Detail":
                ev = _detail()
            else:
                ev = _scope(mode_idx)
            if ev is None:
                return
            kind, delta, new_mode = ev
            if kind == "view":
                view_idx = (view_idx + delta) % len(VIEWS)
                mode_idx = new_mode
                # Re-apply the mode so swapping into Detail (which forces raw)
                # and back doesn't strand us.
                try:
                    ir_set_mode(MODES[mode_idx])
                    ir_flush()
                except Exception:
                    pass
