"""
input_test.py — Live input tester for Temporal Badge.

Shows real-time state of all badge inputs:
  Buttons: X Y B A
  Joystick: raw ADC values (0-4095)
  IMU: tilt X/Y, face-down state

Press all four buttons to exit back to the menu.
"""
import time

while True:
    y = button_pressed(BTN_TRIANGLE)
    a = button_pressed(BTN_CROSS)
    x = button_pressed(BTN_SQUARE)
    b = button_pressed(BTN_CIRCLE)
    jx = joy_x()
    jy = joy_y()

    oled_clear()

    oled_set_cursor(0, 7)
    oled_print("BTN:")
    oled_set_cursor(28, 5)
    oled_print("Y" if y else ".")
    oled_set_cursor(42, 5)
    oled_print("A" if a else ".")
    oled_set_cursor(56, 5)
    oled_print("X" if x else ".")
    oled_set_cursor(70, 5)
    oled_print("B" if b else ".")

    oled_set_cursor(0, 17)
    oled_print("Joystick: X:" + str(jx))
    oled_set_cursor(0, 27)
    oled_print("Joystick: Y:" + str(jy))

    if imu_ready():
        oled_set_cursor(0, 38)
        oled_print("Tilt:  X:" + str(int(imu_tilt_x())) + " Y:" + str(int(imu_tilt_y())))
    else:
        oled_set_cursor(0, 38)
        oled_print("IMU: N/A")

    oled_set_cursor(0, 54)
    oled_print("ALL BUTTONS = exit")

    oled_show()

    if b and x and y and a:
        time.sleep_ms(200)
        exit()

    time.sleep_ms(80)
