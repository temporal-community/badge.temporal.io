#ifdef BADGE_HAS_DOOM

#include "DoomScreen.h"
#include "doom_app.h"
#include "doom_render.h"
#include "doom_resources.h"
#include "../hardware/oled.h"
#include "../hardware/Inputs.h"
#include "../hardware/LEDmatrix.h"
#include "../hardware/Haptics.h"
#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../ble/BadgeBeaconAdv.h"
#endif
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../infra/BadgeConfig.h"
#include "../infra/Filesystem.h"
#include "../screens/AssetLibraryScreen.h"
#include <cstdio>

extern oled badgeDisplay;
extern Inputs inputs;
extern LEDmatrix badgeMatrix;

namespace {
constexpr uint8_t kDoomSettingsCount = 18;
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void DoomScreen::oledPresentCb(const uint8_t* fb, void* user) {
    (void)user;
    uint8_t* buf = badgeDisplay.getBufferPtr();
    if (buf && fb) {
        memcpy(buf, fb, 1024);
        badgeDisplay.sendBuffer();
    }
}

void DoomScreen::matrixPresentCb(const uint8_t pixels[8], void* user) {
    (void)user;
#ifdef BADGE_HAS_LED_MATRIX
    badgeMatrix.drawMaskHardware(pixels, badgeMatrix.getBrightness(), 0);
#endif
}

void DoomScreen::vibrateCb(uint32_t duration_ms, uint8_t strength, void* user) {
    (void)user;
    (void)duration_ms;
#ifdef BADGE_HAS_HAPTICS
    Haptics::shortPulse(strength, duration_ms);
#endif
}

void DoomScreen::getInputCb(void* user, float* joy_x, float* joy_y, uint8_t* buttons) {
    (void)user;
    // Read hardware directly — the scheduler/Inputs service is paused
    // while Doom runs, so we sample GPIOs and ADC ourselves.
    uint16_t rawX = analogRead(JOY_X);
    uint16_t rawY = analogRead(JOY_Y);
    // Board joystick is rotated 90 degrees (same transform as Inputs.cpp)
    uint16_t jx = 4095 - rawY;
    uint16_t jy = rawX;
    *joy_x = ((float)jx - 2047.5f) / 2047.5f;
    *joy_y = ((float)jy - 2047.5f) / 2047.5f;

    uint8_t mask = 0;
    if (digitalRead(BUTTON_UP)    == LOW) mask |= 0x01;
    if (digitalRead(BUTTON_DOWN)  == LOW) mask |= 0x02;
    if (digitalRead(BUTTON_LEFT)  == LOW) mask |= 0x04;
    if (digitalRead(BUTTON_RIGHT) == LOW) mask |= 0x08;
    *buttons = mask;
}

// ── WAD file lookup ──────────────────────────────────────────────────────────

static const char* findWadPath() {
    static const char* candidates[] = {
        "/doom1.wad", "/Doom1.wad", "/DOOM1.WAD", "/DOOM1.wad",
    };
    for (auto path : candidates) {
        if (Filesystem::fileExists(path)) {
            Serial.printf("[DoomScreen] WAD found: %s\n", path);
            return path;
        }
    }
    Serial.println("[DoomScreen] WARNING: no WAD found on filesystem");
    return nullptr;
}

// ── Screen lifecycle ─────────────────────────────────────────────────────────

void DoomScreen::onEnter(GUIManager& gui) {
    phase_ = Phase::kHelp;
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForForeground(true);
#endif
    (void)gui;
}

void DoomScreen::onExit(GUIManager& gui) {
    if (phase_ == Phase::kRunning || phase_ == Phase::kExiting) {
        doom_app_exit();
        doom_app_deinit();
        doom_resources_exit();
    }
    phase_ = Phase::kHelp;
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForForeground(false);
#endif
    (void)gui;
}

bool DoomScreen::needsRender() {
    return phase_ == Phase::kHelp || phase_ == Phase::kSettings ||
           phase_ == Phase::kNoWad;
}

void DoomScreen::render(oled& d, GUIManager& gui) {
    if (phase_ == Phase::kHelp)
        renderHelpScreen(d);
    else if (phase_ == Phase::kSettings)
        renderSettingsScreen(d);
    else if (phase_ == Phase::kNoWad)
        renderNoWadScreen(d);
    (void)gui;
}

void DoomScreen::handleInput(const Inputs& inp, int16_t cursorX,
                             int16_t cursorY, GUIManager& gui) {
    (void)cursorX;
    (void)cursorY;
    const Inputs::ButtonEdges& e = inp.edges();

    if (phase_ == Phase::kHelp) {
        if (e.cancelPressed) {
            gui.popScreen();
        } else if (e.xPressed) {
            phase_ = Phase::kSettings;
            settingsCursor_ = 0;
        } else if (e.confirmPressed) {
            launchDoom(gui);
        }
    } else if (phase_ == Phase::kNoWad) {
        if (e.confirmPressed && badgeConfig.communityAppsUrl()[0]) {
            // Jump straight to the registry detail for the WAD entry,
            // skipping the library list. The detail screen renders
            // size + description so the user still sees what they're
            // committing to before the 4 MB download starts.
            const ota::AssetEntry* entry =
                ota::registry::findById("doom1-shareware");
            if (entry) {
                AssetDetailScreen::setActiveAsset(entry);
                gui.popScreen();
                gui.pushScreen(kScreenAssetDetail);
            } else {
                // Registry not yet refreshed (no WiFi at boot, or first
                // run). Fall back to the library so a refresh fires.
                AssetLibraryScreen::selectAssetById("doom1-shareware");
                gui.popScreen();
                gui.pushScreen(kScreenAssetLibrary);
            }
        } else if (e.cancelPressed || e.confirmPressed ||
                   e.xPressed || e.yPressed) {
            gui.popScreen();
        }
    } else if (phase_ == Phase::kSettings) {
        doom_render_settings_t* s = doom_render_settings();

        if (e.cancelPressed) {
            phase_ = Phase::kHelp;
            return;
        }
        if (e.confirmPressed && settingsCursor_ == kDoomSettingsCount - 1) {
            launchDoom(gui);
            return;
        }

        if (e.upPressed) {
            if (settingsCursor_ > 0) settingsCursor_--;
        }
        if (e.downPressed) {
            if (settingsCursor_ < kDoomSettingsCount - 1) settingsCursor_++;
        }

        int8_t dir = 0;
        if (e.rightPressed) dir = 1;
        if (e.leftPressed)  dir = -1;

        if (dir) {
            switch (settingsCursor_) {
                case 0: {
                    int v = (int)s->dither + dir;
                    if (v < 0) v = 2; if (v > 2) v = 0;
                    s->dither = (doom_dither_mode_t)v;
                    break;
                }
                case 1: {
                    int v = (int)s->splash_dither + dir;
                    if (v < 0) v = 2; if (v > 2) v = 0;
                    s->splash_dither = (doom_dither_mode_t)v;
                    break;
                }
                case 2: {
                    int v = (int)s->gamma_low + dir;
                    if (v < 0) v = 0; if (v > 4) v = 4;
                    s->gamma_low = (uint8_t)v;
                    doom_render_rebuild_gamma();
                    break;
                }
                case 3: {
                    int v = (int)s->gamma_high + dir;
                    if (v < -4) v = -4; if (v > 4) v = 4;
                    s->gamma_high = (int8_t)v;
                    doom_render_rebuild_gamma();
                    break;
                }
                case 4: {
                    int v = (int)s->threshold + dir;
                    if (v < 10) v = 10; if (v > 250) v = 250;
                    s->threshold = (uint8_t)v;
                    break;
                }
                case 5: {
                    int v = (int)s->menu_zoom + dir;
                    if (v < 0) v = 0; if (v > 4) v = 4;
                    s->menu_zoom = (uint8_t)v;
                    break;
                }
                case 6: {
                    s->auto_gamma = s->auto_gamma ? 0 : 1;
                    break;
                }
                case 7: {
                    int v = (int)s->auto_deadband + dir * 2;
                    if (v < 0) v = 0; if (v > 40) v = 40;
                    s->auto_deadband = (uint8_t)v;
                    break;
                }
                case 8: {
                    float v = s->auto_gamma_speed + dir * 0.01f;
                    if (v < 0.005f) v = 0.005f; if (v > 1.0f) v = 1.0f;
                    s->auto_gamma_speed = v;
                    break;
                }
                case 9: {
                    int v = (int)s->splash_target + dir * 5;
                    if (v < 0) v = 0; if (v > 100) v = 100;
                    s->splash_target = (uint8_t)v;
                    break;
                }
                case 10: {
                    int v = (int)s->haptic_fire + dir * 20;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    s->haptic_fire = (uint8_t)v;
                    break;
                }
                case 11: {
                    int v = (int)s->haptic_dmg + dir * 20;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    s->haptic_dmg = (uint8_t)v;
                    break;
                }
                case 12: {
                    int v = (int)s->haptic_use + dir * 20;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    s->haptic_use = (uint8_t)v;
                    break;
                }
                case 13: {
                    s->sound_enable = s->sound_enable ? 0 : 1;
                    break;
                }
                case 14: {
                    int v = (int)s->sound_volume + dir * 20;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    s->sound_volume = (uint8_t)v;
                    break;
                }
                case 15: {
                    int v = (int)s->sound_sample_rate + dir * 1000;
                    if (v < 4000) v = 4000; if (v > 44100) v = 44100;
                    s->sound_sample_rate = (int16_t)v;
                    break;
                }
                case 16: {
                    int v = (int)s->sound_duty + dir * 2;
                    if (v < 1) v = 1; if (v > 200) v = 200;
                    s->sound_duty = (uint8_t)v;
                    break;
                }
                case 17: {
                    int v = (int)s->sound_octave + dir;
                    if (v < -3) v = -3; if (v > 3) v = 3;
                    s->sound_octave = (int8_t)v;
                    break;
                }
            }
        }
    }
}

void DoomScreen::renderHelpScreen(oled& d) {
    d.setDrawColor(1);

    d.setFontPreset(FONT_SMALL);
    d.drawStr(20, 10, "DOOM Controls");
    d.drawHLine(0, 13, 128);

    d.setFontPreset(FONT_TINY);
    d.drawStr(2, 22, "Stick: Move / Turn");
    ButtonGlyphs::drawInlineHintCompact(d, 2, 32, "Y:Fire   A:Use/Open");
    ButtonGlyphs::drawInlineHintCompact(d, 2, 42, "X/B:Strafe  hold=Exit");
    d.drawStr(0, 51, "Src: github.com/id-software/doom");

    d.drawHLine(0, 54, 128);
    ButtonGlyphs::drawInlineHint(d, 2, 63, "Cancel:Back");
    const int playW = ButtonGlyphs::measureInlineHint(d, "Confirm:Play");
    ButtonGlyphs::drawInlineHint(d, (128 - playW) / 2, 63, "Confirm:Play");
    ButtonGlyphs::drawInlineHintRight(d, 126, 63, "X:Settings");
}

void DoomScreen::renderNoWadScreen(oled& d) {
    d.setDrawColor(1);

    d.setFontPreset(FONT_SMALL);
    const char* title = "No DOOM WAD";
    int tw = d.getStrWidth(title);
    d.drawStr((128 - tw) / 2, 11, title);
    d.drawHLine(0, 14, 128);

    d.setFontPreset(FONT_TINY);
    if (badgeConfig.communityAppsUrl()[0]) {
        d.drawStr(2, 24, "doom1.wad not on badge.");
        d.drawStr(2, 36, "Press Confirm to open");
        d.drawStr(2, 45, "Community Apps and");
        d.drawStr(2, 54, "download it over WiFi.");
        d.drawHLine(0, 56, 128);
        ButtonGlyphs::drawInlineHint(d, 2, 63, "Cancel:Back");
        ButtonGlyphs::drawInlineHintRight(d, 126, 63,
                                          "Confirm:Apps");
    } else {
        d.drawStr(2, 24, "doom1.wad not found on");
        d.drawStr(2, 33, "the badge filesystem.");
        d.drawStr(2, 45, "Upload one by going to");
        d.drawStr(2, 54, "ide.jumperless.org");
        d.drawHLine(0, 56, 128);
        ButtonGlyphs::drawInlineHint(d, 2, 63, "Any:Back");
    }
}

void DoomScreen::renderSettingsScreen(oled& d) {
    doom_render_settings_t* s = doom_render_settings();

    static const char* kDitherNames[] = { "Off", "2x2", "4x4" };
    static const char* kGammaLoNames[] = { "None", "Light", "Med", "Heavy", "Max" };
    static const char* kGammaHiNames[] = {
        "-Max", "-Hvy", "-Med", "-Lt", "None", "+Lt", "+Med", "+Hvy", "+Max"
    };
    static const char* kZoomNames[] = { "Off", "1.25x", "1.5x", "2x", "3x" };

    OLEDLayout::drawStatusHeader(d, "DOOM Settings");
    d.setFontPreset(FONT_TINY);

    struct Row { const char* label; const char* value; };
    char thrBuf[8], dbBuf[8], spdBuf[8], splBuf[8];
    char fireBuf[8], dmgBuf[8], useBuf[8];
    char svolBuf[8], srateBuf[8], sdutyBuf[8];
    std::snprintf(thrBuf, sizeof(thrBuf), "%d", s->threshold);
    std::snprintf(dbBuf, sizeof(dbBuf), "+/-%d%%", s->auto_deadband);
    std::snprintf(spdBuf, sizeof(spdBuf), "%.3f", s->auto_gamma_speed);
    std::snprintf(splBuf, sizeof(splBuf), "%d%%", s->splash_target);
    std::snprintf(fireBuf, sizeof(fireBuf), "%d", s->haptic_fire);
    std::snprintf(dmgBuf, sizeof(dmgBuf), "%d", s->haptic_dmg);
    std::snprintf(useBuf, sizeof(useBuf), "%d", s->haptic_use);
    std::snprintf(svolBuf, sizeof(svolBuf), "%d", s->sound_volume);
    std::snprintf(srateBuf, sizeof(srateBuf), "%d", s->sound_sample_rate ? s->sound_sample_rate : 11025);
    std::snprintf(sdutyBuf, sizeof(sdutyBuf), "%d", s->sound_duty);
    char octBuf[8];
    std::snprintf(octBuf, sizeof(octBuf), "%+d", s->sound_octave);

    Row rows[] = {
        { "Dither",      kDitherNames[(int)s->dither] },
        { "Splash",      kDitherNames[(int)s->splash_dither] },
        { "Gamma Low",   kGammaLoNames[s->gamma_low] },
        { "Gamma High",  kGammaHiNames[s->gamma_high + 4] },
        { "Threshold",   thrBuf },
        { "Menu Zoom",   kZoomNames[s->menu_zoom] },
        { "Auto Gamma",  s->auto_gamma ? "On" : "Off" },
        { "Deadband",    dbBuf },
        { "AG Speed",    spdBuf },
        { "Splash Tgt",  splBuf },
        { "Vibe Fire",   fireBuf },
        { "Vibe Damage", dmgBuf },
        { "Vibe Use",    useBuf },
        { "Sound",       s->sound_enable ? "On" : "Off" },
        { "Snd Volume",  svolBuf },
        { "Snd Rate",    srateBuf },
        { "Snd Duty",    sdutyBuf },
        { "Snd Octave",  octBuf },
    };
    constexpr int kVisRows = 6;
    constexpr int kRowY0 = OLEDLayout::kContentTopY + 7;
    constexpr int kRowH  = 7;

    int scroll = 0;
    if (settingsCursor_ >= kVisRows)
        scroll = settingsCursor_ - kVisRows + 1;

    for (int vi = 0; vi < kVisRows && (scroll + vi) < kDoomSettingsCount; vi++) {
        int i = scroll + vi;
        int y = kRowY0 + vi * kRowH;
        if (i == settingsCursor_) {
            OLEDLayout::drawSelectedRow(d, y - 6, kRowH);
        }

        char label[16];
        char value[16];
        std::snprintf(label, sizeof(label), "%s", rows[i].label);
        std::snprintf(value, sizeof(value), "%s", rows[i].value);
        OLEDLayout::fitText(d, label, sizeof(label), 76);
        OLEDLayout::fitText(d, value, sizeof(value), 46);

        d.drawStr(4, y, label);
        int vw = d.getStrWidth(value);
        d.drawStr(124 - vw, y, value);
        d.setDrawColor(1);
    }

    OLEDLayout::clearFooter(d);
    const char* leftHint = settingsCursor_ == 0 ? "Cancel:Back" : "U/D:Nav";
    const char* rightHint =
        settingsCursor_ == kDoomSettingsCount - 1 ? "Confirm:Play" : "L/R:Adj";
    ButtonGlyphs::drawInlineHint(d, 2, OLEDLayout::kFooterBaseY, leftHint);
    ButtonGlyphs::drawInlineHintRight(d, 126, OLEDLayout::kFooterBaseY, rightHint);
}

void DoomScreen::launchDoom(GUIManager& gui) {
    const char* wad_path = findWadPath();
    if (!wad_path) {
        Serial.println("[DoomScreen] no WAD on filesystem; showing kNoWad screen");
        phase_ = Phase::kNoWad;
        (void)gui;
        return;
    }

    gui.deactivate();
    doom_resources_enter();

    delay(100);
    Serial.println("[DoomScreen] background services stopped for Doom");

    doom_app_config_t cfg = {};
    cfg.oled_present = oledPresentCb;
    cfg.oled_user = nullptr;
    cfg.matrix_present = matrixPresentCb;
    cfg.matrix_user = nullptr;
    cfg.vibrate = vibrateCb;
    cfg.vibrate_user = nullptr;
    cfg.get_input = getInputCb;
    cfg.input_user = nullptr;
    cfg.wad_path = wad_path;
    cfg.target_fps = 20;
    cfg.run_by_default = 1;

    if (doom_app_init(&cfg) == DOOM_APP_OK) {
        if (doom_app_enter() == DOOM_APP_OK) {
            phase_ = Phase::kRunning;
            Serial.println("[DoomScreen] game launched");
            return;
        }
        doom_app_deinit();
    }

    Serial.println("[DoomScreen] failed to launch doom");
    doom_resources_exit();
    gui.activate();
}

#endif // BADGE_HAS_DOOM
