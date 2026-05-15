// MicroPython HAL header for Replay embed on ESP32 (Arduino core).

#include <stddef.h>
#include <stdint.h>
#include "py/obj.h"
#include "py/ringbuf.h"
#include "esp_err.h"

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
#endif

// Dupterm / os.dupterm_notify() push into this buffer; UART/serial drain it in mp_hal_stdio_*.
extern ringbuf_t stdin_ringbuf;

#ifndef mp_hal_set_interrupt_char
void mp_hal_set_interrupt_char(int c);
#endif

void mp_hal_get_random(size_t n, uint8_t *buf);
uint32_t replay_random_seed_init(void);

#ifndef mp_hal_ticks_cpu
#define mp_hal_ticks_cpu() (mp_hal_ticks_us())
#endif

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)

#if !CONFIG_FREERTOS_UNICORE
#define MP_TASK_COREID (1)
#else
#define MP_TASK_COREID (0)
#endif

void mp_hal_wake_main_task(void);
void mp_hal_wake_main_task_from_isr(void);

#define MP_HAL_PIN_FMT "%u"
#define mp_hal_pin_obj_t gpio_num_t
mp_hal_pin_obj_t machine_pin_get_id(mp_obj_t pin_in);
#define mp_hal_get_pin_obj(o) machine_pin_get_id(o)
#define mp_hal_pin_name(p) (p)
static inline void mp_hal_pin_input(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}
static inline void mp_hal_pin_output(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
}
static inline void mp_hal_pin_open_drain(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}
static inline void mp_hal_pin_od_low(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 0);
}
static inline void mp_hal_pin_od_high(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 1);
}
static inline int mp_hal_pin_read(mp_hal_pin_obj_t pin) {
    return gpio_get_level(pin);
}
static inline int mp_hal_pin_read_output(mp_hal_pin_obj_t pin) {
    #if defined(GPIO_OUT1_REG)
    return pin < 32
        ? (*(uint32_t *)GPIO_OUT_REG >> pin) & 1
        : (*(uint32_t *)GPIO_OUT1_REG >> (pin - 32)) & 1;
    #else
    return (*(uint32_t *)GPIO_OUT_REG >> pin) & 1;
    #endif
}
static inline void mp_hal_pin_write(mp_hal_pin_obj_t pin, int v) {
    gpio_set_level(pin, v);
}
#define mp_hal_pin_low(pin) mp_hal_pin_write((pin), 0)
#define mp_hal_pin_high(pin) mp_hal_pin_write((pin), 1)

#ifndef mp_hal_delay_us_fast
#define mp_hal_delay_us_fast(us) mp_hal_delay_us(us)
#endif

#else

typedef void *mp_hal_pin_obj_t;
int machine_pin_get_id(mp_obj_t pin_in);

#endif // ESP32

#ifndef check_esp_err
#define check_esp_err(code) ((void)(code))
#endif
