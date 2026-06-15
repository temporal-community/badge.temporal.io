#include "BleBeaconScanner.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

#include "../api/WiFiService.h"

// ============================================================
//  Implementation notes
//
//  The Arduino BLE library binds a long-running BT controller and
//  Bluedroid stack — initialising it costs ~150 ms and a fixed RAM
//  footprint that we don't want to pay until the user opens MAP.
//  begin() therefore performs the BLEDevice::init() lazily on the
//  first startScan() call rather than at boot, so unrelated paths
//  (settings / boop / local IR) keep their pre-BLE memory budget.
//
//  The scan callback runs on the BT host task. The strongest-beacon
//  snapshot is shielded by a portMUX so the MapScreen render path
//  on Core 1 can read it without taking a heavy mutex.
// ============================================================

namespace {

// 15 s freshness window (was 5 s). Foreign iBeacons in the area flood
// the BLE adv queue and our beacon's advs sometimes go a few seconds
// without being decoded; a tight 5 s freshness threshold made the
// status footer flip OFFLINE / ONLINE during normal use even though
// the beacon was still in physical range. 15 s rides through those
// reception gaps without losing the user's location for genuine
// long-form moves.
constexpr uint32_t kBeaconFreshMs   = 15000;
constexpr uint16_t kAppleCompanyId  = 0x004C; // iBeacon mfr-data prefix
constexpr uint8_t  kAdTypeManufacturer = 0xFF;
constexpr uint8_t  kIBeaconType     = 0x02;
constexpr uint8_t  kIBeaconLen      = 0x15;

struct BeaconSnapshot {
  uint16_t uid;
  int8_t   rssi;
  uint32_t lastMs;
};

portMUX_TYPE   s_mux         = portMUX_INITIALIZER_UNLOCKED;
BeaconSnapshot s_best        = {0, -120, 0};
volatile bool  s_initialised = false;
volatile bool  s_initStarted = false;
volatile bool  s_scanPending = false;   // startScan() called before init done
volatile bool  s_scanArmed   = false;   // startScan latched (true ⇒ status icon swaps)
bool           s_scanActive  = false;
BLEScan*       s_scan        = nullptr;
// Set true after shutdownForExclusiveApp() — BLE is gone for the rest
// of this boot. begin*() paths short-circuit so doom-exit doesn't try
// to bring the controller back up (re-init has known use-after-free
// hazards we deliberately avoid).
volatile bool  s_permanentlyDead = false;

// Memory anchor — see header. BT controller's largest single allocation
// is ~30 KB; we previously kept 36 KB for margin, but on the echo-dev
// build internal DRAM fragmentation has tightened enough that the extra
// 6 KB reservation pushes the largest free block below the 24 KB TLS
// floor in steady-state. 30 KB matches the controller's actual peak
// while returning 6 KB to general-purpose internal DRAM at boot. The
// fallback retry size stays the same so anchor reservation is now a
// single attempt rather than primary+fallback.
constexpr size_t kAnchorBytes        = 30 * 1024;
constexpr size_t kAnchorMinBytes     = 30 * 1024;  // last-resort retry size
void*            s_memAnchor         = nullptr;
size_t           s_memAnchorBytes    = 0;
volatile bool    s_anchorDegraded    = false;

// Async session lifecycle: tracks whether a begin/end task is in
// flight so MapScreen rapidly entering/exiting MAP can't spawn
// overlapping tasks that would race on the BT controller and WiFi
// stack initialisation.
volatile bool  s_sessionBusy = false;
volatile bool  s_endRequested = false;
portMUX_TYPE   s_sessionMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t       s_lastCacheClearMs = 0;

// Diagnostic counters — incremented from the BLE host task callback.
// Read on the loop task for periodic "actually scanning?" prints.
volatile uint32_t s_advCount       = 0;  // every advertisement received
volatile uint32_t s_iBeaconHits    = 0;  // matched iBeacon format
volatile uint32_t s_iBeaconAuthOk  = 0;  // matched format AND UUID HMAC
volatile uint32_t s_authFailNoTime = 0;  // dropped because NTP not synced
volatile uint32_t s_authFailMisma  = 0;  // dropped because UUID didn't match any window
// Last observed iBeacon UUID (first 8 bytes only — diagnostic).
volatile uint8_t  s_lastObsUuid[8] = {0};

// HMAC-authenticated UUID cache. Beacons rotate the proximity UUID
// every 30 s as HMAC-SHA256(SHARED_KEY, epoch30_be32)[0..15], where
// epoch30 = floor(unix_time / 30). The badge recomputes the expected
// UUID for ±kCacheWindowRadius windows around the current epoch and
// accepts a beacon only if its proximity UUID matches one of them.
//
// The radius traded latency vs robustness:
//   ±1 ( 90 s tolerance) — original; failed when MAP-session NTP
//                          drift accumulated past one window
//   ±2 (150 s tolerance) — current; tolerates ~2.5 min of drift
//                          which is far more than any reasonable
//                          MAP session would accumulate
constexpr uint32_t kMinValidUnixTime  = 1700000000;
constexpr int      kCacheWindowRadius = 2;
constexpr int      kUuidCacheCount    = 2 * kCacheWindowRadius + 1;
uint32_t       s_cachedEpoch30   = 0;
uint8_t        s_expectedUuid[kUuidCacheCount][16] = {{0}};
bool           s_uuidCacheValid  = false;
portMUX_TYPE   s_uuidMux         = portMUX_INITIALIZER_UNLOCKED;

// Shared 32-byte HMAC key — MUST match BEACON_HMAC_KEY in
// Beacon-Firmware.ino.
//
// Two paths, selectable at build time via -DUSE_EFUSE_HMAC=1:
//   - Software path (default, dev builds): the key is a `const uint8_t[32]`
//     compiled into the firmware. Anyone with a flash dump of this device
//     can recover the secret.
//   - eFuse path (production): the key lives in eFuse BLOCK_KEY1 with
//     purpose HMAC_UP, burned once via espefuse.py. Software calls
//     esp_hmac_calculate(HMAC_KEY1, ...) and never sees the key bytes.
//     The block is one-time-programmable; a wrong burn permanently
//     consumes the slot.
//
// Slot choice: the fleet-wide iBeacon secret goes in BLOCK_KEY1 by
// default. Override with -DBEACON_HMAC_EFUSE_KEY=HMAC_KEY2 etc. if a
// future feature claims KEY1.
#ifndef USE_EFUSE_HMAC
#define USE_EFUSE_HMAC 0
#endif

#ifndef BEACON_HMAC_EFUSE_KEY
#define BEACON_HMAC_EFUSE_KEY HMAC_KEY1
#endif

#if USE_EFUSE_HMAC
#include "esp_hmac.h"
#else
static const uint8_t kBeaconHmacKey[32] = {
  0x65, 0xb5, 0x89, 0xa1, 0xb3, 0xcd, 0xe1, 0x57,
  0x9f, 0x82, 0xea, 0x22, 0x1e, 0xbc, 0xd7, 0x3b,
  0xdc, 0xa5, 0x06, 0xf8, 0x65, 0x81, 0xe6, 0x2c,
  0xfd, 0xac, 0x8d, 0x6a, 0xe2, 0x87, 0xa8, 0x88,
};
#endif

// HMAC-SHA-256 over (epoch30 BE32) keyed by kBeaconHmacKey, returning
// the first 16 bytes. Implemented manually with mbedtls_sha256_context
// (stack-allocated, ~108 bytes per ctx) so no heap allocation happens
// inside the BLE callback path. The previous mbedtls_md_hmac() helper
// allocated an mbedtls_md_context_t (~88 B) on the heap; once the BLE
// scanner ate the badge's largest free internal block down to ~92 B
// the alloc started failing intermittently, returning all-zero
// "expected" UUIDs and silently breaking authentication of every
// real beacon broadcast.
static void computeExpectedUuid(uint32_t epoch30, uint8_t out[16]) {
  const uint8_t input[4] = {
    (uint8_t)(epoch30 >> 24),
    (uint8_t)(epoch30 >> 16),
    (uint8_t)(epoch30 >>  8),
    (uint8_t)(epoch30 & 0xFF),
  };

#if USE_EFUSE_HMAC
  // Hardware path: HMAC peripheral pulls the fleet-wide iBeacon key from
  // eFuse (default BLOCK_KEY1; KEY0 is the per-badge API-auth secret).
  // Key bytes never reach software; an attacker with a flash dump can't
  // recover the secret. Block must have been burned with HMAC_UP purpose.
  uint8_t hmac[32] = {0};
  esp_err_t err = esp_hmac_calculate(BEACON_HMAC_EFUSE_KEY,
                                     input, sizeof(input), hmac);
  if (err != ESP_OK) {
    log_e("esp_hmac_calculate failed: %d (BLOCK_KEY1 not burned?)", err);
    memset(out, 0, 16);
    return;
  }
  memcpy(out, hmac, 16);
#else
  // Software path: in-source key. Pure-mbedTLS-sha256 implementation
  // (no heap alloc) for safety in the BLE callback path.
  uint8_t k_ipad[64];
  uint8_t k_opad[64];
  for (int i = 0; i < 32; i++) {
    k_ipad[i] = kBeaconHmacKey[i] ^ 0x36;
    k_opad[i] = kBeaconHmacKey[i] ^ 0x5C;
  }
  for (int i = 32; i < 64; i++) {
    k_ipad[i] = 0x36;
    k_opad[i] = 0x5C;
  }

  uint8_t inner[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256, not SHA-224
  mbedtls_sha256_update(&ctx, k_ipad, sizeof(k_ipad));
  mbedtls_sha256_update(&ctx, input, sizeof(input));
  mbedtls_sha256_finish(&ctx, inner);
  mbedtls_sha256_free(&ctx);

  uint8_t hmac[32];
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, k_opad, sizeof(k_opad));
  mbedtls_sha256_update(&ctx, inner, sizeof(inner));
  mbedtls_sha256_finish(&ctx, hmac);
  mbedtls_sha256_free(&ctx);

  memcpy(out, hmac, 16);
#endif
}

// Returns true once the cache is populated for the current 30 s window
// (or a usable adjacent window). Returns false if NTP hasn't synced
// — the badge can't authenticate beacons until time is set.
static bool refreshUuidCacheIfNeeded() {
  const time_t now = time(nullptr);
  if (now < (time_t)kMinValidUnixTime) {
    s_uuidCacheValid = false;
    return false;
  }
  const uint32_t epoch30 = (uint32_t)(now / 30);
  if (s_uuidCacheValid && epoch30 == s_cachedEpoch30) return true;

  // Compute UUIDs for the [-radius, +radius] window range around the
  // current epoch off-mutex (HMAC is the expensive bit), then swap
  // them in under the critical section so the BLE callback never
  // reads half-updated entries.
  uint8_t scratch[kUuidCacheCount][16];
  for (int i = 0; i < kUuidCacheCount; ++i) {
    const int32_t off = (int32_t)i - kCacheWindowRadius;
    computeExpectedUuid(epoch30 + off, scratch[i]);
  }

  portENTER_CRITICAL(&s_uuidMux);
  for (int i = 0; i < kUuidCacheCount; ++i) {
    memcpy(s_expectedUuid[i], scratch[i], 16);
  }
  s_cachedEpoch30  = epoch30;
  s_uuidCacheValid = true;
  portEXIT_CRITICAL(&s_uuidMux);
  return true;
}

class BeaconCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    s_advCount++;
    const uint8_t* mfr = nullptr;
    size_t mfrLen = 0;
    const uint8_t* payload = dev.getPayload();
    const size_t payloadLen = dev.getPayloadLength();
    for (size_t pos = 0; payload && pos < payloadLen;) {
      const uint8_t fieldLen = payload[pos++];
      if (fieldLen == 0) break;
      if (pos + fieldLen > payloadLen) break;
      const uint8_t adType = payload[pos];
      const uint8_t* data = payload + pos + 1;
      const size_t dataLen = fieldLen > 0 ? (size_t)fieldLen - 1 : 0;
      if (adType == kAdTypeManufacturer) {
        mfr = data;
        mfrLen = dataLen;
        break;
      }
      pos += fieldLen;
    }
    // iBeacon mfr-data layout:
    //   [0..1]  company ID (LE) — Apple = 0x004C
    //   [2]     type   = 0x02
    //   [3]     length = 0x15 (21 bytes)
    //   [4..19] proximity UUID (16 bytes) — HMAC-SHA256(beacon key, epoch30)[0..15]
    //   [20..21] major (uint16 BE) — room UID (loc code)
    //   [22..23] minor
    //   [24]    measured power
    if (!mfr || mfrLen < 25) return;
    const uint8_t* p = mfr;
    const uint16_t companyId = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (companyId != kAppleCompanyId) return;
    if (p[2] != kIBeaconType || p[3] != kIBeaconLen) return;

    s_iBeaconHits++;
    // Snapshot the first 8 bytes of the observed UUID — the heartbeat
    // log prints this so we can compare against what the beacon
    // serial says it's broadcasting.
    for (int i = 0; i < 8; i++) s_lastObsUuid[i] = p[4 + i];

    // UUID auth: must match one of the expected UUIDs for the current
    // ±1 30 s windows. Without NTP, refreshUuidCacheIfNeeded() returns
    // false and we drop the beacon — that's intentional, the badge
    // shouldn't claim a venue location it can't authenticate.
    if (!refreshUuidCacheIfNeeded()) {
      s_authFailNoTime++;
      return;
    }
    bool authed = false;
    portENTER_CRITICAL(&s_uuidMux);
    for (int w = 0; w < kUuidCacheCount && !authed; w++) {
      if (memcmp(p + 4, s_expectedUuid[w], 16) == 0) authed = true;
    }
    portEXIT_CRITICAL(&s_uuidMux);
    if (!authed) {
      s_authFailMisma++;
      return;
    }
    s_iBeaconAuthOk++;

    // Authenticated — major field carries the room UID (big-endian).
    const uint16_t uid = ((uint16_t)p[20] << 8) | (uint16_t)p[21];
    if (uid == 0) return;
    const int8_t rssi = (int8_t)dev.getRSSI();
    portENTER_CRITICAL(&s_mux);
    const uint32_t now = (uint32_t)millis();
    const bool stale  = (now - s_best.lastMs) > kBeaconFreshMs;
    if (stale || rssi >= s_best.rssi) {
      s_best.uid    = uid;
      s_best.rssi   = rssi;
      s_best.lastMs = now;
    } else if (uid == s_best.uid) {
      s_best.rssi   = rssi;
      s_best.lastMs = now;
    }
    portEXIT_CRITICAL(&s_mux);
  }
};

// Synchronous init — must run from a dedicated task because
// BLEDevice::init() allocates ~30 KB of internal DRAM in one chunk and
// drives the BT controller through a multi-step bring-up. Running on
// the loop task starves the GUI; running after WiFi has fragmented
// internal heap (45 KB free is the empirical floor we crashed at)
// triggers an SDK cleanup-path NULL-deref. begin() is therefore wired
// into setup() *before* osRun()'s blocking WiFi connect so init races
// against fresh memory — same pattern the reference firmware used.
bool runInit() {
  if (s_initialised) return true;
  Serial.printf("[ble] init  internalLargest=%u internalFree=%u psram=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)ESP.getFreePsram());
  // NimBLE's BLEAdvertising::reset() assumes ble_svc_gap_device_name()
  // is non-null. A blank init name is fine for scanning, but it panics
  // when the background badge beacon later asks for an advertiser.
  if (!BLEDevice::init(String("ReplayBadge"))) {
    Serial.println("[ble] BLEDevice::init failed");
    return false;
  }
  s_scan = BLEDevice::getScan();
  if (!s_scan) {
    Serial.println("[ble] getScan returned null");
    return false;
  }
  // wantDuplicates=true so RSSI updates on every packet rather than
  // once per device — needed for the "strongest beacon right now"
  // snapshot to track the user's movement. shouldParse=false keeps the
  // Arduino BLE parser from allocating service-UUID vectors for every
  // unrelated advertisement; BeaconCallback only needs raw mfr data.
  s_scan->setAdvertisedDeviceCallbacks(new BeaconCallback(), true, false);
  s_scan->setActiveScan(false);   // passive: iBeacon advs are enough
  s_scan->setInterval(160);       // 100 ms units → 100 ms
  s_scan->setWindow(150);         // 93.75 ms — near-continuous listen
  s_initialised = true;
  Serial.println("[ble] init OK");
  return true;
}

// Retained as a fallback path; current build runs runInit() synchronously
// from begin() so it completes BEFORE WiFi is started — see begin().
[[maybe_unused]] void initTask(void* /*arg*/) {
  if (runInit()) {
    if (s_scanPending) {
      s_scanPending = false;
      s_scan->start(0, nullptr, false);
      s_scanActive = true;
      Serial.println("[ble] scan started (was pending)");
    }
  } else {
    Serial.println("[ble] init FAILED — MAP proximity disabled");
    s_scanArmed = false;
  }
  vTaskDelete(nullptr);
}

}  // namespace

namespace BleBeaconScanner {

// Forward decls — defined further down in this namespace.
void beginSessionTask(void* arg);
bool runBeginSession();
void runEndSession();

bool claimSession(bool clearEndRequest) {
  bool claimed = false;
  portENTER_CRITICAL(&s_sessionMux);
  if (!s_sessionBusy) {
    s_sessionBusy = true;
    if (clearEndRequest) s_endRequested = false;
    claimed = true;
  }
  portEXIT_CRITICAL(&s_sessionMux);
  return claimed;
}

void releaseSession() {
  portENTER_CRITICAL(&s_sessionMux);
  s_sessionBusy = false;
  portEXIT_CRITICAL(&s_sessionMux);
}

bool sessionEndRequested() {
  portENTER_CRITICAL(&s_sessionMux);
  const bool requested = s_endRequested;
  portEXIT_CRITICAL(&s_sessionMux);
  return requested;
}

// Spawn the begin/end session work on a Core 0 helper task so the
// loop task (Core 1, drives the OLED contrast fade and GUI render)
// never blocks for the WiFi-stop / BLE-init / WiFi-reconnect chain.
// Without this, MapScreen::onEnter freezes the screen at low contrast
// for ~1-2 s and onExit can stall long enough for the loop watchdog
// to trip mid-fade and reset the badge.
bool beginSession() {
  if (s_permanentlyDead) {
    Serial.println("[ble] beginSession ignored: BLE was shutdown for exclusive app");
    return false;
  }
  portENTER_CRITICAL(&s_sessionMux);
  s_scanArmed = true;          // status icon + footer flip immediately
  s_endRequested = false;
  const bool alreadyInitialised = s_initialised;
  portEXIT_CRITICAL(&s_sessionMux);
  // Clear the strongest-beacon snapshot so each MAP entry starts from
  // a blank slate. Otherwise hasFix() can return true on re-entry with
  // a UID from the previous session if the freshness window hasn't
  // expired, defeating the "Locating..." → fix transition the user sees.
  portENTER_CRITICAL(&s_mux);
  s_best = {0, -120, 0};
  portEXIT_CRITICAL(&s_mux);
  if (alreadyInitialised) return true;
  if (s_anchorDegraded) {
    Serial.println("[ble] beginSession: anchor degraded, BLE init may fail");
  }
  if (!claimSession(/*clearEndRequest=*/true)) return false;
  // Async bring-up so the OLED contrast fade keeps animating while
  // WiFi tears down and BLE inits (300-500 ms total). 8 KB stack —
  // BLEDevice::init() has a deep call chain through bluedroid host
  // setup; 4 KB silently corrupted the stack and left init hung
  // (no "init OK" or "init failed" ever logged). Sync fallback in
  // case the spawn still fails so MAP doesn't hang.
  BaseType_t ok = xTaskCreatePinnedToCore(
      beginSessionTask, "ble_begin", 8192, nullptr, 2, nullptr, 0);
  if (ok != pdPASS) {
    Serial.printf("[ble] beginSession task spawn failed largest=%u — running inline\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bool inline_ok = runBeginSession();
    if (inline_ok && !sessionEndRequested() &&
        s_scanPending && s_scan && s_initialised) {
      s_scanPending = false;
      s_scan->start(0, nullptr, false);
      s_scanActive = true;
    }
    if (sessionEndRequested()) runEndSession();
    releaseSession();
    if (!inline_ok) s_scanArmed = false;
    return inline_ok;
  }
  return true;
}

bool beginControllerOnly() {
  if (s_permanentlyDead) return false;
  if (s_initialised) return true;
  if (!claimSession(/*clearEndRequest=*/true)) return false;

  BaseType_t ok = xTaskCreatePinnedToCore(
      beginSessionTask, "ble_begin", 8192, nullptr, 2, nullptr, 0);
  if (ok != pdPASS) {
    Serial.printf("[ble] beginControllerOnly task spawn failed largest=%u — running inline\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bool inline_ok = runBeginSession();
    if (inline_ok && !sessionEndRequested() &&
        s_scanPending && s_scan && s_initialised) {
      s_scanPending = false;
      s_scan->start(0, nullptr, false);
      s_scanActive = true;
    }
    if (sessionEndRequested()) runEndSession();
    releaseSession();
    return inline_ok;
  }
  return true;
}

// Forward decls for helpers used below.
void runEndSession();
void endSessionTask(void* arg);

void endSession() {
  // Always clear the armed flag synchronously so the status footer
  // and BLE icon revert immediately on MAP exit, even before the
  // background work tears the controller down.
  portENTER_CRITICAL(&s_sessionMux);
  s_scanArmed = false;
  s_scanPending = false;
  s_endRequested = true;
  const bool initialised = s_initialised;
  const bool busy = s_sessionBusy;
  portEXIT_CRITICAL(&s_sessionMux);
  if (!initialised && !busy) return;  // nothing to undo
  if (busy) {
    Serial.println("[ble] endSession: previous session task still busy");
    return;
  }
  // Async teardown — BLE deinit (~300 ms) + anchor re-reserve + WiFi
  // reconnect (3-10 s) all run in a Core 0 helper task so the loop
  // task on Core 1 keeps driving the OLED contrast fade. Doing any
  // of this inline races with the GUIManager transition pipeline
  // (LoadProhibited / xTaskPriorityDisinherit asserts in BT cleanup
  // when the loop task touches BLE state mid-deinit).
  if (!claimSession(/*clearEndRequest=*/false)) return;
  BaseType_t ok = xTaskCreatePinnedToCore(
      endSessionTask, "ble_end", 8192, nullptr, 2, nullptr, 0);
  if (ok != pdPASS) {
    Serial.printf("[ble] endSession task spawn failed largest=%u — running inline\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    runEndSession();
    releaseSession();
  }
}

bool reserveMemoryAnchor() {
  if (s_memAnchor) return true;
  s_memAnchorBytes = kAnchorBytes;
  s_memAnchor = heap_caps_malloc(s_memAnchorBytes, MALLOC_CAP_INTERNAL);
  if (!s_memAnchor && kAnchorMinBytes < kAnchorBytes) {
    s_memAnchorBytes = kAnchorMinBytes;
    s_memAnchor = heap_caps_malloc(s_memAnchorBytes, MALLOC_CAP_INTERNAL);
  }
  if (!s_memAnchor) {
    s_memAnchorBytes = 0;
    s_anchorDegraded = true;
    Serial.printf("[ble] anchor reserve FAILED — internal heap too tight "
                  "(largest=%u free=%u)\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return false;
  }
  Serial.printf("[ble] anchor reserved %u bytes%s  remainingLargest=%u\n",
                (unsigned)s_memAnchorBytes,
                (s_memAnchorBytes < kAnchorBytes) ? " fallback" : "",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  s_anchorDegraded = false;
  return true;
}

bool releaseMemoryAnchor() {
  if (!s_memAnchor || s_initialised || s_sessionBusy) return false;
  heap_caps_free(s_memAnchor);
  s_memAnchor = nullptr;
  s_memAnchorBytes = 0;
  Serial.printf("[ble] anchor released for WiFi/TLS  internalLargest=%u internalFree=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

// Synchronous bring-up — runs inside beginSessionTask on Core 0 so the
// loop task (Core 1, drives OLED contrast fade + GUI) stays responsive
// while WiFi is torn down and BLE inits.
bool runBeginSession() {
  if (s_initialised) return true;
  Serial.printf("[ble] beginSession  internalLargest=%u internalFree=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  // Offline firmware has no background API transport to drain before a
  // radio swap. Keep BLE self-contained for future opt-in builds.
  if (WiFi.getMode() != WIFI_OFF) {
    Serial.println("[ble] stopping WiFi for BLE session");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    esp_wifi_deinit();
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  Serial.printf("[ble] post-WiFi-stop  internalLargest=%u internalFree=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  // Release the reserved anchor so BLEDevice::init has the
  // contiguous internal-DRAM block btdm_controller_init needs.
  if (s_memAnchor) {
    heap_caps_free(s_memAnchor);
    s_memAnchor = nullptr;
    Serial.printf("[ble] anchor released  internalLargest=%u internalFree=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
  s_initStarted = true;
  return runInit();
}

// Lightweight teardown: stop scanning but keep the BT controller and
// the BLEScan object alive. Arduino-ESP32 BLE has known use-after-free
// hazards in BLEDevice::deinit / re-init cycles (0xbaad5678 poisoned-
// freed pointer dereferences inside the bluedroid host task), so we
// init once and never tear down.
//
// We also do NOT bring WiFi back. Bluedroid's static + dynamic
// footprint is large enough that esp_wifi_init() returns
// ESP_ERR_NO_MEM after BLE is up (largest internal block drops to
// ~2 KB), and the cascading "create wifi task: failed" retries
// starve the loop task hard enough that the OLED contrast fade
// can't finish — the user sees a stuck low-contrast home screen.
//
// Trade-off: WiFi-dependent features (sync, messages, pings) stop
// working after the first MAP entry. The user reboots to get WiFi
// back. This is the lesser evil compared to BLE deinit crashes.
void runEndSession() {
  if (s_scan && s_scanActive) {
    s_scan->stop();
    s_scan->clearResults();
    s_scanActive = false;
  }
  s_scanArmed   = false;
  s_scanPending = false;
  s_endRequested = false;
  Serial.printf("[ble] endSession  scan stopped (controller stays up, WiFi NOT restarted)  largest=%u free=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void endSessionTask(void* /*arg*/) {
  runEndSession();
  Serial.printf("[ble] endSession task stack_hwm_bytes=%u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  releaseSession();
  vTaskDelete(nullptr);
}

// Body of the shutdown — must run on Core 0 so BLEDevice::deinit
// matches the host task's core (deinit from another core is the
// known LoadProhibited / xTaskPriorityDisinherit hazard).
static void shutdownForExclusiveAppCore0() {
  const bool wasInitialised = s_initialised;
  Serial.printf("[ble] shutdownForExclusiveApp begin  initialised=%d  largest=%u free=%u\n",
                wasInitialised ? 1 : 0,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (s_scan) {
    if (s_scanActive) s_scan->stop();
    s_scan->clearResults();
    s_scanActive = false;
    s_scanArmed = false;
  }

  // Only touch the BT controller / mem_release path when BLE was
  // actually initialised. esp_bt_mem_release on a never-inited
  // controller is undefined behaviour — empirically it released
  // memory regions still in use by the system and corrupted the
  // MicroPython heap, which crashed the GC inside the next Python
  // app launch. When BLE was never up, simply releasing the
  // memory anchor below recovers the headroom we want for doom.
  if (wasInitialised) {
    BLEDevice::deinit(/*release_memory=*/true);
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_mem_release(ESP_BT_MODE_BLE);
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
  }

  s_initialised = false;
  s_initStarted = false;
  s_scanPending = false;
  s_endRequested = false;
  s_scan = nullptr;
  s_permanentlyDead = true;

  if (s_memAnchor) {
    heap_caps_free(s_memAnchor);
    s_memAnchor = nullptr;
    s_memAnchorBytes = 0;
  }

  Serial.printf("[ble] shutdownForExclusiveApp done  largest=%u free=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

struct ShutdownCookie {
  volatile bool done;
};

static void shutdownTaskTramp(void* arg) {
  ShutdownCookie* cookie = static_cast<ShutdownCookie*>(arg);
  shutdownForExclusiveAppCore0();
  if (cookie) cookie->done = true;
  vTaskDelete(nullptr);
}

void shutdownForExclusiveApp() {
  if (s_permanentlyDead) return;

  // Wait briefly for any in-flight session task (begin/end) to clear
  // before tearing the controller down. Up to 2 seconds — beginSession
  // takes ~500 ms in practice.
  const uint32_t waitStart = millis();
  while (s_sessionBusy && (millis() - waitStart) < 2000) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (s_sessionBusy) {
    Serial.println("[ble] shutdownForExclusiveApp: sessionBusy timeout — proceeding anyway");
  }

  // Spawn the deinit on Core 0 so BLEDevice::deinit runs on the
  // same core as the BT host task. Doing this from the loop task
  // (Core 1) is exactly the LoadProhibited/priority-inheritance
  // hazard the existing endSession code goes out of its way to
  // avoid. We block until the helper finishes — doom is going
  // to take over anyway, no benefit to async here.
  ShutdownCookie cookie = { false };
  BaseType_t ok = xTaskCreatePinnedToCore(
      shutdownTaskTramp, "ble_shut", 8192, &cookie, 2, nullptr, 0);
  if (ok != pdPASS) {
    Serial.println("[ble] shutdownForExclusiveApp: helper spawn failed — running inline");
    shutdownForExclusiveAppCore0();
    return;
  }
  // Block up to 5 seconds for the helper. BLE deinit is ~300 ms typical.
  const uint32_t deadline = millis() + 5000;
  while (!cookie.done && millis() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (!cookie.done) {
    Serial.println("[ble] shutdownForExclusiveApp: helper timeout");
    s_permanentlyDead = true;  // mark dead anyway so callers degrade
  }
}

void clearScanCache() {
  // Arduino-ESP32's BLEScan with wantDuplicates=true keeps every
  // received advertisement in an internal std::vector forever, which
  // turns into the dominant heap consumer during a long MAP session
  // (~400 advs/s × 30 s = 12000 entries). MapScreen's heartbeat calls
  // this every 2 s to bound that growth.
  if (s_scan && s_scanActive) {
    s_scan->clearResults();
  }
}

void serviceScanCache() {
  if (!s_scan || !s_scanActive) return;
  const uint32_t nowMs = millis();
  if (nowMs - s_lastCacheClearMs < 2000) return;
  s_lastCacheClearMs = nowMs;
  clearScanCache();
}

void beginSessionTask(void* /*arg*/) {
  const bool ok = runBeginSession();
  if (!ok) {
    portENTER_CRITICAL(&s_sessionMux);
    s_scanArmed = false;
    s_scanPending = false;
    portEXIT_CRITICAL(&s_sessionMux);
  }
  if (ok && !sessionEndRequested() && s_scanPending && s_scan && s_initialised) {
    s_scanPending = false;
    s_scan->start(0, nullptr, false);
    s_scanActive = true;
    Serial.println("[ble] scan started (was pending)");
  }
  if (sessionEndRequested()) runEndSession();
  Serial.printf("[ble] beginSession task stack_hwm_bytes=%u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  releaseSession();
  vTaskDelete(nullptr);
}


void startScan() {
  if (s_permanentlyDead) return;
  portENTER_CRITICAL(&s_sessionMux);
  s_scanArmed = true;
  s_endRequested = false;
  portEXIT_CRITICAL(&s_sessionMux);
  if (!s_initialised || !s_scan) {
    // Init still in flight — record the request so the init task
    // starts the scan as soon as the controller is ready.
    s_scanPending = true;
    return;
  }
  if (s_scanActive) return;
  // Continuous scan: duration=0 keeps the controller scanning until
  // explicit stop. is_continue=false starts a fresh window so the
  // callback delivery resumes promptly.
  s_scan->start(0, nullptr, false);
  s_scanActive = true;
}

void stopScan() {
  portENTER_CRITICAL(&s_sessionMux);
  s_scanPending = false;
  s_scanArmed   = false;
  if (s_sessionBusy) s_endRequested = true;
  portEXIT_CRITICAL(&s_sessionMux);
  if (!s_initialised || !s_scan || !s_scanActive) return;
  s_scan->stop();
  s_scan->clearResults();
  s_scanActive = false;
}

bool isScanning() {
  if (s_permanentlyDead) return false;
  return s_scanArmed;
}

bool blocksWiFi() {
  // After shutdownForExclusiveApp the controller is gone for good and
  // its memory has been released back to the general heap; WiFi is
  // free to use the radio without coordinating with us.
  if (s_permanentlyDead) return false;
  return s_scanArmed || s_scanActive || s_initialised || s_sessionBusy;
}

uint32_t advCount() {
  return s_advCount;
}

uint32_t iBeaconCount() {
  return s_iBeaconHits;
}

uint32_t iBeaconAuthOkCount() {
  return s_iBeaconAuthOk;
}

uint32_t authFailNoTime() {
  return s_authFailNoTime;
}

uint32_t authFailMismatch() {
  return s_authFailMisma;
}

uint32_t currentEpoch30() {
  const time_t now = time(nullptr);
  if (now < (time_t)kMinValidUnixTime) return 0;
  return (uint32_t)(now / 30);
}

uint32_t cachedEpoch30() {
  return s_uuidCacheValid ? s_cachedEpoch30 : 0;
}

void copyLastObservedUuid(uint8_t out[8]) {
  for (int i = 0; i < 8; i++) out[i] = s_lastObsUuid[i];
}

bool isInitialised() {
  return s_initialised;
}

void computeUuidForEpoch(uint32_t epoch30, uint8_t out[16]) {
  computeExpectedUuid(epoch30, out);
}

bool hasFix() {
  portENTER_CRITICAL(&s_mux);
  const uint32_t lastMs = s_best.lastMs;
  const uint16_t uid    = s_best.uid;
  portEXIT_CRITICAL(&s_mux);
  if (uid == 0 || lastMs == 0) return false;
  return ((uint32_t)millis() - lastMs) <= kBeaconFreshMs;
}

uint16_t bestRoomUid() {
  if (!hasFix()) return 0;
  portENTER_CRITICAL(&s_mux);
  const uint16_t uid = s_best.uid;
  portEXIT_CRITICAL(&s_mux);
  return uid;
}

int floorForUid(uint16_t uid) {
  if (uid == 0) return -1;
  if (uid >= 900 && uid < 1000) return 5;   // off-site
  const int f = uid / 100;
  if (f < 0 || f > 4) return -1;
  return f;
}

int sectionForUid(uint16_t uid) {
  if (uid == 0) return -1;
  if (uid >= 900 && uid < 1000) return (uid - 900) / 10;
  const int s = (uid / 10) % 10;
  return s;
}

}  // namespace BleBeaconScanner
