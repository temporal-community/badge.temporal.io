#ifndef DOOM_APP_H
#define DOOM_APP_H

#ifdef BADGE_HAS_DOOM

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*oled_present)(const uint8_t *fb_1bpp_128x64, void *user);
    void *oled_user;

    void (*matrix_present)(const uint8_t pixels_8x8[8], void *user);
    void *matrix_user;

    void (*vibrate)(uint32_t duration_ms, uint8_t strength_0_255, void *user);
    void *vibrate_user;

    void (*get_input)(void *user, float *joy_x, float *joy_y, uint8_t *buttons_bitmask);
    void *input_user;

    const char *wad_path;

    uint32_t target_fps;
    uint8_t  run_by_default;
} doom_app_config_t;

typedef enum {
    DOOM_APP_OK  =  0,
    DOOM_APP_ERR = -1,
} doom_app_status_t;

doom_app_status_t doom_app_init(const doom_app_config_t *cfg);
doom_app_status_t doom_app_enter(void);
doom_app_status_t doom_app_exit(void);
doom_app_status_t doom_app_deinit(void);

bool doom_app_is_running(void);
void doom_app_request_stop(void);
const doom_app_config_t* doom_app_get_config(void);
bool doom_app_stop_requested(void);

#ifdef __cplusplus
}
#endif

#endif // BADGE_HAS_DOOM
#endif // DOOM_APP_H
