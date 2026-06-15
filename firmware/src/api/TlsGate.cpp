#include "TlsGate.h"

#include <Arduino.h>
#include "../infra/HeapDiag.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace badge {

namespace {

// mbedTLS + WiFiClientSecure allocate large *contiguous* internal blocks.
// ESP.getFreeHeap() mixes internal + PSRAM on typical Arduino builds, so
// a badge can show "plenty" of total free heap while internal RAM is too
// low or too fragmented to support a handshake. Always check
// MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT explicitly.
constexpr size_t kMinInternalFreeForTls = 40 * 1024;
constexpr size_t kMinLargestInternalBlockForTls = 26 * 1024;

// Polling slice for `waitForTlsReady`. Tight enough that a freshly
// released gate is reacquired within a frame, loose enough that the
// IDLE / GUI / WiFi tasks make progress between iterations.
constexpr TickType_t kReadyPollTicks = pdMS_TO_TICKS(50);

// Static-init creation of the recursive mutex. arduino-esp32 has the
// FreeRTOS scheduler running before any C++ static initializer fires
// (the Arduino loopTask is itself a FreeRTOS task started by app_main),
// so xSemaphoreCreateRecursiveMutex() is safe in this position. Using
// a static init avoids the racy lazy-init pattern where two simultaneous
// first-time callers from different tasks could each create their own
// mutex and lose the gate semantics.
SemaphoreHandle_t makeMutex() {
  return xSemaphoreCreateRecursiveMutex();
}

SemaphoreHandle_t sMutex = makeMutex();

}  // namespace

bool tlsHeapHeadroomOk() {
  const size_t internalFree =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t largest = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return internalFree >= kMinInternalFreeForTls &&
         largest >= kMinLargestInternalBlockForTls;
}

bool tlsGateBusy() {
  if (!sMutex) return false;
  // xSemaphoreTakeRecursive(0) returns pdTRUE if the calling task can
  // currently acquire (either uncontested or because it already holds
  // the recursive mutex). pdFALSE means "another task is holding it".
  // We immediately give it back so the probe is non-destructive.
  if (xSemaphoreTakeRecursive(sMutex, 0) != pdTRUE) return true;
  xSemaphoreGiveRecursive(sMutex);
  return false;
}

bool waitForTlsReady(uint32_t timeoutMs) {
  if (!sMutex) return tlsHeapHeadroomOk();
  const uint32_t deadline = millis() + timeoutMs;
  while (true) {
    if (!tlsGateBusy() && tlsHeapHeadroomOk()) return true;
    if ((int32_t)(millis() - deadline) >= 0) return false;
    vTaskDelay(kReadyPollTicks);
  }
}

TlsSession::TlsSession(const char* owner, uint32_t timeoutMs)
    : owner_(owner) {
  if (!sMutex) {
    acquired_ = true;
    return;
  }
#if BADGE_HEAP_DIAG_VERBOSE
  // Log a pre-acquisition snapshot when internal heap is already below
  // the TLS threshold — see HeapDiag snapshots when debugging OTA failures.
  if (!tlsHeapHeadroomOk()) {
    const size_t intFree = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[tls] WARNING %s: internal heap too low for TLS "
                  "(free=%u largest=%u, need ≥40K/26K)\n",
                  owner ? owner : "?",
                  (unsigned)intFree, (unsigned)largest);
  }
#endif
  const TickType_t ticks =
      (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  if (xSemaphoreTakeRecursive(sMutex, ticks) == pdTRUE) {
    acquired_ = true;
  }
}

TlsSession::~TlsSession() {
  if (!acquired_ || !sMutex) return;
  xSemaphoreGiveRecursive(sMutex);
}

void kickIdleTaskWatchdog() {
  // Deliberately block this task for ≥1 tick so IDLE on this CPU is
  // scheduled even when every runnable priority-1 sibling is active.
  // 15 ms is short relative to handshake budgets yet long enough at
  // 100 Hz FreeRTOS ticks (where one tick alone can be meaningless).
  vTaskDelay(pdMS_TO_TICKS(15));
}

}  // namespace badge
