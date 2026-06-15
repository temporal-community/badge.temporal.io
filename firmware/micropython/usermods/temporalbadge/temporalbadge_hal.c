#include "temporalbadge_hal.h"
#include "temporalbadge_runtime.h"

int temporalbadge_hal_init(void) {
    return temporalbadge_runtime_init();
}

// OLED -- text

int temporalbadge_hal_oled_print(const char *msg) {
    return temporalbadge_runtime_oled_print(msg);
}

int temporalbadge_hal_oled_println(const char *msg) {
    return temporalbadge_runtime_oled_println(msg);
}

int temporalbadge_hal_oled_clear(int show) {
    return temporalbadge_runtime_oled_clear(show);
}

int temporalbadge_hal_oled_show(void) {
    return temporalbadge_runtime_oled_show();
}

int temporalbadge_hal_oled_set_cursor(int x, int y) {
    return temporalbadge_runtime_oled_set_cursor(x, y);
}

int temporalbadge_hal_oled_set_text_size(int size) {
    return temporalbadge_runtime_oled_set_text_size(size);
}

int temporalbadge_hal_oled_get_text_size(void) {
    return temporalbadge_runtime_oled_get_text_size();
}

int temporalbadge_hal_oled_invert(int invert) {
    return temporalbadge_runtime_oled_invert(invert);
}

// OLED -- text metrics

int temporalbadge_hal_oled_text_width(const char *text) {
    return temporalbadge_runtime_oled_text_width(text);
}

int temporalbadge_hal_oled_text_height(void) {
    return temporalbadge_runtime_oled_text_height();
}

// OLED -- fonts

int temporalbadge_hal_oled_set_font(const char *name) {
    return temporalbadge_runtime_oled_set_font(name);
}

const char *temporalbadge_hal_oled_get_fonts(void) {
    return temporalbadge_runtime_oled_get_fonts();
}

const char *temporalbadge_hal_oled_get_current_font(void) {
    return temporalbadge_runtime_oled_get_current_font();
}

// OLED -- framebuffer / pixel

int temporalbadge_hal_oled_set_pixel(int x, int y, int color) {
    return temporalbadge_runtime_oled_set_pixel(x, y, color);
}

int temporalbadge_hal_oled_get_pixel(int x, int y) {
    return temporalbadge_runtime_oled_get_pixel(x, y);
}

void temporalbadge_hal_oled_draw_box(int x, int y, int w, int h) {
    temporalbadge_runtime_oled_draw_box(x, y, w, h);
}

void temporalbadge_hal_oled_set_draw_color(int color) {
    temporalbadge_runtime_oled_set_draw_color(color);
}

const uint8_t *temporalbadge_hal_oled_get_framebuffer(int *w, int *h, int *buf_size) {
    return temporalbadge_runtime_oled_get_framebuffer(w, h, buf_size);
}

int temporalbadge_hal_oled_set_framebuffer(const uint8_t *data, size_t len) {
    return temporalbadge_runtime_oled_set_framebuffer(data, len);
}

void temporalbadge_hal_oled_get_framebuffer_size(int *w, int *h, int *buf_bytes) {
    temporalbadge_runtime_oled_get_framebuffer_size(w, h, buf_bytes);
}

// Native UI chrome helpers

int temporalbadge_hal_ui_header(const char *title, const char *right) {
    return temporalbadge_runtime_ui_header(title, right);
}

int temporalbadge_hal_ui_action_bar(const char *left_button,
                                    const char *left_label,
                                    const char *right_button,
                                    const char *right_label) {
    return temporalbadge_runtime_ui_action_bar(left_button, left_label,
                                               right_button, right_label);
}

int temporalbadge_hal_ui_chrome(const char *title,
                                const char *right,
                                const char *left_button,
                                const char *left_label,
                                const char *right_button,
                                const char *right_label) {
    return temporalbadge_runtime_ui_chrome(title, right, left_button,
                                           left_label, right_button,
                                           right_label);
}

int temporalbadge_hal_ui_inline_hint(int x, int y, const char *hint) {
    return temporalbadge_runtime_ui_inline_hint(x, y, hint);
}

int temporalbadge_hal_ui_inline_hint_right(int right_x, int y,
                                           const char *hint) {
    return temporalbadge_runtime_ui_inline_hint_right(right_x, y, hint);
}

int temporalbadge_hal_ui_measure_hint(const char *hint) {
    return temporalbadge_runtime_ui_measure_hint(hint);
}

int temporalbadge_hal_ui_grid_cell(int col, int row, const char *label,
                                   int selected, const char *icon_name) {
    return temporalbadge_runtime_ui_grid_cell(col, row, label,
                                               selected, icon_name);
}

int temporalbadge_hal_ui_grid_footer(const char *description) {
    return temporalbadge_runtime_ui_grid_footer(description);
}

int temporalbadge_hal_ui_list_row(int row, const char *label, int selected) {
    return temporalbadge_runtime_ui_list_row(row, label, selected);
}

int temporalbadge_hal_ui_list_rows_visible(void) {
    return temporalbadge_runtime_ui_list_rows_visible();
}

// Buttons

int temporalbadge_hal_button_state(int button_id) {
    return temporalbadge_runtime_button_state(button_id);
}

int temporalbadge_hal_button_pressed(int button_id) {
    return temporalbadge_runtime_button_pressed(button_id);
}

int temporalbadge_hal_button_held_ms(int button_id) {
    return temporalbadge_runtime_button_held_ms(button_id);
}

// Joystick

int temporalbadge_hal_joy_x(void) {
    return temporalbadge_runtime_joy_x();
}

int temporalbadge_hal_joy_y(void) {
    return temporalbadge_runtime_joy_y();
}

// Explicit HTTP helpers

const char *temporalbadge_hal_http_get(const char *url) {
    return temporalbadge_runtime_http_get(url);
}

const char *temporalbadge_hal_http_post(const char *url, const char *body) {
    return temporalbadge_runtime_http_post(url, body);
}

// LED matrix

int temporalbadge_hal_led_set_brightness(uint8_t brightness) {
    return temporalbadge_runtime_led_set_brightness((int)brightness);
}

int temporalbadge_hal_led_clear(void) {
    return temporalbadge_runtime_led_clear();
}

int temporalbadge_hal_led_fill(int brightness) {
    return temporalbadge_runtime_led_fill(brightness);
}

int temporalbadge_hal_led_set_pixel(int x, int y, int brightness) {
    return temporalbadge_runtime_led_set_pixel(x, y, brightness);
}

int temporalbadge_hal_led_get_pixel(int x, int y) {
    return temporalbadge_runtime_led_get_pixel(x, y);
}

int temporalbadge_hal_led_snapshot(uint8_t out[64]) {
    return temporalbadge_runtime_led_snapshot(out);
}

int temporalbadge_hal_led_show_image(const char *name) {
    return temporalbadge_runtime_led_show_image(name);
}

int temporalbadge_hal_led_set_frame(const uint8_t *rows, int brightness) {
    return temporalbadge_runtime_led_set_frame(rows, brightness);
}

int temporalbadge_hal_led_start_animation(const char *name, int interval_ms) {
    return temporalbadge_runtime_led_start_animation(name, interval_ms);
}

int temporalbadge_hal_led_stop_animation(void) {
    return temporalbadge_runtime_led_stop_animation();
}

// IMU

int temporalbadge_hal_imu_ready(void) {
    return temporalbadge_runtime_imu_ready();
}

float temporalbadge_hal_imu_tilt_x(void) {
    return temporalbadge_runtime_imu_tilt_x();
}

float temporalbadge_hal_imu_tilt_y(void) {
    return temporalbadge_runtime_imu_tilt_y();
}

float temporalbadge_hal_imu_accel_z(void) {
    return temporalbadge_runtime_imu_accel_z();
}

int temporalbadge_hal_imu_face_down(void) {
    return temporalbadge_runtime_imu_face_down();
}

int temporalbadge_hal_imu_motion(void) {
    return temporalbadge_runtime_imu_motion();
}

// Haptics

void temporalbadge_hal_haptic_pulse(int strength, int duration_ms, int freq_hz) {
    temporalbadge_runtime_haptic_pulse(strength, duration_ms, freq_hz);
}

int temporalbadge_hal_haptic_strength(void) {
    return temporalbadge_runtime_haptic_strength();
}

void temporalbadge_hal_haptic_set_strength(int value) {
    temporalbadge_runtime_haptic_set_strength(value);
}

void temporalbadge_hal_haptic_off(void) {
    temporalbadge_runtime_haptic_off();
}

void temporalbadge_hal_tone(int freq_hz, int duration_ms, int duty) {
    temporalbadge_runtime_tone(freq_hz, duration_ms, duty);
}

void temporalbadge_hal_no_tone(void) {
    temporalbadge_runtime_no_tone();
}

int temporalbadge_hal_tone_playing(void) {
    return temporalbadge_runtime_tone_playing();
}

// Mouse overlay

void temporalbadge_hal_mouse_overlay(int enable) {
    temporalbadge_runtime_mouse_overlay(enable);
}

void temporalbadge_hal_mouse_set_bitmap(const uint8_t *data, int w, int h) {
    temporalbadge_runtime_mouse_set_bitmap(data, w, h);
}

int temporalbadge_hal_mouse_x(void) {
    return temporalbadge_runtime_mouse_x();
}

int temporalbadge_hal_mouse_y(void) {
    return temporalbadge_runtime_mouse_y();
}

void temporalbadge_hal_mouse_set_pos(int x, int y) {
    temporalbadge_runtime_mouse_set_pos(x, y);
}

int temporalbadge_hal_mouse_clicked(void) {
    return temporalbadge_runtime_mouse_clicked();
}

void temporalbadge_hal_mouse_set_speed(int speed) {
    temporalbadge_runtime_mouse_set_speed(speed);
}

void temporalbadge_hal_mouse_set_mode(int absolute) {
    temporalbadge_runtime_mouse_set_mode(absolute);
}

// IR

int temporalbadge_hal_ir_send(int addr, int cmd) {
    return temporalbadge_runtime_ir_send(addr, cmd);
}

void temporalbadge_hal_ir_start(void) {
    temporalbadge_runtime_ir_start();
}

void temporalbadge_hal_ir_stop(void) {
    temporalbadge_runtime_ir_stop();
}

int temporalbadge_hal_ir_available(void) {
    return temporalbadge_runtime_ir_available();
}

int temporalbadge_hal_ir_read(int *addr_out, int *cmd_out) {
    return temporalbadge_runtime_ir_read(addr_out, cmd_out);
}

// IR multi-word

int temporalbadge_hal_ir_send_words(const uint32_t *words, size_t count) {
    return temporalbadge_runtime_ir_send_words(words, count);
}

int temporalbadge_hal_ir_read_words(uint32_t *out, size_t max_words,
                                     size_t *count_out) {
    return temporalbadge_runtime_ir_read_words(out, max_words, count_out);
}

void temporalbadge_hal_ir_flush(void) {
    temporalbadge_runtime_ir_flush();
}

int temporalbadge_hal_ir_tx_power(int percent) {
    return temporalbadge_runtime_ir_tx_power(percent);
}

// IR Playground

int temporalbadge_hal_ir_set_mode(const char *name) {
    return temporalbadge_runtime_ir_set_mode(name);
}

const char *temporalbadge_hal_ir_get_mode(void) {
    return temporalbadge_runtime_ir_get_mode();
}

int temporalbadge_hal_ir_nec_send(int addr, int cmd, int repeats) {
    return temporalbadge_runtime_ir_nec_send(addr, cmd, repeats);
}

int temporalbadge_hal_ir_nec_read(int *addr_out, int *cmd_out, int *repeat_out) {
    return temporalbadge_runtime_ir_nec_read(addr_out, cmd_out, repeat_out);
}

int temporalbadge_hal_ir_raw_capture(uint16_t *out_pairs, size_t max_pairs) {
    return temporalbadge_runtime_ir_raw_capture(out_pairs, max_pairs);
}

int temporalbadge_hal_ir_raw_send(const uint16_t *pairs,
                                   size_t          pair_count,
                                   uint32_t        carrier_hz) {
    return temporalbadge_runtime_ir_raw_send(pairs, pair_count, carrier_hz);
}

uint32_t temporalbadge_hal_ir_ms_since_tx(void) {
    return temporalbadge_runtime_ir_ms_since_tx();
}

uint32_t temporalbadge_hal_ir_ms_since_rx(void) {
    return temporalbadge_runtime_ir_ms_since_rx();
}

// Badge identity / boops

const char *temporalbadge_hal_my_uuid(void) {
    return temporalbadge_runtime_my_uuid();
}

const char *temporalbadge_hal_boops(void) {
    return temporalbadge_runtime_boops();
}

#if defined(BADGE_ENABLE_MP_DEV)
// Dev/test harness

const char *temporalbadge_hal_dev(int argc, const char **argv) {
    return temporalbadge_runtime_dev(argc, argv);
}
#endif
