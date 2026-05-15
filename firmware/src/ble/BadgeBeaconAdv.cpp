#include "BadgeBeaconAdv.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <time.h>

#include "../api/WiFiService.h"
#include "../identity/BadgeUID.h"
#include "../ir/BadgeIR.h"
#include "BleBeaconScanner.h"

namespace {

// ── Tunables ──────────────────────────────────────────────────────────
constexpr uint32_t kIntervalMs       = 30000;  // base cadence between cycles
constexpr uint32_t kJitterMs         = 10000;  // ±10 s spread
constexpr uint32_t kBroadcastMs      = 5000;   // window length per cycle
// 25 s lead-in: WiFi typically associates within ~10 s and NTP syncs
// in another 1-3 s. Holding off this long means our first iBeacon
// cycle goes out with a properly-set system clock so the rotating
// HMAC UUID is computed against the real epoch (otherwise it'd be
// zeros and unauthenticatable). After this we tear WiFi down and
// keep BLE up for the rest of the boot session.
constexpr uint32_t kInitialDelayMs   = 25000;
constexpr uint32_t kPollWaitMs       = 1000;   // sleep when blocked on BLE
                                                // controller not yet up
constexpr uint16_t kBadgeMajor       = 0xBADD; // sentinel = "this is a badge"
constexpr size_t   kHeadroomBytes    = 2048;   // pre-allocated DRAM slab freed
                                                // right before each broadcast so
                                                // BLEAdvertising::start() has a
                                                // contiguous block even when the
                                                // scanner has fragmented heap

// ── Pre-allocated working state ───────────────────────────────────────
// Allocated once on the broadcaster task's first iteration (after
// BLE controller is up) and reused for every cycle thereafter — so
// the per-cycle path doesn't re-allocate Strings or BLEAdvertisement
// objects under tight heap.
BLEAdvertising*       s_adv          = nullptr;
BLEAdvertisementData* s_advData      = nullptr;
BLEAdvertisementData* s_scanResp     = nullptr;
String                s_mfrData;            // capacity reserved to 25 once
String                s_localName;          // "BADGE-XXXXXX"
TaskHandle_t          s_taskHandle   = nullptr;
void*                 s_headroomSlab = nullptr;
bool                  s_started      = false;
bool                  s_loggedWifiWait = false;
volatile bool         s_pausedForIr  = false;
volatile bool         s_pausedForForeground = false;
volatile bool         s_broadcasting = false;

// ── iBeacon payload ───────────────────────────────────────────────────
// Layout (bytes 0..24, mirroring Beacon-Firmware.ino's emitter):
//   [0..1]  Apple company ID (LE) = 0x004C
//   [2]     iBeacon type   = 0x02
//   [3]     iBeacon length = 0x15
//   [4..19] proximity UUID = HMAC-SHA-256(KEY, epoch30)[0..15]
//   [20..21] major BE — 0xBADD ("badge", not a venue beacon)
//   [22..23] minor BE — last 16 bits of the eFuse MAC (per-attendee)
//   [24]    measured power = 0xC5 (-59 dBm)
void buildBadgePayload(uint8_t out[25]) {
  out[0] = 0x4C;
  out[1] = 0x00;
  out[2] = 0x02;
  out[3] = 0x15;

  uint32_t epoch30 = 0;
  const time_t now = time(nullptr);
  if (now > (time_t)1700000000) epoch30 = (uint32_t)(now / 30);
  uint8_t uuid[16] = {};
  BleBeaconScanner::computeUuidForEpoch(epoch30, uuid);
  memcpy(out + 4, uuid, 16);

  out[20] = (uint8_t)(kBadgeMajor >> 8);
  out[21] = (uint8_t)(kBadgeMajor & 0xFF);

  uint16_t minor = (uint16_t)((uid[4] << 8) | uid[5]);
  out[22] = (uint8_t)(minor >> 8);
  out[23] = (uint8_t)(minor & 0xFF);

  out[24] = 0xC5;
}

void initLocalName() {
  char buf[16];
  snprintf(buf, sizeof(buf), "BADGE-%02X%02X%02X", uid[3], uid[4], uid[5]);
  s_localName = String(buf);
}

void initMfrData() {
  // Reserve 25 bytes once so subsequent assignments inside the
  // broadcast loop never trigger a heap re-allocation.
  s_mfrData = String();
  s_mfrData.reserve(32);
}

// Push the current epoch's payload into s_advData. Called every cycle
// so the broadcast UUID rotates with the venue scheme.
void rebuildAdvData() {
  uint8_t payload[25];
  buildBadgePayload(payload);

  // Use operator= with "" to clear content without freeing the internal
  // buffer that reserve(32) allocated in initMfrData(). Assigning String()
  // would destroy that reservation and cause 4-5 heap reallocations below.
  s_mfrData = "";
  for (size_t i = 0; i < sizeof(payload); ++i) s_mfrData += (char)payload[i];

  *s_advData = BLEAdvertisementData();
  s_advData->setFlags(0x06);
  s_advData->setManufacturerData(s_mfrData);

  *s_scanResp = BLEAdvertisementData();
  s_scanResp->setName(s_localName);
}

// One-time bring-up of cached BLE objects + headroom slab. Returns
// true once the controller is ready and our static state is wired
// up. Idempotent — subsequent calls return true cheaply.
bool ensurePrepared() {
  if (s_advData && s_scanResp && s_adv) return true;
  if (!BleBeaconScanner::isInitialised()) return false;

  if (!s_adv) s_adv = BLEDevice::getAdvertising();
  if (!s_adv) return false;

  if (!s_advData)  s_advData  = new BLEAdvertisementData();
  if (!s_scanResp) s_scanResp = new BLEAdvertisementData();
  initLocalName();
  initMfrData();

  // 100-200 ms intervals — slow enough not to crowd the radio while
  // a 5 s window still surfaces ~30 packets to any nearby scanner.
  s_adv->setMinInterval(0xA0);  // 100 ms
  s_adv->setMaxInterval(0x140); // 200 ms

  if (!s_headroomSlab) {
    s_headroomSlab = heap_caps_malloc(kHeadroomBytes, MALLOC_CAP_INTERNAL);
    Serial.printf("[badgebeacon] headroom slab %u B → %p\n",
                  (unsigned)kHeadroomBytes, s_headroomSlab);
  }
  return true;
}

bool conditionsAllow() {
  if (s_pausedForForeground || s_pausedForIr ||
      irHardwareEnabled || pythonIrListening) return false;
  if (wifiService.isAutoSyncInProgress()) return false;
  if (wifiService.isConnected()) return false;  // don't crowd HTTPS
  return true;
}

bool wifiReadyForBleHandoff() {
  if (wifiService.isAutoSyncInProgress()) return false;
  // Give WiFi first claim after boot. Until the badge has connected at
  // least once, BLE would only consume the controller/internal-heap window
  // and block the retry that could actually get us online.
  return wifiService.hasEverConnected();
}

void broadcastOnce() {
  const bool restartScan = BleBeaconScanner::isScanning();
  if (restartScan) {
    Serial.println("[badgebeacon] pausing scan for broadcast window");
    BleBeaconScanner::stopScan();
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Free the headroom slab right before reconfiguring the advertiser
  // so any internal allocs (e.g. NimBLE's adv-data buffer) have a
  // contiguous block. Reclaim a fresh slab when the window closes
  // so the next cycle starts from the same baseline.
  if (s_headroomSlab) {
    heap_caps_free(s_headroomSlab);
    s_headroomSlab = nullptr;
  }

  rebuildAdvData();
  s_adv->setAdvertisementData(*s_advData);
  s_adv->setScanResponseData(*s_scanResp);
  s_broadcasting = true;
  s_adv->start();
  Serial.printf("[badgebeacon] broadcasting %s for %lu ms\n",
                s_localName.c_str(),
                (unsigned long)kBroadcastMs);

  uint32_t elapsedMs = 0;
  while (elapsedMs < kBroadcastMs &&
         !s_pausedForForeground &&
         !s_pausedForIr && !irHardwareEnabled && !pythonIrListening) {
    constexpr uint32_t kSliceMs = 100;
    vTaskDelay(pdMS_TO_TICKS(kSliceMs));
    elapsedMs += kSliceMs;
  }

  s_adv->stop();
  s_broadcasting = false;
  Serial.println("[badgebeacon] broadcast window closed");

  // Re-claim the headroom slab. If the heap is too tight we'll get a
  // NULL back and continue — the next cycle just runs without the
  // safety net.
  s_headroomSlab = heap_caps_malloc(kHeadroomBytes, MALLOC_CAP_INTERNAL);

  if (restartScan &&
      !s_pausedForForeground &&
      !s_pausedForIr && !irHardwareEnabled && !pythonIrListening) {
    BleBeaconScanner::startScan();
    Serial.println("[badgebeacon] scan resumed after broadcast");
  }
}

uint32_t nextWaitMs() {
  const int32_t jitter =
      (int32_t)(esp_random() % (2 * kJitterMs + 1)) - (int32_t)kJitterMs;
  return (uint32_t)((int32_t)kIntervalMs + jitter);
}

void taskFn(void* /*arg*/) {
  Serial.println("[badgebeacon] task started");
  vTaskDelay(pdMS_TO_TICKS(kInitialDelayMs));

  for (;;) {
    if (!wifiReadyForBleHandoff()) {
      // Let WiFi finish its first real connect/sync opportunity before BLE
      // tears the WiFi stack down. This avoids the WiFi task allocating
      // against the heap after BLE has taken ownership of the radio memory.
      if (!s_loggedWifiWait && !wifiService.hasEverConnected()) {
        Serial.println("[badgebeacon] waiting for first WiFi success before BLE handoff");
        s_loggedWifiWait = true;
      }
      vTaskDelay(pdMS_TO_TICKS(kPollWaitMs));
      continue;
    }
    if (s_loggedWifiWait) {
      Serial.println("[badgebeacon] first WiFi success observed; BLE handoff allowed");
      s_loggedWifiWait = false;
    }
    if (!conditionsAllow()) {
      vTaskDelay(pdMS_TO_TICKS(nextWaitMs()));
      continue;
    }
    if (!BleBeaconScanner::isInitialised()) {
      // Background advertising must not be the thing that hands the radio
      // from WiFi to BLE. MAP is the explicit user action that does that;
      // once MAP has initialised BLE, the advertiser can piggyback.
      vTaskDelay(pdMS_TO_TICKS(nextWaitMs()));
      continue;
    }
    if (!ensurePrepared()) {
      vTaskDelay(pdMS_TO_TICKS(kPollWaitMs));
      continue;
    }
    broadcastOnce();
    Serial.printf("[badgebeacon] task stack_hwm_bytes=%u\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    vTaskDelay(pdMS_TO_TICKS(nextWaitMs()));
  }
}

}  // namespace

namespace BadgeBeaconAdv {

void begin() {
  if (s_started) return;
  s_started = true;
  // 8 KB stack — broadcastOnce()'s call chain into NimBLE's
  // ble_gap_adv_set_data path is the deepest user-facing stack we
  // exercise here, and 4 KB tripped the canary on the first
  // ble_npl_event allocation. 8 KB matches the IR / scan-init tasks.
  // Pinned to Core 0 to share affinity with the BLE host task so
  // adv-state changes don't bounce across cores.
  BaseType_t ok = xTaskCreatePinnedToCore(
      taskFn, "badge_beacon", 8192, nullptr, 2, &s_taskHandle, 0);
  if (ok != pdPASS) {
    Serial.println("[badgebeacon] task spawn failed");
    s_taskHandle = nullptr;
    s_started    = false;
  }
}

void setPausedForIr(bool paused) {
  s_pausedForIr = paused;
}

void setPausedForForeground(bool paused) {
  s_pausedForForeground = paused;
}

bool isBroadcasting() {
  return s_broadcasting;
}

}  // namespace BadgeBeaconAdv
