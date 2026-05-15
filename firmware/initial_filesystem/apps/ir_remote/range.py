"""RANGE \u2014 IR signal-strength diagnostics for two badges.

Three modes, switch with <left>/<right>:
  Bounce   \u2014 ping-pong over badge multi-word transport, live RTT/loss
  Sweep    \u2014 walk TX power 1..50%, count peer reflections per level
  MinPower \u2014 auto-find lowest power that reliably reaches the peer

All three use the badge multi-word path (ir_send_words) so two badges
can negotiate without reconfiguring. The peer should be running the
companion role: Bounce reflects PINGs natively, Sweep / MinPower expect
the peer to be in Bounce so the probes get reflected.
"""

import os
import time

from badge import *
import ir_lib as L


VIEWS = ("Bounce", "Sweep", "MinPwr")

MAGIC_PING = 0xBA00B100
MAGIC_PONG = 0xBA00B200
MAGIC_PROBE = 0xBA00C300

# 5 levels because the body region only fits 4 rows at 8 px (Smallsimple)
# pitch with bar-graph chrome. Picks span the practical range:
#   1 % = whisper, 5 % = same room, 20 % = across the room,
#   33 % = ambient-light tolerant, 50 % = max LED drive.
LEVELS = (1, 5, 10, 20, 50)
PROBES_PER_LEVEL = 8


def _seq16():
    b = os.urandom(2)
    return (b[0] << 8) | b[1]


# ── Bounce ─────────────────────────────────────────────────────────────────


def _bounce():
    sent = 0
    seen = 0
    last_rtt = 0
    rtt_avg = 0
    pending = {}
    last_tx_t = 0

    while True:
        now = time.ticks_ms()
        if time.ticks_diff(now, last_tx_t) > 250:
            seq = _seq16()
            pending[seq] = now
            for k in list(pending.keys()):
                if time.ticks_diff(now, pending[k]) > 2000:
                    del pending[k]
            try:
                ir_send_words([MAGIC_PING | seq])
                sent += 1
                L.set_tx_label("%04X" % seq)
            except Exception:
                pass
            last_tx_t = now

        try:
            got = ir_read_words()
        except Exception:
            got = None
        if got is not None and len(got) >= 1:
            w = int(got[0]) & 0xFFFFFFFF
            tag = w & 0xFFFF0000
            seq = w & 0xFFFF
            if tag == MAGIC_PING:
                # Reflect peer pings.
                try:
                    ir_send_words([MAGIC_PONG | seq])
                    L.set_tx_label("%04X" % seq)
                except Exception:
                    pass
                L.set_rx_label("%04X" % seq)
            elif tag == MAGIC_PONG and seq in pending:
                sent_t = pending.pop(seq)
                last_rtt = time.ticks_diff(now, sent_t)
                rtt_avg = (rtt_avg * 7 + last_rtt) // 8
                seen += 1
                L.set_rx_label("%dms" % last_rtt)

        oled_clear()
        L.chrome("Range/Bounce")
        L.draw_hero("%d ms" % last_rtt if last_rtt else "\u2014")
        loss = 0 if not sent else 100 - min(100, (seen * 100) // sent)
        L.draw_subline("tx %d  rx %d  loss %d%%  avg %dms"
                        % (sent, seen, loss, rtt_avg))
        L.footer(("Left/Right", "view"), ("Back", "back"))
        oled_show()

        if button_pressed(BTN_BACK):
            return None
        if button_pressed(BTN_LEFT):
            return ("view", -1)
        if button_pressed(BTN_RIGHT):
            return ("view", 1)
        time.sleep_ms(15)


# ── Sweep ──────────────────────────────────────────────────────────────────


def _sweep():
    """Walk LEVELS, send PROBES_PER_LEVEL pings per level, count peer pongs."""
    # "Sweep" is short enough to fit at hero size (9x15 * 5 chars = 45 px).
    # The instructions go in the subline where the body font is narrow
    # enough that the full sentence fits.
    L.big_message("Range/Sweep",
                   body="peer must be in Bounce",
                   hero="Sweep",
                   hint_actions=(("Confirm", "go"), ("Back", "back")))
    if not L.wait_yes_no():
        return None

    results = []
    for level in LEVELS:
        try:
            ir_tx_power(level)
        except Exception:
            pass
        ir_flush()
        hits = 0
        for n in range(PROBES_PER_LEVEL):
            if button_pressed(BTN_BACK):
                return None
            try:
                ir_send_words([MAGIC_PROBE | (level << 8) | n])
                L.set_tx_label("%d%%" % level)
            except Exception:
                pass
            deadline = time.ticks_add(time.ticks_ms(), 350)
            while time.ticks_diff(deadline, time.ticks_ms()) > 0:
                try:
                    got = ir_read_words()
                except Exception:
                    got = None
                if got is not None:
                    hits += 1
                    L.auto_rx_label(words=got)
                    break
                time.sleep_ms(12)
        results.append((level, hits))
        _draw_sweep(results, in_progress=True)

    while True:
        _draw_sweep(results, in_progress=False)
        if button_pressed(BTN_BACK):
            return None
        if button_pressed(BTN_LEFT):
            return ("view", -1)
        if button_pressed(BTN_RIGHT):
            return ("view", 1)
        time.sleep_ms(40)


def _draw_sweep(results, in_progress):
    """Body-region bar chart of (level, hits) tuples.

    Vertical layout, body region runs from y=BODY_TOP=10 to FOOTER_RULE_Y-1=53
    (44 px tall). 5 LEVELS at 8 px row pitch = 40 px, leaves 4 px margin.
      - row 0 sits at body_y_top = 11
      - each row: %2d%% label (Smallsimple 5x7, glyph y=row_top..row_top+6)
                  bar (5 px tall, glyph-aligned)
                  hits count (Smallsimple, right side)
    """
    oled_clear()
    L.chrome("Range/Sweep")
    L.use_font("label")  # Smallsimple 5x7 — denser data table

    body_y_top = L.BODY_TOP + 1  # = 11; one px below chrome rule
    row_pitch = 8
    bar_h = 5
    label_w = 18           # "50%" needs ~16 px in 5x7
    count_w = 22           # "8/8" needs ~14 px + margin
    bar_x = label_w + 2
    bar_w = L.SCREEN_W - bar_x - count_w - 2

    for i, (lv, h) in enumerate(results):
        row_top = body_y_top + i * row_pitch
        if row_top + row_pitch > L.FOOTER_RULE_Y - 1:
            break
        # Label (left)
        L._print_at(0, row_top, "%2d%%" % lv)
        # Bar
        bar_top = row_top + 1  # nudge down 1 px so it visually centres on text
        L._frame(bar_x, bar_top, bar_w, bar_h)
        fill_px = ((bar_w - 2) * h) // PROBES_PER_LEVEL
        if fill_px > 0:
            L._fill(bar_x + 1, bar_top + 1, fill_px, bar_h - 2)
        # Count (right)
        L._print_at(L.SCREEN_W - count_w + 2, row_top,
                     "%d/%d" % (h, PROBES_PER_LEVEL))
    if in_progress:
        L.footer(("Back", "stop"))
    else:
        L.footer(("Left/Right", "view"), ("Back", "back"))
    oled_show()


# ── Min Power ──────────────────────────────────────────────────────────────


def _min_power():
    """Walk power levels, stop at the first one that hits the threshold."""
    L.big_message("Range/MinPwr",
                   body="peer must be in Bounce",
                   hero="MinPwr",
                   hint_actions=(("Confirm", "go"), ("Back", "back")))
    if not L.wait_yes_no():
        return None

    PROBES = 8
    THRESH = 6
    LEVELS_CASCADE = (1, 2, 3, 5, 8, 12, 18, 25, 33, 45)
    rows = []
    found = None

    for level in LEVELS_CASCADE:
        try:
            ir_tx_power(level)
        except Exception:
            pass
        ir_flush()
        hits = 0
        for n in range(PROBES):
            if button_pressed(BTN_BACK):
                return None
            try:
                ir_send_words([0xBA00C400 | (level << 8) | n])
                L.set_tx_label("%d%%" % level)
            except Exception:
                pass
            deadline = time.ticks_add(time.ticks_ms(), 300)
            while time.ticks_diff(deadline, time.ticks_ms()) > 0:
                try:
                    got = ir_read_words()
                except Exception:
                    got = None
                if got is not None:
                    hits += 1
                    L.auto_rx_label(words=got)
                    break
                time.sleep_ms(12)
        rows.append((level, hits))
        if hits >= THRESH:
            found = level
            break

    while True:
        oled_clear()
        L.chrome("Range/MinPwr")
        L.draw_hero((str(found) + "%") if found else "no good")
        L.draw_subline("min reliable" if found else "tried 1-45%")
        L.footer(("Left/Right", "view"), ("Back", "back"))
        oled_show()

        if button_pressed(BTN_BACK):
            return None
        if button_pressed(BTN_LEFT):
            return ("view", -1)
        if button_pressed(BTN_RIGHT):
            return ("view", 1)
        time.sleep_ms(40)


# ── Top-level ──────────────────────────────────────────────────────────────


def run():
    view_idx = 0
    with L.IrSession("badge", power=10):
        while True:
            view = VIEWS[view_idx]
            if view == "Bounce":
                ev = _bounce()
            elif view == "Sweep":
                ev = _sweep()
            else:
                ev = _min_power()
            if ev is None:
                return
            view_idx = (view_idx + ev[1]) % len(VIEWS)
