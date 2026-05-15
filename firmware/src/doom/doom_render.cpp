#ifdef BADGE_HAS_DOOM

#include "doom_render.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <cmath>

extern "C" {
#include "doomgeneric.h"
}

static constexpr int kSrcW = DOOMGENERIC_RESX;  // 160
static constexpr int kSrcH = DOOMGENERIC_RESY;  // 100
static constexpr int kDstW = 128;
static constexpr int kDstH = 64;

static uint8_t* s_luma_buf = nullptr;
static uint8_t  s_gamma_lut[256];
static float    s_adj_thresh = 0.0f;

static doom_render_settings_t s_settings = {
    .dither           = DOOM_DITHER_OFF,
    .splash_dither    = DOOM_DITHER_OFF,
    .gamma_low        = 3,
    .gamma_high       = -1,
    .threshold        = 120,
    .menu_zoom        = 2,
    .auto_gamma       = 1,
    .auto_deadband    = 11,
    .splash_target    = 21,
    .haptic_fire      = 255,
    .haptic_dmg       = 255,
    .haptic_use       = 180,
    .sound_enable     = 1,
    .sound_volume     = 255,
    .sound_duty       = 18,
    .sound_octave     = 0,
    .haptic_freq      = 90,
    .sound_sample_rate = 10000,
    .auto_gamma_speed = 0.27f,
};

static const uint8_t kBayer2x2[2][2] = {
    { 0, 2 },
    { 3, 1 },
};

static const uint8_t kBayer4x4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static const float kGammaExponents[] = {
    1.0f, 0.7f, 0.5f, 0.35f, 0.25f
};

static void rebuild_gamma_lut(void) {
    int lo = s_settings.gamma_low;
    int hi = s_settings.gamma_high;
    if (lo < 0) lo = 0; if (lo > 4) lo = 4;
    if (hi < -4) hi = -4; if (hi > 4) hi = 4;
    float exp_lo = kGammaExponents[lo];

    for (int i = 0; i < 256; i++) {
        float v = (float)i / 255.0f;
        float lifted = powf(v, exp_lo);

        float highlight;
        if (hi == 0) {
            highlight = v;
        } else if (hi < 0) {
            // Darken highlights: pow(v, 1/exp) where exp<1 gives exponent>1
            float exp_h = kGammaExponents[-hi];
            highlight = powf(v, 1.0f / exp_h);
        } else {
            // Brighten highlights: 1 - pow(1-v, 1/exp)
            float exp_h = kGammaExponents[hi];
            highlight = 1.0f - powf(1.0f - v, 1.0f / exp_h);
        }

        float blend = v * v;
        float g = lifted * (1.0f - blend) + highlight * blend;
        int out = (int)(g * 255.0f + 0.5f);
        s_gamma_lut[i] = (uint8_t)(out > 255 ? 255 : (out < 0 ? 0 : out));
    }
}

void doom_render_rebuild_gamma(void) { rebuild_gamma_lut(); }

void doom_render_init(void) {
    if (!s_luma_buf) {
        s_luma_buf = (uint8_t*)heap_caps_malloc(kDstW * kDstH, MALLOC_CAP_SPIRAM);
        if (!s_luma_buf) {
            s_luma_buf = (uint8_t*)heap_caps_malloc(kDstW * kDstH, MALLOC_CAP_8BIT);
        }
    }
    s_adj_thresh = (float)s_settings.threshold;
    rebuild_gamma_lut();
}

void doom_render_deinit(void) {
    if (s_luma_buf) {
        heap_caps_free(s_luma_buf);
        s_luma_buf = nullptr;
    }
}

doom_render_settings_t* doom_render_settings(void) {
    return &s_settings;
}

static void downsample_region(const uint32_t* rgba_src,
                              int crop_x, int crop_y,
                              int crop_w, int crop_h) {
    const uint32_t x_step = ((uint32_t)crop_w << 16) / kDstW;
    const uint32_t y_step = ((uint32_t)crop_h << 16) / kDstH;

    for (int dy = 0; dy < kDstH; dy++) {
        uint32_t sy0 = crop_y + ((dy * y_step) >> 16);
        uint32_t sy1 = crop_y + (((dy + 1) * y_step) >> 16);
        if (sy1 > (uint32_t)(crop_y + crop_h)) sy1 = crop_y + crop_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < kDstW; dx++) {
            uint32_t sx0 = crop_x + ((dx * x_step) >> 16);
            uint32_t sx1 = crop_x + (((dx + 1) * x_step) >> 16);
            if (sx1 > (uint32_t)(crop_x + crop_w)) sx1 = crop_x + crop_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            uint32_t acc = 0;
            uint32_t cnt = (sy1 - sy0) * (sx1 - sx0);
            for (uint32_t sy = sy0; sy < sy1; sy++) {
                const uint32_t* row = rgba_src + sy * kSrcW;
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    uint32_t px = row[sx];
                    acc += (77*((px>>16)&0xFF) + 150*((px>>8)&0xFF) + 29*(px&0xFF)) >> 8;
                }
            }
            s_luma_buf[dy * kDstW + dx] = s_gamma_lut[acc / cnt];
        }
    }
}

static void quantize_threshold(uint8_t* oled_fb, uint8_t thresh) {
    for (int y = 0; y < kDstH; y++) {
        uint8_t page = y >> 3;
        uint8_t bit  = 1 << (y & 7);
        for (int x = 0; x < kDstW; x++) {
            if (s_luma_buf[y * kDstW + x] > thresh)
                oled_fb[page * kDstW + x] |= bit;
        }
    }
}

static void quantize_bayer2(uint8_t* oled_fb) {
    for (int y = 0; y < kDstH; y++) {
        uint8_t page = y >> 3;
        uint8_t bit  = 1 << (y & 7);
        for (int x = 0; x < kDstW; x++) {
            uint8_t t = kBayer2x2[y & 1][x & 1] * 64;
            if (s_luma_buf[y * kDstW + x] > t)
                oled_fb[page * kDstW + x] |= bit;
        }
    }
}

static void quantize_bayer4(uint8_t* oled_fb) {
    for (int y = 0; y < kDstH; y++) {
        uint8_t page = y >> 3;
        uint8_t bit  = 1 << (y & 7);
        for (int x = 0; x < kDstW; x++) {
            uint8_t t = kBayer4x4[y & 3][x & 3] * 16;
            if (s_luma_buf[y * kDstW + x] > t)
                oled_fb[page * kDstW + x] |= bit;
        }
    }
}

void doom_render_frame(const uint32_t* rgba_src, uint8_t* oled_fb_out,
                       int render_hint) {
    if (!rgba_src || !oled_fb_out || !s_luma_buf) return;

    // Zoom for interactive menus only, not splash/title screens
    bool zoom = (render_hint == DOOM_HINT_MENU && s_settings.menu_zoom > 0);
    int crop_x = 0, crop_y = 0, crop_w = kSrcW, crop_h = kSrcH;

    if (zoom) {
        // zoom levels: 1=1.25x, 2=1.5x, 3=2x, 4=3x
        static const int kZoomW[] = { 160, 128, 107, 80, 54 };
        static const int kZoomH[] = { 100,  80,  67, 50, 34 };
        int z = s_settings.menu_zoom;
        if (z > 4) z = 4;
        crop_w = kZoomW[z];
        crop_h = kZoomH[z];
        crop_x = (kSrcW - crop_w) / 2;
        crop_y = (kSrcH - crop_h) / 2;
    }

    downsample_region(rgba_src, crop_x, crop_y, crop_w, crop_h);

    // Auto-gamma: float threshold converges toward target white%.
    // Uses proportional gain so it always moves but progressively
    // slower as it approaches equilibrium -- no deadband needed.
    if (s_settings.auto_gamma) {
        int target_pct = (render_hint == DOOM_HINT_SPLASH && s_settings.splash_target > 0)
                         ? (int)s_settings.splash_target : 40;

        int total = kDstW * kDstH;
        int above = 0;
        uint8_t cur_thresh = (uint8_t)(s_adj_thresh + 0.5f);
        for (int i = 0; i < total; i++) {
            if (s_luma_buf[i] > cur_thresh) above++;
        }

        float error = (float)(above * 100) / (float)total - (float)target_pct;
        float gain = s_settings.auto_gamma_speed;
        if (gain < 0.005f) gain = 0.005f;
        if (gain > 1.0f)   gain = 1.0f;
        float deadband = (float)s_settings.auto_deadband;
        float adj = 0.0f;
        if (error > deadband)
            adj = (error - deadband) * gain;
        else if (error < -deadband)
            adj = (error + deadband) * gain;
        s_adj_thresh += adj;
        if (s_adj_thresh < 20.0f)  s_adj_thresh = 20.0f;
        if (s_adj_thresh > 235.0f) s_adj_thresh = 235.0f;

        memset(oled_fb_out, 0, 1024);
        doom_dither_mode_t dm = (render_hint == DOOM_HINT_SPLASH)
                                ? s_settings.splash_dither : s_settings.dither;
        switch (dm) {
            case DOOM_DITHER_2X2: quantize_bayer2(oled_fb_out); break;
            case DOOM_DITHER_4X4: quantize_bayer4(oled_fb_out); break;
            default:              quantize_threshold(oled_fb_out, (uint8_t)(s_adj_thresh + 0.5f)); break;
        }
    } else {
        memset(oled_fb_out, 0, 1024);
        if (render_hint == DOOM_HINT_MENU) {
            quantize_threshold(oled_fb_out, s_settings.threshold);
        } else if (render_hint == DOOM_HINT_SPLASH) {
            switch (s_settings.splash_dither) {
                case DOOM_DITHER_2X2: quantize_bayer2(oled_fb_out); break;
                case DOOM_DITHER_4X4: quantize_bayer4(oled_fb_out); break;
                default:              quantize_threshold(oled_fb_out, s_settings.threshold); break;
            }
        } else {
            switch (s_settings.dither) {
                case DOOM_DITHER_2X2: quantize_bayer2(oled_fb_out); break;
                case DOOM_DITHER_4X4: quantize_bayer4(oled_fb_out); break;
                default:              quantize_threshold(oled_fb_out, s_settings.threshold); break;
            }
        }
    }
}

#endif // BADGE_HAS_DOOM
