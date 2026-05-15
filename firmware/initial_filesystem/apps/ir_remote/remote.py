"""REMOTE \u2014 universal IR controller for TV / Audio / Projector / AC,
plus the Custom 8-slot bind-and-replay UR, Macro recorder, and TV-B-Gone.

Top-level menu:
  TV / Audio / Projector / AC   \u2014 vendor codebook + button picker
  Custom                        \u2014 8 free-form slots bound to badge buttons
  Macro                         \u2014 record up to 16 NEC frames + replay
  TV-B-Gone                     \u2014 cycle bundled TV power-off codes

All sub-modes use ir_lib.IrSession for mode hygiene + the standard
top status bar + footer hints. The TX/RX labels are auto-populated by
ir_lib.nec_send / raw_send / nec_read so the bar always reflects the
last action.
"""

import os
import time

from badge import *
import ir_lib as L


# ── Pickers ─────────────────────────────────────────────────────────────────


def _pick_vendor(category, title):
    vendors = L.codebook(category)
    if not vendors:
        L.big_message(title, "no codes yet", "Press Custom to record.",
                      hint_actions=(("Confirm", "ok"), ("Back", "back")))
        L.wait_button((BTN_CONFIRM, BTN_BACK))
        return None
    items = [{"label": name} for (name, _buttons) in vendors]
    idx = L.list_menu(items, title=title + " vendor", hint_label="open")
    if idx < 0:
        return None
    return vendors[idx]  # (name, buttons_dict)


def _pick_button(buttons, vendor_name):
    """Pick-and-send loop. Keeps the cursor on whichever button you
    just sent so repeated presses (Vol+, Vol+, Vol+...) don't make
    the cursor jump back to the top of the list every time."""
    keys = list(buttons.keys())
    keys.sort()
    items = [{"label": k} for k in keys]
    cursor = 0
    while True:
        idx, cursor = L.list_menu_with_cursor(
            items, title=vendor_name, hint_label="send",
            initial_cursor=cursor)
        if idx < 0:
            return
        label = keys[idx]
        payload = buttons[label]
        # Don't override the auto-derived label — the lib formats raw
        # hex (e.g. "07/02") which fits the chrome bar; "vendor button"
        # overflows into the activity indicator.
        L.emit_remote_button(payload)


# ── TV / Audio / Projector \u2014 vendor + button drilldown ─────────────────────


def _do_codebook_category(category, title, mode="nec"):
    with L.IrSession(mode):
        while True:
            picked = _pick_vendor(category, title)
            if picked is None:
                return
            vendor_name, buttons = picked
            _pick_button(buttons, vendor_name)


def tv():
    _do_codebook_category("tv", "TV")


def audio():
    _do_codebook_category("audio", "Audio")


def projector():
    _do_codebook_category("projector", "Projector")


# ── AC \u2014 6 named state slots, raw-mode (Coolix presets are raw) ──────────


AC_SLOT_LABELS = ("Off", "Dehumid", "Cool-Hi", "Cool-Lo", "Heat-Hi", "Heat-Lo")


def _ac_overrides_path(vendor_slug):
    return L.SLOTS_DIR + "/ac_" + vendor_slug + ".txt"


def _slug(name):
    out = ""
    for ch in name.lower():
        if ch.isalpha() or ch.isdigit():
            out += ch
    return out


def _load_ac_overrides(vendor_slug):
    """Returns a dict[label] -> raw bytes for any user-recorded AC state.
    Falls back silently if the file doesn't exist yet."""
    out = {}
    try:
        with open(_ac_overrides_path(vendor_slug), "r") as fh:
            data = fh.read()
    except OSError:
        return out
    for line in data.split("\n"):
        line = line.strip()
        if not line or "|" not in line:
            continue
        label, blob_hex = line.split("|", 1)
        if blob_hex:
            try:
                out[label] = bytes(int(blob_hex[i:i + 2], 16)
                                    for i in range(0, len(blob_hex), 2))
            except Exception:
                pass
    return out


def _save_ac_override(vendor_slug, label, blob):
    L.ensure_slots_dir()
    overrides = _load_ac_overrides(vendor_slug)
    overrides[label] = blob
    try:
        with open(_ac_overrides_path(vendor_slug), "w") as fh:
            for k, v in overrides.items():
                fh.write(k + "|" + "".join("{:02x}".format(b) for b in v) + "\n")
        return True
    except OSError:
        return False


def _record_ac_state(label):
    """Switch to raw mode briefly and capture the user's remote press."""
    L.big_message("Record " + label,
                   body="Aim your AC remote",
                   hero="press " + label,
                   hint_actions=(("Cancel", "abort"),))
    try:
        ir_set_mode("raw")
    except Exception:
        return None
    ir_flush()
    deadline = time.ticks_add(time.ticks_ms(), 8000)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        if button_pressed(BTN_BACK):
            return None
        try:
            buf = ir_raw_capture()
        except Exception:
            buf = None
        if buf:
            return buf
        time.sleep_ms(20)
    return None


def ac():
    vendors = L.codebook("ac")
    if not vendors:
        vendors = (("Custom AC", {label: None for label in AC_SLOT_LABELS}),)
    items = [{"label": name} for (name, _buttons) in vendors]
    items.append({"label": "+ New (record only)"})

    with L.IrSession("raw"):
        while True:
            idx = L.list_menu(items, title="AC vendor", hint_label="open")
            if idx < 0:
                return
            if idx == len(vendors):
                vendor_name = "MyAC"
                preset = {label: None for label in AC_SLOT_LABELS}
            else:
                vendor_name, preset = vendors[idx]
            slug = _slug(vendor_name)
            _ac_browse(vendor_name, preset, slug)


def _ac_browse(vendor_name, preset_buttons, vendor_slug):
    cursor = 0
    while True:
        overrides = _load_ac_overrides(vendor_slug)
        rows = []
        for label in AC_SLOT_LABELS:
            has_override = label in overrides
            has_preset = label in preset_buttons and preset_buttons[label] is not None
            tag = "*" if has_override else ("\u2713" if has_preset else "\u2014")
            rows.append({"label": tag + " " + label})
        rows.append({"label": "Back"})
        idx, cursor = L.list_menu_with_cursor(
            rows, title=vendor_name, hint_label="send/Y rec",
            initial_cursor=cursor)
        if idx < 0 or idx == len(AC_SLOT_LABELS):
            return
        label = AC_SLOT_LABELS[idx]

        # Cross button (BTN_PRESETS = Y) records; Confirm sends.
        if button(BTN_PRESETS):
            blob = _record_ac_state(label)
            if blob is not None:
                _save_ac_override(vendor_slug, label, blob)
                L.big_message("Saved", body=label + " (" + vendor_name + ")",
                               hero=str(len(blob) // 4) + "p",
                               hint_actions=(("Confirm", "ok"),))
                L.wait_button((BTN_CONFIRM, BTN_BACK))
            else:
                L.big_message("No signal", body="Try again, hold remote 6\"",
                               hero="\u2716",
                               hint_actions=(("Confirm", "ok"),))
                L.wait_button((BTN_CONFIRM, BTN_BACK))
            continue

        # Otherwise transmit \u2014 prefer override, fall back to preset.
        # Default labels keep the chrome TX field tight ("38kHz raw 84p"
        #/"07/02") so they don't overflow into the activity bar.
        sent = False
        if label in overrides:
            sent = L.raw_send(overrides[label], 38000)
        elif label in preset_buttons and preset_buttons[label] is not None:
            sent = L.emit_remote_button(preset_buttons[label])
        if not sent:
            L.big_message("Empty", body="Hold Y to record this slot",
                           hero=label,
                           hint_actions=(("Confirm", "ok"),))
            L.wait_button((BTN_CONFIRM, BTN_BACK))


# ── Custom \u2014 8-slot bind-and-replay (one slot per badge button) ───────────


CUSTOM_SLOTS = (
    ("Up",    BTN_UP),
    ("Down",  BTN_DOWN),
    ("Left",  BTN_LEFT),
    ("Right", BTN_RIGHT),
    ("Cross",  BTN_CROSS),     # X
    ("Triangle", BTN_TRIANGLE), # Y
    ("Circle",   BTN_CIRCLE),   # B (Confirm by default)
    ("Square",   BTN_SQUARE),   # A
)


def _custom_pick_layout():
    layouts = L.list_layouts()
    items = [{"label": "+ new layout"}]
    items.extend({"label": l} for l in layouts)
    idx = L.list_menu(items, title="Custom layouts", hint_label="open")
    if idx < 0:
        return None
    if idx == 0:
        return "MyRemote"
    return layouts[idx - 1]


def _capture_for_custom_slot(slot_label):
    """First try NEC; fall back to raw."""
    L.big_message("Bind " + slot_label,
                   body="Aim a remote",
                   hero="press button",
                   hint_actions=(("Cancel", "abort"),))
    try:
        ir_set_mode("nec")
    except Exception:
        pass
    ir_flush()
    deadline = time.ticks_add(time.ticks_ms(), 8000)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        if button_pressed(BTN_BACK):
            return None
        try:
            f = ir_nec_read()
        except Exception:
            f = None
        if f is not None:
            addr, cmd, is_repeat = f
            if not is_repeat:
                return {"kind": "nec", "addr": addr, "cmd": cmd, "repeats": 0}
        time.sleep_ms(15)

    # NEC timed out \u2014 try raw.
    try:
        ir_set_mode("raw")
    except Exception:
        return None
    ir_flush()
    L.big_message("Bind " + slot_label, body="Trying raw\u2026",
                   hero="press button",
                   hint_actions=(("Cancel", "abort"),))
    buf = L.raw_capture_blocking(4000)
    try:
        ir_set_mode("nec")
    except Exception:
        pass
    if buf:
        return {"kind": "raw", "carrier_hz": 38000, "data": buf}
    return None


def _custom_replay(slot):
    if not slot:
        return False
    if slot["kind"] == "nec":
        return L.nec_send(slot["addr"], slot["cmd"], slot.get("repeats", 0))
    if slot["kind"] == "raw":
        try:
            ir_set_mode("raw")
        except Exception:
            return False
        ok = L.raw_send(slot["data"], slot.get("carrier_hz", 38000))
        try:
            ir_set_mode("nec")
        except Exception:
            pass
        return ok
    return False


def custom():
    layout = _custom_pick_layout()
    if layout is None:
        return
    slots = L.load_layout(layout)

    with L.IrSession("nec"):
        binding_target = None
        msg = ""
        msg_t = 0
        while True:
            now = time.ticks_ms()
            oled_clear()
            L.chrome("Custom")
            for i, (lbl, _btn) in enumerate(CUSTOM_SLOTS):
                state = "\u2713" if slots.get(lbl) else "\u2014"
                if L.BODY_FIRST_BASE_Y + i * L.BODY_LINE_H >= L.FOOTER_RULE_Y - 2:
                    break
                L.draw_body_line(i, state + " " + lbl)
            if msg and time.ticks_diff(now, msg_t) < 1500:
                L.use_font("body")
                L._print_at(80, L.BODY_FIRST_BASE_Y, msg)
            footer_actions = [("Confirm", "save"), ("Back", "exit")]
            if binding_target:
                footer_actions = [("Back", "cancel bind")]
            L.footer(*footer_actions, ("Up", "rec on next press"))
            oled_show()

            if button_pressed(BTN_BACK):
                if binding_target:
                    binding_target = None
                    continue
                return
            if button_pressed(BTN_CONFIRM):
                if L.save_layout(layout, slots):
                    msg = "saved"
                else:
                    msg = "save fail"
                msg_t = time.ticks_ms()
                continue
            if button_pressed(BTN_UP):  # Y on the badge
                # arm bind on next button press
                binding_target = "next"
                msg = "bind: press a btn"
                msg_t = time.ticks_ms()
                continue

            # Detect slot presses.
            pressed_label = None
            for lbl, btn in CUSTOM_SLOTS:
                if button_pressed(btn):
                    pressed_label = lbl
                    break
            if pressed_label is not None:
                if binding_target is not None:
                    captured = _capture_for_custom_slot(pressed_label)
                    if captured:
                        slots[pressed_label] = captured
                        msg = pressed_label + " bound"
                    else:
                        msg = "no signal"
                    msg_t = time.ticks_ms()
                    binding_target = None
                else:
                    if _custom_replay(slots.get(pressed_label)):
                        msg = "TX " + pressed_label
                    else:
                        msg = pressed_label + " empty"
                    msg_t = time.ticks_ms()
                # debounce
                while True:
                    held = False
                    for _lbl, btn in CUSTOM_SLOTS:
                        if button(btn):
                            held = True
                            break
                    if not held:
                        break
                    time.sleep_ms(15)
            time.sleep_ms(30)


# ── Macro \u2014 record/replay a sequence of NEC frames ─────────────────────────


def macro():
    steps = []
    recording = False
    last_t = None
    msg = ""
    msg_t = 0

    with L.IrSession("nec"):
        while True:
            now = time.ticks_ms()
            oled_clear()
            L.chrome("Macro REC" if recording else "Macro")
            visible = max(1, (L.FOOTER_RULE_Y - L.BODY_TOP) // L.BODY_LINE_H)
            start = max(0, len(steps) - visible)
            for i in range(visible):
                idx = start + i
                if idx >= len(steps):
                    break
                addr, cmd, gap = steps[idx]
                L.draw_body_line(i, "%2d  %02X/%02X +%dms"
                                     % (idx + 1, addr, cmd, gap))
            if not steps:
                L.draw_body_line(0, "(empty - hit rec)")
            if msg and time.ticks_diff(now, msg_t) < 1400:
                L.draw_subline(msg)

            if recording:
                L.footer(("Confirm", "stop rec"), ("Back", "back"))
            else:
                actions = [("Confirm", "rec"), ("Back", "back"),
                            ("Up", "play"), ("Down", "clear")]
                L.footer(*actions)
            oled_show()

            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                if recording:
                    recording = False
                    last_t = None
                else:
                    if len(steps) >= 16:
                        msg = "macro full"
                        msg_t = time.ticks_ms()
                    else:
                        recording = True
                        last_t = None
                continue
            if recording:
                try:
                    f = ir_nec_read()
                except Exception:
                    f = None
                if f is not None and not f[2] and len(steps) < 16:
                    addr, cmd, _ = f
                    gap = 0 if last_t is None else max(0, time.ticks_diff(now, last_t))
                    last_t = now
                    steps.append((addr, cmd, gap))
                    L.auto_rx_label(addr=addr, cmd=cmd, repeat=False)
            else:
                if button_pressed(BTN_UP):
                    if not steps:
                        msg = "nothing to play"
                        msg_t = time.ticks_ms()
                    else:
                        for addr, cmd, gap in steps:
                            if gap > 0:
                                time.sleep_ms(min(gap, 5000))
                            L.nec_send(addr, cmd, 0,
                                        label="%02X/%02X" % (addr, cmd))
                            time.sleep_ms(80)
                        msg = "played %d" % len(steps)
                        msg_t = time.ticks_ms()
                if button_pressed(BTN_DOWN):
                    steps = []
                    msg = "cleared"
                    msg_t = time.ticks_ms()
            time.sleep_ms(20)


# ── TV-B-Gone \u2014 cycle bundled TV power codes ──────────────────────────────


def _tvbgone_codes():
    """Power codes pulled from the TV codebook plus the legacy list.
    (vendor, addr, cmd) tuples. NEC-only \u2014 Sony/RC5 TVs are out of scope."""
    seen = set()
    codes = []
    for vendor, buttons in L.codebook("tv"):
        p = buttons.get("Power")
        if p and p[0] == "nec":
            key = (p[1], p[2])
            if key not in seen:
                seen.add(key)
                codes.append((vendor, p[1], p[2]))
    # Bundle the curated power-off-only list too (extra brands).
    legacy = L.codebook("tvbgone")
    if legacy:
        for vendor, addr, cmd in legacy:
            key = (addr, cmd)
            if key not in seen:
                seen.add(key)
                codes.append((vendor, addr, cmd))
    return codes


def tvbgone():
    codes = _tvbgone_codes()
    if not codes:
        L.big_message("TV-B-Gone", body="no codes loaded",
                       hero="\u2716",
                       hint_actions=(("Back", "back"),))
        L.wait_button((BTN_CONFIRM, BTN_BACK))
        return

    # Confirmation.
    L.big_message("TV-B-Gone", body="kill nearby TVs?",
                   hero=str(len(codes)) + " codes",
                   hint_actions=(("Confirm", "fire"), ("Back", "cancel")))
    if not L.wait_yes_no():
        return

    with L.IrSession("nec", power=50):
        i = 0
        paused = False
        while i < len(codes):
            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                paused = not paused
                while button(BTN_CONFIRM):
                    time.sleep_ms(15)
            if paused:
                _draw_tvbgone(codes, i, paused=True)
                time.sleep_ms(40)
                continue

            vendor, addr, cmd = codes[i]
            _draw_tvbgone(codes, i, paused=False)
            L.nec_send(addr, cmd, 2)
            time.sleep_ms(220)
            i += 1

        L.big_message("TV-B-Gone", body="all codes sent",
                       hero="done",
                       hint_actions=(("Back", "back"),))
        L.wait_button((BTN_CONFIRM, BTN_BACK))


def _draw_tvbgone(codes, i, paused):
    oled_clear()
    L.chrome("TV-B-Gone")
    vendor, addr, cmd = codes[i]
    L.draw_hero(vendor[:12])
    L.draw_subline("%d/%d  -  %02X/%02X" % (i + 1, len(codes), addr, cmd))
    if paused:
        L.footer(("Confirm", "resume"), ("Back", "exit"))
    else:
        L.footer(("Confirm", "pause"), ("Back", "stop"))
    oled_show()


# ── REMOTE top-level launcher ──────────────────────────────────────────────


def run():
    items = [
        {"label": "TV", "fn": tv},
        {"label": "Audio", "fn": audio},
        {"label": "Projector", "fn": projector},
        {"label": "AC", "fn": ac},
        {"label": "Custom", "fn": custom},
        {"label": "Macro", "fn": macro},
        {"label": "TV-B-Gone", "fn": tvbgone},
    ]
    while True:
        idx = L.list_menu(items, title="REMOTE", hint_label="open")
        if idx < 0:
            return
        items[idx]["fn"]()
