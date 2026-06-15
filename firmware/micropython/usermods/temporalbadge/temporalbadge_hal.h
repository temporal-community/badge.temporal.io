#ifndef TEMPORALBADGE_HAL_H
#define TEMPORALBADGE_HAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int temporalbadge_hal_init(void);

// OLED -- text
int temporalbadge_hal_oled_print(const char *msg);
int temporalbadge_hal_oled_println(const char *msg);
int temporalbadge_hal_oled_clear(int show);
int temporalbadge_hal_oled_show(void);
int temporalbadge_hal_oled_set_cursor(int x, int y);
int temporalbadge_hal_oled_set_text_size(int size);
int temporalbadge_hal_oled_get_text_size(void);
int temporalbadge_hal_oled_invert(int invert);

// OLED -- text metrics
int temporalbadge_hal_oled_text_width(const char *text);
int temporalbadge_hal_oled_text_height(void);

// OLED -- fonts
int temporalbadge_hal_oled_set_font(const char *name);
const char *temporalbadge_hal_oled_get_fonts(void);
const char *temporalbadge_hal_oled_get_current_font(void);

// OLED -- framebuffer / pixel
int temporalbadge_hal_oled_set_pixel(int x, int y, int color);
int temporalbadge_hal_oled_get_pixel(int x, int y);
void temporalbadge_hal_oled_draw_box(int x, int y, int w, int h);
void temporalbadge_hal_oled_set_draw_color(int color);
const uint8_t *temporalbadge_hal_oled_get_framebuffer(int *w, int *h, int *buf_size);
int temporalbadge_hal_oled_set_framebuffer(const uint8_t *data, size_t len);
void temporalbadge_hal_oled_get_framebuffer_size(int *w, int *h, int *buf_bytes);

// Native UI chrome helpers
int temporalbadge_hal_ui_header(const char *title, const char *right);
int temporalbadge_hal_ui_action_bar(const char *left_button,
                                    const char *left_label,
                                    const char *right_button,
                                    const char *right_label);
int temporalbadge_hal_ui_chrome(const char *title,
                                const char *right,
                                const char *left_button,
                                const char *left_label,
                                const char *right_button,
                                const char *right_label);
int temporalbadge_hal_ui_inline_hint(int x, int y, const char *hint);
int temporalbadge_hal_ui_inline_hint_right(int right_x, int y,
                                           const char *hint);
int temporalbadge_hal_ui_measure_hint(const char *hint);

// Native 2x2 grid menu primitives.
int temporalbadge_hal_ui_grid_cell(int col, int row,
                                   const char *label, int selected,
                                   const char *icon_name);
int temporalbadge_hal_ui_grid_footer(const char *description);

// Native list-menu row.
int temporalbadge_hal_ui_list_row(int row, const char *label, int selected);
int temporalbadge_hal_ui_list_rows_visible(void);

// Buttons
int temporalbadge_hal_button_state(int button_id);
int temporalbadge_hal_button_pressed(int button_id);
int temporalbadge_hal_button_held_ms(int button_id);

// Joystick
int temporalbadge_hal_joy_x(void);
int temporalbadge_hal_joy_y(void);

// Explicit HTTP helpers
const char *temporalbadge_hal_http_get(const char *url);
const char *temporalbadge_hal_http_post(const char *url, const char *body);

// LED matrix
int temporalbadge_hal_led_set_brightness(uint8_t brightness);
int temporalbadge_hal_led_clear(void);
int temporalbadge_hal_led_fill(int brightness);
int temporalbadge_hal_led_set_pixel(int x, int y, int brightness);
int temporalbadge_hal_led_get_pixel(int x, int y);
// Snapshot the full 8×8 logical LED state from the matrix framebuffer.
// out must hold 64 bytes; filled row-major (out[y*8+x]).  Returns 1 on
// success, 0 if the matrix is not initialized.
int temporalbadge_hal_led_snapshot(uint8_t out[64]);
int temporalbadge_hal_led_show_image(const char *name);
int temporalbadge_hal_led_set_frame(const uint8_t *rows, int brightness);
int temporalbadge_hal_led_start_animation(const char *name, int interval_ms);
int temporalbadge_hal_led_stop_animation(void);

// IMU
int temporalbadge_hal_imu_ready(void);
float temporalbadge_hal_imu_tilt_x(void);
float temporalbadge_hal_imu_tilt_y(void);
float temporalbadge_hal_imu_accel_z(void);
int temporalbadge_hal_imu_face_down(void);
int temporalbadge_hal_imu_motion(void);

// Haptics
void temporalbadge_hal_haptic_pulse(int strength, int duration_ms, int freq_hz);
int temporalbadge_hal_haptic_strength(void);
void temporalbadge_hal_haptic_set_strength(int value);
void temporalbadge_hal_haptic_off(void);
void temporalbadge_hal_tone(int freq_hz, int duration_ms, int duty);
void temporalbadge_hal_no_tone(void);
int temporalbadge_hal_tone_playing(void);

// Mouse overlay
void temporalbadge_hal_mouse_overlay(int enable);
void temporalbadge_hal_mouse_set_bitmap(const uint8_t *data, int w, int h);
int  temporalbadge_hal_mouse_x(void);
int  temporalbadge_hal_mouse_y(void);
void temporalbadge_hal_mouse_set_pos(int x, int y);
int  temporalbadge_hal_mouse_clicked(void);
void temporalbadge_hal_mouse_set_speed(int speed);
void temporalbadge_hal_mouse_set_mode(int absolute);

// IR
int temporalbadge_hal_ir_send(int addr, int cmd);
void temporalbadge_hal_ir_start(void);
void temporalbadge_hal_ir_stop(void);
int temporalbadge_hal_ir_available(void);
int temporalbadge_hal_ir_read(int *addr_out, int *cmd_out);

// IR multi-word
int temporalbadge_hal_ir_send_words(const uint32_t *words, size_t count);
int temporalbadge_hal_ir_read_words(uint32_t *out, size_t max_words,
                                     size_t *count_out);
void temporalbadge_hal_ir_flush(void);
int  temporalbadge_hal_ir_tx_power(int percent);

// IR Playground
int         temporalbadge_hal_ir_set_mode(const char *name);
const char *temporalbadge_hal_ir_get_mode(void);
int temporalbadge_hal_ir_nec_send(int addr, int cmd, int repeats);
int temporalbadge_hal_ir_nec_read(int *addr_out,
                                   int *cmd_out,
                                   int *repeat_out);
int temporalbadge_hal_ir_raw_capture(uint16_t *out_pairs,
                                      size_t   max_pairs);
int temporalbadge_hal_ir_raw_send(const uint16_t *pairs,
                                   size_t          pair_count,
                                   uint32_t        carrier_hz);
uint32_t temporalbadge_hal_ir_ms_since_tx(void);
uint32_t temporalbadge_hal_ir_ms_since_rx(void);

// Badge identity / boops
const char *temporalbadge_hal_my_uuid(void);
const char *temporalbadge_hal_boops(void);

#if defined(BADGE_ENABLE_MP_DEV)
// Dev/test harness
const char *temporalbadge_hal_dev(int argc, const char **argv);
#endif

#ifdef __cplusplus
}
#endif

#endif
