"""
tilt_ball.py — Tilt the badge to move a dot on the LED matrix.

Uses the IMU accelerometer to map tilt angle to an LED position.
The dot follows gravity — tilt left and it rolls left, etc.

B exits.
"""
import time

if not imu_ready():
    oled_clear()
    oled_set_cursor(0, 20)
    oled_print("Tilt Ball")
    oled_set_cursor(0, 34)
    oled_print("No IMU detected")
    oled_show()
    time.sleep_ms(2000)
    exit()

oled_clear()
oled_set_cursor(0, 10)
oled_print("Tilt Ball")
oled_set_cursor(0, 24)
oled_print("Tilt badge to")
oled_set_cursor(0, 36)
oled_print("move the dot!")
oled_set_cursor(0, 52)
oled_print("B = exit")
oled_show()
time.sleep_ms(1500)

led_brightness(40)

while True:
    if button_pressed(BTN_BACK):
        break

    tx = imu_tilt_x()
    ty = imu_tilt_y()

    px = int((tx + 1000) * 7 / 2000)
    py = int((ty + 1000) * 7 / 2000)
    if px < 0:
        px = 0
    if px > 7:
        px = 7
    if py < 0:
        py = 0
    if py > 7:
        py = 7

    mask = [0, 0, 0, 0, 0, 0, 0, 0]
    mask[py] = 1 << (7 - px)
    led_set_frame(mask, 60)

    oled_clear()
    oled_set_cursor(0, 0)
    oled_print("Tilt Ball")
    oled_set_cursor(0, 14)
    oled_print("X:" + str(int(tx)) + " Y:" + str(int(ty)))
    oled_set_cursor(0, 28)
    oled_print("LED:" + str(px) + "," + str(py))
    oled_set_cursor(0, 52)
    oled_print("B = exit")
    oled_show()

    time.sleep_ms(30)

led_clear()
oled_clear(True)
exit()
