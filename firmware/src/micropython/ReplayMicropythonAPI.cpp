// MicroPython ↔ C++ bridge: thin aggregator. Per-subsystem entrypoints
// (display, input, LED, IMU, haptics, mouse, IR, badge-data, dev) live in
// firmware/src/badge_mp_api/mp_api_*.cpp. This file owns the shared
// volatile flags, the mpy_oled_/mpy_led_note_activity transitions, and
// the once-per-tick service pump that feeds inputs/IMU/LED/haptics and
// services the mouse-overlay click latch.
//
// GIL story (relevant when REPLAY_ENABLE_THREAD=1):
//   mpy_service_pump() is called from two contexts:
//     1. mpy_hal_delay_ms() inside MicroPythonBridge.cpp — Python thread is
//        already on the GIL; we don't touch any MP state here, so we don't
//        need to release it. The upstream MICROPY_EVENT_POLL_HOOK would do
//        a `MP_THREAD_GIL_EXIT(); ulTaskNotifyTake(...); MP_THREAD_GIL_ENTER();`
//        and our pump runs at finer granularity than that, so we're fine.
//     2. The Arduino main loop's `mpy_poll()` — runs on the Arduino task
//        which is NOT a MicroPython thread; no GIL involvement at all.
//   If a future change makes the pump touch MP state, wrap that section in
//   `MP_THREAD_GIL_ENTER()/MP_THREAD_GIL_EXIT()`.

#include <Arduino.h>

#include "../infra/BadgeConfig.h"
#include "ui/GUI.h"
#include "hardware/Inputs.h"
#include "hardware/LEDmatrix.h"
#include "hardware/oled.h"
#ifdef BADGE_HAS_IMU
#include "hardware/IMU.h"
#endif
#ifdef BADGE_HAS_HAPTICS
#include "hardware/Haptics.h"
#endif

#include "badge_mp_api/Internal.h"

extern "C" {
#include "matrix_app_api.h"
}

extern oled badgeDisplay;
extern Inputs inputs;
extern LEDmatrix badgeMatrix;
extern GUIManager guiManager;
#ifdef BADGE_HAS_IMU
extern IMU imu;
#endif

volatile bool mpy_oled_hold = false;
volatile bool mpy_led_hold = false;
volatile bool mpy_app_force_exit = false;

namespace {
bool s_pump_ready = false;
}

extern "C" void mpy_pump_set_ready(void) { s_pump_ready = true; }

void mpy_oled_note_activity(void)
{
    if (!mpy_oled_hold)
    {
        mpy_oled_hold = true;
        badgeDisplay.setFontFamilyAndSlot(
            static_cast<uint8_t>(badgeConfig.get(kFontFamily)),
            static_cast<uint8_t>(badgeConfig.get(kFontSize)));
    }
}

void mpy_led_note_activity(void)
{
    if (!mpy_led_hold)
    {
        mpy_led_hold = true;
        badgeMatrix.setMicropythonMode(true);
    }
}

extern "C" void mpy_service_pump(void)
{
    if (!s_pump_ready)
        return;
    inputs.service();

    if (mp_mouse_overlay_enabled())
    {
        mp_mouse_pump_tick();
    }

#ifdef BADGE_HAS_IMU
    imu.service();
    if (imu.flipChanged())
    {
        const auto orient = imu.getOrientation();
        const uint32_t flipAt = imu.flipChangedAtMs();
        const uint32_t consumeAt = millis();
        // Keep interactive controls in normal orientation. Inverted badge
        // posture is surfaced only through passive nametag/display assets.
        badgeDisplay.setFlipped(false);
        inputs.setOrientation(BadgeOrientation::kUpright);
#ifdef BADGE_HAS_LED_MATRIX
        const bool matrixNeedsRotation = (orient == BadgeOrientation::kUpright);
        badgeMatrix.setFlipped(matrixNeedsRotation);
#endif
        // Keep the GUI's nametag-overlay state in sync even while MPY
        // owns the OLED (mpy_oled_hold suppresses the actual render)
        // so when the app exits we don't suddenly snap to the wrong
        // overlay state based on a stale orientation.
        guiManager.setNametagMode(orient == BadgeOrientation::kInverted);
        imu.clearFlipChanged();
        const uint32_t doneAt = millis();
        Serial.printf("[FLIP] mpy-pump consume dt=%lu ms apply dt=%lu ms (orient=%s)\n",
                      static_cast<unsigned long>(consumeAt - flipAt),
                      static_cast<unsigned long>(doneAt - consumeAt),
                      orient == BadgeOrientation::kInverted ? "inverted" : "upright");
    }
#endif
#ifdef BADGE_HAS_LED_MATRIX
    badgeMatrix.service();
    matrix_app_service_tick(millis());
#endif
#ifdef BADGE_HAS_HAPTICS
    Haptics::checkPulseEnd();
    CoilTone::checkToneEnd();
#endif

}
