"""
gc_bench.py — GC pause and gc_collect() pre-emption benchmark.

Two tests:
  1. GC pause: allocate short-lived objects in a tight loop. Each iteration
     creates a bytearray that immediately becomes garbage, so the GC must
     collect periodically. Measure allocation time per iteration; any alloc
     that takes >= 5ms indicates a GC cycle. Expected max: <= 20ms.
  2. gc_collect() pre-emption: call gc.collect(), then run display ops;
     compare vs. baseline (no explicit collect). Delta should be <= 5ms.
"""
import time
import gc

oled_clear()
oled_set_cursor(32, 20)
oled_print("GC Bench")
oled_set_cursor(16, 34)
oled_print("Running...")
oled_show()

# ── Test 1: GC pause ──────────────────────────────────────────────────────────

ITERS = 2000
max_pause = 0
gc_count = 0

for i in range(ITERS):
    t0 = time.ticks_ms()
    _ = bytearray(512)
    t1 = time.ticks_ms()
    elapsed = time.ticks_diff(t1, t0)
    if elapsed >= 5:
        if elapsed > max_pause:
            max_pause = elapsed
        gc_count += 1

sc007 = max_pause <= 20
print("GC_BENCH gc_count=" + str(gc_count) + " max_pause_ms=" + str(max_pause) + " SC007=" + ("PASS" if sc007 else "FAIL"))

oled_clear()
oled_set_cursor(0, 10)
oled_print("GC pauses: " + str(gc_count))
oled_set_cursor(0, 22)
oled_print("Max: " + str(max_pause) + "ms")
oled_set_cursor(16, 34)
oled_print("SC-007 " + ("PASS" if sc007 else "FAIL"))
oled_show()

time.sleep_ms(2500)

# ── Test 2: gc_collect() pre-emption ─────────────────────────────────────────

t0 = time.ticks_ms()
oled_clear()
oled_set_cursor(0, 0)
oled_print("baseline")
oled_show()
baseline_ms = time.ticks_diff(time.ticks_ms(), t0)

gc.collect()
t0 = time.ticks_ms()
oled_clear()
oled_set_cursor(0, 0)
oled_print("post-gc")
oled_show()
gc_ms = time.ticks_diff(time.ticks_ms(), t0)

delta = abs(gc_ms - baseline_ms)
preempt_pass = delta <= 5
print("GC_BENCH baseline_ms=" + str(baseline_ms) + " gc_ms=" + str(gc_ms) + " delta=" + str(delta) + " preempt=" + ("PASS" if preempt_pass else "FAIL"))

oled_clear()
oled_set_cursor(0, 10)
oled_print("Base: " + str(baseline_ms) + "ms")
oled_set_cursor(0, 22)
oled_print("GC:   " + str(gc_ms) + "ms")
oled_set_cursor(0, 34)
oled_print("Delt: " + str(delta) + "ms")
oled_set_cursor(48, 48)
oled_print("PASS" if preempt_pass else "FAIL")
oled_show()

time.sleep_ms(3000)
exit()
