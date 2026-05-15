// Included from extmod/modmachine.c during host QSTR generation only.
// Firmware builds use `MICROPY_PY_MACHINE_INCLUDEFILE "ports/esp32/modmachine.c"`.

extern void mpy_hal_delay_ms(unsigned int ms);

#define MICROPY_PY_MACHINE_EXTRA_GLOBALS

static void mp_machine_idle(void) {
    mpy_hal_delay_ms(1);
}
