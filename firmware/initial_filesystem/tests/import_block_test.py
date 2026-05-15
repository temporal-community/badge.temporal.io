"""
import_block_test.py — Module blocking verification.

Attempts to import 'machine' and 'esp32' — both must raise ImportError,
not crash or silently succeed. Verifies mpconfigport.h correctly excludes
raw hardware modules from the embedded runtime.
"""
import time

oled_clear()
oled_set_cursor(0, 10)
oled_print("Import Block Test")
oled_show()
time.sleep_ms(500)

machine_ok = False
esp32_ok = False

try:
    import machine
    machine_ok = False
except ImportError:
    machine_ok = True

try:
    import esp32
    esp32_ok = False
except ImportError:
    esp32_ok = True

oled_clear()
oled_set_cursor(0, 10)
oled_print("machine blocked:")
oled_set_cursor(56, 22)
oled_print("PASS" if machine_ok else "FAIL")
oled_set_cursor(0, 34)
oled_print("esp32 blocked:")
oled_set_cursor(56, 46)
oled_print("PASS" if esp32_ok else "FAIL")
oled_show()

time.sleep_ms(3000)
exit()
