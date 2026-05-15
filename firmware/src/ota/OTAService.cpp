#include "OTAService.h"

#include <Arduino.h>

#include "AssetRegistry.h"
#include "BadgeOTA.h"

namespace ota {

OTAService otaService;

namespace {

constexpr uint32_t kTickIntervalMs = 30000;        // 30 s
constexpr uint32_t kHealthyBootMs = 30000;         // 30 s post-setup before
                                                   // we mark the running app
                                                   // valid (rollback gate)

uint32_t sLastTickMs = 0;
uint32_t sBootMs = 0;
bool sRollbackHandled = false;

}  // namespace

void OTAService::service() {
  const uint32_t now = millis();
  if (sBootMs == 0) sBootMs = now;

  // Rollback gate. If we booted into ESP_OTA_IMG_PENDING_VERIFY and
  // we've been ticking for 30 s without a panic/reset, mark the
  // running app as valid so the bootloader stops considering rollback.
  if (!sRollbackHandled && (now - sBootMs) >= kHealthyBootMs) {
    if (runningPendingVerify()) {
      markCurrentAppValidIfPending();
    }
    sRollbackHandled = true;
  }

  if (sLastTickMs != 0 && (now - sLastTickMs) < kTickIntervalMs) return;
  sLastTickMs = now;

  // Both ticks bail out early if WiFi is down or the cooldown hasn't
  // elapsed; cheap to call.
  ota::tick();
  registry::tick();
}

}  // namespace ota
