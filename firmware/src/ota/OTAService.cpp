#include "OTAService.h"

#include <Arduino.h>

#include "../api/TlsGate.h"
#include "../api/WiFiService.h"
#include "../infra/DebugLog.h"
#include "../infra/HeapDiag.h"
#include "AssetRegistry.h"
#include "BadgeOTA.h"
#include "OTAHttp.h"

#include <esp_heap_caps.h>

namespace ota {

OTAService otaService;

namespace {

constexpr uint32_t kHealthyBootMs = 30000;         // 30 s post-setup before
                                                   // we mark the running app
                                                   // valid (rollback gate)

// Defer the post-WiFi-up OTA check by a couple of seconds so DHCP /
// SNTP / clock-sync settle and the GUI has finished its first paint
// before we kick a blocking HTTPS handshake. Used to be a fragile
// workaround for back-to-back TLS contention; now it is purely a
// QoS smoothing window — the actual TLS serialization is enforced by
// `badge::TlsSession` inside `OTAHttp::getJson` / `Stream::open`.
constexpr uint32_t kWifiOtaDeferMs = 2500;

uint32_t sBootMs = 0;
bool sRollbackHandled = false;
bool sWifiWasConnected = false;
// Post-connect sequencing.
//   0 = wifi down / idle
//   1 = wifi up; waiting `kWifiOtaDeferMs` before kicking the check
//   2 = check worker spawned; waiting for it to drain
//   3 = registry refresh spawned; latched done
//   4 = done until wifi disconnects
uint8_t sPostConnectPhase = 0;
uint32_t sPostConnectMarkMs = 0;

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

  const bool wifiUp = wifiService.isConnected();
  if (!wifiUp) {
    sWifiWasConnected = false;
    sPostConnectPhase = 0;
    return;
  }
  if (!sWifiWasConnected) {
    sWifiWasConnected = true;
    sPostConnectPhase = 1;
    sPostConnectMarkMs = now;
    HeapDiag::printSnapshot("wifi-first-up");
  }
  if (sPostConnectPhase >= 4) return;

  // Both phases dispatch ASYNCHRONOUSLY to Core-0 workers. The Arduino
  // main loop (Core 1) MUST NOT call `ota::checkNow` directly any more
  // — that would block the main loop on the `badge::TlsSession` gate
  // whenever the registry worker (also on Core 0) is mid-handshake,
  // and a stuck handshake on a fragmented heap can outrun the
  // IDLE-task watchdog. Spawn-and-poll keeps Core 1 free for the GUI.
  switch (sPostConnectPhase) {
    case 1: {
      if ((uint32_t)(now - sPostConnectMarkMs) < kWifiOtaDeferMs) return;

      // Community Apps kicks `registry_refresh` on Core 0; while that fetch
      // is in-flight, defer Boot OTA-check spawn so TLS + writeToStream
      // bursts do not overlap on the TWDT'ed worker core under one gate.
      if (registry::isRefreshing()) return;

      DBG("[ota-svc] WiFi up — OTA check (async)\n");
      // Returns false if a check is already in flight — that's fine,
      // we'll observe `isCheckingAsync() == false` at phase 2 once it
      // drains, regardless of which path spawned it.
      (void)ota::beginCheckAsync(true);
      sPostConnectPhase = 2;
      sPostConnectMarkMs = now;
      break;
    }
    case 2: {
      // Wait for the OTA-check worker to drain before kicking the
      // registry refresh. They would serialise through the gate
      // anyway, but sequencing them at the scheduler level keeps the
      // Serial output linear and avoids two TLS sessions queueing on
      // the gate at boot when there's no benefit to parallelism.
      if (ota::isCheckingAsync()) return;

      DBG("[ota-svc] registry refresh (async, after OTA)\n");
      // false here means a refresh is already in flight (e.g.
      // AssetLibraryScreen.onEnter beat us to it). Latch done either
      // way — we don't respawn checks while WiFi stays up.
      (void)registry::beginRefreshAsync(true);
      sPostConnectPhase = 3;
      sPostConnectMarkMs = now;
      break;
    }
    case 3: {
      // Final latch: wait for the registry refresh to drain so we
      // emit one `scheduler_done` probe at a clean point.
      if (registry::isRefreshing()) return;
      sPostConnectPhase = 4;
      break;
    }
    default:
      break;
  }
}

}  // namespace ota
