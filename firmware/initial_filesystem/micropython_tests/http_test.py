"""HTTP smoke test for explicit hacker networking.

This does not use the Replay API. It only calls badge.http_get() against
an unrelated public test endpoint when WiFi credentials are configured.
"""
import time

URL = "https://httpcats.com/200.json"

oled_clear()
oled_set_cursor(24, 10)
oled_print("HTTP Test")
oled_show()

oled_set_cursor(0, 24)
oled_print("httpcats.com")
oled_set_cursor(0, 38)
oled_print("Fetching...")
oled_show()

try:
    import badge
    resp = badge.http_get(URL)

    oled_clear()
    oled_set_cursor(8, 10)
    oled_print("HTTP GET OK")
    oled_set_cursor(0, 24)
    oled_print(("len=" + str(len(resp)))[:20])
    oled_set_cursor(0, 38)
    oled_print(resp[:20])
    oled_set_cursor(0, 52)
    oled_print("Press > to exit")
    oled_show()

    deadline = time.ticks_add(time.ticks_ms(), 10000)
    while not button_pressed(BTN_RIGHT):
        if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
            break
        time.sleep_ms(50)

except Exception as e:
    oled_clear()
    oled_set_cursor(8, 20)
    oled_print("HTTP FAILED")
    oled_set_cursor(0, 36)
    oled_print(str(e)[:20])
    oled_show()
    time.sleep_ms(3000)

exit()
