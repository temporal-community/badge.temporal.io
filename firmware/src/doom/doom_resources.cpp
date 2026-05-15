#ifdef BADGE_HAS_DOOM

#include "doom_resources.h"

#include "../api/WiFiService.h"
#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../ble/BadgeBeaconAdv.h"
#include "../ble/BleBeaconScanner.h"
#endif
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/Power.h"
#include "../infra/Scheduler.h"
#include "../ir/BadgeIR.h"
#include "../led/LEDAppRuntime.h"
#include "../micropython/MicroPythonMatrixService.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

extern Inputs inputs;
extern MicroPythonMatrixService microPythonMatrix;
extern Scheduler scheduler;
extern SleepService sleepService;

namespace {

bool s_active = false;
bool s_restore_ir_hw = false;
bool s_restore_python_ir = false;

constexpr const char* kDoomPausedServices[] = {
    "Inputs",
    "Haptics",
    "LEDmatrix",
    "MPMatrixApp",
    "LEDAppRuntime",
    "FileBrowser",
    "BatteryGauge",
    "SleepService",
    "IMU",
    "ConfigWatcher",
};

void setServicesActive(bool active) {
    for (const char* name : kDoomPausedServices) {
        scheduler.setServiceState(name, active);
    }
}

void waitForIrHardwareDown() {
    for (uint8_t i = 0; i < 25 && irHwIsUp(); ++i) {
        delay(10);
    }
}

}  // namespace

bool doom_resources_active(void) {
    return s_active;
}

void doom_resources_enter(void) {
    if (s_active) return;

    s_active = true;
    s_restore_ir_hw = irHardwareEnabled;
    s_restore_python_ir = pythonIrListening;

    sleepService.caffeine = true;
    Power::enterPerformanceMode();

#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForForeground(true);
    BleBeaconScanner::stopScan();
    BleBeaconScanner::clearScanCache();
#endif
    Serial.printf("[doom_resources] pre-pause heap largest=%u free=%u psram=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Doom needs ~24 KB contiguous internal DRAM for its task stack.
    // Once Map has been entered the BLE controller permanently locks
    // ~60 KB of internal heap (see BleBeaconScanner runEndSession
    // notes), which leaves the largest free block at ~10 KB and doom
    // fails to launch. Doom takes over the badge entirely so we can
    // afford a best-effort BLE shutdown here — re-init isn't safe in
    // arduino-esp32 anyway, and BLE stays unavailable until reboot.
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BleBeaconScanner::shutdownForExclusiveApp();
#endif
    Serial.printf("[doom_resources] post-ble-shutdown heap largest=%u free=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    setServicesActive(false);
    scheduler.setServiceState("GUI", false);
    scheduler.setServiceState("OLED", false);
    scheduler.setServiceState("Network", false);
    scheduler.setExecutionDivisors(1, 1, 1);

    microPythonMatrix.beginOverride();
    ledAppRuntime.beginOverride();

    irHardwareEnabled = false;
    pythonIrListening = false;
    irDrainPythonRx();
    waitForIrHardwareDown();

    inputs.suspendInterrupts();

#ifdef BADGE_HAS_HAPTICS
    Haptics::off();
#endif

    wifiService.disconnect();
    WiFi.mode(WIFI_OFF);
    // Full driver deinit so the next WiFi.mode(STA) starts from a
    // clean state. Without this, lwIP / esp_wifi internals carry
    // stale handles across the doom session.
    esp_wifi_stop();
    esp_wifi_deinit();

    Serial.printf("[doom_resources] post-radio heap largest=%u free=%u psram=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    Serial.println("[doom_resources] badge services paused for Doom");
}

void doom_resources_exit(void) {
    if (!s_active) return;

    Haptics::off();

    irHardwareEnabled = s_restore_ir_hw;
    pythonIrListening = s_restore_python_ir;
    irDrainPythonRx();

    inputs.resumeInterrupts();
    inputs.resyncButtons();

    ledAppRuntime.endOverride();
    microPythonMatrix.endOverride();

    setServicesActive(true);
    scheduler.setServiceState("GUI", true);
    scheduler.setServiceState("OLED", false);
    scheduler.setServiceState("Network", true);
    scheduler.setExecutionDivisors(Power::Policy::schedulerHighDivisor,
                                   Power::Policy::schedulerNormalDivisor,
                                   Power::Policy::schedulerLowDivisor);

    // doom_resources_enter already did esp_wifi_stop + esp_wifi_deinit,
    // so the driver is fully torn down. Just give the system a brief
    // moment to settle before wifiService.begin() schedules the next
    // sync, and let the worker's first WiFi.mode(STA) bring the
    // driver back up via Arduino-ESP32's normal lazy-init path.
    delay(50);

    wifiService.begin();
    sleepService.caffeine = true;
    Power::exitPerformanceMode();
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForForeground(false);
#endif

    s_active = false;
    Serial.println("[doom_resources] badge services restored after Doom");
}

static bool s_keyboard_overlay = false;

void doom_resources_pause_for_keyboard(void) {
    if (!s_active || s_keyboard_overlay) return;
    s_keyboard_overlay = true;
    inputs.resumeInterrupts();
    inputs.resyncButtons();
    scheduler.setServiceState("GUI", true);
    scheduler.setServiceState("Inputs", true);
    Serial.println("[doom_resources] paused for keyboard overlay");
}

bool doom_resources_keyboard_active(void) {
    return s_keyboard_overlay;
}

void doom_resources_resume_from_keyboard(void) {
    if (!s_keyboard_overlay) return;
    s_keyboard_overlay = false;
    scheduler.setServiceState("GUI", false);
    scheduler.setServiceState("Inputs", false);
    inputs.suspendInterrupts();
    Serial.println("[doom_resources] resumed from keyboard overlay");
}

#endif  // BADGE_HAS_DOOM
