#pragma once

// Shared internals for the firmware/src/badge_mp_api/ TUs.
// Definitions for these symbols live in firmware/src/ReplayMicropythonAPI.cpp
// (the volatile flags + activity helpers stay with the service pump) and in
// mp_api_mouse.cpp (the overlay state + pump tick).

#include <stdint.h>

extern volatile bool mpy_oled_hold;
extern volatile bool mpy_led_hold;

// Side-effect helpers — defined in ReplayMicropythonAPI.cpp.
// First touch from MicroPython transitions ownership of the OLED / LED
// matrix away from the GUI/native renderers; both helpers are idempotent.
void mpy_oled_note_activity(void);
void mpy_led_note_activity(void);

// Mouse overlay surface — defined in mp_api_mouse.cpp. The service pump
// needs to (a) composite the cursor over freshly-drawn OLED frames, and
// (b) advance the overlay's internal joystick/click state once per tick.
void mp_mouse_overlay_composite(void);
void mp_mouse_overlay_restore(void);
bool mp_mouse_overlay_enabled(void);
void mp_mouse_pump_tick(void);
