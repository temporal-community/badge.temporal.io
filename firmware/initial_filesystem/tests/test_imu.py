import time
print("=== 4. IMU ===")
oled_clear(True)
oled_set_cursor(0, 0)
oled_println("4. IMU")

r = imu_ready()
print("  ready=" + str(r))
oled_println("ready:" + str(r))

if r:
    tx = imu_tilt_x()
    ty = imu_tilt_y()
    az = imu_accel_z()
    fd = imu_face_down()
    mo = imu_motion()
    print("  tilt=" + str(int(tx)) + "," + str(int(ty)) + " z=" + str(int(az)))
    print("  face_down=" + str(fd) + " motion=" + str(mo))
    oled_println("X:" + str(int(tx)) + " Y:" + str(int(ty)))
    oled_println("Z:" + str(int(az)))
    oled_println("fd:" + str(fd) + " mo:" + str(mo))
    time.sleep_ms(1200)

    oled_clear(True)
    oled_set_cursor(0, 0)
    oled_println("Tilt Ball (3s)")
    i = 0
    while i < 60:
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
        m = [0, 0, 0, 0, 0, 0, 0, 0]
        m[py] = 1 << (7 - px)
        led_set_frame(m, 60)
        time.sleep_ms(50)
        i = i + 1
    led_clear()
    print("  tilt ball done")
else:
    oled_println("(no IMU)")

time.sleep_ms(300)
