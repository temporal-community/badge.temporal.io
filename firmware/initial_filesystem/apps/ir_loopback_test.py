"""
ir_test.py — Multi-word IR diagnostic for a single badge.

Modes:
  UP    — loopback with random nonce; frame size cycles through 1/3/8
  DOWN  — passive listen for 3 s (no TX)
  LEFT  — cycle TX power: 5%, 10%, 20%, 33% (default), 45%
  RIGHT — exit

On startup, runs self-validation that invalid ir_send_words inputs
(empty list, >8 words) raise ValueError from the module boundary.
"""
import time
import os

ir_start()

SIZES = [1, 3, 8]
POWER_LEVELS = [1, 3, 10, 33, 50]

test_num = 0
pass_count = 0
fail_count = 0
power_idx = POWER_LEVELS.index(ir_tx_power())  # start at current level

def show_status(msg1, msg2="", msg3=""):
    oled_clear()
    oled_set_cursor(0, 10)
    oled_print(msg1)
    print(msg1)
    if msg2:
        oled_set_cursor(0, 24)
        oled_print(msg2)
        print(msg2)
    if msg3:
        oled_set_cursor(0, 38)
        oled_print(msg3)
        print(msg3)
    oled_show()

def rand16():
    b = os.urandom(2)
    return (b[0] << 8) | b[1]

def build_payload(size):
    """Header (0xDEAD) + nonce + counter, padded to `size` with random."""
    n = rand16()
    base = [0xDEAD, n, (n ^ 0xFFFF) & 0xFFFF, test_num & 0xFFFF]
    while len(base) < size:
        base.append(rand16())
    return base[:size]

# ── self-validation: malformed inputs ────────────────────────────────────────
print("=== Self-validation ===")
malformed_pass = 0
malformed_fail = 0

try:
    ir_send_words([])
    print("FAIL: empty list accepted")
    malformed_fail += 1
except ValueError:
    print("PASS: empty list rejected")
    malformed_pass += 1

try:
    ir_send_words([0] * 9)
    print("FAIL: 9 words accepted")
    malformed_fail += 1
except ValueError:
    print("PASS: 9 words rejected")
    malformed_pass += 1

try:
    ir_tx_power(0)
    print("FAIL: 0% accepted")
    malformed_fail += 1
except ValueError:
    print("PASS: 0% rejected")
    malformed_pass += 1

try:
    ir_tx_power(51)
    print("FAIL: 51% accepted")
    malformed_fail += 1
except ValueError:
    print("PASS: 51% rejected")
    malformed_pass += 1

show_status("Self-validation",
            "malform P:" + str(malformed_pass) + " F:" + str(malformed_fail),
            "A=send B=exit")
time.sleep_ms(1200)

show_status("IR Diagnostic",
            "Power: " + str(ir_tx_power()) + "%",
            "X pow Y listen")

# ── main loop ────────────────────────────────────────────────────────────────
while True:
    if button_pressed(BTN_BACK):
        break

    if button_pressed(BTN_SQUARE):
        power_idx = (power_idx + 1) % len(POWER_LEVELS)
        new_power = POWER_LEVELS[power_idx]
        ir_tx_power(new_power)
        show_status("TX power",
                    str(new_power) + "%",
                    "A=send")

    if button_pressed(BTN_CONFIRM):
        test_num += 1
        size = SIZES[(test_num - 1) % len(SIZES)]
        tx = build_payload(size)

        ir_flush()

        show_status("#" + str(test_num) + " TX " + str(size) + "w",
                    "pow " + str(ir_tx_power()) + "%",
                    str(tx[:3]) + ("..." if size > 3 else ""))

        ir_send_words(tx)

        # Each NEC bit is ~1.7 ms avg, each word is 32 bits, so budget
        # ~55 ms/word + leader/CRC/end overhead + slack.
        deadline_ms = 200 + size * 60
        if deadline_ms < 800:
            deadline_ms = 800

        frames = []
        t0 = time.ticks_ms()
        while time.ticks_diff(time.ticks_ms(), t0) < deadline_ms:
            got = ir_read_words()
            if got is not None:
                frames.append(list(got))
                continue
            time.sleep_ms(10)

        if not frames:
            fail_count += 1
            show_status("#" + str(test_num) + " NO RX",
                        str(size) + "w pow " + str(ir_tx_power()) + "%",
                        "P:" + str(pass_count) + " F:" + str(fail_count))
        else:
            match = any(f == tx for f in frames)
            if match and len(frames) == 1:
                pass_count += 1
                show_status("#" + str(test_num) + " PASS",
                            str(size) + "w pow " + str(ir_tx_power()) + "%",
                            "P:" + str(pass_count) + " F:" + str(fail_count))
            elif match:
                pass_count += 1
                show_status("#" + str(test_num) + " PASS*",
                            "+" + str(len(frames) - 1) + " extra frames",
                            "P:" + str(pass_count) + " F:" + str(fail_count))
            else:
                fail_count += 1
                for i, f in enumerate(frames):
                    print("  fr" + str(i) + ": " + str(f))
                show_status("#" + str(test_num) + " MISMATCH",
                            str(len(frames)) + " fr, 0 match",
                            "P:" + str(pass_count) + " F:" + str(fail_count))

    if button_pressed(BTN_PRESETS):
        ir_flush()
        show_status("Listening 3s...",
                    "(no TX)",
                    "pow " + str(ir_tx_power()) + "%")

        frames = []
        t0 = time.ticks_ms()
        while time.ticks_diff(time.ticks_ms(), t0) < 3000:
            got = ir_read_words()
            if got is not None:
                frames.append(list(got))
            time.sleep_ms(20)

        if not frames:
            show_status("Listen: quiet",
                        "0 frames in 3s",
                        "(RX clean)")
        else:
            print("Listen got " + str(len(frames)) + " frames:")
            for i, f in enumerate(frames):
                print("  fr" + str(i) + ": " + str(f))
            show_status("Listen: " + str(len(frames)) + " fr",
                        "see serial",
                        "(ambient?)")

    time.sleep_ms(50)

ir_stop()
show_status("Done",
            "P:" + str(pass_count) + " F:" + str(fail_count),
            "malform " + str(malformed_pass) + "/" + str(malformed_pass + malformed_fail))
time.sleep_ms(2000)
exit()
