#include "PanicReset.h"
#include "HardwareConfig.h"
#include "oled.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <driver/gpio.h>

extern volatile bool mpy_app_force_exit;

namespace {

// Tick period × tick count = real-world hold duration. Keep the
// callsite log lines + Help screen in sync with these.
constexpr uint32_t kPollPeriodUs   = 50000;    // 50 ms
constexpr uint16_t kMpyExitTicks   = 20;        // 1 s
constexpr uint16_t kRebootTicks    = 40;        // 2 s
constexpr uint16_t kForceRebootTicks = 80;     // 4 s — hard cutoff

constexpr bool kShowRebootScreen  = false;
constexpr bool kWaitForRelease    = false;

oled* s_display           = nullptr;
esp_timer_handle_t s_timer = nullptr;
uint16_t s_holdTicks      = 0;
bool s_rebootPending      = false;
bool s_mpyExitFired       = false;

bool allButtonsPressed() {
    return gpio_get_level(static_cast<gpio_num_t>(BUTTON_UP))    == 0
        && gpio_get_level(static_cast<gpio_num_t>(BUTTON_DOWN))  == 0
        && gpio_get_level(static_cast<gpio_num_t>(BUTTON_LEFT))  == 0
        && gpio_get_level(static_cast<gpio_num_t>(BUTTON_RIGHT)) == 0;
}

void showRebootScreen() {
    if (!s_display) return;
    s_display->clearBuffer();
    s_display->setFontPreset(FONT_SMALL);
    s_display->setDrawColor(1);

    const char* line1 = "SYSTEM RESET";
    int w1 = s_display->getStrWidth(line1);
    s_display->drawStr((128 - w1) / 2, 28, line1);

    const char* line2 = "Release to reboot...";
    int w2 = s_display->getStrWidth(line2);
    s_display->drawStr((128 - w2) / 2, 44, line2);

    s_display->sendBuffer();
}

void printDiagnostics() {
    Serial.println("\n=== PANIC RESET ===");
    Serial.printf("  uptime_ms:  %lu\n", (unsigned long)millis());
    Serial.printf("  free_heap:  %u\n", (unsigned)esp_get_free_heap_size());
    Serial.printf("  min_heap:   %u\n", (unsigned)esp_get_minimum_free_heap_size());
    Serial.printf("  free_psram: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.printf("  last_reset: %d\n", (int)esp_reset_reason());
    Serial.println("===================\n");
}

void panicResetTimerCb(void*) {
    if (!allButtonsPressed()) {
        if (s_rebootPending && kWaitForRelease) {
            Serial.println("[PanicReset] buttons released — rebooting");
            esp_restart();
        }
        s_holdTicks = 0;
        s_rebootPending = false;
        s_mpyExitFired = false;
        return;
    }

    ++s_holdTicks;

    if (s_holdTicks == kMpyExitTicks && !s_mpyExitFired) {
        mpy_app_force_exit = true;
        s_mpyExitFired = true;
        Serial.println("[PanicReset] 1s — MicroPython force-exit fired");
    }

    if (s_holdTicks == kRebootTicks && !s_rebootPending) {
        s_rebootPending = true;
        printDiagnostics();
        if (kShowRebootScreen) showRebootScreen();
        if (!kWaitForRelease) {
            Serial.println("[PanicReset] 2s — rebooting immediately");
            esp_restart();
        }
    }

    if (s_holdTicks >= kForceRebootTicks) {
        Serial.println("[PanicReset] 4s timeout — forcing reboot");
        esp_restart();
    }
}

}  // namespace

namespace PanicReset {

void begin(oled* display) {
    s_display = display;

    const esp_timer_create_args_t args = {
        .callback = panicResetTimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "panic_rst",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &s_timer);
    esp_timer_start_periodic(s_timer, kPollPeriodUs);
    Serial.println("[PanicReset] armed (1s=mpy-exit, 2s=reset, 4s=force)");
}

bool rebootPending() {
    return s_rebootPending;
}

}  // namespace PanicReset
