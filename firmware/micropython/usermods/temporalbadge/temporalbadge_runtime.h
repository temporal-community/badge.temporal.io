#ifndef TEMPORALBADGE_RUNTIME_H
#define TEMPORALBADGE_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int temporalbadge_runtime_init(void);

// OLED -- text
int temporalbadge_runtime_oled_print(const char *msg);
int temporalbadge_runtime_oled_println(const char *msg);
int temporalbadge_runtime_oled_clear(int show);
int temporalbadge_runtime_oled_show(void);
int temporalbadge_runtime_oled_set_cursor(int x, int y);
int temporalbadge_runtime_oled_set_text_size(int size);
int temporalbadge_runtime_oled_get_text_size(void);
int temporalbadge_runtime_oled_invert(int invert);

// OLED -- text metrics
int temporalbadge_runtime_oled_text_width(const char *text);
int temporalbadge_runtime_oled_text_height(void);

// OLED -- fonts
int temporalbadge_runtime_oled_set_font(const char *name);
const char *temporalbadge_runtime_oled_get_fonts(void);
const char *temporalbadge_runtime_oled_get_current_font(void);

// OLED -- framebuffer / pixel
int temporalbadge_runtime_oled_set_pixel(int x, int y, int color);
int temporalbadge_runtime_oled_get_pixel(int x, int y);
void temporalbadge_runtime_oled_draw_box(int x, int y, int w, int h);
void temporalbadge_runtime_oled_set_draw_color(int color);
const uint8_t *temporalbadge_runtime_oled_get_framebuffer(int *w, int *h, int *buf_size);
int temporalbadge_runtime_oled_set_framebuffer(const uint8_t *data, size_t len);
void temporalbadge_runtime_oled_get_framebuffer_size(int *w, int *h, int *buf_bytes);

// Native UI chrome helpers
int temporalbadge_runtime_ui_header(const char *title, const char *right);
int temporalbadge_runtime_ui_action_bar(const char *left_button,
                                        const char *left_label,
                                        const char *right_button,
                                        const char *right_label);
int temporalbadge_runtime_ui_chrome(const char *title,
                                    const char *right,
                                    const char *left_button,
                                    const char *left_label,
                                    const char *right_button,
                                    const char *right_label);
int temporalbadge_runtime_ui_inline_hint(int x, int y, const char *hint);
int temporalbadge_runtime_ui_inline_hint_right(int right_x, int y,
                                               const char *hint);
int temporalbadge_runtime_ui_measure_hint(const char *hint);

// Native 2x2 grid menu primitives — render exactly like the home grid.
int temporalbadge_runtime_ui_grid_cell(int col, int row,
                                        const char *label, int selected,
                                        const char *icon_name);
int temporalbadge_runtime_ui_grid_footer(const char *description);

// Native list-menu row + visible-row count.
int temporalbadge_runtime_ui_list_row(int row, const char *label,
                                       int selected);
int temporalbadge_runtime_ui_list_rows_visible(void);

// Buttons
int temporalbadge_runtime_button_state(int button_id);
int temporalbadge_runtime_button_pressed(int button_id);
int temporalbadge_runtime_button_held_ms(int button_id);

// Joystick
int temporalbadge_runtime_joy_x(void);
int temporalbadge_runtime_joy_y(void);

// Explicit HTTP helpers. These may bring WiFi up only when called.
const char *temporalbadge_runtime_http_get(const char *url);
const char *temporalbadge_runtime_http_post(const char *url, const char *body);

// LED matrix
int temporalbadge_runtime_led_set_brightness(int brightness);
int temporalbadge_runtime_led_clear(void);
int temporalbadge_runtime_led_fill(int brightness);
int temporalbadge_runtime_led_set_pixel(int x, int y, int brightness);
int temporalbadge_runtime_led_get_pixel(int x, int y);
int temporalbadge_runtime_led_show_image(const char *name);
int temporalbadge_runtime_led_set_frame(const uint8_t *rows, int brightness);
int temporalbadge_runtime_led_start_animation(const char *name, int interval_ms);
int temporalbadge_runtime_led_stop_animation(void);

// Background matrix app host
int temporalbadge_runtime_matrix_host_apply_brightness(int brightness);
void temporalbadge_runtime_matrix_host_set_active(int active);
int temporalbadge_runtime_matrix_default_interval_ms(void);
int temporalbadge_runtime_matrix_default_brightness(void);

// IMU
int temporalbadge_runtime_imu_ready(void);
float temporalbadge_runtime_imu_tilt_x(void);
float temporalbadge_runtime_imu_tilt_y(void);
float temporalbadge_runtime_imu_accel_z(void);
int temporalbadge_runtime_imu_face_down(void);
int temporalbadge_runtime_imu_motion(void);

// Haptics
void temporalbadge_runtime_haptic_pulse(int strength, int duration_ms, int freq_hz);
int temporalbadge_runtime_haptic_strength(void);
void temporalbadge_runtime_haptic_set_strength(int value);
void temporalbadge_runtime_haptic_off(void);
void temporalbadge_runtime_tone(int freq_hz, int duration_ms, int duty);
void temporalbadge_runtime_no_tone(void);
int temporalbadge_runtime_tone_playing(void);

// Mouse overlay
void temporalbadge_runtime_mouse_overlay(int enable);
void temporalbadge_runtime_mouse_set_bitmap(const uint8_t *data, int w, int h);
int  temporalbadge_runtime_mouse_x(void);
int  temporalbadge_runtime_mouse_y(void);
void temporalbadge_runtime_mouse_set_pos(int x, int y);
int  temporalbadge_runtime_mouse_clicked(void);
void temporalbadge_runtime_mouse_set_speed(int speed);
void temporalbadge_runtime_mouse_set_mode(int absolute);

// IR
int temporalbadge_runtime_ir_send(int addr, int cmd);
void temporalbadge_runtime_ir_start(void);
void temporalbadge_runtime_ir_stop(void);
int temporalbadge_runtime_ir_available(void);
int temporalbadge_runtime_ir_read(int *addr_out, int *cmd_out);

// IR multi-word
int temporalbadge_runtime_ir_send_words(const uint32_t *words, size_t count);
int temporalbadge_runtime_ir_read_words(uint32_t *out, size_t max_words,
                                         size_t *count_out);
void temporalbadge_runtime_ir_flush(void);

// Get/set IR TX power (carrier duty %).  percent < 0 = query only.
// Returns the resulting power level, or -1 on error.
int temporalbadge_runtime_ir_tx_power(int percent);

// IR Playground (consumer NEC + raw symbol modes)
int         temporalbadge_runtime_ir_set_mode(const char *name);
const char *temporalbadge_runtime_ir_get_mode(void);
int temporalbadge_runtime_ir_nec_send(int addr, int cmd, int repeats);
int temporalbadge_runtime_ir_nec_read(int *addr_out,
                                       int *cmd_out,
                                       int *repeat_out);
int temporalbadge_runtime_ir_raw_capture(uint16_t *out_pairs,
                                          size_t   max_pairs);
int temporalbadge_runtime_ir_raw_send(const uint16_t *pairs,
                                       size_t          pair_count,
                                       uint32_t        carrier_hz);
uint32_t temporalbadge_runtime_ir_ms_since_tx(void);
uint32_t temporalbadge_runtime_ir_ms_since_rx(void);

// Badge identity / boops
const char *temporalbadge_runtime_my_uuid(void);
const char *temporalbadge_runtime_boops(void);

// Apps registry — re-scan /apps/<slug>/main.py for self-describing
// MicroPython apps and rebuild the main-menu grid. Returns the number
// of dynamic apps now registered. Used by JumperIDE / users to refresh
// the menu after writing a new script without rebooting.
int temporalbadge_runtime_rescan_apps(void);

// Wall-clock time. Sets the device RTC from a Unix epoch and returns the epoch
// now visible to time(nullptr), or -1 on error.
int64_t temporalbadge_runtime_set_time(int64_t epoch);

// ── NVS-backed key/value store (badge.kv_*) ────────────────────────────
//
// Survives every flash type (firmware reflash, fatfs.bin reflash, full
// esptool batch). Use this for game saves, scores, user prefs that
// must persist across reflashes — files on FATFS do NOT survive a
// fatfs.bin reflash. See firmware/docs/STORAGE-MODEL.md.
//
// Type tags (one ASCII byte prepended to the stored blob):
//   's' UTF-8 string (no NUL terminator stored)
//   'i' signed 64-bit integer (little-endian, 8 bytes)
//   'f' double  (little-endian, 8 bytes)
//   'b' raw bytes
//
// kv_put: returns 0 on success, -1 on failure (key invalid, NVS full,
//   etc.). `type` is the ASCII tag; `data`/`len` is the payload (NOT
//   including the tag byte — the runtime prepends it).
// kv_get: returns payload length on success (without tag), -1 on
//   missing/error. `*out_type` receives the tag byte.
// kv_delete: returns 0 on success, -1 if key absent.
// kv_keys: writes a JSON array of key strings into `buf`; returns the
//   byte length written (excluding NUL), -1 on error.
//
// Limits: 32 chars per key, 1 KB per value, 64 keys total.
int  temporalbadge_runtime_kv_put(const char *key, char type,
                                  const uint8_t *data, size_t len);
int  temporalbadge_runtime_kv_get(const char *key, char *out_type,
                                  uint8_t *buf, size_t buf_cap);
int  temporalbadge_runtime_kv_delete(const char *key);
int  temporalbadge_runtime_kv_keys(char *buf, size_t buf_cap);

#if defined(BADGE_ENABLE_MP_DEV)
// Dev/test harness — called from MicroPython as badge.dev(*string_args).
const char *temporalbadge_runtime_dev(int argc, const char **argv);
#endif

#ifdef __cplusplus
}
#endif

#endif
