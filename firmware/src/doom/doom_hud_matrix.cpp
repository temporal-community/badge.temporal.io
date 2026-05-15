#ifdef BADGE_HAS_DOOM

#include "doom_hud_matrix.h"
#include "doom_app.h"
#include <Arduino.h>
#include <cstring>

// ── 8px-tall font for scrolling text ─────────────────────────────────────────
// Each glyph is 8 bits tall (one byte per column), variable width 1-5 cols.
// Stored as: { width, col0, col1, ... }

static const uint8_t kFont_A[] = { 4, 0x7E, 0x11, 0x11, 0x7E };
static const uint8_t kFont_B[] = { 4, 0x7F, 0x49, 0x49, 0x36 };
static const uint8_t kFont_C[] = { 4, 0x3E, 0x41, 0x41, 0x22 };
static const uint8_t kFont_D[] = { 4, 0x7F, 0x41, 0x41, 0x3E };
static const uint8_t kFont_E[] = { 4, 0x7F, 0x49, 0x49, 0x41 };
static const uint8_t kFont_F[] = { 4, 0x7F, 0x09, 0x09, 0x01 };
static const uint8_t kFont_G[] = { 4, 0x3E, 0x41, 0x51, 0x72 };
static const uint8_t kFont_H[] = { 4, 0x7F, 0x08, 0x08, 0x7F };
static const uint8_t kFont_I[] = { 3, 0x41, 0x7F, 0x41 };
static const uint8_t kFont_J[] = { 4, 0x20, 0x40, 0x40, 0x3F };
static const uint8_t kFont_K[] = { 4, 0x7F, 0x08, 0x14, 0x63 };
static const uint8_t kFont_L[] = { 4, 0x7F, 0x40, 0x40, 0x40 };
static const uint8_t kFont_M[] = { 5, 0x7F, 0x02, 0x0C, 0x02, 0x7F };
static const uint8_t kFont_N[] = { 4, 0x7F, 0x06, 0x18, 0x7F };
static const uint8_t kFont_O[] = { 4, 0x3E, 0x41, 0x41, 0x3E };
static const uint8_t kFont_P[] = { 4, 0x7F, 0x09, 0x09, 0x06 };
static const uint8_t kFont_Q[] = { 4, 0x3E, 0x41, 0x61, 0x7E };
static const uint8_t kFont_R[] = { 4, 0x7F, 0x09, 0x19, 0x66 };
static const uint8_t kFont_S[] = { 4, 0x26, 0x49, 0x49, 0x32 };
static const uint8_t kFont_T[] = { 5, 0x01, 0x01, 0x7F, 0x01, 0x01 };
static const uint8_t kFont_U[] = { 4, 0x3F, 0x40, 0x40, 0x3F };
static const uint8_t kFont_V[] = { 5, 0x07, 0x18, 0x60, 0x18, 0x07 };
static const uint8_t kFont_W[] = { 5, 0x3F, 0x40, 0x30, 0x40, 0x3F };
static const uint8_t kFont_X[] = { 5, 0x63, 0x14, 0x08, 0x14, 0x63 };
static const uint8_t kFont_Y[] = { 5, 0x03, 0x04, 0x78, 0x04, 0x03 };
static const uint8_t kFont_Z[] = { 4, 0x61, 0x51, 0x49, 0x47 };
static const uint8_t kFont_0[] = { 4, 0x3E, 0x51, 0x49, 0x3E };
static const uint8_t kFont_1[] = { 3, 0x42, 0x7F, 0x40 };
static const uint8_t kFont_2[] = { 4, 0x62, 0x51, 0x49, 0x46 };
static const uint8_t kFont_3[] = { 4, 0x22, 0x49, 0x49, 0x36 };
static const uint8_t kFont_4[] = { 4, 0x18, 0x14, 0x12, 0x7F };
static const uint8_t kFont_5[] = { 4, 0x2F, 0x49, 0x49, 0x31 };
static const uint8_t kFont_6[] = { 4, 0x3E, 0x49, 0x49, 0x32 };
static const uint8_t kFont_7[] = { 4, 0x01, 0x71, 0x09, 0x07 };
static const uint8_t kFont_8[] = { 4, 0x36, 0x49, 0x49, 0x36 };
static const uint8_t kFont_9[] = { 4, 0x26, 0x49, 0x49, 0x3E };
static const uint8_t kFont_SP[] = { 2, 0x00, 0x00 };
static const uint8_t kFont_EX[] = { 1, 0x5F };
static const uint8_t kFont_PD[] = { 1, 0x40 };
static const uint8_t kFont_DH[] = { 2, 0x08, 0x08 };
static const uint8_t kFont_PL[] = { 3, 0x08, 0x1C, 0x08 };

static const uint8_t* font_glyph(char c) {
    if (c >= 'A' && c <= 'Z') {
        static const uint8_t* alpha[] = {
            kFont_A, kFont_B, kFont_C, kFont_D, kFont_E, kFont_F, kFont_G,
            kFont_H, kFont_I, kFont_J, kFont_K, kFont_L, kFont_M, kFont_N,
            kFont_O, kFont_P, kFont_Q, kFont_R, kFont_S, kFont_T, kFont_U,
            kFont_V, kFont_W, kFont_X, kFont_Y, kFont_Z
        };
        return alpha[c - 'A'];
    }
    if (c >= 'a' && c <= 'z') {
        static const uint8_t* alpha[] = {
            kFont_A, kFont_B, kFont_C, kFont_D, kFont_E, kFont_F, kFont_G,
            kFont_H, kFont_I, kFont_J, kFont_K, kFont_L, kFont_M, kFont_N,
            kFont_O, kFont_P, kFont_Q, kFont_R, kFont_S, kFont_T, kFont_U,
            kFont_V, kFont_W, kFont_X, kFont_Y, kFont_Z
        };
        return alpha[c - 'a'];
    }
    if (c >= '0' && c <= '9') {
        static const uint8_t* digits[] = {
            kFont_0, kFont_1, kFont_2, kFont_3, kFont_4,
            kFont_5, kFont_6, kFont_7, kFont_8, kFont_9
        };
        return digits[c - '0'];
    }
    switch (c) {
        case '!': return kFont_EX;
        case '.': return kFont_PD;
        case '-': return kFont_DH;
        case '+': return kFont_PL;
        default:  return kFont_SP;
    }
}

// ── 8x8 icons for flash notifications ────────────────────────────────────────

// Health: solid heart. Easier to recognize on 8x8 than a medical cross.
static const uint8_t kIcon_Health[8] = {
    0x66, 0xFF, 0xFF, 0x7E, 0x7E, 0x3C, 0x18, 0x00
};
// Ammo: vertical round / shell.
static const uint8_t kIcon_Ammo[8] = {
    0x18, 0x3C, 0x3C, 0x18, 0x18, 0x18, 0x3C, 0x00
};
// Armor: shield shape.
static const uint8_t kIcon_Armor[8] = {
    0x3C, 0x7E, 0xFF, 0xDB, 0xFF, 0x7E, 0x3C, 0x18
};
// Key: key shape
static const uint8_t kIcon_Key[8] = {
    0x00, 0x1C, 0x22, 0x22, 0x1C, 0x08, 0x3E, 0x08
};
// Powerup: star
static const uint8_t kIcon_Power[8] = {
    0x08, 0x2A, 0x1C, 0x7F, 0x1C, 0x2A, 0x08, 0x00
};

// Icon-shaped idle meters. The outline stays visible while the fill rises
// bottom-up, which reads better than tiny text labels on the badge matrix.
static const uint8_t kHeartOutline[8] = {
    0x66, 0x99, 0x81, 0x81, 0x42, 0x24, 0x18, 0x00
};
static const uint8_t kHeartFill[8] = {
    0x66, 0xFF, 0xFF, 0x7E, 0x7E, 0x3C, 0x18, 0x00
};
static const uint8_t kShieldOutline[8] = {
    0x3C, 0x42, 0x81, 0x81, 0x81, 0x42, 0x24, 0x18
};
static const uint8_t kShieldFill[8] = {
    0x00, 0x3C, 0x7E, 0x7E, 0x7E, 0x3C, 0x18, 0x00
};
static const uint8_t kAmmoOutline[8] = {
    0x18, 0x24, 0x24, 0x24, 0x24, 0x24, 0x3C, 0x00
};
static const uint8_t kAmmoFill[8] = {
    0x18, 0x3C, 0x3C, 0x18, 0x18, 0x18, 0x3C, 0x00
};

// ── Notification state ───────────────────────────────────────────────────────
enum NotifyMode { kNotifyNone, kNotifyFlash, kNotifyScroll };

static NotifyMode s_notify_mode = kNotifyNone;

// Flash state
static const uint8_t* s_flash_icon = nullptr;
static uint32_t s_flash_start_ms = 0;
static constexpr int kFlashCount = 3;
static constexpr int kFlashOnMs = 180;
static constexpr int kFlashOffMs = 100;
static constexpr int kFlashTotalMs = kFlashCount * (kFlashOnMs + kFlashOffMs);

// Scroll state
static char s_scroll_buf[80];
static int  s_scroll_len = 0;
static int  s_scroll_total_w = 0;
static int  s_scroll_pos = 0;
static uint32_t s_scroll_last_ms = 0;

static constexpr int kScrollPixPerSec = 18;
static constexpr int kScrollStartX = 8;
static constexpr uint32_t kHudPageMs = 1100;
static constexpr uint32_t kHudEventHoldMs = 1500;

// ── HUD state ────────────────────────────────────────────────────────────────
static uint8_t s_hud_rows[8];

enum HudPage : uint8_t {
    kPageHealth = 0,
    kPageArmor  = 1,
    kPageAmmo   = 2,
    kPageKeys   = 3,
    kPageNone   = 0xFF,
};

static bool s_have_snapshot = false;
static uint8_t s_last_health = 0;
static uint8_t s_last_armor = 0;
static uint8_t s_last_ammo = 0;
static uint8_t s_last_keys = 0;
static uint8_t s_forced_page = kPageNone;
static uint8_t s_pending_page = kPageNone;
static uint32_t s_forced_page_until_ms = 0;

static bool time_reached(uint32_t now, uint32_t target) {
    return (int32_t)(now - target) >= 0;
}

static void activate_event_page(uint8_t page, uint32_t now) {
    s_forced_page = page;
    s_forced_page_until_ms = now + kHudEventHoldMs;
}

static void reset_hud_pages(void) {
    s_have_snapshot = false;
    s_last_health = 0;
    s_last_armor = 0;
    s_last_ammo = 0;
    s_last_keys = 0;
    s_forced_page = kPageNone;
    s_pending_page = kPageNone;
    s_forced_page_until_ms = 0;
}

void doom_hud_init(void) {
    memset(s_hud_rows, 0, sizeof(s_hud_rows));
    s_notify_mode = kNotifyNone;
    s_scroll_len = 0;
    reset_hud_pages();
}

void doom_hud_deinit(void) {
    const doom_app_config_t* cfg = doom_app_get_config();
    if (cfg && cfg->matrix_present) {
        memset(s_hud_rows, 0, sizeof(s_hud_rows));
        cfg->matrix_present(s_hud_rows, cfg->matrix_user);
    }
    s_notify_mode = kNotifyNone;
    reset_hud_pages();
}

// Classify a Doom message and pick an icon or decide to scroll
static bool starts_with(const char* str, const char* prefix) {
    return strncasecmp(str, prefix, strlen(prefix)) == 0;
}

static const uint8_t* classify_icon(const char* msg) {
    if (starts_with(msg, "Picked up a stimpack") ||
        starts_with(msg, "Picked up a medikit") ||
        starts_with(msg, "Picked up a health"))
        return kIcon_Health;

    if (starts_with(msg, "Picked up the armor") ||
        starts_with(msg, "Picked up the MegaArmor") ||
        starts_with(msg, "Picked up an armor"))
        return kIcon_Armor;

    if (starts_with(msg, "Picked up a clip") ||
        starts_with(msg, "Picked up a box") ||
        starts_with(msg, "Picked up a rocket") ||
        starts_with(msg, "Picked up an energy") ||
        starts_with(msg, "Picked up 4 shotgun") ||
        starts_with(msg, "Picked up a backpack"))
        return kIcon_Ammo;

    if (strstr(msg, "keycard") || strstr(msg, "skull key"))
        return kIcon_Key;

    if (starts_with(msg, "Supercharge") ||
        starts_with(msg, "Berserk") ||
        starts_with(msg, "Invulnerability") ||
        starts_with(msg, "MegaSphere"))
        return kIcon_Power;

    return nullptr; // scroll for everything else (weapons, etc.)
}

void doom_hud_set_message(const char* msg) {
    if (!msg || !msg[0]) {
        s_notify_mode = kNotifyNone;
        return;
    }

    const uint8_t* icon = classify_icon(msg);
    if (icon) {
        s_flash_icon = icon;
        s_flash_start_ms = millis();
        s_notify_mode = kNotifyFlash;
    } else {
        int len = strlen(msg);
        if (len > (int)sizeof(s_scroll_buf) - 1) len = sizeof(s_scroll_buf) - 1;
        memcpy(s_scroll_buf, msg, len);
        s_scroll_buf[len] = '\0';
        s_scroll_len = len;

        s_scroll_total_w = 0;
        for (int i = 0; i < len; i++) {
            const uint8_t* g = font_glyph(s_scroll_buf[i]);
            s_scroll_total_w += g[0] + 1;
        }

        s_scroll_pos = -kScrollStartX;
        s_scroll_last_ms = millis();
        s_notify_mode = kNotifyScroll;
    }
}

static void render_scroll_frame(uint8_t* rows) {
    memset(rows, 0, 8);
    int x_offset = s_scroll_pos;
    int char_x = 0;
    for (int i = 0; i < s_scroll_len; i++) {
        const uint8_t* g = font_glyph(s_scroll_buf[i]);
        int gw = g[0];
        for (int col = 0; col < gw; col++) {
            int screen_x = char_x + col - x_offset;
            if (screen_x >= 0 && screen_x < 8) {
                uint8_t col_data = g[1 + col];
                for (int bit = 0; bit < 8; bit++) {
                    if (col_data & (1 << bit))
                        rows[bit] |= (1 << (7 - screen_x));
                }
            }
        }
        char_x += gw + 1;
    }
}

static uint8_t bit_count8(uint8_t v) {
    uint8_t count = 0;
    while (v) {
        count += v & 1;
        v >>= 1;
    }
    return count;
}

static void draw_icon_meter(uint8_t* rows, const uint8_t outline[8],
                            const uint8_t fill_mask[8], uint8_t value) {
    memcpy(rows, outline, 8);

    int total_px = 0;
    for (int r = 0; r < 8; r++) total_px += bit_count8(fill_mask[r]);

    int filled = ((int)value * total_px + 50) / 100;
    if (filled > total_px) filled = total_px;
    if (value > 0 && filled == 0) filled = 1;

    for (int r = 7; r >= 0 && filled > 0; r--) {
        for (int bit = 0; bit < 8 && filled > 0; bit++) {
            uint8_t mask = (1 << bit);
            if (fill_mask[r] & mask) {
                rows[r] |= mask;
                filled--;
            }
        }
    }
}

static void draw_key_slot(uint8_t* rows, uint8_t x, bool owned) {
    const uint8_t mask = (uint8_t)(0xC0 >> x);
    if (owned) {
        for (uint8_t r = 1; r <= 6; r++) rows[r] |= mask;
        return;
    }

    rows[1] |= mask;
    rows[6] |= mask;
    rows[2] |= (uint8_t)(0x80 >> x);
    rows[5] |= (uint8_t)(0x40 >> x);
}

static void draw_key_page(uint8_t* rows, uint8_t keys) {
    memset(rows, 0, 8);
    draw_key_slot(rows, 0, (keys & 0x01) != 0);  // red slot: left
    draw_key_slot(rows, 3, (keys & 0x02) != 0);  // blue slot: center
    draw_key_slot(rows, 6, (keys & 0x04) != 0);  // yellow slot: right
}

static uint8_t hud_page_for_time(uint32_t now) {
    if (s_forced_page != kPageNone) {
        if (!time_reached(now, s_forced_page_until_ms)) {
            return s_forced_page;
        }
        s_forced_page = kPageNone;
    }
    return (uint8_t)((now / kHudPageMs) & 0x03);
}

static uint8_t changed_hud_page(uint8_t health, uint8_t armor,
                                uint8_t ammo, uint8_t keys) {
    if (!s_have_snapshot) {
        s_have_snapshot = true;
        s_last_health = health;
        s_last_armor = armor;
        s_last_ammo = ammo;
        s_last_keys = keys;
        return kPageNone;
    }

    uint8_t page = kPageNone;
    if (health != s_last_health) {
        page = kPageHealth;
    } else if (armor != s_last_armor) {
        page = kPageArmor;
    } else if (ammo != s_last_ammo) {
        page = kPageAmmo;
    } else if (keys != s_last_keys) {
        page = kPageKeys;
    }

    s_last_health = health;
    s_last_armor = armor;
    s_last_ammo = ammo;
    s_last_keys = keys;
    return page;
}

static void note_hud_change(uint8_t health, uint8_t armor, uint8_t ammo,
                            uint8_t keys, uint32_t now) {
    uint8_t page = changed_hud_page(health, armor, ammo, keys);
    if (page == kPageNone) return;

    if (s_notify_mode == kNotifyNone) {
        activate_event_page(page, now);
    } else {
        s_pending_page = page;
    }
}

static void activate_pending_page(uint32_t now) {
    if (s_pending_page == kPageNone) return;
    activate_event_page(s_pending_page, now);
    s_pending_page = kPageNone;
}

static void draw_idle_hud(uint8_t health, uint8_t armor, uint8_t ammo,
                          uint8_t keys, uint32_t now) {
    const uint8_t page = hud_page_for_time(now);
    switch (page) {
        case 0:
            draw_icon_meter(s_hud_rows, kHeartOutline, kHeartFill, health);
            break;
        case 1:
            draw_icon_meter(s_hud_rows, kShieldOutline, kShieldFill, armor);
            break;
        case 2:
            draw_icon_meter(s_hud_rows, kAmmoOutline, kAmmoFill, ammo);
            break;
        default:
            draw_key_page(s_hud_rows, keys);
            break;
    }

    if (page == 0 && health > 0 && health <= 25 && ((now / 280) & 1)) {
        s_hud_rows[0] |= 0x81;
        s_hud_rows[2] |= 0x81;
    }
}

void doom_hud_update(uint8_t health, uint8_t armor, uint8_t ammo, uint8_t keys) {
    uint32_t now = millis();
    note_hud_change(health, armor, ammo, keys, now);

    if (s_notify_mode == kNotifyFlash) {
        uint32_t elapsed = now - s_flash_start_ms;
        if (elapsed >= (uint32_t)kFlashTotalMs) {
            s_notify_mode = kNotifyNone;
            activate_pending_page(now);
        } else {
            uint32_t cycle_pos = elapsed % (kFlashOnMs + kFlashOffMs);
            bool on = (cycle_pos < (uint32_t)kFlashOnMs);
            if (on && s_flash_icon) {
                memcpy(s_hud_rows, s_flash_icon, 8);
            } else {
                memset(s_hud_rows, 0, 8);
            }
            const doom_app_config_t* cfg = doom_app_get_config();
            if (cfg && cfg->matrix_present)
                cfg->matrix_present(s_hud_rows, cfg->matrix_user);
            return;
        }
    }

    if (s_notify_mode == kNotifyScroll) {
        uint32_t dt = now - s_scroll_last_ms;
        int advance = (int)(dt * kScrollPixPerSec / 1000);
        if (advance > 0) {
            s_scroll_pos += advance;
            s_scroll_last_ms = now;
        }
        if (s_scroll_pos > s_scroll_total_w) {
            s_notify_mode = kNotifyNone;
            activate_pending_page(now);
        } else {
            render_scroll_frame(s_hud_rows);
            const doom_app_config_t* cfg = doom_app_get_config();
            if (cfg && cfg->matrix_present)
                cfg->matrix_present(s_hud_rows, cfg->matrix_user);
            return;
        }
    }

    draw_idle_hud(health, armor, ammo, keys, now);

    const doom_app_config_t* cfg = doom_app_get_config();
    if (cfg && cfg->matrix_present) {
        cfg->matrix_present(s_hud_rows, cfg->matrix_user);
    }
}

#endif // BADGE_HAS_DOOM
