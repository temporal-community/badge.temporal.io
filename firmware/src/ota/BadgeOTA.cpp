#include "BadgeOTA.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "OTAHttp.h"
#include "../api/WiFiService.h"
#include "../hardware/Power.h"
#include "../identity/BadgeVersion.h"
#include "../infra/BadgeConfig.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
int replay_bdev_reformat_and_reboot(void);
}

extern BatteryGauge batteryGauge;

namespace ota {

namespace {

constexpr const char* kNvsNamespace = "badge_ota";
constexpr const char* kNvsLastEpoch = "last_epoch";
constexpr const char* kNvsLatestTag = "latest_tag";
constexpr const char* kNvsAssetUrl  = "asset_url";
constexpr const char* kNvsAssetSize = "asset_size";

constexpr size_t kTagMax = 32;
constexpr size_t kUrlMax = 256;

char sLatestTag[kTagMax] = "";
char sAssetUrl[kUrlMax] = "";
size_t sAssetSize = 0;
time_t sLastCheckEpoch = 0;
char sLastError[80] = "";
bool sBegun = false;
bool sPendingVerify = false;
bool sValidated = false;

void setError(const char* msg) {
  if (!msg) msg = "";
  std::strncpy(sLastError, msg, sizeof(sLastError) - 1);
  sLastError[sizeof(sLastError) - 1] = '\0';
}

void persistCache() {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  p.putString(kNvsLatestTag, sLatestTag);
  p.putString(kNvsAssetUrl, sAssetUrl);
  p.putULong(kNvsAssetSize, static_cast<uint32_t>(sAssetSize));
  p.putULong(kNvsLastEpoch, static_cast<uint32_t>(sLastCheckEpoch));
  p.end();
}

void loadCache() {
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  p.getString(kNvsLatestTag, sLatestTag, sizeof(sLatestTag));
  p.getString(kNvsAssetUrl, sAssetUrl, sizeof(sAssetUrl));
  sAssetSize = p.getULong(kNvsAssetSize, 0);
  sLastCheckEpoch = static_cast<time_t>(p.getULong(kNvsLastEpoch, 0));
  p.end();
}

// Strip a leading 'v' / 'V' from a tag string. Many GitHub releases
// tag as "v1.2.3" — semver compare wants the numeric part.
const char* stripV(const char* s) {
  if (!s) return "";
  if (*s == 'v' || *s == 'V') return s + 1;
  return s;
}

// Parse up to three dotted numeric components (a.b.c). Missing
// components default to 0. Trailing pre-release tags ("-rc1") are
// ignored — they sort earlier than the same numeric version, but
// we treat them as equal for "newer?" purposes (close enough for a
// conference badge OTA).
void parseSemver(const char* s, int* a, int* b, int* c) {
  *a = *b = *c = 0;
  if (!s) return;
  s = stripV(s);
  *a = atoi(s);
  const char* dot1 = std::strchr(s, '.');
  if (!dot1) return;
  *b = atoi(dot1 + 1);
  const char* dot2 = std::strchr(dot1 + 1, '.');
  if (!dot2) return;
  *c = atoi(dot2 + 1);
}

int compareSemver(const char* a, const char* b) {
  int aa, ab, ac, ba, bb, bc;
  parseSemver(a, &aa, &ab, &ac);
  parseSemver(b, &ba, &bb, &bc);
  if (aa != ba) return (aa < ba) ? -1 : 1;
  if (ab != bb) return (ab < bb) ? -1 : 1;
  if (ac != bc) return (ac < bc) ? -1 : 1;
  return 0;
}

bool batteryAllowsInstall() {
#ifdef BADGE_HAS_BATTERY_GAUGE
  if (!batteryGauge.isReady()) return true;
  // Allow install if charger is plugged in regardless of charge level
  // — the worst-case brownout is mitigated and the bootloader will
  // rollback if the new image fails.
  if (batteryGauge.usbPresent()) return true;
  return batteryGauge.stateOfChargePercent() >=
         static_cast<float>(kMinBatteryPct);
#else
  return true;
#endif
}

}  // namespace

void begin() {
  if (sBegun) return;
  sBegun = true;
  loadCache();

  // Detect "we just OTA-installed and haven't been validated yet".
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
      sPendingVerify = (state == ESP_OTA_IMG_PENDING_VERIFY);
      Serial.printf("[ota] running partition state=%d pending_verify=%d\n",
                    (int)state, sPendingVerify ? 1 : 0);
    }
  }
  Serial.printf("[ota] cache loaded: tag='%s' size=%u last_epoch=%lu\n",
                sLatestTag, (unsigned)sAssetSize,
                (unsigned long)sLastCheckEpoch);
}

void tick() {
  if (!sBegun) return;
  if (!wifiService.isConnected()) return;
  if (!wifiService.clockReady()) return;
  const time_t now = time(nullptr);
  if (now <= 0) return;
  if (sLastCheckEpoch != 0 &&
      (uint32_t)(now - sLastCheckEpoch) < kCheckCooldownSec) {
    return;
  }
  Serial.println("[ota] daily check fired");
  checkNow(false);
}

CheckResult checkNow(bool ignoreCooldown) {
  if (!sBegun) begin();
  setError("");

  if (!ignoreCooldown && sLastCheckEpoch != 0 &&
      wifiService.clockReady()) {
    const time_t now = time(nullptr);
    if (now > 0 && (uint32_t)(now - sLastCheckEpoch) < kCheckCooldownSec) {
      return CheckResult::kCooldownActive;
    }
  }

  char url[256];
  const char* overrideUrl = badgeConfig.otaManifestUrl();
  if (overrideUrl && overrideUrl[0]) {
    std::snprintf(url, sizeof(url), "%s", overrideUrl);
  } else {
    std::snprintf(url, sizeof(url),
                  "https://api.github.com/repos/%s/releases/latest",
                  OTA_GITHUB_REPO);
  }

  char* body = nullptr;
  size_t bodyLen = 0;
  HttpResult httpRes = getJson(url, &body, &bodyLen, kJsonMaxBytes, 20000);
  if (!httpRes.ok) {
    setError(httpRes.error);
    if (body) std::free(body);
    return CheckResult::kNetworkError;
  }

  // GitHub release JSON. We only care about `tag_name` + the asset
  // whose `name` matches OTA_ASSET_NAME.
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "json parse: %s", err.c_str());
    setError(buf);
    std::free(body);
    return CheckResult::kParseError;
  }

  const char* tag = doc["tag_name"] | "";
  if (!tag[0]) {
    setError("release missing tag_name");
    std::free(body);
    return CheckResult::kParseError;
  }

  // Find matching asset.
  const char* assetUrl = nullptr;
  size_t assetSize = 0;
  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject a : assets) {
    const char* name = a["name"] | "";
    if (std::strcmp(name, OTA_ASSET_NAME) == 0) {
      assetUrl = a["browser_download_url"] | "";
      assetSize = a["size"] | 0u;
      break;
    }
  }

  if (!assetUrl || !assetUrl[0]) {
    // Cache the tag (so we can show "v0.1.4 has no asset") but mark
    // result as no-asset.
    std::strncpy(sLatestTag, tag, sizeof(sLatestTag) - 1);
    sLatestTag[sizeof(sLatestTag) - 1] = '\0';
    sAssetUrl[0] = '\0';
    sAssetSize = 0;
    sLastCheckEpoch = wifiService.clockReady() ? time(nullptr) : sLastCheckEpoch;
    persistCache();
    char buf[80];
    std::snprintf(buf, sizeof(buf),
                  "release %s has no '%s' asset", tag, OTA_ASSET_NAME);
    setError(buf);
    std::free(body);
    return CheckResult::kNoMatchingAsset;
  }

  std::strncpy(sLatestTag, tag, sizeof(sLatestTag) - 1);
  sLatestTag[sizeof(sLatestTag) - 1] = '\0';
  std::strncpy(sAssetUrl, assetUrl, sizeof(sAssetUrl) - 1);
  sAssetUrl[sizeof(sAssetUrl) - 1] = '\0';
  sAssetSize = assetSize;
  sLastCheckEpoch = wifiService.clockReady() ? time(nullptr) : 1;  // 1 = "checked once, no clock"
  persistCache();
  std::free(body);

  const int cmp = compareSemver(sLatestTag, FIRMWARE_VERSION);
  Serial.printf("[ota] latest=%s current=%s cmp=%d size=%u\n",
                sLatestTag, FIRMWARE_VERSION, cmp, (unsigned)sAssetSize);

  if (cmp > 0) return CheckResult::kOkNewerAvailable;
  if (cmp == 0) return CheckResult::kOkUpToDate;
  return CheckResult::kOkOlder;
}

bool updateAvailable() {
  if (!sBegun) return false;
  if (sLatestTag[0] == '\0') return false;
  if (sAssetUrl[0] == '\0') return false;
  return compareSemver(sLatestTag, FIRMWARE_VERSION) > 0;
}

const char* latestKnownTag() { return sLatestTag; }
const char* latestKnownAssetUrl() { return sAssetUrl; }
size_t latestKnownAssetSize() { return sAssetSize; }
time_t lastCheckEpoch() { return sLastCheckEpoch; }
const char* lastErrorMessage() { return sLastError; }

InstallResult installCached(InstallProgressCb cb, void* user) {
  setError("");
  if (sAssetUrl[0] == '\0') return InstallResult::kNoAssetCached;
  if (!batteryAllowsInstall()) {
    setError("battery too low — plug in to update");
    return InstallResult::kBatteryTooLow;
  }
  if (!wifiService.connect()) {
    setError("wifi unavailable");
    return InstallResult::kWifiUnavailable;
  }

  // Hold WiFi awake + CPU at 240 MHz across the multi-MB firmware
  // pull. Same rationale as the AssetRegistry installer — modem sleep
  // and CPU scaling between chunks cap throughput at single-digit
  // percentages of the link rate.
  ThroughputBoost boost;
  Stream s;
  if (!s.open(sAssetUrl, 30000)) {
    setError(s.lastError());
    return InstallResult::kHttpError;
  }
  size_t total = s.contentLength();
  if (total == 0 && sAssetSize > 0) total = sAssetSize;

  if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "Update.begin failed: %s",
                  Update.errorString());
    setError(buf);
    return InstallResult::kUpdateBeginFailed;
  }

  // 4 KB chunks — bigger than the historic 2 KB without bloating the
  // worker's stack budget. Update.write batches into the flash sector
  // size internally so larger reads here mostly buy us fewer
  // HTTPClient::available polls and fewer SHA/CRC update calls inside
  // arduino-esp32's Update layer.
  uint8_t chunk[4096];
  size_t written = 0;
  uint32_t lastReport = 0;
  while (true) {
    int got = s.read(chunk, sizeof(chunk));
    if (got < 0) {
      Update.abort();
      setError("stream read failed");
      return InstallResult::kHttpError;
    }
    if (got == 0) {
      // EOF or stream stopped — accept if we got everything.
      if (total > 0 && written < total) {
        Update.abort();
        char buf[80];
        std::snprintf(buf, sizeof(buf),
                      "stream short: %u/%u",
                      (unsigned)written, (unsigned)total);
        setError(buf);
        return InstallResult::kHttpError;
      }
      break;
    }
    size_t w = Update.write(chunk, got);
    if (w != static_cast<size_t>(got)) {
      Update.abort();
      char buf[80];
      std::snprintf(buf, sizeof(buf),
                    "Update.write %u/%d", (unsigned)w, got);
      setError(buf);
      return InstallResult::kWriteFailed;
    }
    written += w;
    if (cb && (millis() - lastReport > 250 ||
               (total > 0 && written >= total))) {
      lastReport = millis();
      InstallProgress prog{written, total, false, InstallResult::kOk};
      cb(prog, user);
    }
    if (total > 0 && written >= total) break;
  }

  if (!Update.end(true)) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "Update.end failed: %s",
                  Update.errorString());
    setError(buf);
    return InstallResult::kEndFailed;
  }

  if (cb) {
    InstallProgress done{written, total, true, InstallResult::kOk};
    cb(done, user);
  }

  Serial.printf("[ota] install complete (%u bytes); rebooting\n",
                (unsigned)written);
  return InstallResult::kOk;
}

void markCurrentAppValidIfPending() {
  if (!sPendingVerify || sValidated) return;
  esp_err_t rc = esp_ota_mark_app_valid_cancel_rollback();
  if (rc == ESP_OK) {
    sValidated = true;
    Serial.println("[ota] running app marked valid; rollback cancelled");
  } else {
    Serial.printf("[ota] mark_valid failed rc=%d\n", (int)rc);
  }
}

bool runningPendingVerify() {
  return sPendingVerify && !sValidated;
}

// ── Storage helpers ───────────────────────────────────────────────────────

size_t ffatPartitionBytes() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  return part ? static_cast<size_t>(part->size) : 0;
}

size_t ffatVolumeBytes() {
  FATFS* fs = replay_get_fatfs();
  if (!fs) return 0;
  // Force FATFS to populate n_fatent / csize / ssize. The fields may
  // be stale until f_getfree walks the FAT (this matches what
  // MicroPython's `os.statvfs` does — see vfs_fat.c).
  DWORD freeClusters = 0;
  if (f_getfree(fs, &freeClusters) != FR_OK) return 0;
  if (fs->n_fatent < 2) return 0;
  // Sector size is dynamic on this build — `MICROPY_FATFS_MAX_SS`
  // is 4096, so FATFS exposes the per-volume `ssize` field instead
  // of the compile-time FF_MIN_SS=512. Hardcoding 512 underestimates
  // total bytes by 8× and rounds the storage line to 0 MB.
  const uint32_t sectorBytes = static_cast<uint32_t>(fs->ssize);
  uint64_t totalClusters = static_cast<uint64_t>(fs->n_fatent - 2);
  uint64_t bytes = totalClusters *
                   static_cast<uint64_t>(fs->csize) *
                   static_cast<uint64_t>(sectorBytes);
  return static_cast<size_t>(bytes);
}

bool ffatExpansionAvailable() {
  size_t partBytes = ffatPartitionBytes();
  size_t volBytes = ffatVolumeBytes();
  if (partBytes == 0 || volBytes == 0) return false;
  // Only surface the option when the gap is meaningful — 256 KB
  // accounts for FAT metadata overhead so a freshly-formatted volume
  // doesn't trigger a false "expand" prompt.
  if (partBytes <= volBytes) return false;
  return (partBytes - volBytes) >= (256u * 1024u);
}

void reformatFfatAndReboot() {
  Serial.println("[ota] reformatting ffat — all user data will be lost");
  // The replay_bdev helper unmounts, mkfs's, and ESP.restart()s. It
  // does not return on success.
  int rc = replay_bdev_reformat_and_reboot();
  // If we get here, something went wrong. Reboot anyway to recover.
  Serial.printf("[ota] reformat helper returned rc=%d; rebooting\n", rc);
  delay(300);
  ESP.restart();
}

}  // namespace ota
