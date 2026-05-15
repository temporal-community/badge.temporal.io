#ifdef BADGE_HAS_DOOM

#include "doom_input.h"
#include "doom_app.h"
#include <Arduino.h>
#include <cmath>
#include "hardware/Inputs.h"

extern "C" {
#include "doomkeys.h"
}

static constexpr float kDeadzone = 0.15f;
static constexpr float kWeaponThreshold = 0.5f;
static constexpr uint32_t kWeaponRepeatMs = 200;
static constexpr uint32_t kExitComboMs = 1500;

static constexpr int kKeyQueueSize = 16;
struct KeyEvent { int pressed; unsigned char key; };
static KeyEvent s_key_queue[kKeyQueueSize];
static volatile int s_queue_head = 0;
static volatile int s_queue_tail = 0;

static bool s_prev_keys[256];
static uint32_t s_exit_combo_start = 0;
static uint32_t s_last_weapon_cycle = 0;

static constexpr uint32_t kFireRepeatMs = 200;
static uint32_t s_fire_repeat_next = 0;
static bool s_fire_held = false;

static void enqueue_key(int pressed, unsigned char key) {
    int next = (s_queue_head + 1) % kKeyQueueSize;
    if (next == s_queue_tail) return;
    s_key_queue[s_queue_head] = { pressed, key };
    s_queue_head = next;
}

static void set_key(unsigned char key, bool pressed) {
    if (pressed != s_prev_keys[key]) {
        enqueue_key(pressed ? 1 : 0, key);
        s_prev_keys[key] = pressed;
    }
}

static float apply_curve(float v) {
    if (fabsf(v) < kDeadzone) return 0.0f;
    float sign = v < 0.0f ? -1.0f : 1.0f;
    float clamped = fminf(fabsf(v), 1.0f);
    return sign * clamped * clamped;
}

void doom_input_init(void) {
    s_queue_head = 0;
    s_queue_tail = 0;
    s_exit_combo_start = 0;
    s_last_weapon_cycle = 0;
    s_fire_held = false;
    s_fire_repeat_next = 0;
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
}

bool doom_input_poll(void) {
    float joy_x = 0, joy_y = 0;
    uint8_t buttons = 0;
    const doom_app_config_t* cfg = doom_app_get_config();
    if (cfg && cfg->get_input) {
        cfg->get_input(cfg->input_user, &joy_x, &joy_y, &buttons);
    }

    joy_x = apply_curve(joy_x);
    joy_y = apply_curve(joy_y);

    bool btn_up    = (buttons & DOOM_BTN_UP) != 0;
    bool btn_down  = (buttons & DOOM_BTN_DOWN) != 0;
    bool btn_left  = (buttons & DOOM_BTN_LEFT) != 0;
    bool btn_right = (buttons & DOOM_BTN_RIGHT) != 0;

    // UP+DOWN = Escape (menu back / dismiss)
    bool up_down_combo = btn_up && btn_down;
    set_key(KEY_ESCAPE, up_down_combo);

    // Exit Doom: hold LEFT + RIGHT for kExitComboMs
    if (btn_left && btn_right) {
        if (s_exit_combo_start == 0) {
            s_exit_combo_start = millis();
        } else if (millis() - s_exit_combo_start >= kExitComboMs) {
            doom_app_request_stop();
            return true;
        }
    } else {
        s_exit_combo_start = 0;
    }

    // When UP+DOWN are held together, suppress fire/use/enter so only
    // Escape is sent.
    if (!up_down_combo) {
        set_key(KEY_ENTER, btn_down);
        set_key(KEY_USE,   btn_down);

        uint32_t now = millis();
        if (btn_up) {
            if (!s_fire_held) {
                // Fresh press -- fire immediately, reset repeat timer
                s_fire_held = true;
                s_fire_repeat_next = now + kFireRepeatMs;
                enqueue_key(1, KEY_FIRE);
            } else if (now >= s_fire_repeat_next) {
                // Held long enough -- generate press+release cycle for repeat
                enqueue_key(0, KEY_FIRE);
                enqueue_key(1, KEY_FIRE);
                s_fire_repeat_next = now + kFireRepeatMs;
            }
        } else {
            if (s_fire_held) {
                s_fire_held = false;
                enqueue_key(0, KEY_FIRE);
            }
        }
    } else {
        set_key(KEY_ENTER, false);
        set_key(KEY_USE,   false);
        if (s_fire_held) {
            s_fire_held = false;
            enqueue_key(0, KEY_FIRE);
        }
    }



    // LEFT/RIGHT = strafe directly, joystick = turn + forward/back
    set_key(KEY_STRAFE_L, btn_left && !btn_right);
    set_key(KEY_STRAFE_R, btn_right && !btn_left);
    set_key(KEY_LEFTARROW, joy_x < -0.3f);
    set_key(KEY_RIGHTARROW, joy_x > 0.3f);
    set_key(KEY_UPARROW, joy_y < -0.3f);
    set_key(KEY_DOWNARROW, joy_y > 0.3f);

    // Weapon cycle: hold RIGHT + joystick Y
    if (btn_right && !btn_left) {
        uint32_t now = millis();
        if (fabsf(joy_y) > kWeaponThreshold &&
            now - s_last_weapon_cycle > kWeaponRepeatMs) {
            if (joy_y < -kWeaponThreshold) {
                enqueue_key(1, '4');
                enqueue_key(0, '4');
            } else {
                enqueue_key(1, '6');
                enqueue_key(0, '6');
            }
            s_last_weapon_cycle = now;
        }
    }

    return false;
}

int doom_input_get_key(int* pressed, unsigned char* key) {
    if (s_queue_tail == s_queue_head) return 0;
    *pressed = s_key_queue[s_queue_tail].pressed;
    *key = s_key_queue[s_queue_tail].key;
    s_queue_tail = (s_queue_tail + 1) % kKeyQueueSize;
    return 1;
}

#endif // BADGE_HAS_DOOM
