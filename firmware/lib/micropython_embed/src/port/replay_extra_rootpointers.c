// Included only in the host QSTR preprocessor pass (-DNO_QSTR).
// Registers vm root pointers for machine.I2S / machine.UART without pulling
// FreeRTOS headers (extmod/machine_i2s.c + ports/esp32/machine_i2s.c does).
// Firmware builds omit NO_QSTR; this file is empty and real registrations
// live in the included port sources.
#if defined(NO_QSTR)

#include "py/mpconfig.h"

struct _machine_i2s_obj_t;

#if MICROPY_PY_MACHINE_I2S
MP_REGISTER_ROOT_POINTER(struct _machine_i2s_obj_t *machine_i2s_obj[2]);
#endif

#if MICROPY_PY_MACHINE_UART
MP_REGISTER_ROOT_POINTER(void *machine_uart_objs[3]);
#endif

struct _machine_timer_obj_t;
MP_REGISTER_ROOT_POINTER(struct _machine_timer_obj_t *machine_timer_obj_head);

// Must match GPIO_PIN_COUNT / SOC_GPIO_PIN_COUNT for the badge target (ESP32-S3 = 49).
// Host QSTR cpp can resolve GPIO_PIN_COUNT differently than the firmware SDK; use a literal.
MP_REGISTER_ROOT_POINTER(mp_obj_t machine_pin_irq_handler[49]);

struct _esp_espnow_obj_t;
MP_REGISTER_ROOT_POINTER(struct _esp_espnow_obj_t *espnow_singleton);

struct _mp_bluetooth_nimble_root_pointers_t;
MP_REGISTER_ROOT_POINTER(struct _mp_bluetooth_nimble_root_pointers_t *bluetooth_nimble_root_pointers);
MP_REGISTER_ROOT_POINTER(mp_obj_t bluetooth);

#endif
