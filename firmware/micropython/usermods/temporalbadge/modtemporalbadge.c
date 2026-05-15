#include <stdint.h>
#include <string.h>

#include "py/objfun.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/objtuple.h"
#include "py/mphal.h"
#include "py/mpprint.h"
#include "py/runtime.h"
#include "temporalbadge_hal.h"
#include "temporalbadge_runtime.h"
#include "matrix_app_api.h"

extern int mpy_hal_stdin_read(void);
extern int mpy_hal_stdin_available(void);

void led_app_runtime_restore_ambient(void);
void led_app_runtime_begin_override(void);
void led_app_runtime_end_override(void);

static const char *optional_str(mp_obj_t obj) {
    return obj == mp_const_none ? NULL : mp_obj_str_get_str(obj);
}

// ── init ────────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_init(void) {
    int rc = temporalbadge_hal_init();
    if (rc < 0) {
        mp_raise_OSError(-rc);
    }
    return mp_obj_new_int(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_init_obj, temporalbadge_init);

// ── OLED -- text ────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_oled_print(mp_obj_t msg_obj) {
    const char *msg = mp_obj_str_get_str(msg_obj);
    temporalbadge_hal_oled_print(msg);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_print_obj, temporalbadge_oled_print);

static mp_obj_t temporalbadge_oled_println(mp_obj_t msg_obj) {
    const char *msg = mp_obj_str_get_str(msg_obj);
    temporalbadge_hal_oled_println(msg);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_println_obj, temporalbadge_oled_println);

static mp_obj_t temporalbadge_oled_clear(size_t n_args, const mp_obj_t *args) {
    int show = (n_args > 0) ? mp_obj_is_true(args[0]) : 0;
    temporalbadge_hal_oled_clear(show);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_oled_clear_obj, 0, 1,
                                            temporalbadge_oled_clear);

static mp_obj_t temporalbadge_oled_show(void) {
    temporalbadge_hal_oled_show();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_oled_show_obj, temporalbadge_oled_show);

static mp_obj_t temporalbadge_oled_set_cursor(mp_obj_t x_obj, mp_obj_t y_obj) {
    int x = mp_obj_get_int(x_obj);
    int y = mp_obj_get_int(y_obj);
    temporalbadge_hal_oled_set_cursor(x, y);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(temporalbadge_oled_set_cursor_obj,
                                  temporalbadge_oled_set_cursor);

static mp_obj_t temporalbadge_oled_set_text_size(mp_obj_t size_obj) {
    int size = mp_obj_get_int(size_obj);
    int rc = temporalbadge_hal_oled_set_text_size(size);
    return mp_obj_new_bool(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_set_text_size_obj,
                                  temporalbadge_oled_set_text_size);

static mp_obj_t temporalbadge_oled_get_text_size(void) {
    return mp_obj_new_int(temporalbadge_hal_oled_get_text_size());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_oled_get_text_size_obj,
                                  temporalbadge_oled_get_text_size);

static mp_obj_t temporalbadge_oled_invert(mp_obj_t inv_obj) {
    int inv = mp_obj_is_true(inv_obj);
    temporalbadge_hal_oled_invert(inv);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_invert_obj,
                                  temporalbadge_oled_invert);

// ── OLED -- text metrics ────────────────────────────────────────────────────

static mp_obj_t temporalbadge_oled_text_width(mp_obj_t text_obj) {
    const char *text = mp_obj_str_get_str(text_obj);
    return mp_obj_new_int(temporalbadge_hal_oled_text_width(text));
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_text_width_obj,
                                  temporalbadge_oled_text_width);

static mp_obj_t temporalbadge_oled_text_height(void) {
    return mp_obj_new_int(temporalbadge_hal_oled_text_height());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_oled_text_height_obj,
                                  temporalbadge_oled_text_height);

// ── OLED -- fonts ───────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_oled_set_font(mp_obj_t name_obj) {
    const char *name = mp_obj_str_get_str(name_obj);
    int rc = temporalbadge_hal_oled_set_font(name);
    return mp_obj_new_bool(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_set_font_obj,
                                  temporalbadge_oled_set_font);

static mp_obj_t temporalbadge_oled_get_fonts(void) {
    const char *csv = temporalbadge_hal_oled_get_fonts();
    return mp_obj_new_str(csv, strlen(csv));
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_oled_get_fonts_obj,
                                  temporalbadge_oled_get_fonts);

static mp_obj_t temporalbadge_oled_get_current_font(void) {
    const char *name = temporalbadge_hal_oled_get_current_font();
    return mp_obj_new_str(name, strlen(name));
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_oled_get_current_font_obj,
                                  temporalbadge_oled_get_current_font);

// ── OLED -- framebuffer / pixel ─────────────────────────────────────────────

static mp_obj_t temporalbadge_oled_set_pixel(size_t n_args,
                                              const mp_obj_t *args) {
    int x = mp_obj_get_int(args[0]);
    int y = mp_obj_get_int(args[1]);
    int color = mp_obj_get_int(args[2]);
    temporalbadge_hal_oled_set_pixel(x, y, color);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_oled_set_pixel_obj,
                                            3, 3,
                                            temporalbadge_oled_set_pixel);

static mp_obj_t temporalbadge_oled_get_pixel(size_t n_args,
                                              const mp_obj_t *args) {
    int x = mp_obj_get_int(args[0]);
    int y = mp_obj_get_int(args[1]);
    int val = temporalbadge_hal_oled_get_pixel(x, y);
    return mp_obj_new_int(val);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_oled_get_pixel_obj,
                                            2, 2,
                                            temporalbadge_oled_get_pixel);

static mp_obj_t temporalbadge_oled_draw_box(size_t n_args,
                                             const mp_obj_t *args) {
    int x = mp_obj_get_int(args[0]);
    int y = mp_obj_get_int(args[1]);
    int w = mp_obj_get_int(args[2]);
    int h = mp_obj_get_int(args[3]);
    temporalbadge_hal_oled_draw_box(x, y, w, h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_oled_draw_box_obj,
                                            4, 4,
                                            temporalbadge_oled_draw_box);

static mp_obj_t temporalbadge_oled_set_draw_color(mp_obj_t color_obj) {
    int color = mp_obj_get_int(color_obj);
    temporalbadge_hal_oled_set_draw_color(color);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_set_draw_color_obj,
                                  temporalbadge_oled_set_draw_color);

static mp_obj_t temporalbadge_oled_get_framebuffer(void) {
    int w, h, buf_size;
    const uint8_t *buf =
        temporalbadge_hal_oled_get_framebuffer(&w, &h, &buf_size);
    if (buf == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_bytes(buf, buf_size);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_oled_get_framebuffer_obj,
                                  temporalbadge_oled_get_framebuffer);

static mp_obj_t temporalbadge_oled_set_framebuffer(mp_obj_t data_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    int rc = temporalbadge_hal_oled_set_framebuffer(
        (const uint8_t *)bufinfo.buf, bufinfo.len);
    return mp_obj_new_bool(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_oled_set_framebuffer_obj,
                                  temporalbadge_oled_set_framebuffer);

static mp_obj_t temporalbadge_oled_get_framebuffer_size(void) {
    int w, h, buf_bytes;
    temporalbadge_hal_oled_get_framebuffer_size(&w, &h, &buf_bytes);
    mp_obj_t tuple[3];
    tuple[0] = mp_obj_new_int(w);
    tuple[1] = mp_obj_new_int(h);
    tuple[2] = mp_obj_new_int(buf_bytes);
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_oled_get_framebuffer_size_obj,
                                  temporalbadge_oled_get_framebuffer_size);

// ── Native UI chrome helpers ────────────────────────────────────────────────

static mp_obj_t temporalbadge_ui_header(size_t n_args,
                                         const mp_obj_t *args) {
    const char *title = optional_str(args[0]);
    const char *right = n_args > 1 ? optional_str(args[1]) : NULL;
    temporalbadge_hal_ui_header(title, right);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ui_header_obj,
                                            1, 2,
                                            temporalbadge_ui_header);

static mp_obj_t temporalbadge_ui_action_bar(size_t n_args,
                                             const mp_obj_t *args) {
    const char *left_button = n_args > 0 ? optional_str(args[0]) : NULL;
    const char *left_label = n_args > 1 ? optional_str(args[1]) : NULL;
    const char *right_button = n_args > 2 ? optional_str(args[2]) : NULL;
    const char *right_label = n_args > 3 ? optional_str(args[3]) : NULL;
    temporalbadge_hal_ui_action_bar(left_button, left_label, right_button,
                                    right_label);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ui_action_bar_obj,
                                            0, 4,
                                            temporalbadge_ui_action_bar);

static mp_obj_t temporalbadge_ui_chrome(size_t n_args,
                                         const mp_obj_t *args) {
    const char *title = optional_str(args[0]);
    const char *right = n_args > 1 ? optional_str(args[1]) : NULL;
    const char *left_button = n_args > 2 ? optional_str(args[2]) : NULL;
    const char *left_label = n_args > 3 ? optional_str(args[3]) : NULL;
    const char *right_button = n_args > 4 ? optional_str(args[4]) : NULL;
    const char *right_label = n_args > 5 ? optional_str(args[5]) : NULL;
    temporalbadge_hal_ui_chrome(title, right, left_button, left_label,
                                right_button, right_label);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ui_chrome_obj,
                                            1, 6,
                                            temporalbadge_ui_chrome);

static mp_obj_t temporalbadge_ui_inline_hint(size_t n_args,
                                              const mp_obj_t *args) {
    int x = mp_obj_get_int(args[0]);
    int y = mp_obj_get_int(args[1]);
    const char *hint = optional_str(args[2]);
    return mp_obj_new_int(temporalbadge_hal_ui_inline_hint(x, y, hint));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ui_inline_hint_obj,
                                            3, 3,
                                            temporalbadge_ui_inline_hint);

static mp_obj_t temporalbadge_ui_inline_hint_right(size_t n_args,
                                                    const mp_obj_t *args) {
    int right_x = mp_obj_get_int(args[0]);
    int y = mp_obj_get_int(args[1]);
    const char *hint = optional_str(args[2]);
    return mp_obj_new_int(
        temporalbadge_hal_ui_inline_hint_right(right_x, y, hint));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    temporalbadge_ui_inline_hint_right_obj, 3, 3,
    temporalbadge_ui_inline_hint_right);

static mp_obj_t temporalbadge_ui_measure_hint(mp_obj_t hint_obj) {
    const char *hint = optional_str(hint_obj);
    return mp_obj_new_int(temporalbadge_hal_ui_measure_hint(hint));
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_ui_measure_hint_obj,
                                  temporalbadge_ui_measure_hint);

static mp_obj_t temporalbadge_ui_grid_cell(size_t n_args,
                                            const mp_obj_t *args) {
    int col = mp_obj_get_int(args[0]);
    int row = mp_obj_get_int(args[1]);
    const char *label = mp_obj_str_get_str(args[2]);
    int selected = mp_obj_is_true(args[3]);
    const char *icon = (n_args > 4) ? optional_str(args[4]) : NULL;
    temporalbadge_hal_ui_grid_cell(col, row, label, selected, icon);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ui_grid_cell_obj,
                                            4, 5,
                                            temporalbadge_ui_grid_cell);

static mp_obj_t temporalbadge_ui_grid_footer(size_t n_args,
                                              const mp_obj_t *args) {
    const char *desc = (n_args > 0) ? optional_str(args[0]) : NULL;
    temporalbadge_hal_ui_grid_footer(desc);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ui_grid_footer_obj,
                                            0, 1,
                                            temporalbadge_ui_grid_footer);

static mp_obj_t temporalbadge_ui_list_row(size_t n_args,
                                           const mp_obj_t *args) {
    int row = mp_obj_get_int(args[0]);
    const char *label = mp_obj_str_get_str(args[1]);
    int selected = mp_obj_is_true(args[2]);
    temporalbadge_hal_ui_list_row(row, label, selected);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ui_list_row_obj,
                                            3, 3,
                                            temporalbadge_ui_list_row);

static mp_obj_t temporalbadge_ui_list_rows_visible(void) {
    return mp_obj_new_int(temporalbadge_hal_ui_list_rows_visible());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ui_list_rows_visible_obj,
                                  temporalbadge_ui_list_rows_visible);

// ── Buttons ─────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_button(mp_obj_t button_id_obj) {
    int button_id = mp_obj_get_int(button_id_obj);
    int state = temporalbadge_hal_button_state(button_id);
    if (state < 0) {
        mp_raise_OSError(-state);
    }
    return mp_obj_new_bool(state != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_button_obj, temporalbadge_button);

// ── Joystick ────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_joy_x(void) {
    return mp_obj_new_int(temporalbadge_hal_joy_x());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_joy_x_obj, temporalbadge_joy_x);

static mp_obj_t temporalbadge_joy_y(void) {
    return mp_obj_new_int(temporalbadge_hal_joy_y());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_joy_y_obj, temporalbadge_joy_y);

// ── Explicit HTTP helpers ──────────────────────────────────────────────────

static mp_obj_t temporalbadge_http_get(mp_obj_t url_obj) {
    const char *url = mp_obj_str_get_str(url_obj);
    const char *resp = temporalbadge_hal_http_get(url);
    if (resp == NULL) {
        mp_raise_OSError(12);
    }
    return mp_obj_new_str(resp, strlen(resp));
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_http_get_obj,
                                  temporalbadge_http_get);

static mp_obj_t temporalbadge_http_post(mp_obj_t url_obj, mp_obj_t body_obj) {
    const char *url = mp_obj_str_get_str(url_obj);
    const char *body = mp_obj_str_get_str(body_obj);
    const char *resp = temporalbadge_hal_http_post(url, body);
    if (resp == NULL) {
        mp_raise_OSError(12);
    }
    return mp_obj_new_str(resp, strlen(resp));
}
static MP_DEFINE_CONST_FUN_OBJ_2(temporalbadge_http_post_obj,
                                  temporalbadge_http_post);

// ── Enhanced buttons ────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_button_pressed(mp_obj_t button_id_obj) {
    int button_id = mp_obj_get_int(button_id_obj);
    int rc = temporalbadge_hal_button_pressed(button_id);
    if (rc < 0) {
        mp_raise_OSError(-rc);
    }
    return mp_obj_new_bool(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_button_pressed_obj,
                                  temporalbadge_button_pressed);

static mp_obj_t temporalbadge_button_held_ms(mp_obj_t button_id_obj) {
    int button_id = mp_obj_get_int(button_id_obj);
    int rc = temporalbadge_hal_button_held_ms(button_id);
    if (rc < 0) {
        mp_raise_OSError(-rc);
    }
    return mp_obj_new_int(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_button_held_ms_obj,
                                  temporalbadge_button_held_ms);

// ── LED matrix ──────────────────────────────────────────────────────────────

static void matrix_app_note_external_draw(void);

static mp_obj_t temporalbadge_led_brightness(mp_obj_t val_obj) {
    int value = mp_obj_get_int(val_obj);
    if (value < 0) {
        value = 0;
    } else if (value > 255) {
        value = 255;
    }
    int rc = temporalbadge_hal_led_set_brightness((uint8_t)value);
    if (rc < 0) {
        mp_raise_OSError(-rc);
    }
    return mp_obj_new_int(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_led_brightness_obj,
                                  temporalbadge_led_brightness);

static mp_obj_t temporalbadge_led_clear(void) {
    matrix_app_note_external_draw();
    temporalbadge_hal_led_clear();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_led_clear_obj,
                                  temporalbadge_led_clear);

static mp_obj_t temporalbadge_led_fill(size_t n_args, const mp_obj_t *args) {
    matrix_app_note_external_draw();
    int brightness = (n_args > 0) ? mp_obj_get_int(args[0]) : -1;
    temporalbadge_hal_led_fill(brightness);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_led_fill_obj, 0, 1,
                                            temporalbadge_led_fill);

static mp_obj_t temporalbadge_led_set_pixel(size_t n_args,
                                             const mp_obj_t *args) {
    matrix_app_note_external_draw();
    int x = mp_obj_get_int(args[0]);
    int y = mp_obj_get_int(args[1]);
    int b = mp_obj_get_int(args[2]);
    int rc = temporalbadge_hal_led_set_pixel(x, y, b);
    if (rc < 0) {
        mp_raise_OSError(-rc);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_led_set_pixel_obj,
                                            3, 3,
                                            temporalbadge_led_set_pixel);

static mp_obj_t temporalbadge_led_get_pixel(mp_obj_t x_obj, mp_obj_t y_obj) {
    int x = mp_obj_get_int(x_obj);
    int y = mp_obj_get_int(y_obj);
    int val = temporalbadge_hal_led_get_pixel(x, y);
    if (val < 0) {
        mp_raise_OSError(-val);
    }
    return mp_obj_new_int(val);
}
static MP_DEFINE_CONST_FUN_OBJ_2(temporalbadge_led_get_pixel_obj,
                                  temporalbadge_led_get_pixel);

static mp_obj_t temporalbadge_led_show_image(mp_obj_t name_obj) {
    matrix_app_note_external_draw();
    const char *name = mp_obj_str_get_str(name_obj);
    int rc = temporalbadge_hal_led_show_image(name);
    if (rc < 0) {
        mp_raise_OSError(-rc);
    }
    return mp_obj_new_bool(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_led_show_image_obj,
                                  temporalbadge_led_show_image);

static mp_obj_t temporalbadge_led_set_frame(size_t n_args,
                                             const mp_obj_t *args) {
    matrix_app_note_external_draw();
    mp_obj_t rows_obj = args[0];
    uint8_t mask[8];

    mp_buffer_info_t bufinfo;
    if (mp_get_buffer(rows_obj, &bufinfo, MP_BUFFER_READ)) {
        if (bufinfo.len != 8) {
            mp_raise_ValueError(MP_ERROR_TEXT("led_set_frame requires 8 rows"));
        }
        memcpy(mask, bufinfo.buf, 8);
    } else {
        size_t len;
        mp_obj_t *items;
        mp_obj_get_array(rows_obj, &len, &items);
        if (len != 8) {
            mp_raise_ValueError(MP_ERROR_TEXT("led_set_frame requires 8 rows"));
        }
        for (int i = 0; i < 8; i++) {
            mask[i] = (uint8_t)mp_obj_get_int(items[i]);
        }
    }
    int brightness = (n_args > 1) ? mp_obj_get_int(args[1]) : -1;
    temporalbadge_hal_led_set_frame(mask, brightness);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_led_set_frame_obj,
                                            1, 2,
                                            temporalbadge_led_set_frame);

static mp_obj_t temporalbadge_led_start_animation(size_t n_args,
                                                    const mp_obj_t *args) {
    matrix_app_note_external_draw();
    const char *name = mp_obj_str_get_str(args[0]);
    int interval_ms = (n_args > 1) ? mp_obj_get_int(args[1]) : -1;
    int rc = temporalbadge_hal_led_start_animation(name, interval_ms);
    return mp_obj_new_bool(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_led_start_animation_obj,
                                            1, 2,
                                            temporalbadge_led_start_animation);

static mp_obj_t temporalbadge_led_stop_animation(void) {
    matrix_app_note_external_draw();
    temporalbadge_hal_led_stop_animation();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_led_stop_animation_obj,
                                  temporalbadge_led_stop_animation);

static mp_obj_t temporalbadge_led_override_begin(void) {
    matrix_app_begin_override();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_led_override_begin_obj,
                                  temporalbadge_led_override_begin);

static mp_obj_t temporalbadge_led_override_end(void) {
    matrix_app_end_override();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_led_override_end_obj,
                                  temporalbadge_led_override_end);

// ── IMU ─────────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_imu_ready(void) {
    return mp_obj_new_bool(temporalbadge_hal_imu_ready());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_imu_ready_obj,
                                  temporalbadge_imu_ready);

static mp_obj_t temporalbadge_imu_tilt_x(void) {
    return mp_obj_new_float(temporalbadge_hal_imu_tilt_x());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_imu_tilt_x_obj,
                                  temporalbadge_imu_tilt_x);

static mp_obj_t temporalbadge_imu_tilt_y(void) {
    return mp_obj_new_float(temporalbadge_hal_imu_tilt_y());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_imu_tilt_y_obj,
                                  temporalbadge_imu_tilt_y);

static mp_obj_t temporalbadge_imu_accel_z(void) {
    return mp_obj_new_float(temporalbadge_hal_imu_accel_z());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_imu_accel_z_obj,
                                  temporalbadge_imu_accel_z);

static mp_obj_t temporalbadge_imu_face_down(void) {
    return mp_obj_new_bool(temporalbadge_hal_imu_face_down());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_imu_face_down_obj,
                                  temporalbadge_imu_face_down);

static mp_obj_t temporalbadge_imu_motion(void) {
    return mp_obj_new_bool(temporalbadge_hal_imu_motion());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_imu_motion_obj,
                                  temporalbadge_imu_motion);

// ── Haptics ─────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_haptic_pulse(size_t n_args,
                                            const mp_obj_t *args) {
    int strength    = (n_args > 0) ? mp_obj_get_int(args[0]) : -1;
    int duration_ms = (n_args > 1) ? mp_obj_get_int(args[1]) : -1;
    int freq_hz     = (n_args > 2) ? mp_obj_get_int(args[2]) : -1;
    temporalbadge_hal_haptic_pulse(strength, duration_ms, freq_hz);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_haptic_pulse_obj,
                                            0, 3,
                                            temporalbadge_haptic_pulse);

static mp_obj_t temporalbadge_haptic_strength(size_t n_args,
                                               const mp_obj_t *args) {
    if (n_args > 0) {
        int value = mp_obj_get_int(args[0]);
        temporalbadge_hal_haptic_set_strength(value);
    }
    return mp_obj_new_int(temporalbadge_hal_haptic_strength());
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_haptic_strength_obj,
                                            0, 1,
                                            temporalbadge_haptic_strength);

static mp_obj_t temporalbadge_haptic_off(void) {
    temporalbadge_hal_haptic_off();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_haptic_off_obj,
                                  temporalbadge_haptic_off);

static mp_obj_t temporalbadge_tone(size_t n_args, const mp_obj_t *args) {
    int freq_hz     = mp_obj_get_int(args[0]);
    int duration_ms = (n_args > 1) ? mp_obj_get_int(args[1]) : -1;
    int duty        = (n_args > 2) ? mp_obj_get_int(args[2]) : -1;
    temporalbadge_hal_tone(freq_hz, duration_ms, duty);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_tone_obj, 1, 3,
                                            temporalbadge_tone);

static mp_obj_t temporalbadge_no_tone(void) {
    temporalbadge_hal_no_tone();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_no_tone_obj,
                                  temporalbadge_no_tone);

static mp_obj_t temporalbadge_tone_playing(void) {
    return mp_obj_new_bool(temporalbadge_hal_tone_playing());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_tone_playing_obj,
                                  temporalbadge_tone_playing);

// ── IR ──────────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_ir_send(mp_obj_t addr_obj, mp_obj_t cmd_obj) {
    int addr = mp_obj_get_int(addr_obj);
    int cmd  = mp_obj_get_int(cmd_obj);
    temporalbadge_hal_ir_send(addr, cmd);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(temporalbadge_ir_send_obj,
                                  temporalbadge_ir_send);

static mp_obj_t temporalbadge_ir_start(void) {
    temporalbadge_hal_ir_start();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_start_obj,
                                  temporalbadge_ir_start);

static mp_obj_t temporalbadge_ir_stop(void) {
    temporalbadge_hal_ir_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_stop_obj,
                                  temporalbadge_ir_stop);

static mp_obj_t temporalbadge_ir_available(void) {
    return mp_obj_new_bool(temporalbadge_hal_ir_available());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_available_obj,
                                  temporalbadge_ir_available);

static mp_obj_t temporalbadge_ir_read(void) {
    int addr = 0, cmd = 0;
    int rc = temporalbadge_hal_ir_read(&addr, &cmd);
    if (rc == 0) {
        mp_obj_t items[2] = { mp_obj_new_int(addr), mp_obj_new_int(cmd) };
        return mp_obj_new_tuple(2, items);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_read_obj,
                                  temporalbadge_ir_read);

// ── Mouse overlay ───────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_mouse_overlay(mp_obj_t enable_obj) {
    int enable = mp_obj_is_true(enable_obj);
    temporalbadge_hal_mouse_overlay(enable);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_mouse_overlay_obj,
                                  temporalbadge_mouse_overlay);

static mp_obj_t temporalbadge_mouse_set_bitmap(size_t n_args,
                                                const mp_obj_t *args) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_READ);
    int w = mp_obj_get_int(args[1]);
    int h = mp_obj_get_int(args[2]);
    temporalbadge_hal_mouse_set_bitmap(
        (const uint8_t *)bufinfo.buf, w, h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_mouse_set_bitmap_obj,
                                            3, 3,
                                            temporalbadge_mouse_set_bitmap);

static mp_obj_t temporalbadge_mouse_x(void) {
    return mp_obj_new_int(temporalbadge_hal_mouse_x());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_mouse_x_obj,
                                  temporalbadge_mouse_x);

static mp_obj_t temporalbadge_mouse_y(void) {
    return mp_obj_new_int(temporalbadge_hal_mouse_y());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_mouse_y_obj,
                                  temporalbadge_mouse_y);

static mp_obj_t temporalbadge_mouse_set_pos(mp_obj_t x_obj,
                                             mp_obj_t y_obj) {
    int x = mp_obj_get_int(x_obj);
    int y = mp_obj_get_int(y_obj);
    temporalbadge_hal_mouse_set_pos(x, y);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(temporalbadge_mouse_set_pos_obj,
                                  temporalbadge_mouse_set_pos);

static mp_obj_t temporalbadge_mouse_clicked(void) {
    return mp_obj_new_int(temporalbadge_hal_mouse_clicked());
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_mouse_clicked_obj,
                                  temporalbadge_mouse_clicked);

static mp_obj_t temporalbadge_mouse_set_speed(mp_obj_t speed_obj) {
    int speed = mp_obj_get_int(speed_obj);
    temporalbadge_hal_mouse_set_speed(speed);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_mouse_set_speed_obj,
                                  temporalbadge_mouse_set_speed);

static mp_obj_t temporalbadge_mouse_set_mode(mp_obj_t mode_obj) {
    int absolute = mp_obj_is_true(mode_obj);
    temporalbadge_hal_mouse_set_mode(absolute);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_mouse_set_mode_obj,
                                  temporalbadge_mouse_set_mode);

// ── Serial stdin helpers ────────────────────────────────────────────────────

static mp_obj_t temporalbadge_serial_available(void) {
    return mp_obj_new_bool(mpy_hal_stdin_available() > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_serial_available_obj,
                                  temporalbadge_serial_available);

static mp_obj_t temporalbadge_serial_read(void) {
    int c = mpy_hal_stdin_read();
    if (c < 0) {
        return mp_const_none;
    }
    char data[1] = {(char)c};
    return mp_obj_new_str(data, 1);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_serial_read_obj,
                                  temporalbadge_serial_read);

// ── IR multi-word ───────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_ir_send_words(mp_obj_t seq_obj) {
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(seq_obj, &len, &items);
    if (len == 0 || len > 64) {
        mp_raise_ValueError(MP_ERROR_TEXT("ir_send_words: 1-64 words"));
    }
    uint32_t words[64];
    for (size_t i = 0; i < len; i++) {
        words[i] = (uint32_t)mp_obj_get_int_truncated(items[i]);
    }
    int rc = temporalbadge_hal_ir_send_words(words, len);
    if (rc < 0) {
        mp_raise_OSError(1);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_ir_send_words_obj,
                                  temporalbadge_ir_send_words);

static mp_obj_t temporalbadge_ir_read_words(void) {
    uint32_t words[64];
    size_t count = 0;
    int rc = temporalbadge_hal_ir_read_words(words, 64, &count);
    if (rc != 0 || count == 0) {
        return mp_const_none;
    }
    mp_obj_t *items = m_new(mp_obj_t, count);
    for (size_t i = 0; i < count; i++) {
        items[i] = mp_obj_new_int_from_uint(words[i]);
    }
    mp_obj_t tuple = mp_obj_new_tuple(count, items);
    m_del(mp_obj_t, items, count);
    return tuple;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_read_words_obj,
                                  temporalbadge_ir_read_words);

static mp_obj_t temporalbadge_ir_flush(void) {
    temporalbadge_hal_ir_flush();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_flush_obj,
                                  temporalbadge_ir_flush);

static mp_obj_t temporalbadge_ir_tx_power(size_t n_args,
                                           const mp_obj_t *args) {
    int percent = (n_args > 0) ? mp_obj_get_int(args[0]) : -1;
    int rc = temporalbadge_hal_ir_tx_power(percent);
    if (rc < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("ir_tx_power: 1..50"));
    }
    return mp_obj_new_int(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ir_tx_power_obj,
                                            0, 1,
                                            temporalbadge_ir_tx_power);

// ── IR Playground (consumer NEC + raw symbols) ──────────────────────────────

static mp_obj_t temporalbadge_ir_set_mode(mp_obj_t name_obj) {
    const char *name = mp_obj_str_get_str(name_obj);
    int rc = temporalbadge_hal_ir_set_mode(name);
    if (rc != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "ir_set_mode: expects 'badge'|'nec'|'raw' (or boop in flight)"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_ir_set_mode_obj,
                                  temporalbadge_ir_set_mode);

static mp_obj_t temporalbadge_ir_get_mode(void) {
    const char *name = temporalbadge_hal_ir_get_mode();
    return mp_obj_new_str(name, strlen(name));
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_get_mode_obj,
                                  temporalbadge_ir_get_mode);

static mp_obj_t temporalbadge_ir_nec_send(size_t n_args,
                                           const mp_obj_t *args) {
    int addr    = mp_obj_get_int(args[0]);
    int cmd     = mp_obj_get_int(args[1]);
    int repeats = (n_args > 2) ? mp_obj_get_int(args[2]) : 0;
    int rc = temporalbadge_hal_ir_nec_send(addr, cmd, repeats);
    if (rc != 0) {
        mp_raise_OSError(1);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ir_nec_send_obj,
                                            2, 3,
                                            temporalbadge_ir_nec_send);

static mp_obj_t temporalbadge_ir_nec_read(void) {
    int addr = 0, cmd = 0, repeat = 0;
    int rc = temporalbadge_hal_ir_nec_read(&addr, &cmd, &repeat);
    if (rc != 0) {
        return mp_const_none;
    }
    mp_obj_t items[3] = {
        mp_obj_new_int(addr),
        mp_obj_new_int(cmd),
        mp_obj_new_bool(repeat),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_nec_read_obj,
                                  temporalbadge_ir_nec_read);

#define IR_RAW_PY_MAX_PAIRS 512U

static mp_obj_t temporalbadge_ir_raw_capture(void) {
    static uint16_t buf[IR_RAW_PY_MAX_PAIRS * 2U];
    int n = temporalbadge_hal_ir_raw_capture(buf, IR_RAW_PY_MAX_PAIRS);
    if (n <= 0) {
        return mp_const_none;
    }
    return mp_obj_new_bytes((const byte *)buf,
                             (size_t)n * 2U * sizeof(uint16_t));
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_raw_capture_obj,
                                  temporalbadge_ir_raw_capture);

static mp_obj_t temporalbadge_ir_raw_send(size_t n_args,
                                           const mp_obj_t *args) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_READ);
    if ((bufinfo.len % (2U * sizeof(uint16_t))) != 0U) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "ir_raw_send: buffer length must be a multiple of 4 bytes"));
    }
    size_t pair_count = bufinfo.len / (2U * sizeof(uint16_t));
    if (pair_count == 0U || pair_count > IR_RAW_PY_MAX_PAIRS) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "ir_raw_send: 1..512 mark/space pairs"));
    }
    uint32_t carrier_hz = (n_args > 1) ? (uint32_t)mp_obj_get_int(args[1]) : 0U;
    int rc = temporalbadge_hal_ir_raw_send(
        (const uint16_t *)bufinfo.buf, pair_count, carrier_hz);
    if (rc != 0) {
        mp_raise_OSError(1);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_ir_raw_send_obj,
                                            1, 2,
                                            temporalbadge_ir_raw_send);

static mp_obj_t temporalbadge_ir_activity(void) {
    uint32_t tx = temporalbadge_hal_ir_ms_since_tx();
    uint32_t rx = temporalbadge_hal_ir_ms_since_rx();
    mp_obj_t items[2] = {
        (tx == UINT32_MAX) ? mp_const_none : mp_obj_new_int_from_uint(tx),
        (rx == UINT32_MAX) ? mp_const_none : mp_obj_new_int_from_uint(rx),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_ir_activity_obj,
                                  temporalbadge_ir_activity);

// ── Badge identity / boops ──────────────────────────────────────────────────

static mp_obj_t temporalbadge_my_uuid(void) {
    const char *uuid = temporalbadge_hal_my_uuid();
    return mp_obj_new_str(uuid, strlen(uuid));
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_my_uuid_obj,
                                  temporalbadge_my_uuid);

static mp_obj_t temporalbadge_boops(void) {
    const char *json = temporalbadge_hal_boops();
    return mp_obj_new_str(json, strlen(json));
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_boops_obj,
                                  temporalbadge_boops);

// ── Background matrix app host ──────────────────────────────────────────────

#define MATRIX_APP_DEFAULT_INTERVAL_MS 150u
#define MATRIX_APP_DEFAULT_BRIGHTNESS  12u
#define MATRIX_APP_MIN_INTERVAL_MS     5u

MP_REGISTER_ROOT_POINTER(mp_obj_t matrix_app_active_cb);

static uint32_t s_matrix_app_interval_ms = MATRIX_APP_DEFAULT_INTERVAL_MS;
static uint8_t s_matrix_app_brightness = MATRIX_APP_DEFAULT_BRIGHTNESS;
static uint32_t s_matrix_app_last_tick_ms = 0;
static uint32_t s_matrix_app_invocations = 0;
static uint8_t s_matrix_app_overridden = 0;
static uint8_t s_matrix_app_in_call = 0;

static uint8_t s_foreground_session = 0;
static uint8_t s_foreground_claimed = 0;
static uint8_t s_foreground_we_overrode = 0;

static uint32_t matrix_default_interval_from_runtime(void) {
    int v = temporalbadge_runtime_matrix_default_interval_ms();
    if (v < (int)MATRIX_APP_MIN_INTERVAL_MS) {
        v = (int)MATRIX_APP_DEFAULT_INTERVAL_MS;
    }
    return (uint32_t)v;
}

static uint8_t matrix_default_brightness_from_runtime(void) {
    int v = temporalbadge_runtime_matrix_default_brightness();
    if (v < 0) {
        v = 0;
    } else if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

static inline bool matrix_app_cb_unset(mp_obj_t cb) {
    return cb == MP_OBJ_NULL || cb == mp_const_none;
}

static mp_obj_t matrix_app_entry_callback(mp_obj_t entry) {
    if (matrix_app_cb_unset(entry)) {
        return mp_const_none;
    }
    if (mp_obj_get_type(entry) == &mp_type_tuple) {
        mp_obj_tuple_t *tuple = MP_OBJ_TO_PTR(entry);
        return tuple->len > 0 ? tuple->items[0] : mp_const_none;
    }
    return entry;
}

static mp_obj_t matrix_app_callback_globals(mp_obj_t cb) {
    if (matrix_app_cb_unset(cb)) {
        return mp_const_none;
    }
    const mp_obj_type_t *type = mp_obj_get_type(cb);
    if (type == &mp_type_fun_bc) {
        mp_obj_fun_bc_t *fun = MP_OBJ_TO_PTR(cb);
        if (fun->context != NULL && fun->context->module.globals != NULL) {
            return MP_OBJ_FROM_PTR(fun->context->module.globals);
        }
    }
    return mp_const_none;
}

static mp_obj_t matrix_app_make_entry(mp_obj_t cb) {
    if (matrix_app_cb_unset(cb)) {
        return mp_const_none;
    }
    mp_obj_t items[2] = {
        cb,
        matrix_app_callback_globals(cb),
    };
    return mp_obj_new_tuple(2, items);
}

static void matrix_app_apply_active_state(void) {
    mp_obj_t cb = matrix_app_entry_callback(MP_STATE_VM(matrix_app_active_cb));
    temporalbadge_runtime_matrix_host_set_active(matrix_app_cb_unset(cb) ? 0 : 1);
}

static void matrix_app_note_external_draw(void) {
    if (!s_foreground_session) {
        return;
    }
    if (s_matrix_app_in_call || s_foreground_claimed || s_matrix_app_overridden) {
        return;
    }
    if (matrix_app_cb_unset(matrix_app_entry_callback(MP_STATE_VM(matrix_app_active_cb)))) {
        return;
    }
    s_matrix_app_overridden = 1;
    s_foreground_we_overrode = 1;
}

static void matrix_app_note_explicit_start(void) {
    if (!s_foreground_session || s_matrix_app_in_call) {
        return;
    }
    s_foreground_claimed = 1;
    if (s_foreground_we_overrode) {
        s_matrix_app_overridden = 0;
        s_foreground_we_overrode = 0;
    }
}

static int matrix_app_restore_saved_ambient(void) {
    if (s_matrix_app_in_call) {
        return 0;
    }
    led_app_runtime_restore_ambient();
    return 1;
}

void matrix_app_foreground_begin(void) {
    s_foreground_session = 1;
    s_foreground_claimed = 0;
    s_foreground_we_overrode = 0;
}

void matrix_app_foreground_end(void) {
    int should_restore = s_foreground_we_overrode ? 1 : 0;
    if (s_foreground_we_overrode) {
        s_matrix_app_overridden = 0;
        s_matrix_app_last_tick_ms = 0;
    }
    s_foreground_session = 0;
    s_foreground_claimed = 0;
    s_foreground_we_overrode = 0;
    if (should_restore) {
        matrix_app_restore_saved_ambient();
    }
}

static void matrix_app_assign_active_entry(mp_obj_t entry, uint32_t interval_ms,
                                           uint8_t brightness) {
    if (interval_ms < MATRIX_APP_MIN_INTERVAL_MS) {
        interval_ms = MATRIX_APP_MIN_INTERVAL_MS;
    }
    MP_STATE_VM(matrix_app_active_cb) = entry;
    s_matrix_app_interval_ms = interval_ms;
    s_matrix_app_brightness = brightness;
    s_matrix_app_last_tick_ms = 0;

    mp_obj_t cb = matrix_app_entry_callback(entry);
    if (!matrix_app_cb_unset(cb)) {
        temporalbadge_runtime_matrix_host_apply_brightness((int)brightness);
    }
    matrix_app_apply_active_state();
}

static void matrix_app_assign_active(mp_obj_t cb, uint32_t interval_ms,
                                     uint8_t brightness) {
    matrix_app_assign_active_entry(matrix_app_make_entry(cb), interval_ms, brightness);
}

static mp_obj_t matrix_app_start(size_t n_args, const mp_obj_t *args) {
    mp_obj_t cb = args[0];
    if (cb != mp_const_none && !mp_obj_is_callable(cb)) {
        mp_raise_TypeError(MP_ERROR_TEXT("matrix_app_start: callable required"));
    }
    matrix_app_note_explicit_start();
    uint32_t interval_ms = (n_args > 1)
        ? (uint32_t)mp_obj_get_int(args[1])
        : matrix_default_interval_from_runtime();
    int brightness_arg = (n_args > 2)
        ? mp_obj_get_int(args[2])
        : matrix_default_brightness_from_runtime();
    if (brightness_arg < 0) {
        brightness_arg = 0;
    } else if (brightness_arg > 255) {
        brightness_arg = 255;
    }
    matrix_app_assign_active(cb, interval_ms, (uint8_t)brightness_arg);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(matrix_app_start_obj, 1, 3,
                                            matrix_app_start);

static mp_obj_t matrix_app_set_speed(mp_obj_t interval_obj) {
    uint32_t interval_ms = (uint32_t)mp_obj_get_int(interval_obj);
    if (interval_ms < MATRIX_APP_MIN_INTERVAL_MS) {
        interval_ms = MATRIX_APP_MIN_INTERVAL_MS;
    }
    s_matrix_app_interval_ms = interval_ms;
    s_matrix_app_last_tick_ms = 0;
    return mp_obj_new_int_from_uint(interval_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrix_app_set_speed_obj,
                                  matrix_app_set_speed);

static mp_obj_t matrix_app_set_brightness(mp_obj_t brightness_obj) {
    int brightness = mp_obj_get_int(brightness_obj);
    if (brightness < 0) {
        brightness = 0;
    } else if (brightness > 255) {
        brightness = 255;
    }
    s_matrix_app_brightness = (uint8_t)brightness;
    temporalbadge_runtime_matrix_host_apply_brightness(brightness);
    return mp_obj_new_int(brightness);
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrix_app_set_brightness_obj,
                                  matrix_app_set_brightness);

static mp_obj_t matrix_app_stop(void) {
    matrix_app_assign_active(mp_const_none,
                             MATRIX_APP_DEFAULT_INTERVAL_MS,
                             MATRIX_APP_DEFAULT_BRIGHTNESS);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(matrix_app_stop_obj, matrix_app_stop);

static mp_obj_t matrix_app_active(void) {
    return mp_obj_new_bool(
        !matrix_app_cb_unset(matrix_app_entry_callback(MP_STATE_VM(matrix_app_active_cb))));
}
static MP_DEFINE_CONST_FUN_OBJ_0(matrix_app_active_obj, matrix_app_active);

static mp_obj_t matrix_app_info(void) {
    mp_obj_t items[6];
    items[0] = mp_obj_new_bool(
        !matrix_app_cb_unset(matrix_app_entry_callback(MP_STATE_VM(matrix_app_active_cb))));
    items[1] = mp_const_false;
    items[2] = mp_obj_new_int_from_uint(s_matrix_app_interval_ms);
    items[3] = mp_obj_new_int_from_uint(s_matrix_app_brightness);
    items[4] = mp_obj_new_bool(s_matrix_app_overridden);
    items[5] = mp_obj_new_int_from_uint(s_matrix_app_invocations);
    return mp_obj_new_tuple(6, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(matrix_app_info_obj, matrix_app_info);

int matrix_app_service_tick(uint32_t now_ms) {
    if (s_matrix_app_in_call || s_matrix_app_overridden) {
        return 0;
    }

    mp_obj_t active_entry = MP_STATE_VM(matrix_app_active_cb);
    mp_obj_t cb = matrix_app_entry_callback(active_entry);
    if (matrix_app_cb_unset(cb)) {
        return 0;
    }

    uint32_t interval = s_matrix_app_interval_ms;
    if (interval < MATRIX_APP_MIN_INTERVAL_MS) {
        interval = MATRIX_APP_MIN_INTERVAL_MS;
    }
    if (s_matrix_app_last_tick_ms != 0 &&
        (now_ms - s_matrix_app_last_tick_ms) < interval) {
        return 0;
    }
    s_matrix_app_last_tick_ms = now_ms;
    s_matrix_app_in_call = 1;

    nlr_buf_t nlr;
    int invoked = 0;
    if (nlr_push(&nlr) == 0) {
        mp_call_function_1(cb, mp_obj_new_int_from_uint(now_ms));
        nlr_pop();
        invoked = 1;
    } else {
        mp_printf(&mp_plat_print, "[mh] matrix callback failed; stopping\n");
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        MP_STATE_VM(matrix_app_active_cb) = mp_const_none;
        matrix_app_apply_active_state();
    }

    s_matrix_app_invocations++;
    s_matrix_app_in_call = 0;
    return invoked;
}

void matrix_app_begin_override(void) {
    s_matrix_app_overridden = 1;
    led_app_runtime_begin_override();
}

void matrix_app_end_override(void) {
    s_matrix_app_overridden = 0;
    s_matrix_app_last_tick_ms = 0;
    led_app_runtime_end_override();
    matrix_app_restore_saved_ambient();
}

int matrix_app_is_overridden(void) {
    return s_matrix_app_overridden ? 1 : 0;
}

int matrix_app_is_active(void) {
    return !matrix_app_cb_unset(
        matrix_app_entry_callback(MP_STATE_VM(matrix_app_active_cb))) ? 1 : 0;
}

void matrix_app_stop_from_c(void) {
    MP_STATE_VM(matrix_app_active_cb) = mp_const_none;
    matrix_app_apply_active_state();
}

uint32_t matrix_app_interval_ms(void) {
    return s_matrix_app_interval_ms;
}

#if defined(BADGE_ENABLE_MP_DEV)
// ── dev (test harness) ──────────────────────────────────────────────────────
//
// Variadic string-arg dispatcher.  Every positional arg is coerced to a
// C string via mp_obj_str_get_str and forwarded to the HAL shim.  Return
// value is a str (empty string if the C side returned NULL).  See
// temporalbadge_runtime_dev for the full subcommand table.

static mp_obj_t temporalbadge_dev(size_t n_args, const mp_obj_t *args) {
    // Cap the number of args we forward; the test harness never uses
    // more than a handful.  Stack-allocated, no malloc.
    #define TBDG_MAX_DEV_ARGS 10
    const char *argv[TBDG_MAX_DEV_ARGS];
    size_t argc = (n_args < TBDG_MAX_DEV_ARGS) ? n_args : TBDG_MAX_DEV_ARGS;
    for (size_t i = 0; i < argc; i++) {
        argv[i] = mp_obj_str_get_str(args[i]);
    }
    const char *result = temporalbadge_hal_dev((int)argc, argv);
    if (!result) result = "";
    return mp_obj_new_str(result, strlen(result));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(temporalbadge_dev_obj, 0,
                                    temporalbadge_dev);
#endif

// ── apps registry ───────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_rescan_apps(void) {
    int count = temporalbadge_runtime_rescan_apps();
    return mp_obj_new_int(count);
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_rescan_apps_obj,
                                  temporalbadge_rescan_apps);

// ── time ───────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_set_time(mp_obj_t epoch_obj) {
    int64_t epoch = mp_obj_get_int(epoch_obj);
    int64_t actual = temporalbadge_runtime_set_time(epoch);
    if (actual < 0) {
        mp_raise_OSError(1);
    }
    return mp_obj_new_int_from_ll(actual);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_set_time_obj,
                                  temporalbadge_set_time);

// ── badge.kv — NVS-backed key/value store ──────────────────────────────────
//
// Survives every flash type (firmware OTA, fatfs.bin reflash, full
// factory flash). Use this for game saves / scores / user prefs that
// must persist across reflashes — files on FATFS do NOT survive a
// `pio run -t uploadfs` rewrite. See firmware/docs/STORAGE-MODEL.md.
//
// Type policy (mirrored on the C side):
//   str      → tag 's', UTF-8 bytes
//   int      → tag 'i', 8-byte little-endian (signed)
//   float    → tag 'f', 8-byte little-endian (double)
//   bytes    → tag 'b', raw payload
// Other Python types raise TypeError.

#define BADGE_KV_MAX_VALUE 1024

static int badge_kv_dispatch_put(const char *key, mp_obj_t value) {
    if (mp_obj_is_str(value)) {
        size_t n = 0;
        const char *s = mp_obj_str_get_data(value, &n);
        return temporalbadge_runtime_kv_put(key, 's', (const uint8_t *)s, n);
    }
    if (mp_obj_is_type(value, &mp_type_bytes) ||
        mp_obj_is_type(value, &mp_type_bytearray)) {
        mp_buffer_info_t bi;
        mp_get_buffer_raise(value, &bi, MP_BUFFER_READ);
        return temporalbadge_runtime_kv_put(key, 'b',
                                             (const uint8_t *)bi.buf, bi.len);
    }
    if (mp_obj_is_int(value)) {
        // int64_t fits a Python small int comfortably; large ints clip.
        long long v = mp_obj_get_int_truncated(value);
        uint8_t buf[8];
        for (int i = 0; i < 8; ++i) buf[i] = (uint8_t)(v >> (8 * i));
        return temporalbadge_runtime_kv_put(key, 'i', buf, 8);
    }
    if (mp_obj_is_float(value)) {
        double v = mp_obj_get_float(value);
        uint8_t buf[8];
        memcpy(buf, &v, 8);
        return temporalbadge_runtime_kv_put(key, 'f', buf, 8);
    }
    return -2;  // unsupported type
}

static mp_obj_t temporalbadge_kv_put(mp_obj_t key_obj, mp_obj_t value_obj) {
    const char *key = mp_obj_str_get_str(key_obj);
    int rc = badge_kv_dispatch_put(key, value_obj);
    if (rc == -2) {
        mp_raise_TypeError(MP_ERROR_TEXT("badge.kv_put: value must be str, "
                                          "bytes, int, or float"));
    }
    if (rc < 0) {
        // Use the MP errno surface; on the embed port the
        // py/mperrno.h variant is defined to a literal int matching
        // ESP-IDF's `EIO`. We can't rely on `<errno.h>` symbols here
        // because the usermod is included before that header gets
        // pulled in.
        mp_raise_OSError(5);  // EIO
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(temporalbadge_kv_put_obj,
                                  temporalbadge_kv_put);

static mp_obj_t temporalbadge_kv_get(size_t n_args, const mp_obj_t *args) {
    const char *key = mp_obj_str_get_str(args[0]);
    mp_obj_t default_val = (n_args > 1) ? args[1] : mp_const_none;

    char tag = 0;
    uint8_t buf[BADGE_KV_MAX_VALUE];
    int len = temporalbadge_runtime_kv_get(key, &tag, buf, sizeof(buf));
    if (len < 0) return default_val;

    switch (tag) {
        case 's':
            return mp_obj_new_str((const char *)buf, len);
        case 'b':
            return mp_obj_new_bytes(buf, len);
        case 'i': {
            if (len != 8) return default_val;
            // Reassemble little-endian signed int64 by going through
            // an unsigned accumulator and reinterpreting at the end —
            // this is well-defined and avoids the UB-prone "shift a
            // negative value" path the obvious code triggers.
            unsigned long long u = 0;
            for (int i = 7; i >= 0; --i) {
                u = (u << 8) | buf[i];
            }
            long long v;
            memcpy(&v, &u, sizeof(v));
            return mp_obj_new_int_from_ll(v);
        }
        case 'f': {
            if (len != 8) return default_val;
            double v;
            memcpy(&v, buf, 8);
            return mp_obj_new_float(v);
        }
        default:
            return default_val;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(temporalbadge_kv_get_obj, 1, 2,
                                            temporalbadge_kv_get);

static mp_obj_t temporalbadge_kv_delete(mp_obj_t key_obj) {
    const char *key = mp_obj_str_get_str(key_obj);
    int rc = temporalbadge_runtime_kv_delete(key);
    return mp_obj_new_bool(rc == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_kv_delete_obj,
                                  temporalbadge_kv_delete);

static mp_obj_t temporalbadge_kv_keys(void) {
    char buf[1024];
    int len = temporalbadge_runtime_kv_keys(buf, sizeof(buf));
    if (len < 0) {
        return mp_obj_new_list(0, NULL);
    }
    // Parse the JSON array we just built (cheap because we control the
    // format: never any escapes). Yields list[str].
    mp_obj_t out = mp_obj_new_list(0, NULL);
    const char *p = buf;
    while (*p && *p != '[') ++p;
    if (*p != '[') return out;
    ++p;
    while (*p && *p != ']') {
        while (*p == ',' || *p == ' ') ++p;
        if (*p != '"') break;
        ++p;
        const char *start = p;
        while (*p && *p != '"') ++p;
        if (*p != '"') break;
        mp_obj_list_append(out, mp_obj_new_str(start, p - start));
        ++p;
    }
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_kv_keys_obj,
                                  temporalbadge_kv_keys);

// ── exit ────────────────────────────────────────────────────────────────────

static mp_obj_t temporalbadge_exit(void) {
    mp_raise_type(&mp_type_SystemExit);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(temporalbadge_exit_obj, temporalbadge_exit);

// ── Diagnostic helpers ──────────────────────────────────────────────────────
//
// `badge.set_repl_trace(on)` toggles byte-level REPL tracing in the C
// bridge. Off by default; flip on when investigating "Enter does
// nothing / chars vanish / wrong REPL mode" reports. Implementation lives
// in MicroPythonBridge.cpp.
extern void mpy_set_repl_trace(int on);

static mp_obj_t temporalbadge_set_repl_trace(mp_obj_t enabled_obj) {
    mpy_set_repl_trace(mp_obj_is_true(enabled_obj) ? 1 : 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(temporalbadge_set_repl_trace_obj,
                                  temporalbadge_set_repl_trace);

// ── Module globals table ────────────────────────────────────────────────────

static const mp_rom_map_elem_t temporalbadge_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_badge) },

    // Button constants
    { MP_ROM_QSTR(MP_QSTR_BTN_RIGHT), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_BTN_DOWN),  MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_BTN_LEFT),  MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_BTN_UP),    MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_BTN_CIRCLE),   MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_BTN_CROSS),    MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_BTN_SQUARE),   MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_BTN_TRIANGLE), MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_BTN_CONFIRM),  MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_BTN_SAVE),     MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_BTN_BACK),     MP_ROM_INT(5) },
    { MP_ROM_QSTR(MP_QSTR_BTN_PRESETS),  MP_ROM_INT(3) },

    // LED matrix image constants (match LEDmatrixImages string IDs)
    { MP_ROM_QSTR(MP_QSTR_IMG_SMILEY),    MP_ROM_QSTR(MP_QSTR_smiley) },
    { MP_ROM_QSTR(MP_QSTR_IMG_HEART),     MP_ROM_QSTR(MP_QSTR_heart) },
    { MP_ROM_QSTR(MP_QSTR_IMG_ARROW_UP),  MP_ROM_QSTR(MP_QSTR_arrow_up) },
    { MP_ROM_QSTR(MP_QSTR_IMG_ARROW_DOWN),MP_ROM_QSTR(MP_QSTR_arrow_down) },
    { MP_ROM_QSTR(MP_QSTR_IMG_X_MARK),    MP_ROM_QSTR(MP_QSTR_x_mark) },
    { MP_ROM_QSTR(MP_QSTR_IMG_DOT),       MP_ROM_QSTR(MP_QSTR_dot) },

    // LED matrix animation constants
    { MP_ROM_QSTR(MP_QSTR_ANIM_SPINNER),       MP_ROM_QSTR(MP_QSTR_spinner) },
    { MP_ROM_QSTR(MP_QSTR_ANIM_BLINK_SMILEY),  MP_ROM_QSTR(MP_QSTR_blink_smiley) },
    { MP_ROM_QSTR(MP_QSTR_ANIM_PULSE_HEART),   MP_ROM_QSTR(MP_QSTR_pulse_heart) },

    // Init
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&temporalbadge_init_obj) },

    // OLED -- text
    { MP_ROM_QSTR(MP_QSTR_oled_print),         MP_ROM_PTR(&temporalbadge_oled_print_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_println),        MP_ROM_PTR(&temporalbadge_oled_println_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_clear),          MP_ROM_PTR(&temporalbadge_oled_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_show),           MP_ROM_PTR(&temporalbadge_oled_show_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_set_cursor),     MP_ROM_PTR(&temporalbadge_oled_set_cursor_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_set_text_size),  MP_ROM_PTR(&temporalbadge_oled_set_text_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_get_text_size),  MP_ROM_PTR(&temporalbadge_oled_get_text_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_invert),         MP_ROM_PTR(&temporalbadge_oled_invert_obj) },

    // OLED -- text metrics
    { MP_ROM_QSTR(MP_QSTR_oled_text_width),        MP_ROM_PTR(&temporalbadge_oled_text_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_text_height),       MP_ROM_PTR(&temporalbadge_oled_text_height_obj) },

    // OLED -- fonts
    { MP_ROM_QSTR(MP_QSTR_oled_set_font),         MP_ROM_PTR(&temporalbadge_oled_set_font_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_get_fonts),         MP_ROM_PTR(&temporalbadge_oled_get_fonts_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_get_current_font),  MP_ROM_PTR(&temporalbadge_oled_get_current_font_obj) },

    // OLED -- drawing
    { MP_ROM_QSTR(MP_QSTR_oled_set_pixel),           MP_ROM_PTR(&temporalbadge_oled_set_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_get_pixel),           MP_ROM_PTR(&temporalbadge_oled_get_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_draw_box),            MP_ROM_PTR(&temporalbadge_oled_draw_box_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_set_draw_color),      MP_ROM_PTR(&temporalbadge_oled_set_draw_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_get_framebuffer),      MP_ROM_PTR(&temporalbadge_oled_get_framebuffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_set_framebuffer),      MP_ROM_PTR(&temporalbadge_oled_set_framebuffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_oled_get_framebuffer_size), MP_ROM_PTR(&temporalbadge_oled_get_framebuffer_size_obj) },

    // Native UI chrome helpers
    { MP_ROM_QSTR(MP_QSTR_ui_header),            MP_ROM_PTR(&temporalbadge_ui_header_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_action_bar),        MP_ROM_PTR(&temporalbadge_ui_action_bar_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_chrome),            MP_ROM_PTR(&temporalbadge_ui_chrome_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_inline_hint),       MP_ROM_PTR(&temporalbadge_ui_inline_hint_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_inline_hint_right), MP_ROM_PTR(&temporalbadge_ui_inline_hint_right_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_measure_hint),      MP_ROM_PTR(&temporalbadge_ui_measure_hint_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_grid_cell),         MP_ROM_PTR(&temporalbadge_ui_grid_cell_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_grid_footer),       MP_ROM_PTR(&temporalbadge_ui_grid_footer_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_list_row),          MP_ROM_PTR(&temporalbadge_ui_list_row_obj) },
    { MP_ROM_QSTR(MP_QSTR_ui_list_rows_visible), MP_ROM_PTR(&temporalbadge_ui_list_rows_visible_obj) },

    // Buttons & joystick
    { MP_ROM_QSTR(MP_QSTR_button),         MP_ROM_PTR(&temporalbadge_button_obj) },
    { MP_ROM_QSTR(MP_QSTR_button_pressed), MP_ROM_PTR(&temporalbadge_button_pressed_obj) },
    { MP_ROM_QSTR(MP_QSTR_button_held_ms), MP_ROM_PTR(&temporalbadge_button_held_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_joy_x),          MP_ROM_PTR(&temporalbadge_joy_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_joy_y),          MP_ROM_PTR(&temporalbadge_joy_y_obj) },

    // Explicit HTTP helpers
    { MP_ROM_QSTR(MP_QSTR_http_get),        MP_ROM_PTR(&temporalbadge_http_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_http_post),       MP_ROM_PTR(&temporalbadge_http_post_obj) },

    // LED matrix
    { MP_ROM_QSTR(MP_QSTR_led_brightness),      MP_ROM_PTR(&temporalbadge_led_brightness_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_clear),            MP_ROM_PTR(&temporalbadge_led_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_fill),             MP_ROM_PTR(&temporalbadge_led_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_set_pixel),        MP_ROM_PTR(&temporalbadge_led_set_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_get_pixel),        MP_ROM_PTR(&temporalbadge_led_get_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_show_image),       MP_ROM_PTR(&temporalbadge_led_show_image_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_set_frame),        MP_ROM_PTR(&temporalbadge_led_set_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_start_animation),  MP_ROM_PTR(&temporalbadge_led_start_animation_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_stop_animation),   MP_ROM_PTR(&temporalbadge_led_stop_animation_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_override_begin),   MP_ROM_PTR(&temporalbadge_led_override_begin_obj) },
    { MP_ROM_QSTR(MP_QSTR_led_override_end),     MP_ROM_PTR(&temporalbadge_led_override_end_obj) },
    { MP_ROM_QSTR(MP_QSTR_matrix_app_start),     MP_ROM_PTR(&matrix_app_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_matrix_app_set_speed), MP_ROM_PTR(&matrix_app_set_speed_obj) },
    { MP_ROM_QSTR(MP_QSTR_matrix_app_set_brightness), MP_ROM_PTR(&matrix_app_set_brightness_obj) },
    { MP_ROM_QSTR(MP_QSTR_matrix_app_stop),      MP_ROM_PTR(&matrix_app_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_matrix_app_active),    MP_ROM_PTR(&matrix_app_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_matrix_app_info),      MP_ROM_PTR(&matrix_app_info_obj) },

    // IMU
    { MP_ROM_QSTR(MP_QSTR_imu_ready),     MP_ROM_PTR(&temporalbadge_imu_ready_obj) },
    { MP_ROM_QSTR(MP_QSTR_imu_tilt_x),    MP_ROM_PTR(&temporalbadge_imu_tilt_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_imu_tilt_y),    MP_ROM_PTR(&temporalbadge_imu_tilt_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_imu_accel_z),   MP_ROM_PTR(&temporalbadge_imu_accel_z_obj) },
    { MP_ROM_QSTR(MP_QSTR_imu_face_down), MP_ROM_PTR(&temporalbadge_imu_face_down_obj) },
    { MP_ROM_QSTR(MP_QSTR_imu_motion),    MP_ROM_PTR(&temporalbadge_imu_motion_obj) },

    // Haptics
    { MP_ROM_QSTR(MP_QSTR_haptic_pulse),    MP_ROM_PTR(&temporalbadge_haptic_pulse_obj) },
    { MP_ROM_QSTR(MP_QSTR_haptic_strength), MP_ROM_PTR(&temporalbadge_haptic_strength_obj) },
    { MP_ROM_QSTR(MP_QSTR_haptic_off),      MP_ROM_PTR(&temporalbadge_haptic_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_tone),            MP_ROM_PTR(&temporalbadge_tone_obj) },
    { MP_ROM_QSTR(MP_QSTR_no_tone),         MP_ROM_PTR(&temporalbadge_no_tone_obj) },
    { MP_ROM_QSTR(MP_QSTR_tone_playing),    MP_ROM_PTR(&temporalbadge_tone_playing_obj) },

    // USB serial stdin
    { MP_ROM_QSTR(MP_QSTR_serial_available), MP_ROM_PTR(&temporalbadge_serial_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_serial_read),      MP_ROM_PTR(&temporalbadge_serial_read_obj) },

    // IR
    { MP_ROM_QSTR(MP_QSTR_ir_send),       MP_ROM_PTR(&temporalbadge_ir_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_start),      MP_ROM_PTR(&temporalbadge_ir_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_stop),       MP_ROM_PTR(&temporalbadge_ir_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_available),  MP_ROM_PTR(&temporalbadge_ir_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_read),       MP_ROM_PTR(&temporalbadge_ir_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_send_words), MP_ROM_PTR(&temporalbadge_ir_send_words_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_read_words), MP_ROM_PTR(&temporalbadge_ir_read_words_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_flush),      MP_ROM_PTR(&temporalbadge_ir_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_tx_power),   MP_ROM_PTR(&temporalbadge_ir_tx_power_obj) },

    // IR Playground (consumer NEC + raw symbol modes)
    { MP_ROM_QSTR(MP_QSTR_ir_set_mode),    MP_ROM_PTR(&temporalbadge_ir_set_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_get_mode),    MP_ROM_PTR(&temporalbadge_ir_get_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_nec_send),    MP_ROM_PTR(&temporalbadge_ir_nec_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_nec_read),    MP_ROM_PTR(&temporalbadge_ir_nec_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_raw_capture), MP_ROM_PTR(&temporalbadge_ir_raw_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_raw_send),    MP_ROM_PTR(&temporalbadge_ir_raw_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_activity),    MP_ROM_PTR(&temporalbadge_ir_activity_obj) },

    // Badge identity / boops
    { MP_ROM_QSTR(MP_QSTR_my_uuid), MP_ROM_PTR(&temporalbadge_my_uuid_obj) },
    { MP_ROM_QSTR(MP_QSTR_boops),   MP_ROM_PTR(&temporalbadge_boops_obj) },

    // Apps registry — refresh the main-menu grid after writing a new
    // /apps/<slug>/main.py from JumperIDE.
    { MP_ROM_QSTR(MP_QSTR_rescan_apps), MP_ROM_PTR(&temporalbadge_rescan_apps_obj) },

    // Time
    { MP_ROM_QSTR(MP_QSTR_set_time), MP_ROM_PTR(&temporalbadge_set_time_obj) },

    // NVS-backed key/value store. Survives every kind of flash; use
    // for game saves / scores / user prefs that must persist across
    // a fatfs.bin reflash. See firmware/docs/STORAGE-MODEL.md.
    { MP_ROM_QSTR(MP_QSTR_kv_put),    MP_ROM_PTR(&temporalbadge_kv_put_obj) },
    { MP_ROM_QSTR(MP_QSTR_kv_get),    MP_ROM_PTR(&temporalbadge_kv_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_kv_delete), MP_ROM_PTR(&temporalbadge_kv_delete_obj) },
    { MP_ROM_QSTR(MP_QSTR_kv_keys),   MP_ROM_PTR(&temporalbadge_kv_keys_obj) },

    // Mouse overlay
    { MP_ROM_QSTR(MP_QSTR_mouse_overlay),    MP_ROM_PTR(&temporalbadge_mouse_overlay_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_set_bitmap), MP_ROM_PTR(&temporalbadge_mouse_set_bitmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_x),          MP_ROM_PTR(&temporalbadge_mouse_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_y),          MP_ROM_PTR(&temporalbadge_mouse_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_set_pos),    MP_ROM_PTR(&temporalbadge_mouse_set_pos_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_clicked),    MP_ROM_PTR(&temporalbadge_mouse_clicked_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_set_speed),  MP_ROM_PTR(&temporalbadge_mouse_set_speed_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_set_mode),   MP_ROM_PTR(&temporalbadge_mouse_set_mode_obj) },

    // Mouse mode constants
    { MP_ROM_QSTR(MP_QSTR_MOUSE_ABSOLUTE),   MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_MOUSE_RELATIVE),   MP_ROM_INT(0) },

#if defined(BADGE_ENABLE_MP_DEV)
    // Dev / test harness
    { MP_ROM_QSTR(MP_QSTR_dev),  MP_ROM_PTR(&temporalbadge_dev_obj) },
#endif

    // Control
    { MP_ROM_QSTR(MP_QSTR_exit), MP_ROM_PTR(&temporalbadge_exit_obj) },

    // Diagnostics
    { MP_ROM_QSTR(MP_QSTR_set_repl_trace), MP_ROM_PTR(&temporalbadge_set_repl_trace_obj) },
};
static MP_DEFINE_CONST_DICT(temporalbadge_globals, temporalbadge_globals_table);

const mp_obj_module_t temporalbadge_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&temporalbadge_globals,
};

MP_REGISTER_MODULE(MP_QSTR_badge, temporalbadge_user_cmodule);
