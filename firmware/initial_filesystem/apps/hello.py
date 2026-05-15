"""
hello.py — Reference app for the Temporal Badge Python runtime.

Demonstrates:
  oled_clear / oled_set_cursor / oled_print / oled_show — OLED display
  button_pressed(BTN_BACK) — poll the cancel/back button
  exit()                 — clean exit back to the main menu

All badge functions are auto-imported into global scope.
"""
import time
import gc

oled_clear()
oled_set_cursor(10, 28)
oled_print("Hello, World!")
oled_set_cursor(8, 42)
oled_print("Temporal Badge")
oled_show()

time.sleep_ms(1500)

oled_clear()
oled_set_cursor(4, 28)
oled_print("Press B to exit")
oled_set_cursor(4, 42)
oled_print("A=select B=back")
oled_show()

gc.collect()

deadline = time.ticks_add(time.ticks_ms(), 8000)
while not button_pressed(BTN_BACK):
    if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
        break
    time.sleep_ms(20)

oled_clear()
oled_set_cursor(48, 32)
oled_print("Bye!")
oled_show()

time.sleep_ms(500)
exit()
