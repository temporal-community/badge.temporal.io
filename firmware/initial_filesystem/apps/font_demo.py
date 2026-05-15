"""
font_demo.py — Cycle through all available fonts on the OLED display.

Shows each font name rendered in that font, along with a sample text line.
Use the joystick to navigate; press B to exit.
"""
import time

JOY_LOW = 1100
JOY_HIGH = 3000
NAV_MS = 170

fonts_csv = oled_get_fonts()
fonts = fonts_csv.split(",") if fonts_csv else []

if not fonts:
    oled_clear()
    oled_set_cursor(0, 20)
    oled_print("No fonts found")
    oled_show()
    time.sleep_ms(2000)
    exit()

idx = 0

def show_font(i):
    name = fonts[i]
    oled_set_font(name)
    oled_clear()
    oled_set_cursor(0, 5)
    oled_print(str(i + 1) + "/" + str(len(fonts)))
    h = oled_text_height()
    w = oled_text_width(name)
    oled_set_cursor(0, h + 4)
    oled_print(name)
    oled_set_cursor(0, (h + 4) * 2)
    oled_print("AaBb 0123")
    oled_set_cursor(0, 60)
    oled_set_font(fonts[0])
    oled_print("Joy nav B exit")
    oled_show()
    oled_set_font(name)

show_font(idx)
last_nav = 0

while True:
    if button_pressed(BTN_BACK):
        break

    now = time.ticks_ms()
    if time.ticks_diff(now, last_nav) >= NAV_MS:
        jy = joy_y()
        if jy < JOY_LOW:
            idx = (idx - 1) % len(fonts)
            show_font(idx)
            last_nav = now
        elif jy > JOY_HIGH:
            idx = (idx + 1) % len(fonts)
            show_font(idx)
            last_nav = now

    time.sleep_ms(50)

oled_set_font(fonts[0])
oled_clear(True)
exit()
