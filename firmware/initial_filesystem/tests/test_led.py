import time
print("=== 3. LED Matrix ===")
oled_clear(True)
oled_set_cursor(0, 0)
oled_println("3. LED Matrix")

led_brightness(20)
led_fill()
oled_println("fill")
print("  fill")
time.sleep_ms(400)

led_clear()
oled_println("clear")
print("  clear")
time.sleep_ms(300)

oled_println("diagonal")
print("  diagonal")
for i in range(8):
    led_set_pixel(i, i, 80)
time.sleep_ms(600)

v = led_get_pixel(3, 3)
print("  get_pixel(3,3)=" + str(v))

oled_println("smiley")
print("  smiley")
led_show_image(IMG_SMILEY)
time.sleep_ms(600)

oled_println("heart")
print("  heart")
led_show_image(IMG_HEART)
time.sleep_ms(600)

oled_println("set_frame")
print("  checkerboard")
led_set_frame([170, 85, 170, 85, 170, 85, 170, 85], 40)
time.sleep_ms(800)

oled_println("spinner anim")
print("  spinner")
led_start_animation(ANIM_SPINNER, 80)
time.sleep_ms(1500)

oled_println("pulse_heart")
print("  pulse_heart")
led_start_animation(ANIM_PULSE_HEART, 100)
time.sleep_ms(1500)

led_stop_animation()
led_clear()
print("  done")
time.sleep_ms(300)
