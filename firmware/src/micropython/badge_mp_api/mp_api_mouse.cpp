// mp_api_mouse.cpp — Python-facing facade for the shared MouseOverlay.
//
// Spec-010: the cursor compositor + state machine moved out to
// `firmware/src/ui/MouseOverlay.{h,cpp}` so native C++ screens can drive
// the same overlay when needed. This file now just provides the C ABI
// symbols that MicroPython usermods + the existing
// service-pump callers expect.
//
// External symbols preserved (do not rename without updating callers):
//   * `mp_mouse_overlay_composite`  — pre-flush XOR draw
//   * `mp_mouse_overlay_restore`    — post-flush XOR draw (self-inverse)
//   * `mp_mouse_overlay_enabled`    — read enabled flag
//   * `mp_mouse_pump_tick`          — per-tick input integrator
//   * `temporalbadge_runtime_mouse_*` — Python `badge.mouse_*` bindings

#include "Internal.h"
#include "temporalbadge_runtime.h"
#include "../../ui/MouseOverlay.h"

// ── Internal surface (called by ReplayMicropythonAPI.cpp + mp_api_display.cpp)

void mp_mouse_overlay_composite(void) { MouseOverlay::compositeIfEnabled(); }
void mp_mouse_overlay_restore(void)   { MouseOverlay::compositeIfEnabled(); }
bool mp_mouse_overlay_enabled(void)   { return MouseOverlay::isEnabled(); }
void mp_mouse_pump_tick(void)         { MouseOverlay::pumpTick(); }

// ── Python-facing entrypoints (extern "C" — exposed via badge_mp QSTRs)

extern "C" void temporalbadge_runtime_mouse_overlay(int enable) {
    MouseOverlay::setEnabled(enable != 0);
    if (enable) {
        mpy_oled_note_activity();
    }
}

extern "C" void temporalbadge_runtime_mouse_set_bitmap(
    const uint8_t* data, int w, int h) {
    MouseOverlay::setBitmap(data, w, h);
}

extern "C" int temporalbadge_runtime_mouse_x(void) {
    return MouseOverlay::x();
}

extern "C" int temporalbadge_runtime_mouse_y(void) {
    return MouseOverlay::y();
}

extern "C" void temporalbadge_runtime_mouse_set_pos(int x, int y) {
    MouseOverlay::setPos(x, y);
}

extern "C" int temporalbadge_runtime_mouse_clicked(void) {
    return MouseOverlay::takeClick();
}

extern "C" void temporalbadge_runtime_mouse_set_speed(int speed) {
    MouseOverlay::setSpeed(speed);
}

extern "C" void temporalbadge_runtime_mouse_set_mode(int absolute) {
    MouseOverlay::setMode(absolute ? MouseOverlay::kAbsolute
                                   : MouseOverlay::kRelative);
}
