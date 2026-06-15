#include "AssetRegistry.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

#include "OTAHttp.h"
#include "../api/WiFiService.h"
#include "../infra/BadgeConfig.h"
#include "../infra/DebugLog.h"
#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"

namespace ota::registry {

namespace {

constexpr const char* kNvsNamespace = "badge_assets";
constexpr const char* kNvsLastEpoch = "last_epoch";
constexpr uint32_t kRefreshCooldownSec = 24 * 60 * 60;
// Headroom for the community apps registry. The published file is ~31
// KB at the time of writing and growing as more apps land. Both the
// body buffer (in OTAHttp.cpp) and the JsonDocument are allocated via
// the PSRAM-preferring allocator, so this doesn't cost us internal
// heap. Bumped from 64 KB → 96 KB after Bug B: ArduinoJson v6's
// `BasicJsonDocument` resizes its pool while parsing, and on a
// fragmented PSRAM the allocator can return nullptr mid-parse for a
// growth attempt while still reporting `Ok` (it just stops adding tail
// elements). Giving the doc more headroom up front means it never has
// to resize during a healthy parse — bytes are cheap, missing apps are
// not.
constexpr size_t kRegistryJsonMax = 128 * 1024;

AssetEntry* sAssets = nullptr;
uint8_t sAssetCount = 0;
// Pool of sub-files for kind=app entries. Allocated from PSRAM during
// refresh() — at ~400 bytes per entry and ~50 files in a typical
// community registry, this is too large to keep in BSS. The pool is
// freed and reallocated each refresh so we never hold stale state.
AssetFileEntry* sFiles = nullptr;
uint16_t sFileCount = 0;
uint16_t sFileCap = 0;
char sLastError[96] = "";
time_t sLastRefreshEpoch = 0;
bool sBegun = false;

// Background refresh state. The flag is atomic so the GUI thread can
// poll `isRefreshing()` without coordinating with the worker. The
// pending-ignore-cooldown bool is read once at task entry; safe to
// stash before the task starts because beginRefreshAsync() guards
// against re-entry via the flag.
std::atomic<bool> sRefreshing{false};
// Held for the whole install() / installAll() window so refresh()
// cannot repopulate sFiles[] while a download loop is reading URLs.
std::atomic<bool> sInstalling{false};
bool sRefreshIgnoreCooldown = false;
RegistryRefresh sLastRefreshResult = RegistryRefresh::kOk;

// Forward — defined later in this TU; called once at the tail of
// refresh() so we don't pay the FATFS-walk cost on every render.
void adoptInstalledFromDisk();

void setError(const char* msg) {
  if (!msg) msg = "";
  std::strncpy(sLastError, msg, sizeof(sLastError) - 1);
  sLastError[sizeof(sLastError) - 1] = '\0';
}

void copyField(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  std::strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// Reject URLs that picked up PSRAM garbage during a truncated JSON
// parse (non-printable bytes) or never parsed as strings at all.
bool isPlausibleUrl(const char* url) {
  if (!url || url[0] == '\0') return false;
  if (std::strncmp(url, "http://", 7) != 0 &&
      std::strncmp(url, "https://", 8) != 0) {
    return false;
  }
  for (const char* p = url; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

bool nvsKey(char* out, size_t cap, const char* prefix, const char* id) {
  // Preferences keys are limited to 15 chars. We trim aggressively.
  if (!out || !prefix || !id) return false;
  size_t plen = std::strlen(prefix);
  if (plen >= cap) return false;
  std::memcpy(out, prefix, plen);
  size_t room = cap - plen - 1;
  size_t ilen = std::strlen(id);
  if (ilen > room) ilen = room;
  std::memcpy(out + plen, id, ilen);
  out[plen + ilen] = '\0';
  return true;
}

// Loads the persisted "installed version" string for an id. Empty
// string if never installed.
//
// We `isKey()`-guard the read because Preferences::getString prints a
// loud `nvs_get_str len fail: ... NOT_FOUND` to Serial on every miss,
// and AssetLibraryScreen::formatItem() calls into here once per row
// per frame. For the common "asset not installed yet" case that turns
// into a flood of logs and pushes the GUI service over its frame
// budget. isKey is silent when the key is absent.
void loadInstalledVersion(const char* id, char* out, size_t cap) {
  out[0] = '\0';
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  char key[16];
  if (nvsKey(key, sizeof(key), "v_", id) && p.isKey(key)) {
    p.getString(key, out, cap);
  }
  p.end();
}

void persistInstalledVersion(const char* id, const char* version) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  char key[16];
  if (nvsKey(key, sizeof(key), "v_", id)) {
    if (version && version[0]) p.putString(key, version);
    else p.remove(key);
  }
  p.end();
}

void persistRefreshEpoch(time_t epoch) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  p.putULong(kNvsLastEpoch, static_cast<uint32_t>(epoch));
  p.end();
}

void loadRefreshEpoch() {
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  sLastRefreshEpoch =
      static_cast<time_t>(p.getULong(kNvsLastEpoch, 0));
  p.end();
}

// Convert binary digest to lowercase hex.
void hexEncode(const uint8_t* in, size_t len, char* out, size_t cap) {
  static const char hex[] = "0123456789abcdef";
  size_t need = len * 2 + 1;
  if (cap < need) {
    if (cap > 0) out[0] = '\0';
    return;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = hex[in[i] >> 4];
    out[i * 2 + 1] = hex[in[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

bool hexEqualsCaseInsensitive(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
    if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
    if (ca != cb) return false;
    ++a; ++b;
  }
  return *a == '\0' && *b == '\0';
}

// Returns the FATFS volume's free byte count, or 0 if it cannot be
// queried. Uses fs->ssize (actual sector size) — the ESP32
// wear-levelling layer presents 4096-byte sectors, not the 512-byte
// default that oofatfs supports. Hardcoding 512 underreports free
// space by 8x and made every install fail the min_free_bytes gate
// even on a near-empty partition.
uint64_t getFreeBytes() {
  FATFS* fs = replay_get_fatfs();
  if (!fs) return 0;
  DWORD freeClusters = 0;
  if (f_getfree(fs, &freeClusters) != FR_OK) return 0;
  uint32_t clusterBytes =
      static_cast<uint32_t>(fs->csize) * static_cast<uint32_t>(fs->ssize);
  return static_cast<uint64_t>(freeClusters) * clusterBytes;
}

bool ensureFreeSpace(size_t needed, uint64_t* outFreeBytes = nullptr) {
  uint64_t freeBytes = getFreeBytes();
  if (outFreeBytes) *outFreeBytes = freeBytes;
  if (needed == 0) return true;
  if (freeBytes == 0) return true;  // unknown — defer to write failure
  return freeBytes >= needed;
}

// Recursively f_mkdir each path component below the FATFS root so
// nested asset paths like "/apps/breaksnake/tardigotchi/sprites.py"
// land in directories that already exist. f_mkdir returns
// FR_EXIST when the directory is already there; we treat that as
// success.
bool ensureDirsForPath(FATFS* fs, const char* fullPath) {
  if (!fs || !fullPath || fullPath[0] != '/') return false;
  char buf[kAssetPathMax * 2];
  std::strncpy(buf, fullPath, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  // Strip the trailing component (file name) — we only mkdir parents.
  char* slash = std::strrchr(buf, '/');
  if (!slash || slash == buf) return true;  // root only
  *slash = '\0';
  // Walk left-to-right creating each segment.
  for (char* p = buf + 1; *p; ++p) {
    if (*p != '/') continue;
    *p = '\0';
    FRESULT r = f_mkdir(fs, buf);
    if (r != FR_OK && r != FR_EXIST) return false;
    *p = '/';
  }
  FRESULT r = f_mkdir(fs, buf);
  return r == FR_OK || r == FR_EXIST;
}

void freeFilePool() {
  if (sFiles) {
    std::free(sFiles);
    sFiles = nullptr;
  }
  sFileCount = 0;
  sFileCap = 0;
}

// True iff the asset's destination is already fully present on disk
// at the expected size. Used to:
//   * adopt files that landed via `pio run -t uploadfs`, badge_sync,
//     JumperIDE sync, or a prior firmware that didn't persist the
//     NVS `v_<id>` marker, so statusOf can report them as installed
//     rather than queueing a fresh download.
//   * short-circuit `install()` when the bytes are already in place
//     — without this, the free-space gate fails for big assets like
//     the DOOM WAD whose own footprint dominates the free count, and
//     the user sees "insufficient space" on an asset they already have.
//
// kFile checks the file size matches `entry.size`.
// kApp checks every bundled file exists, with a size match for each
// file that the registry declared a size for.
//
// `entry.size == 0` (registry omitted size) is treated as "can't
// verify" and returns false — we'd rather redownload than mistakenly
// adopt a stub.
bool destFullyPresent(const AssetEntry& entry) {
  if (entry.dest_path[0] == '\0') return false;
  if (entry.kind == AssetKind::kFile) {
    if (entry.size == 0) return false;
    if (!Filesystem::fileExists(entry.dest_path)) return false;
    const int32_t onDisk = Filesystem::fileSize(entry.dest_path);
    return onDisk >= 0 && static_cast<uint32_t>(onDisk) == entry.size;
  }
  if (entry.fileCount == 0 || sFiles == nullptr) return false;
  if (!Filesystem::fileExists(entry.dest_path)) return false;
  for (uint16_t i = 0; i < entry.fileCount; ++i) {
    const AssetFileEntry& fe = sFiles[entry.firstFileIdx + i];
    char destFull[kAssetPathMax * 2];
    if (fe.path[0] == '/') {
      std::snprintf(destFull, sizeof(destFull), "%s%s",
                    entry.dest_path, fe.path);
    } else {
      std::snprintf(destFull, sizeof(destFull), "%s/%s",
                    entry.dest_path, fe.path);
    }
    if (!Filesystem::fileExists(destFull)) return false;
    if (fe.size > 0) {
      const int32_t onDisk = Filesystem::fileSize(destFull);
      if (onDisk < 0 || static_cast<uint32_t>(onDisk) != fe.size) {
        return false;
      }
    }
  }
  return true;
}

// Block until the background registry refresh task finishes so
// install() never reads sFiles[] / sAssets[] while refresh() is
// repopulating them (use-after-free → mangled download URLs).
// Mirrors BadgeOTA::prepareFirmwareInstallHeap().
bool waitForRefreshIdle() {
  constexpr uint32_t kRegistryWaitMs = 120000;
  const uint32_t t0 = millis();
  while (sRefreshing.load(std::memory_order_acquire)) {
    if ((uint32_t)(millis() - t0) >= kRegistryWaitMs) {
      setError("registry refresh in progress");
      return false;
    }
    delay(20);
    yield();
  }
  return true;
}

bool waitForInstallIdle() {
  constexpr uint32_t kInstallWaitMs = 120000;
  const uint32_t t0 = millis();
  while (sInstalling.load(std::memory_order_acquire)) {
    if ((uint32_t)(millis() - t0) >= kInstallWaitMs) {
      setError("install in progress");
      return false;
    }
    delay(20);
    yield();
  }
  return true;
}

// RAII: wait for refresh, then pin sInstalling for the scope.
class InstallHold {
 public:
  explicit InstallHold(bool acquire) : held_(false) {
    if (!acquire) return;
    if (!waitForRefreshIdle()) return;
    sInstalling.store(true, std::memory_order_release);
    held_ = true;
  }
  ~InstallHold() {
    if (held_) sInstalling.store(false, std::memory_order_release);
  }
  explicit operator bool() const { return held_; }

 private:
  bool held_;
};

bool reserveFilePool(uint16_t want) {
  if (want <= sFileCap) return true;
  // Round up to the nearest 16 to keep realloc churn down across
  // refreshes.
  uint16_t newCap = (want + 15u) & ~15u;
  void* p = BadgeMemory::allocPreferPsram(
      static_cast<size_t>(newCap) * sizeof(AssetFileEntry));
  if (!p) return false;
  if (sFiles && sFileCount > 0) {
    std::memcpy(p, sFiles,
                static_cast<size_t>(sFileCount) * sizeof(AssetFileEntry));
  }
  if (sFiles) std::free(sFiles);
  sFiles = static_cast<AssetFileEntry*>(p);
  sFileCap = newCap;
  return true;
}

}  // namespace

void begin() {
  if (sBegun) return;
  sBegun = true;
  sAssets = static_cast<AssetEntry*>(
      BadgeMemory::allocPreferPsram(kMaxRegistryAssets * sizeof(AssetEntry)));
  if (sAssets) {
    std::memset(sAssets, 0, kMaxRegistryAssets * sizeof(AssetEntry));
  }
  loadRefreshEpoch();
  DBG("[registry] cache loaded: last_epoch=%lu\n",
                (unsigned long)sLastRefreshEpoch);
}

void tick() {
  if (!sBegun) return;
  if (!badgeConfig.communityAppsUrl()[0]) return;
  if (!wifiService.isConnected()) return;
  if (!wifiService.clockReady()) return;
  const time_t now = time(nullptr);
  if (now <= 0) return;
  if (sLastRefreshEpoch != 0 &&
      (uint32_t)(now - sLastRefreshEpoch) < kRefreshCooldownSec) {
    return;
  }
  DBG("[registry] daily refresh fired\n");
  refresh(false);
}

RegistryRefresh refresh(bool ignoreCooldown) {
  if (!sBegun) begin();
  if (!sAssets) {
    setError("sAssets alloc failed");
    sLastRefreshResult = RegistryRefresh::kParseError;
    return RegistryRefresh::kParseError;
  }
  setError("");

  if (!waitForInstallIdle()) {
    sLastRefreshResult = RegistryRefresh::kInstallInProgress;
    return RegistryRefresh::kInstallInProgress;
  }

  const char* url = badgeConfig.communityAppsUrl();
  if (!url || !url[0]) {
    setError("community_apps_url not configured");
    sLastRefreshResult = RegistryRefresh::kNotConfigured;
    return RegistryRefresh::kNotConfigured;
  }

  if (!ignoreCooldown && sLastRefreshEpoch != 0 &&
      wifiService.clockReady()) {
    const time_t now = time(nullptr);
    if (now > 0 && (uint32_t)(now - sLastRefreshEpoch) < kRefreshCooldownSec) {
      sLastRefreshResult = RegistryRefresh::kCooldownActive;
      return RegistryRefresh::kCooldownActive;
    }
  }

  DBG("[registry] GET %s\n", url);
  char* body = nullptr;
  size_t bodyLen = 0;
  HttpResult httpRes =
      getJson(url, &body, &bodyLen, kRegistryJsonMax, 20000);
  if (!httpRes.ok) {
    DBG("[registry] refresh failed: code=%d %s\n",
                  httpRes.httpCode, httpRes.error);
    setError(httpRes.error);
    if (body) std::free(body);
    sLastRefreshResult = RegistryRefresh::kNetworkError;
    return RegistryRefresh::kNetworkError;
  }

  // Allocate the parser document from PSRAM — at 96 KB it would
  // otherwise be a meaningful chunk of the ~150 KB free internal heap,
  // and we'd be holding the body buffer alongside it during parse.
  BadgeMemory::PsramJsonDocument doc(kRegistryJsonMax);
  DeserializationError err = deserializeJson(doc, body);

  std::free(body);
  if (err) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "json parse: %s", err.c_str());
    setError(buf);
    sLastRefreshResult = RegistryRefresh::kParseError;
    return RegistryRefresh::kParseError;
  }

  JsonArray assets = doc["assets"].as<JsonArray>();
  // Capture the JSON-side asset count BEFORE we walk it — used for the
  // post-loop mismatch check below. `assets.size()` walks the parsed
  // tree; if PsramAllocator returned nullptr mid-parse this number will
  // already be smaller than what the server actually sent, and we have
  // no way to recover from that here. Comparing it to `sAssetCount`
  // catches the *separate* class where a parsed-asset entry was
  // dropped during our own loop (missing required field).
  const size_t parsedAssetsFromJson = assets.size();
  sAssetCount = 0;
  // Reset the file-pool cursor; reserveFilePool grows as needed.
  sFileCount = 0;
  for (JsonObject a : assets) {
    if (sAssetCount >= kMaxRegistryAssets) break;
    AssetEntry& e = sAssets[sAssetCount];
    std::memset(&e, 0, sizeof(e));
    e.kind = AssetKind::kFile;
    copyField(e.id, sizeof(e.id), a["id"] | "");
    if (e.id[0] == '\0') continue;
    copyField(e.name, sizeof(e.name), a["name"] | e.id);
    copyField(e.version, sizeof(e.version), a["version"] | "0");
    copyField(e.description, sizeof(e.description),
              a["description"] | "");
    e.size = a["size"] | 0u;
    e.min_free_bytes = a["min_free_bytes"] | 0u;

    // Determine kind. Prefer the explicit `kind` field; fall back to
    // shape inspection (presence of `dest_dir` + `files`) so older
    // registries without `kind` still parse.
    const char* kindStr = a["kind"] | "";
    JsonArray files = a["files"].as<JsonArray>();
    const bool isApp =
        (std::strcmp(kindStr, "app") == 0) ||
        (a["dest_dir"].is<const char*>() && !files.isNull());

    if (isApp) {
      e.kind = AssetKind::kApp;
      // dest_dir is the on-disk root for the app bundle. We store it
      // in dest_path so the existing UI code paths "just work" with
      // the same field.
      copyField(e.dest_path, sizeof(e.dest_path), a["dest_dir"] | "");
      if (e.dest_path[0] == '\0') continue;

      // Reserve room in the file pool and copy each entry across.
      const uint16_t startIdx = sFileCount;
      uint16_t added = 0;
      const uint16_t expectedFiles =
          static_cast<uint16_t>(files.size());
      for (JsonObject f : files) {
        if (!reserveFilePool(sFileCount + 1)) {
          DBG("[registry] file pool alloc failed\n");
          break;
        }
        AssetFileEntry& fe = sFiles[sFileCount];
        std::memset(&fe, 0, sizeof(fe));
        copyField(fe.path, sizeof(fe.path), f["path"] | "");
        copyField(fe.url, sizeof(fe.url), f["url"] | "");
        copyField(fe.sha256, sizeof(fe.sha256), f["sha256"] | "");
        fe.size = f["size"] | 0u;
        if (fe.path[0] == '\0' || !isPlausibleUrl(fe.url)) continue;
        ++sFileCount;
        ++added;
      }
      if (added == 0) continue;  // app with zero usable files — drop
      // A short file list means ArduinoJson ran out of PSRAM mid-parse.
      // Installing the stub would mark the bundle "done" and skip the
      // rest forever — drop the whole app and try again next refresh.
      if (expectedFiles > 0 && added != expectedFiles) {
        DBG("[registry] drop %s: partial file list %u/%u\n",
            e.id, static_cast<unsigned>(added),
            static_cast<unsigned>(expectedFiles));
        sFileCount = startIdx;
        continue;
      }
      e.firstFileIdx = startIdx;
      e.fileCount = added;
    } else {
      copyField(e.url, sizeof(e.url), a["url"] | "");
      copyField(e.sha256, sizeof(e.sha256), a["sha256"] | "");
      copyField(e.dest_path, sizeof(e.dest_path), a["dest_path"] | "");
      if (e.dest_path[0] == '\0' || !isPlausibleUrl(e.url)) continue;
    }

    sAssetCount++;
  }

  // Intentional: persist whatever parsed cleanly rather than refusing
  // the whole refresh if `sAssetCount != parsedAssetsFromJson`. The
  // per-entry `continue` guards in the loop above skip malformed rows,
  // so the adopted set is internally consistent — just potentially
  // shorter than the source JSON.
  (void)parsedAssetsFromJson;

  sLastRefreshEpoch = wifiService.clockReady() ? time(nullptr) : 1;
  persistRefreshEpoch(sLastRefreshEpoch);
  DBG("[registry] parsed %u assets\n",
                static_cast<unsigned>(sAssetCount));
  // One-shot adoption of files already on disk. Centralising it here
  // (instead of inside statusOf()) keeps every render-time call to
  // statusOf() pure: no NVS writes, no FATFS walk for app bundles.
  adoptInstalledFromDisk();
  sLastRefreshResult = RegistryRefresh::kOk;
  return RegistryRefresh::kOk;
}

namespace {
struct AsyncCtx {
  bool ignoreCooldown;
};

void refreshTask(void* arg) {
  AsyncCtx* ctx = static_cast<AsyncCtx*>(arg);
  bool ignore = ctx ? ctx->ignoreCooldown : false;
  delete ctx;
  RegistryRefresh r = refresh(ignore);
  sLastRefreshResult = r;
  sRefreshing.store(false);
  vTaskDelete(nullptr);
}
}  // namespace

bool beginRefreshAsync(bool ignoreCooldown) {
  if (!sBegun) begin();
  bool expected = false;
  if (!sRefreshing.compare_exchange_strong(expected, true)) {
    return false;  // already running
  }
  AsyncCtx* ctx = new AsyncCtx{ignoreCooldown};
  // 8 KB stack: HTTPClient + the 64 KB JsonDocument is PSRAM-backed
  // (BadgeMemory::PsramJsonDocument), but the parse + WiFi stack
  // frames still need a bit of internal RAM headroom. Core 0 keeps
  // it off the GUI loop on Core 1 while keeping TLS headroom on badges
  // that have just completed the boot-edge OTA check.
  //
  // Pinned to Core 1, prio 1: Core 0 hosts WiFi (prio 23), lwIP (prio 18),
  // and irTask (prio 1). During a TLS handshake mbedTLS BigNum math runs
  // synchronously in the worker while lwIP/WiFi service TLS records — the
  // pair collectively occupied Core 0 for >5 s and tripped IDLE0's TWDT,
  // regardless of whether the worker was at prio 0 or prio 1. Core 1 hosts
  // the Arduino main loop which yields every iteration, so IDLE1 has
  // generous headroom even with a busy network worker.
  BaseType_t ok = xTaskCreatePinnedToCore(
      &refreshTask, "registry_refresh", 8 * 1024, ctx,
      tskIDLE_PRIORITY + 1, nullptr, 1);
  if (ok != pdPASS) {
    delete ctx;
    sRefreshing.store(false);
    return false;
  }
  return true;
}

bool isRefreshing() { return sRefreshing.load(std::memory_order_acquire); }
bool isInstalling() { return sInstalling.load(std::memory_order_acquire); }
RegistryRefresh lastRefreshResult() { return sLastRefreshResult; }

size_t count() { return sAssetCount; }

const AssetEntry* at(size_t index) {
  if (!sAssets || index >= sAssetCount) return nullptr;
  return &sAssets[index];
}

const AssetEntry* findById(const char* id) {
  if (!id || !sAssets) return nullptr;
  for (size_t i = 0; i < sAssetCount; ++i) {
    if (std::strcmp(sAssets[i].id, id) == 0) return &sAssets[i];
  }
  return nullptr;
}

AssetStatus statusOf(const AssetEntry& entry) {
  char installed[kAssetVersionMax] = "";
  loadInstalledVersion(entry.id, installed, sizeof(installed));
  // Pure read: no NVS writes, no FATFS-walk for non-installed assets.
  // The "adopt-from-disk" path (DOOM WAD landed via uploadfs, etc.)
  // runs once at the end of refresh() via adoptInstalledFromDisk()
  // so the GUI thread never pays for it on render.
  if (installed[0] == '\0') {
    return AssetStatus::kNotInstalled;
  }
  // File on disk must also exist; otherwise treat as not-installed
  // even if NVS says we did install once (user could have wiped FS).
  if (!Filesystem::fileExists(entry.dest_path)) {
    return AssetStatus::kNotInstalled;
  }
  if (std::strcmp(installed, entry.version) != 0) {
    return AssetStatus::kUpdateAvailable;
  }
  // NVS says installed at this version — still verify every bundled
  // file (or single-file size) is present. A truncated registry
  // parse can leave a one-file stub that would otherwise read as
  // kInstalled and never be retried by Download all.
  if (!destFullyPresent(entry)) {
    return AssetStatus::kUpdateAvailable;
  }
  return AssetStatus::kInstalled;
}

namespace {
// Walk every parsed registry entry; for any entry without an NVS
// installed-version record, check whether the bytes are already on
// disk (uploadfs / badge_sync / older firmware) and persist the
// version marker so subsequent statusOf() calls treat it as
// kInstalled without doing the FATFS walk. Runs once at the end of
// refresh() — the heavy lifting (per-file fileExists for kApp
// bundles) happens off the GUI thread when called from the async
// refresh task.
void adoptInstalledFromDisk() {
  for (uint8_t i = 0; i < sAssetCount; ++i) {
    const AssetEntry& e = sAssets[i];
    char installed[kAssetVersionMax] = "";
    loadInstalledVersion(e.id, installed, sizeof(installed));
    if (installed[0] != '\0') continue;
    if (!destFullyPresent(e)) continue;
    DBG("[registry] adopt existing %s (id=%s, version=%s)\n",
                  e.dest_path, e.id, e.version);
    persistInstalledVersion(e.id, e.version);
  }
}
}  // namespace

const char* installedVersionOf(const AssetEntry& entry) {
  static char sBuf[kAssetVersionMax];
  loadInstalledVersion(entry.id, sBuf, sizeof(sBuf));
  return sBuf;
}

namespace {

// Download a single URL to destPath. The caller has already verified
// free-space and (for app installs) ensured the parent directory
// exists. cb fires as progress accumulates but never with done=true —
// the dispatching install() decides when the asset as a whole is
// finished. Returns AssetInstallResult::kOk on success.
AssetInstallResult installFileToPath(const char* url,
                                     const char* expectedSha256,
                                     uint32_t expectedSize,
                                     const char* destPath,
                                     AssetProgressCb cb, void* user,
                                     size_t* outWritten) {
  if (outWritten) *outWritten = 0;
  if (!destPath || destPath[0] != '/') {
    setError("dest_path must be absolute");
    return AssetInstallResult::kDestPathInvalid;
  }

  // Sized for the worst-case nested app path
  // (kAssetPathMax * 2 from the destFull buffer in install()), plus
  // ".tmp" + NUL. Old kAssetPathMax + 8 sizing was inherited from the
  // single-file kFile path where dest_path is bounded at kAssetPathMax
  // — too tight once kApp paths started concatenating dir + file.
  char tmpPath[kAssetPathMax * 2 + 8];
  int n = std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", destPath);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmpPath)) {
    setError("dest path too long");
    return AssetInstallResult::kDestPathInvalid;
  }

  FATFS* fs = replay_get_fatfs();
  if (!fs) {
    setError("filesystem not mounted");
    return AssetInstallResult::kFsWriteError;
  }

  if (!isPlausibleUrl(url)) {
    setError("bad asset url");
    return AssetInstallResult::kHttpError;
  }

  DBG("[registry] install GET %s -> %s\n", url, destPath);
  // Hold the radio awake + CPU at 240 MHz for the whole download.
  // Without this, WiFi modem sleep + dynamic CPU scaling between
  // chunks easily collapses throughput from ~250 KB/s to ~30 KB/s
  // on a TLS stream.
  // Best-effort: make sure every parent directory of destPath exists.
  // Single-file assets with nested dest_paths (e.g. /docs/README.md
  // when /docs/ doesn't yet exist) would otherwise hit FR_NO_PATH at
  // f_open below. Caller-side mkdir for app bundles still happens
  // before this is called; this is the safety net for kFile callers.
  {
    Filesystem::IOLock dirLock;
    if (!ensureDirsForPath(fs, destPath)) {
      DBG("[registry] install fail: mkdir parents for %s\n",
                    destPath);
      setError("mkdir parents failed");
      return AssetInstallResult::kFsWriteError;
    }
  }

  ThroughputBoost boost;
  Stream s;
  if (!s.open(url, 30000)) {
    DBG("[registry] install fail: stream open %s: %s\n",
                  url, s.lastError());
    setError(s.lastError());
    return AssetInstallResult::kHttpError;
  }
  // Canonical total: prefer the registry-declared size (we can trust it
  // across resumes), fall back to the server's Content-Length on the
  // initial 200. After a Range-resumed 206 the server only knows the
  // *remaining* slice size, so caching `total` from the first response
  // here matters — we use it for progress + short-stream detection
  // throughout the retry loop.
  size_t total = expectedSize;
  if (total == 0) total = s.contentLength();

  // Open the .tmp file under the FATFS lock for the whole download.
  // We hold the lock the whole time so other writers can't race
  // through directory cluster bookkeeping while we extend the file.
  Filesystem::IOLock lock;
  f_unlink(fs, tmpPath);  // best-effort cleanup of stale .tmp

  FIL fil;
  FRESULT ores = f_open(fs, &fil, tmpPath, FA_WRITE | FA_CREATE_NEW);
  if (ores != FR_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "open tmp fr=%d", (int)ores);
    setError(buf);
    DBG("[registry] install fail: %s for %s\n", buf, tmpPath);
    return AssetInstallResult::kFsWriteError;
  }

  mbedtls_sha256_context shaCtx;
  bool wantSha = expectedSha256 && expectedSha256[0] != '\0';
  if (wantSha) {
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);
  }

  // Retry budget: a 4 MB asset over conference WiFi typically completes
  // on the first attempt, but a single transient stall (5-10 s of dead
  // air, AP roaming, neighbour interference) used to abort the whole
  // download. Resume-with-Range lets each retry pick up at `written`
  // bytes instead of restarting the multi-megabyte stream from scratch.
  // Four retries on top of the initial attempt cover the typical
  // single-stall case without dragging out a genuinely dead network.
  constexpr int kMaxRetries = 4;
  int retries = 0;

  // 8 KB chunks ≈ 8x fewer iterations than the historic 1 KB buffer.
  // Each iteration pays for an HTTPClient::available poll, an
  // f_write call (FATFS sector bookkeeping), an mbedtls_sha256_update
  // (HW-accelerated on S3 but per-call overhead still matters), and a
  // progress-callback check. At 4 MB this is the difference between
  // ~33 KB/s and ~250 KB/s on a healthy WiFi link. Prefer PSRAM so the
  // 8 KiB staging buffer does not squeeze mbedTLS on concurrent HTTPS.
  constexpr size_t kChunkBytes = 8192;
  uint8_t* chunk = static_cast<uint8_t*>(BadgeMemory::allocPreferPsram(kChunkBytes));
  if (!chunk) {
    setError("chunk alloc failed");
    f_close(&fil);
    f_unlink(fs, tmpPath);
    if (wantSha) mbedtls_sha256_free(&shaCtx);
    return AssetInstallResult::kFsWriteError;
  }
  size_t written = 0;
  uint32_t lastReport = 0;
  bool ok = true;
  while (true) {
    int got = s.read(chunk, kChunkBytes);
    const bool isShortEof =
        (got == 0 && total > 0 && written < total);
    if (got < 0 || isShortEof) {
      // Transient: stream dropped or the per-read window expired
      // before the server delivered the rest. Reopen with a Range
      // header at the current write offset and continue. Servers that
      // honour the header reply 206 Partial Content (Stream::open
      // accepts both 200 and 206); servers that ignore it reply 200
      // and we restart the file from scratch as a fallback.
      if (retries++ >= kMaxRetries) {
        char buf[80];
        std::snprintf(buf, sizeof(buf),
                      "stream short %u/%u after %d retries",
                      (unsigned)written, (unsigned)total,
                      retries - 1);
        setError(buf);
        ok = false;
        break;
      }
      DBG("[registry] stream stall at %u/%u, retry %d (resume)\n",
          (unsigned)written, (unsigned)total, retries);
      s.close();
      // Linear backoff: 500 ms, 1 s, 1.5 s, 2 s. Long enough that a
      // briefly overwhelmed AP can recover, short enough that an
      // attentive user doesn't think the install is stuck.
      delay(500U * static_cast<uint32_t>(retries));
      if (!s.open(url, 30000, written)) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "resume open failed: %s",
                      s.lastError());
        setError(buf);
        ok = false;
        break;
      }
      if (s.httpCode() == 200 && written > 0) {
        // Server ignored Range and is sending the whole file from
        // byte 0. Easiest recovery: rewind the .tmp file, reset the
        // SHA context, and consume the stream as a fresh start. This
        // wastes the bytes we already wrote, but jsDelivr / GitHub
        // CDN / ibiblio all honour Range so the path is rare in
        // practice.
        DBG("[registry] server ignored Range; restarting from 0\n");
        f_lseek(&fil, 0);
        f_truncate(&fil);
        written = 0;
        if (wantSha) {
          mbedtls_sha256_free(&shaCtx);
          mbedtls_sha256_init(&shaCtx);
          mbedtls_sha256_starts(&shaCtx, 0);
        }
      }
      continue;
    }
    if (got == 0) break;  // genuine EOF
    UINT wrote = 0;
    FRESULT wres = f_write(&fil, chunk, (UINT)got, &wrote);
    if (wres != FR_OK || wrote != (UINT)got) {
      char buf[64];
      std::snprintf(buf, sizeof(buf),
                    "f_write fr=%d %u/%d", (int)wres, (unsigned)wrote, got);
      setError(buf);
      ok = false;
      break;
    }
    if (wantSha) mbedtls_sha256_update(&shaCtx, chunk, got);
    written += wrote;
    if (cb && (millis() - lastReport > 250 ||
               (total > 0 && written >= total))) {
      lastReport = millis();
      AssetProgress p{written, total, false, AssetInstallResult::kOk};
      cb(p, user);
    }
    if (total > 0 && written >= total) break;
  }
  f_sync(&fil);
  f_close(&fil);
  std::free(chunk);

  if (!ok) {
    if (wantSha) mbedtls_sha256_free(&shaCtx);
    f_unlink(fs, tmpPath);
    DBG("[registry] install fail: %s (write loop) for %s\n",
                  sLastError, destPath);
    if (outWritten) *outWritten = written;
    return AssetInstallResult::kFsWriteError;
  }

  if (wantSha) {
    uint8_t digest[32];
    mbedtls_sha256_finish(&shaCtx, digest);
    mbedtls_sha256_free(&shaCtx);
    char hex[65];
    hexEncode(digest, sizeof(digest), hex, sizeof(hex));
    if (!hexEqualsCaseInsensitive(hex, expectedSha256)) {
      char buf[80];
      std::snprintf(buf, sizeof(buf),
                    "sha256 mismatch (got %.16s...)", hex);
      setError(buf);
      DBG("[registry] install fail: %s for %s\n", buf, destPath);
      f_unlink(fs, tmpPath);
      if (outWritten) *outWritten = written;
      return AssetInstallResult::kSha256Mismatch;
    }
  }

  // Atomic rename: drop the live file, promote the .tmp.
  f_unlink(fs, destPath);
  FRESULT rres = f_rename(fs, tmpPath, destPath);
  if (rres != FR_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rename fr=%d", (int)rres);
    setError(buf);
    DBG("[registry] install fail: %s for %s\n", buf, destPath);
    f_unlink(fs, tmpPath);
    if (outWritten) *outWritten = written;
    return AssetInstallResult::kFsWriteError;
  }

  DBG("[registry] installed file -> %s (%u bytes)\n",
                destPath, (unsigned)written);
  if (outWritten) *outWritten = written;
  return AssetInstallResult::kOk;
}

// Snapshot of a kind=app bundle copied out of sFiles[] at install
// start so a later registry refresh cannot mutate URLs mid-download.
class AppBundleSnapshot {
 public:
  AppBundleSnapshot() = default;
  ~AppBundleSnapshot() {
    if (files_) std::free(files_);
  }

  AppBundleSnapshot(const AppBundleSnapshot&) = delete;
  AppBundleSnapshot& operator=(const AppBundleSnapshot&) = delete;

  bool load(const AssetEntry& entry) {
    if (entry.fileCount == 0 || sFiles == nullptr) return false;
    const size_t bytes =
        static_cast<size_t>(entry.fileCount) * sizeof(AssetFileEntry);
    void* p = BadgeMemory::allocPreferPsram(bytes);
    if (!p) return false;
    files_ = static_cast<AssetFileEntry*>(p);
    count_ = entry.fileCount;
    std::memcpy(files_, &sFiles[entry.firstFileIdx], bytes);
    for (uint16_t i = 0; i < count_; ++i) {
      if (!isPlausibleUrl(files_[i].url)) return false;
    }
    return true;
  }

  const AssetFileEntry& operator[](uint16_t i) const { return files_[i]; }
  uint16_t count() const { return count_; }

 private:
  AssetFileEntry* files_ = nullptr;
  uint16_t count_ = 0;
};

}  // namespace

bool install(const AssetEntry& entry, AssetProgressCb cb, void* user) {
  setError("");
  if (entry.dest_path[0] != '/') {
    setError("dest_path must be absolute");
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kDestPathInvalid};
      cb(p, user);
    }
    return false;
  }

  // Nested call from installAll() — the outer InstallHold already owns
  // sInstalling. Standalone installs acquire the hold here.
  const bool nested = sInstalling.load(std::memory_order_acquire);
  InstallHold hold(!nested);
  if (!nested && !hold) {
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kRegistryBusy};
      cb(p, user);
    }
    return false;
  }

  // Adopt-on-install: if the destination is already fully present at
  // the expected size, skip the download and just record the version.
  // statusOf() now does the same on read, but a per-asset Install press
  // from AssetDetailScreen bypasses statusOf entirely — and the
  // free-space gate below would otherwise fail for assets whose own
  // existing footprint dominates the free count (DOOM WAD: 4.5 MB
  // against ~2 MB residual free on a typical ffat partition).
  if (destFullyPresent(entry)) {
    DBG("[registry] install %s: already on disk, adopting\n",
                  entry.id);
    persistInstalledVersion(entry.id, entry.version);
    if (cb) {
      AssetProgress p{entry.size, entry.size, true, AssetInstallResult::kOk};
      cb(p, user);
    }
    return true;
  }

  // Free-space gate. For app bundles we rely on the registry-declared
  // total bundle size; the per-file sizes inside it are summed only as
  // a sanity check below if min_free_bytes is missing.
  size_t needed = entry.min_free_bytes;
  if (needed == 0) needed = entry.size + 1024;
  uint64_t freeBytes = 0;
  if (!ensureFreeSpace(needed, &freeBytes)) {
    DBG("[registry] insufficient space: need=%u have=%llu\n",
                  (unsigned)needed, (unsigned long long)freeBytes);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "need %u, have %u KB",
                  (unsigned)(needed / 1024),
                  (unsigned)(freeBytes / 1024));
    setError(buf);
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kInsufficientSpace};
      cb(p, user);
    }
    return false;
  }
  DBG("[registry] space ok: need=%u have=%llu\n",
                (unsigned)needed, (unsigned long long)freeBytes);

  if (entry.kind == AssetKind::kFile) {
    char fileUrl[kAssetUrlMax];
    copyField(fileUrl, sizeof(fileUrl), entry.url);
    if (!isPlausibleUrl(fileUrl)) {
      setError("bad asset url");
      if (cb) {
        AssetProgress p{0, 0, true, AssetInstallResult::kHttpError};
        cb(p, user);
      }
      return false;
    }
    size_t written = 0;
    AssetInstallResult code = installFileToPath(
        fileUrl, entry.sha256, entry.size, entry.dest_path,
        cb, user, &written);
    if (code != AssetInstallResult::kOk) {
      if (cb) {
        AssetProgress p{written, entry.size, true, code};
        cb(p, user);
      }
      return false;
    }
    persistInstalledVersion(entry.id, entry.version);
    DBG("[registry] installed %s -> %s (%u bytes)\n",
                  entry.id, entry.dest_path, (unsigned)written);
    if (cb) {
      AssetProgress p{written, entry.size, true, AssetInstallResult::kOk};
      cb(p, user);
    }
    return true;
  }

  // ── kApp ────────────────────────────────────────────────────────────
  // dest_path here is the bundle directory, e.g. "/apps/zigmoji". Each
  // file's `path` is relative to it.
  DBG("[registry] install app %s (%u files) -> %s\n",
                entry.id, static_cast<unsigned>(entry.fileCount),
                entry.dest_path);
  FATFS* fs = replay_get_fatfs();
  if (!fs) {
    setError("filesystem not mounted");
    DBG("[registry] install fail: filesystem not mounted\n");
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kFsWriteError};
      cb(p, user);
    }
    return false;
  }
  AppBundleSnapshot bundle;
  if (!bundle.load(entry)) {
    setError("app bundle snapshot failed");
    DBG("[registry] install fail: %s bundle snapshot (count=%u)\n",
                  entry.id, static_cast<unsigned>(entry.fileCount));
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kHttpError};
      cb(p, user);
    }
    return false;
  }
  const uint16_t bundleFileCount = bundle.count();
  // We hold a single outer IOLock for the whole bundle install — the
  // recursive mutex lets installFileToPath's own IOLock just bump the
  // counter, and we avoid any window where MicroPython could rename
  // or unlink directories out from under us mid-bundle.
  //
  // Note: we do NOT explicitly f_mkdir(dest_path) here because plain
  // f_mkdir is non-recursive and FR_NO_PATH-fails when an intermediate
  // parent (e.g. "/apps/") doesn't exist on a fresh FFAT. Instead each
  // per-file ensureDirsForPath() below walks every segment of the
  // destination and f_mkdirs them in order, which creates the bundle
  // root and any nested sub-directories on the way to the first file.
  Filesystem::IOLock bundleLock;

  size_t totalBundleBytes = entry.size;
  if (totalBundleBytes == 0) {
    for (uint16_t i = 0; i < bundleFileCount; ++i) {
      totalBundleBytes += bundle[i].size;
    }
  }
  size_t doneBundleBytes = 0;

  for (uint16_t i = 0; i < bundleFileCount; ++i) {
    const AssetFileEntry& fe = bundle[i];
    char destFull[kAssetPathMax * 2];
    // Allow file paths that already start with '/'; otherwise insert one.
    int n;
    if (fe.path[0] == '/') {
      n = std::snprintf(destFull, sizeof(destFull), "%s%s",
                        entry.dest_path, fe.path);
    } else {
      n = std::snprintf(destFull, sizeof(destFull), "%s/%s",
                        entry.dest_path, fe.path);
    }
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(destFull)) {
      setError("dest path too long");
      DBG("[registry] install fail: %s path too long for %s + %s\n",
                    entry.id, entry.dest_path, fe.path);
      if (cb) {
        AssetProgress p{doneBundleBytes, totalBundleBytes, true,
                        AssetInstallResult::kDestPathInvalid};
        cb(p, user);
      }
      return false;
    }
    if (!ensureDirsForPath(fs, destFull)) {
      char buf[80];
      std::snprintf(buf, sizeof(buf), "mkdir parents for %s", destFull);
      setError(buf);
      DBG("[registry] install fail: %s\n", buf);
      if (cb) {
        AssetProgress p{doneBundleBytes, totalBundleBytes, true,
                        AssetInstallResult::kFsWriteError};
        cb(p, user);
      }
      return false;
    }

    // Wrap the per-file callback so the UI sees bundle-wide progress
    // (so the bar fills smoothly across all files of an app), not just
    // the current sub-file's bytes.
    struct WrapCtx {
      AssetProgressCb inner;
      void* innerUser;
      size_t prior;          // bytes already finished from earlier files
      size_t bundleTotal;
    } ctx{cb, user, doneBundleBytes, totalBundleBytes};

    AssetProgressCb fileCb = +[](const AssetProgress& sub, void* u) {
      auto* c = static_cast<WrapCtx*>(u);
      if (!c || !c->inner) return;
      AssetProgress p{c->prior + sub.bytesWritten, c->bundleTotal,
                      false, AssetInstallResult::kOk};
      c->inner(p, c->innerUser);
    };

    size_t written = 0;
    AssetInstallResult code = installFileToPath(
        fe.url, fe.sha256, fe.size, destFull,
        cb ? fileCb : nullptr, &ctx, &written);
    doneBundleBytes += written;
    if (code != AssetInstallResult::kOk) {
      DBG("[registry] install fail: %s file %u/%u (%s) code=%d %s\n",
                    entry.id, static_cast<unsigned>(i + 1),
                    static_cast<unsigned>(bundleFileCount),
                    fe.path, static_cast<int>(code), sLastError);
      if (cb) {
        AssetProgress p{doneBundleBytes, totalBundleBytes, true, code};
        cb(p, user);
      }
      return false;
    }
  }

  persistInstalledVersion(entry.id, entry.version);
  DBG("[registry] installed app %s -> %s (%u files, %u bytes)\n",
                entry.id, entry.dest_path,
                (unsigned)bundleFileCount, (unsigned)doneBundleBytes);
  if (cb) {
    AssetProgress p{doneBundleBytes, totalBundleBytes, true,
                    AssetInstallResult::kOk};
    cb(p, user);
  }
  return true;
}

InstallAllResult installAll(AssetBatchHeadlineCb headlineCb,
                            AssetProgressCb progressCb,
                            void* user) {
  InstallAllResult out{0, 0, 0};

  InstallHold batch(true);
  if (!batch) return out;

  // Snapshot pending ids while the install lock is held so a refresh
  // cannot shrink or reorder sAssets[] between counting and download.
  char pendingIds[kMaxRegistryAssets][kAssetIdMax];
  uint8_t pendingCount = 0;
  for (uint8_t i = 0; i < sAssetCount && pendingCount < kMaxRegistryAssets;
       ++i) {
    AssetStatus s = statusOf(sAssets[i]);
    if (s == AssetStatus::kInstalled) continue;
    copyField(pendingIds[pendingCount], kAssetIdMax, sAssets[i].id);
    ++pendingCount;
  }
  out.total = pendingCount;
  if (pendingCount == 0) return out;

  for (uint8_t pi = 0; pi < pendingCount; ++pi) {
    const AssetEntry* e = findById(pendingIds[pi]);
    if (!e) {
      ++out.failed;
      continue;
    }
    if (headlineCb) headlineCb(*e, pi, pendingCount, user);
    if (install(*e, progressCb, user)) {
      out.succeeded++;
    } else {
      out.failed++;
    }
  }
  return out;
}

bool remove(const AssetEntry& entry) {
  setError("");
  if (entry.kind == AssetKind::kApp) {
    // For app bundles, recursively remove the whole dest_dir tree.
    // FATFS f_unlink doesn't recurse; we walk children manually. The
    // pool lists every file we own — we delete those, collect their
    // unique parent dirs, then unlink each parent dir from deepest to
    // shallowest. Anything the user dropped into the bundle dir
    // out-of-band stays put (the final f_unlink on the bundle root
    // will fail with FR_DENIED, which is fine).
    FATFS* fs = replay_get_fatfs();
    if (fs && sFiles) {
      Filesystem::IOLock lock;
      // Collected sub-directory paths (relative to dest_path), with
      // the longest first so we delete leaves before parents.
      char dirs[8][kAssetPathMax];
      uint8_t dirCount = 0;

      for (uint16_t i = 0; i < entry.fileCount; ++i) {
        const AssetFileEntry& fe = sFiles[entry.firstFileIdx + i];
        char destFull[kAssetPathMax * 2];
        if (fe.path[0] == '/') {
          std::snprintf(destFull, sizeof(destFull), "%s%s",
                        entry.dest_path, fe.path);
        } else {
          std::snprintf(destFull, sizeof(destFull), "%s/%s",
                        entry.dest_path, fe.path);
        }
        f_unlink(fs, destFull);

        // Track every parent dir below dest_path so we can rmdir them
        // after all files are gone.
        char parent[kAssetPathMax];
        std::strncpy(parent, destFull, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char* slash = std::strrchr(parent, '/');
        while (slash && slash != parent) {
          *slash = '\0';
          if (std::strcmp(parent, entry.dest_path) == 0) break;
          // Dedup.
          bool seen = false;
          for (uint8_t d = 0; d < dirCount; ++d) {
            if (std::strcmp(dirs[d], parent) == 0) { seen = true; break; }
          }
          if (!seen && dirCount < (uint8_t)(sizeof(dirs) / sizeof(dirs[0]))) {
            std::strncpy(dirs[dirCount], parent, kAssetPathMax - 1);
            dirs[dirCount][kAssetPathMax - 1] = '\0';
            ++dirCount;
          }
          slash = std::strrchr(parent, '/');
        }
      }

      // Sort dirs by descending length so leaves are unlinked first.
      for (uint8_t i = 0; i < dirCount; ++i) {
        for (uint8_t j = i + 1; j < dirCount; ++j) {
          if (std::strlen(dirs[j]) > std::strlen(dirs[i])) {
            char tmp[kAssetPathMax];
            std::memcpy(tmp, dirs[i], kAssetPathMax);
            std::memcpy(dirs[i], dirs[j], kAssetPathMax);
            std::memcpy(dirs[j], tmp, kAssetPathMax);
          }
        }
      }
      for (uint8_t i = 0; i < dirCount; ++i) f_unlink(fs, dirs[i]);

      // Best-effort bundle-root removal.
      f_unlink(fs, entry.dest_path);
    }
  } else {
    Filesystem::removeFile(entry.dest_path);
  }
  persistInstalledVersion(entry.id, "");
  return true;
}

const char* lastErrorMessage() { return sLastError; }
time_t lastRefreshEpoch() { return sLastRefreshEpoch; }

}  // namespace ota::registry
