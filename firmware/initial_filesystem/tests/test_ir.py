import time
print("=== 6. IR ===")
oled_clear(True)
oled_set_cursor(0, 0)
oled_println("6. IR")

ir_start()
print("  ir_start()")
oled_println("listening")

a = ir_available()
print("  available=" + str(a))

ir_send(0x42, 0x01)
print("  ir_send(0x42,0x01)")
oled_println("sent 0x42,0x01")

time.sleep_ms(200)
a = ir_available()
print("  available=" + str(a))

f = ir_read()
print("  ir_read()=" + str(f))

ir_stop()
print("  ir_stop()")
oled_println("stopped OK")
time.sleep_ms(500)
