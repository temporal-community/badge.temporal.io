#include "MouseOverlay.h"

#include <Arduino.h>
#include <string.h>

#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "Images.h"

extern oled   badgeDisplay;
extern Inputs inputs;

namespace MouseOverlay {

namespace {

struct State {
    bool     enabled       = false;
    bool     absolute      = false;
    int16_t  x             = 64;
    int16_t  y             = 32;
    int16_t  speed         = 3;
    uint32_t lastMoveMs    = 0;
    int32_t  accumX256     = 0;
    int32_t  accumY256     = 0;

    const uint8_t* bitmap  = GUIImages::kCursorBitmap;
    uint8_t  w             = GUIImages::kCursorW;
    uint8_t  h             = GUIImages::kCursorH;
    uint8_t  hotX          = 0;
    uint8_t  hotY          = 0;

    int8_t   click_btn     = -1;
    bool     click_pending = false;
};

State s;
uint8_t sCustomCursorBuf[128];

constexpr uint32_t kMovePeriodMs  = 16;

inline int16_t clampDeflection(int32_t v) {
    if (v >  2047) return  2047;
    if (v < -2047) return -2047;
    return (int16_t)v;
}

void resetMotionState() {
    s.lastMoveMs = 0;
    s.accumX256 = 0;
    s.accumY256 = 0;
}

void overlay_draw_xor() {
    if (!s.enabled) return;
    const int dx = s.x - s.hotX;
    const int dy = s.y - s.hotY;
    const int bpr = (s.w + 7) / 8;

    badgeDisplay.setDrawColor(2);
    for (int row = 0; row < s.h; ++row) {
        const int py = dy + row;
        if (py < 0 || py >= 64) continue;
        for (int col = 0; col < s.w; ++col) {
            const int px = dx + col;
            if (px < 0 || px >= 128) continue;
            const uint8_t srcByte = pgm_read_byte(
                &s.bitmap[row * bpr + (col / 8)]);
            const uint8_t mask = 0x80 >> (col & 7);
            if (srcByte & mask) {
                badgeDisplay.drawPixel(px, py);
            }
        }
    }
    badgeDisplay.setDrawColor(1);
}

}  // namespace

// ── Lifecycle ──────────────────────────────────────────────────────────────

void setEnabled(bool on) {
    if (on && !s.enabled) {
        resetMotionState();
    }
    s.enabled = on;
}
bool isEnabled()         { return s.enabled; }

// ── Position ───────────────────────────────────────────────────────────────

int  x() { return s.x; }
int  y() { return s.y; }

void setPos(int xx, int yy) {
    if (xx < 0)   xx = 0;
    if (xx > 127) xx = 127;
    if (yy < 0)   yy = 0;
    if (yy > 63)  yy = 63;
    s.x = (int16_t)xx;
    s.y = (int16_t)yy;
}

// ── Movement ───────────────────────────────────────────────────────────────

void setMode(Mode m) {
    s.absolute = (m == kAbsolute);
    resetMotionState();
}

void setSpeed(int speed) {
    if (speed < 1)  speed = 1;
    if (speed > 20) speed = 20;
    s.speed = (int16_t)speed;
}

void setBitmap(const uint8_t* data, int w, int h) {
    if (!data || w <= 0 || h <= 0) {
        s.bitmap = GUIImages::kCursorBitmap;
        s.w = GUIImages::kCursorW;
        s.h = GUIImages::kCursorH;
        s.hotX = 0;
        s.hotY = 0;
        return;
    }
    if (w > 32) w = 32;
    if (h > 32) h = 32;
    const int bpr = (w + 7) / 8;
    const int total = bpr * h;
    if (total > (int)sizeof(sCustomCursorBuf)) return;
    memcpy(sCustomCursorBuf, data, total);
    s.bitmap = sCustomCursorBuf;
    s.w      = (uint8_t)w;
    s.h      = (uint8_t)h;
    s.hotX   = (uint8_t)(w / 2);
    s.hotY   = (uint8_t)(h / 2);
}

// ── Click latching ─────────────────────────────────────────────────────────

int takeClick() {
    if (!s.click_pending) return -1;
    s.click_pending = false;
    return s.click_btn;
}

// ── Per-frame pump ─────────────────────────────────────────────────────────

void pumpTick() {
    if (!s.enabled) return;

    const uint32_t now = millis();

    if (s.absolute) {
        // Absolute: joystick raw position maps to cursor position.
        s.x = (int16_t)((uint32_t)inputs.joyX() * 127 / 4095);
        s.y = (int16_t)((uint32_t)inputs.joyY() * 63 / 4095);
    } else if (now - s.lastMoveMs >= kMovePeriodMs) {
        s.lastMoveMs = now;

        // Joystick deflection from center.
        int32_t jx = (int32_t)inputs.joyX() - 2047;
        int32_t jy = (int32_t)inputs.joyY() - 2047;

        const int16_t combinedX = clampDeflection(jx);
        const int16_t combinedY = clampDeflection(jy);

        s.accumX256 += ((int32_t)combinedX * s.speed * 256) / 2047;
        s.accumY256 += ((int32_t)combinedY * s.speed * 256) / 2047;

        const int16_t dx = (int16_t)(s.accumX256 / 256);
        const int16_t dy = (int16_t)(s.accumY256 / 256);
        s.accumX256 -= (int32_t)dx * 256;
        s.accumY256 -= (int32_t)dy * 256;

        s.x += dx;
        s.y += dy;

        if (s.x < 0)   { s.x = 0;   s.accumX256 = 0; }
        if (s.x > 127) { s.x = 127; s.accumX256 = 0; }
        if (s.y < 0)   { s.y = 0;   s.accumY256 = 0; }
        if (s.y > 63)  { s.y = 63;  s.accumY256 = 0; }
    }

    // Click capture from face-button edges. Keep the original Python API
    // ids stable while exposing the clearer B/A/X/Y names.
    const Inputs::ButtonEdges& e = inputs.edges();
    int8_t  btn = -1;
    uint8_t hw  = 0xFF;
    if (e.bPressed)      { btn = 0; hw = 3; }
    else if (e.aPressed) { btn = 1; hw = 1; }
    else if (e.xPressed) { btn = 2; hw = 2; }
    else if (e.yPressed) { btn = 3; hw = 0; }
    if (btn >= 0) {
        s.click_btn     = btn;
        s.click_pending = true;
        inputs.clearPressEdge(hw);
    }
}

// ── Composite / restore ────────────────────────────────────────────────────

void compositeIfEnabled() {
    overlay_draw_xor();  // self-inverse via XOR; same fn for restore
}

}  // namespace MouseOverlay
