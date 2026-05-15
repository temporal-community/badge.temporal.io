// Minimal machine backend for the Replay embed port.
// This keeps extmod/modmachine enabled while we progressively wire in
// full ports/esp32 machine functionality.

extern void mpy_hal_delay_ms(unsigned int ms);

static void mp_machine_idle(void) {
    mpy_hal_delay_ms(1);
}
