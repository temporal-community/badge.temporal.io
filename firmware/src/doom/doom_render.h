#ifndef DOOM_RENDER_H
#define DOOM_RENDER_H

#ifdef BADGE_HAS_DOOM

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOOM_DITHER_OFF  = 0,
    DOOM_DITHER_2X2  = 1,
    DOOM_DITHER_4X4  = 2,
} doom_dither_mode_t;

typedef struct {
    doom_dither_mode_t dither;         // gameplay dither: off, 2x2, 4x4
    doom_dither_mode_t splash_dither;  // splash/title dither: off, 2x2, 4x4
    uint8_t  gamma_low;   // shadow lift: 0=none, 1=light, 2=med, 3=heavy, 4=max
    int8_t   gamma_high;  // highlight: -4..-1=darken, 0=linear, 1..4=brighten
    uint8_t  threshold;   // binary threshold when dither is off (0-255)
    uint8_t  menu_zoom;   // 0=off, 1=1.25x, 2=1.5x, 3=2x, 4=3x
    uint8_t  auto_gamma;      // 1=auto-adjust threshold per frame
    uint8_t  auto_deadband;   // +/- % deadband around target (0-40)
    uint8_t  splash_target;   // target white% for splash screens (0-100, 0=use gameplay 50%)
    uint8_t  haptic_fire;  // fire vibrate strength (0-255, 0=off)
    uint8_t  haptic_dmg;  // damage vibrate strength (0-255, 0=off)
    uint8_t  haptic_use;  // door/use vibrate strength (0-255, 0=off)
    uint8_t  sound_enable;      // 1=CoilTone sound output on
    uint8_t  sound_volume;      // scales CoilTone duty (0-255)
    uint8_t  sound_duty;        // CoilTone duty cycle (1-200, higher=louder but more motor spin)
    int8_t   sound_octave;     // octave shift for music: -3..+3 (default 0)
    int32_t  haptic_freq; // motor PWM frequency Hz (0=default)
    int16_t  sound_sample_rate; // SFX pitch scale (0=native, multiplied by this/11025)
    float    auto_gamma_speed; // proportional gain for threshold controller (0.005-1.0)
} doom_render_settings_t;

void doom_render_init(void);
void doom_render_deinit(void);

doom_render_settings_t* doom_render_settings(void);
void doom_render_rebuild_gamma(void);

// render_hint: 0=gameplay, 1=menu overlay, 2=splash/title
void doom_render_frame(const uint32_t* rgba_src, uint8_t* oled_fb_out,
                       int render_hint);

#define DOOM_HINT_GAMEPLAY 0
#define DOOM_HINT_MENU     1
#define DOOM_HINT_SPLASH   2

#ifdef __cplusplus
}
#endif

#endif // BADGE_HAS_DOOM
#endif // DOOM_RENDER_H
