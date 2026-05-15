# Test imports for enabled MicroPython modules.
# Keep this list updated whenever modules change in mpconfigport.h.

import time

oled_clear()
oled_set_cursor(0, 0)
oled_print("Import Test")
oled_show()

MODULES = [
    "sys",
    "os",
    "time",
    "random",
    "math",
    "cmath",
    "struct",
    "array",
    "binascii",
    "json",
    "collections",
    "errno",
    "gc",
    "io",
    "micropython",
    "network",
    "uctypes",
    "badge",
]

passed = 0
failed = []
for name in MODULES:
    try:
        __import__(name)
        print("OK:", name)
        passed += 1
    except Exception as exc:
        print("FAIL:", name, "-", exc)
        failed.append(name)

oled_clear()
oled_set_cursor(0, 0)
oled_print("Import Test")
oled_set_cursor(0, 14)
oled_print("Passed: " + str(passed) + "/" + str(len(MODULES)))

if failed:
    oled_set_cursor(0, 28)
    oled_print("Failed:")
    y = 40
    for name in failed[:3]:
        oled_set_cursor(4, y)
        oled_print(name)
        y += 10
    print("Missing modules:", ", ".join(failed))
else:
    oled_set_cursor(0, 28)
    oled_print("All imports OK!")
    print("All module imports passed.")

oled_show()

time.sleep_ms(3000)
exit()
