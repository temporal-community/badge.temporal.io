"""
api_test.py — Interactive test menu for all badge MicroPython API functions.
Use the joystick to move the cursor and BTN_CONFIRM to select a test.
BTN_BACK returns to the menu from a running test.
All badge functions are in global scope via `from badge import *`.
"""
import time
import gc

LINE_H = 8
VISIBLE_LINES = 6

def wait_or_skip(ms=2000):
    deadline = time.ticks_add(time.ticks_ms(), ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        if button(BTN_UP):
            time.sleep_ms(150)
            return True
        if button(BTN_DOWN):
            return False
        time.sleep_ms(20)
    return True

def header(title):
    gc.collect()
    oled_clear(True)
    oled_set_cursor(0, 0)
    oled_println(title)
    print("\n=== " + title + " ===")

def report(msg):
    oled_println(msg)
    print("  " + msg)

def at(x, y, msg):
    oled_set_cursor(x, y)
    oled_print(msg)
    oled_show()

# ── Individual tests ─────────────────────────────────────────────────────────

def test_oled():
    header("1. OLED")
    report("print works")
    w, h, sz = oled_get_framebuffer_size()
    report(str(w) + "x" + str(h) + " " + str(sz) + "B")

    oled_set_text_size(2)
    ts = oled_get_text_size()
    report("text_size=" + str(ts))
    oled_set_text_size(4)

    font = oled_get_current_font()
    report("font: " + font)

    fonts_csv = oled_get_fonts()
    report("fonts: " + str(len(fonts_csv.split(","))))

    fb = oled_get_framebuffer()
    report("fb len=" + str(len(fb)))

    oled_set_pixel(64, 32, 1)
    px = oled_get_pixel(64, 32)
    report("pixel(64,32)=" + str(px))

    oled_invert(True)
    time.sleep_ms(300)
    oled_invert(False)
    report("invert OK")

    wait_or_skip(1500)

def test_buttons():
    header("2. Buttons")
    report("Press each button")
    report("UP to skip")

    pressed = [False, False, False, False]
    names = ["RIGHT", "DOWN", "LEFT", "UP"]

    for _ in range(150):
        for i in range(4):
            if button_pressed(i):
                pressed[i] = True
                ms = button_held_ms(i)
                report(names[i] + " (" + str(ms) + "ms)")
        if all(pressed):
            break
        if button(BTN_UP):
            break
        time.sleep_ms(30)

    report("done: " + str(sum(pressed)) + "/4")
    wait_or_skip(1000)

def test_joystick():
    header("3. Joystick")
    report("Move stick around")
    report("UP to skip")

    x_min = 9999
    x_max = 0
    y_min = 9999
    y_max = 0

    for _ in range(80):
        x = joy_x()
        y = joy_y()
        if x < x_min:
            x_min = x
        if x > x_max:
            x_max = x
        if y < y_min:
            y_min = y
        if y > y_max:
            y_max = y
        at(0, 40, "X:" + str(x) + "  Y:" + str(y) + "      ")
        if button(BTN_UP):
            break
        time.sleep_ms(50)

    header("3. Joystick")
    report("X: " + str(x_min) + "-" + str(x_max))
    report("Y: " + str(y_min) + "-" + str(y_max))
    wait_or_skip(500)

def test_led():
    header("4a. LED Brightness")
    report("ramp up")
    for b in range(0, 60, 5):
        led_brightness(b)
        led_fill()
        time.sleep_ms(60)
    report("ramp down")
    for b in range(60, 0, -5):
        led_brightness(b)
        led_fill()
        time.sleep_ms(60)
    led_clear()
    led_brightness(20)
    if not wait_or_skip(500):
        return

    header("4b. LED Pixels")
    report("diagonal")
    led_clear()
    for i in range(8):
        led_set_pixel(i, i, 80)
        time.sleep_ms(60)
    for i in range(8):
        led_set_pixel(7 - i, i, 40)
        time.sleep_ms(60)

    val = led_get_pixel(3, 3)
    report("get(3,3)=" + str(val))
    val2 = led_get_pixel(4, 3)
    report("get(4,3)=" + str(val2))
    time.sleep_ms(500)
    if not wait_or_skip(500):
        return

    header("4c. LED Images")
    images = [IMG_SMILEY, IMG_HEART, IMG_ARROW_UP, IMG_ARROW_DOWN, IMG_X_MARK, IMG_DOT]
    for name in images:
        led_show_image(name)
        at(0, 14, name + "           ")
        print("  " + name)
        time.sleep_ms(500)
    report(str(len(images)) + " images OK")
    if not wait_or_skip(500):
        return

    header("4d. LED Frames")
    report("checkerboard")
    led_set_frame([170, 85, 170, 85, 170, 85, 170, 85], 40)
    time.sleep_ms(800)
    report("border")
    led_set_frame([255, 129, 129, 129, 129, 129, 129, 255], 50)
    time.sleep_ms(800)
    report("custom X")
    led_set_frame([
        0b10000001, 0b01000010, 0b00100100, 0b00011000,
        0b00011000, 0b00100100, 0b01000010, 0b10000001,
    ], 60)
    time.sleep_ms(800)
    if not wait_or_skip(500):
        return

    header("4e. LED Animations")
    anims = [
        (ANIM_SPINNER, 80, "spinner"),
        (ANIM_BLINK_SMILEY, 400, "blink_smiley"),
        (ANIM_PULSE_HEART, 100, "pulse_heart"),
    ]
    for name, ival, label in anims:
        led_start_animation(name, ival)
        at(0, 14, label + " @" + str(ival) + "ms    ")
        print("  " + label + " @" + str(ival) + "ms")
        time.sleep_ms(2000)
    led_stop_animation()
    led_clear()
    report("stopped")
    wait_or_skip(500)

def test_imu():
    header("5. IMU")
    ready = imu_ready()
    report("ready: " + str(ready))

    if ready:
        report("Tilt the badge!")
        for _ in range(80):
            tx = imu_tilt_x()
            ty = imu_tilt_y()
            az = imu_accel_z()
            fd = imu_face_down()
            at(0, 35, "X:" + str(int(tx)) + " Y:" + str(int(ty)) + "      ")
            at(0, 46, "Z:" + str(int(az)) + " fd:" + str(fd) + "     ")
            if imu_motion():
                at(0, 52, "MOTION!")
                print("  MOTION detected!")
            if button(BTN_UP):
                break
            time.sleep_ms(50)
        header("5. IMU")
        report("tilt=(" + str(int(imu_tilt_x())) + "," + str(int(imu_tilt_y())) + ")")
    else:
        report("(no IMU)")
    wait_or_skip(500)

def test_tilt_ball():
    if not imu_ready():
        header("6. Tilt Ball")
        report("(no IMU)")
        wait_or_skip(500)
        return

    header("6. Tilt Ball")
    report("Tilt to move dot")
    report("UP to skip")

    i = 0
    while i < 200:
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
        if button(BTN_UP):
            break
        time.sleep_ms(30)
        i = i + 1

    led_clear()
    report("done")
    wait_or_skip(300)

def test_haptics():
    header("7. Haptics")
    s = haptic_strength()
    report("strength: " + str(s))

    report("default pulse")
    haptic_pulse()
    time.sleep_ms(400)

    report("strong (105,80)")
    haptic_pulse(105, 80)
    time.sleep_ms(400)

    report("gentle (40,20)")
    haptic_pulse(40, 20)
    time.sleep_ms(400)

    haptic_strength(100)
    rb = haptic_strength()
    report("set 100 -> " + str(rb))
    haptic_strength(s)

    haptic_off()
    report("haptic_off() OK")

    print("  medium (70,50,200)")
    haptic_pulse(70, 50, 200)
    time.sleep_ms(400)

    wait_or_skip(1000)

def test_tone():
    header("8. Coil Tone")
    report("C major scale")

    notes = [262, 294, 330, 349, 392, 440, 494, 523]
    for freq in notes:
        tone(freq, 150)
        at(0, 21, str(freq) + " Hz    ")
        time.sleep_ms(200)

    report("descending")
    for freq in [523, 494, 440, 392, 349, 330, 294, 262]:
        tone(freq, 100)
        time.sleep_ms(130)

    time.sleep_ms(200)
    report("timed off: " + str(not tone_playing()))

    report("440Hz hold")
    tone(440)
    time.sleep_ms(800)
    report("playing=" + str(tone_playing()))
    no_tone()
    report("no_tone -> " + str(not tone_playing()))

    header("8b. Tone chirp")
    report("chirp up")
    for f in range(200, 2000, 100):
        tone(f, 30)
        time.sleep_ms(35)
    time.sleep_ms(100)
    report("chirp done")
    wait_or_skip(1000)

def test_ir():
    header("9. IR")
    ir_start()
    report("ir_start()")

    a = ir_available()
    report("available=" + str(a))

    ir_send(0x42, 0x01)
    report("sent 0x42,0x01")

    time.sleep_ms(200)
    report("available=" + str(ir_available()))

    f = ir_read()
    report("read=" + str(f))

    ir_stop()
    report("ir_stop()")
    wait_or_skip(1000)

def test_heap():
    header("10. Heap Stats")
    gc.collect()
    free = gc.mem_free()
    alloc = gc.mem_alloc()
    total = free + alloc
    report("total: " + str(total // 1024) + "KB")
    report("free:  " + str(free // 1024) + "KB")
    report("used:  " + str(alloc // 1024) + "KB")
    report(str(free * 100 // total) + "% free")
    wait_or_skip(2000)

def test_mouse():
    header("11. Mouse Demo")
    report("Move cursor around!")
    report("RIGHT = click")
    mouse_overlay(True)

    clicks = 0
    for _ in range(300):
        mx = mouse_x()
        my = mouse_y()
        oled_set_cursor(0, 35)
        oled_print("pos: " + str(mx) + "," + str(my) + "     ")
        oled_set_cursor(0, 46)
        oled_print("clicks: " + str(clicks) + "     ")
        oled_show()
        btn = mouse_clicked()
        if btn >= 0:
            clicks += 1
            if btn == BTN_LEFT:
                break
        time.sleep_ms(30)

    mouse_overlay(False)
    report("done, " + str(clicks) + " clicks")
    wait_or_skip(500)

# ── Menu system ──────────────────────────────────────────────────────────────

TESTS = [
    ("OLED Display",  test_oled),
    ("Buttons",       test_buttons),
    ("Joystick",      test_joystick),
    ("LED Matrix",    test_led),
    ("IMU",           test_imu),
    ("Tilt Ball",     test_tilt_ball),
    ("Haptics",       test_haptics),
    ("Coil Tone",     test_tone),
    ("IR",            test_ir),
    ("Heap Stats",    test_heap),
    ("Mouse Demo",    test_mouse),
    ("Run All",       None),
]

def draw_menu(sel, scroll):
    oled_clear()
    oled_set_cursor(0, 0)
    oled_print("API Tests")

    oled_set_cursor(80, 0)
    oled_print(str(sel + 1) + "/" + str(len(TESTS)))

    for i in range(VISIBLE_LINES):
        idx = scroll + i
        if idx >= len(TESTS):
            break
        y = 12 + i * LINE_H
        oled_set_cursor(2, y + 1)
        oled_print(TESTS[idx][0])
        if idx == sel:
            oled_set_draw_color(2)
            oled_draw_box(0, y, 128, LINE_H)
            oled_set_draw_color(1)

    oled_show()

def run_all():
    for name, fn in TESTS:
        if fn is not None:
            print("\n--- Running: " + name + " ---")
            fn()
    led_clear()
    haptic_off()
    no_tone()
    header("ALL TESTS DONE")
    report("All tests passed!")
    haptic_pulse(200, 100)
    led_show_image(IMG_SMILEY)
    time.sleep_ms(2000)
    led_clear()

def menu():
    mouse_set_mode(MOUSE_RELATIVE)
    mouse_set_speed(6)
    mouse_overlay(True)
    mouse_set_pos(64, 32)
    sel = 0
    scroll = 0

    while True:
        if sel < scroll:
            scroll = sel
        if sel >= scroll + VISIBLE_LINES:
            scroll = sel - VISIBLE_LINES + 1

        draw_menu(sel, scroll)

        mx = mouse_x()
        my = mouse_y()
        if mx >= 2:
            row = (my - 12) // LINE_H
            if 0 <= row < VISIBLE_LINES:
                candidate = scroll + row
                if 0 <= candidate < len(TESTS):
                    sel = candidate

        btn = mouse_clicked()
        if btn == BTN_CONFIRM:
            mouse_overlay(False)
            name, fn = TESTS[sel]
            if fn is None:
                run_all()
            else:
                print("\n--- Running: " + name + " ---")
                fn()
            led_clear()
            haptic_off()
            no_tone()
            mouse_overlay(True)

        if btn == BTN_BACK:
            break

        if btn == BTN_UP:
            if sel > 0:
                sel -= 1

        if btn == BTN_DOWN:
            if sel < len(TESTS) - 1:
                sel += 1

        time.sleep_ms(30)

    mouse_overlay(False)
    oled_clear(True)
    print("Menu exited.")

# ── Entry point ──────────────────────────────────────────────────────────────

menu()
