import time
print("=== 2. Buttons & Joystick ===")
oled_clear(True)
oled_set_cursor(0, 0)
oled_println("2. Buttons+Joy")

n = ["RIGHT", "DOWN", "LEFT", "UP"]
for i in range(4):
    s = button(i)
    p = button_pressed(i)
    m = button_held_ms(i)
    print("  " + n[i] + ": " + str(s) + " edge=" + str(p) + " held=" + str(m))

x = joy_x()
y = joy_y()
oled_println("J:" + str(x) + "," + str(y))
print("  joy: " + str(x) + "," + str(y))
oled_println("OK")
time.sleep_ms(1000)
