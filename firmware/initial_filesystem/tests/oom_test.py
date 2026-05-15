"""
oom_test.py — OOM recovery verification.

Allocates until MemoryError, then relies on the runtime to catch
the exception and display "App crashed" before returning to the menu.
"""
import time

oled_clear()
oled_set_cursor(32, 20)
oled_print("OOM Test")
oled_set_cursor(8, 34)
oled_print("Allocating...")
oled_show()

time.sleep_ms(500)

a = []
while True:
    a.append(b"x" * 100)
