"""
ir_poll_test.py — IR polling constraint verification.

Polls ir_available() / ir_read() every 50ms for 2 minutes.
Counts frames received. Run a second badge sending IR frames continuously.

Expected: No IRremote buffer overflow. Displays frame count on screen.
Exit: BTN_BACK (cancels test early).
"""
import time

oled_clear()
oled_set_cursor(8, 10)
oled_print("IR Poll Test")
oled_set_cursor(0, 24)
oled_print("Polling at 50ms")
oled_set_cursor(0, 48)
oled_print("B=exit")
oled_show()

time.sleep_ms(1000)

DURATION_MS = 120000
POLL_MS = 50

ir_start()
start = time.ticks_ms()
frames = 0
overflows = 0

while True:
    now = time.ticks_ms()
    elapsed = time.ticks_diff(now, start)

    if button_pressed(BTN_BACK):
        break

    if elapsed >= DURATION_MS:
        break

    if ir_available():
        result = ir_read()
        if result is not None:
            frames += 1
        else:
            overflows += 1

    if (elapsed // 2000) != ((elapsed - POLL_MS) // 2000):
        remaining = (DURATION_MS - elapsed) // 1000
        oled_clear()
        oled_set_cursor(8, 10)
        oled_print("IR Poll Test")
        oled_set_cursor(0, 24)
        oled_print("Frames: " + str(frames))
        oled_set_cursor(0, 34)
        oled_print("Errors: " + str(overflows))
        oled_set_cursor(0, 44)
        oled_print("Rem: " + str(remaining) + "s")
        oled_show()

    time.sleep_ms(POLL_MS)

ir_stop()

oled_clear()
oled_set_cursor(8, 10)
oled_print("IR Poll Done")
oled_set_cursor(0, 22)
oled_print("Frames: " + str(frames))
oled_set_cursor(0, 34)
oled_print("Errors: " + str(overflows))
oled_set_cursor(48, 48)
oled_print("PASS" if overflows == 0 else "FAIL")
oled_show()

time.sleep_ms(3000)
exit()
