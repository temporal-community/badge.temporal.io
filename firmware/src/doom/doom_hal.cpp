// DoomGeneric include kept outside #ifdef so PlatformIO LDF detects the
// library dependency during its regex-based header scan.
extern "C" {
#include "doomgeneric.h"
}

#ifdef BADGE_HAS_DOOM

#include "doom_app.h"
#include "doom_render.h"
#include "doom_input.h"
#include "doom_hud_matrix.h"
#include "hardware/Haptics.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

// Pull Doom engine state for the HUD. Doom's "boolean" clashes with
// Arduino's, so we suppress the redefinition for this TU.
#define boolean doom_boolean_t
extern "C" {
#include "doomstat.h"
#include "doomdef.h"
#include "d_player.h"
#include "d_items.h"
}
#undef boolean

#ifdef FEATURE_SOUND
void doom_sound_haptic_boost(uint16_t duration_ms);
#endif

static constexpr size_t kOledFbBytes = 1024;
static uint8_t* s_oled_fb = nullptr;

// Message hook: hu_stuff.c writes here before clearing plr->message
volatile const char* doom_pending_message = nullptr;

extern "C" void DG_Init(void) {
    Serial.println("[doom_hal] DG_Init");
    doom_pending_message = nullptr;
    if (!s_oled_fb) {
        s_oled_fb = static_cast<uint8_t*>(
            heap_caps_malloc(kOledFbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_oled_fb) {
            s_oled_fb = static_cast<uint8_t*>(
                heap_caps_malloc(kOledFbBytes, MALLOC_CAP_8BIT));
        }
    }
    if (s_oled_fb) {
        memset(s_oled_fb, 0, kOledFbBytes);
    } else {
        Serial.println("[doom_hal] WARNING: OLED framebuffer allocation failed");
    }
}

extern "C" void DG_DrawFrame(void) {
    if (doom_app_stop_requested()) return;
    if (!s_oled_fb) return;

    int hint;
    if (menuactive)
        hint = DOOM_HINT_MENU;
    else if (gamestate != GS_LEVEL)
        hint = DOOM_HINT_SPLASH;
    else
        hint = DOOM_HINT_GAMEPLAY;
    doom_render_frame(DG_ScreenBuffer, s_oled_fb, hint);

    const doom_app_config_t* cfg = doom_app_get_config();
    if (cfg && cfg->oled_present) {
        cfg->oled_present(s_oled_fb, cfg->oled_user);
    }

    // Check for pending HUD message from hu_stuff.c hook
    const char* pending = (const char*)doom_pending_message;
    if (pending) {
        doom_hud_set_message(pending);
        doom_pending_message = nullptr;
    }

    uint8_t health = 0, armor = 0, ammo = 0, keys = 0;
    if (gamestate == GS_LEVEL) {
        player_t* p = &players[consoleplayer];
        int h = p->mo ? p->mo->health : p->health;
        health = (uint8_t)(h > 100 ? 100 : (h < 0 ? 0 : h));

        int ap = p->armorpoints;
        armor = (uint8_t)(ap > 100 ? 100 : (ap < 0 ? 0 : ap));

        ammotype_t at = weaponinfo[p->readyweapon].ammo;
        if (at != am_noammo && p->maxammo[at] > 0) {
            int pct = p->ammo[at] * 100 / p->maxammo[at];
            ammo = (uint8_t)(pct > 100 ? 100 : (pct < 0 ? 0 : pct));
        }

        if (p->cards[it_redcard]    || p->cards[it_redskull])    keys |= 0x01;
        if (p->cards[it_bluecard]   || p->cards[it_blueskull])   keys |= 0x02;
        if (p->cards[it_yellowcard] || p->cards[it_yellowskull]) keys |= 0x04;

        doom_render_settings_t* rs = doom_render_settings();

#ifdef FEATURE_SOUND
        static int s_prev_extralight = 0;
        if (p->extralight > 0 && s_prev_extralight == 0 && rs->haptic_fire) {
            doom_sound_haptic_boost(100);
        }
        s_prev_extralight = p->extralight;

        static int s_prev_damagecount = 0;
        if (p->damagecount > s_prev_damagecount && rs->haptic_dmg) {
            doom_sound_haptic_boost(200);
        }
        s_prev_damagecount = p->damagecount;
#endif
    }
    doom_hud_update(health, armor, ammo, keys);

    doom_input_poll();

    Haptics::checkPulseEnd();
    CoilTone::checkToneEnd();
}

extern "C" void DG_SleepMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

extern "C" uint32_t DG_GetTicksMs(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

extern "C" int DG_GetKey(int* pressed, unsigned char* key) {
    return doom_input_get_key(pressed, key);
}

extern "C" void DG_SetWindowTitle(const char* title) {
    (void)title;
}

void doom_hal_deinit(void) {
    if (s_oled_fb) {
        heap_caps_free(s_oled_fb);
        s_oled_fb = nullptr;
        Serial.printf("[doom_hal] released %u byte OLED framebuffer\n",
                      (unsigned)kOledFbBytes);
    }
}

#endif // BADGE_HAS_DOOM
