// MouseOverlay — XOR cursor compositor with joystick input.
//
// Originally lived inside the MicroPython API layer
// (`micropython/badge_mp_api/mp_api_mouse.cpp`) as a Python-only feature.
// Spec-010 promotes it here so native C++ screens can also drive a
// virtual mouse cursor. The Python facade in mp_api_mouse.cpp now
// delegates to this module; Python API surface (`mouse_overlay`,
// `mouse_x`, etc.) is unchanged.
//
// In relative mode, joystick deflection from center becomes cursor
// velocity. In absolute mode, raw joystick position maps directly to
// screen coordinates.
//
// Compositing is XOR (`setDrawColor(2)`) so the cursor reads on either
// fill. Drawing twice cancels out, so `compositeIfEnabled()` is also
// the restore-after-flush hook.
//
// Threading: all state lives in this translation unit. Functions are
// called from Core 1 only (GUI tick + display flush hooks). No locking.

#pragma once

#include <stdint.h>

class oled;

namespace MouseOverlay {

enum Mode : uint8_t {
    kRelative = 0,  // joystick deflection → cursor velocity
    kAbsolute = 1,  // joystick raw position → cursor position
};

// Lifecycle ─────────────────────────────────────────────────────────────────
void setEnabled(bool on);
bool isEnabled();

// Position ──────────────────────────────────────────────────────────────────
int  x();
int  y();
void setPos(int x, int y);

// Movement ──────────────────────────────────────────────────────────────────
void setMode(Mode m);
void setSpeed(int speed);  // 1..20; default 3

// Cursor bitmap. Pass nullptr or w/h<=0 to revert to the default cursor.
// Buffer is copied into a static 128-byte cache; oversize requests rejected.
void setBitmap(const uint8_t* data, int w, int h);

// Click latching. Each press of B/A/X/Y during pumpTick()
// stores the button id (B=0, A=1, X=2, Y=3) and consumes the
// edge from `inputs`. takeClick() returns the latched id and clears it,
// or -1 when no click is pending.
int takeClick();

// Per-frame pump: read joystick and update the cursor state.
// Safe to call regardless of enabled — no-ops when disabled.
void pumpTick();

// XOR-draws the cursor over the framebuffer at its current position.
// No-op when disabled. Self-inverse: calling twice restores the original
// pixels (used for both pre-flush composite and post-flush restore).
void compositeIfEnabled();

}  // namespace MouseOverlay
