// AssetRegistry.h — Generic file fetcher driven by a remote registry.json.
//
// Lets the badge install user-facing asset files (DOOM WAD, sound packs,
// fonts, anything else) on demand from a configurable URL set in
// `settings.txt` (`community_apps_url`, formerly `asset_registry_url`).
//
// Schema v2 (registry/community_apps.json) supports two entry kinds:
// single files (`kind: "file"`) and multi-file app bundles
// (`kind: "app"`). Both kinds are installed through registry::install.
//
//   {
//     "schema_version": 2,
//     "assets": [
//       {
//         "id": "doom1-shareware",
//         "kind": "file",
//         "name": "DOOM 1 Shareware WAD",
//         "version": "1.9",
//         "url": "https://.../doom1.wad",
//         "sha256": "<hex>",            // optional, corruption check only
//         "size": 4196020,
//         "dest_path": "/doom1.wad",
//         "min_free_bytes": 4500000,    // optional
//         "description": "..."
//       },
//       {
//         "id": "tardigotchi",
//         "kind": "app",
//         "name": "Tardigotchi",
//         "version": "<sha256-stem>",
//         "dest_dir": "/apps/tardigotchi",
//         "size": 33744,
//         "description": "...",
//         "files": [                     // inlined; no separate manifest.json
//           {"path": "/main.py",   "size": 187,
//            "sha256": "...", "url": "https://..."},
//           {"path": "/engine.py", "size": 24006,
//            "sha256": "...", "url": "https://..."}
//         ]
//       }
//     ]
//   }
//
// Per-asset state (installed version) is persisted in NVS under
// namespace `badge_assets` keyed by asset id. Single-file installs
// stream into `<dest_path>.tmp` and atomic-rename; on failure the
// live file is left untouched.

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace ota {

constexpr uint8_t kMaxRegistryAssets = 48;
constexpr size_t kAssetIdMax = 32;
constexpr size_t kAssetNameMax = 48;
constexpr size_t kAssetVersionMax = 16;
constexpr size_t kAssetUrlMax = 256;
constexpr size_t kAssetPathMax = 64;
constexpr size_t kAssetSha256Max = 72;  // hex chars, NUL
constexpr size_t kAssetDescMax = 96;

enum class AssetKind : uint8_t {
  kFile,   // single file at dest_path; url + sha256 on the entry
  kApp,    // multi-file bundle under dest_path (treated as dest_dir);
           // per-file metadata lives in the parallel files[] pool
};

// One sub-file inside a kind=app bundle. Path is relative to the
// owning AssetEntry::dest_path (which is the bundle's dest_dir for
// apps). The pool of these is allocated from PSRAM during refresh —
// see registry internals.
struct AssetFileEntry {
  char path[kAssetPathMax];
  char url[kAssetUrlMax];
  char sha256[kAssetSha256Max];
  uint32_t size;
};

struct AssetEntry {
  char id[kAssetIdMax];
  char name[kAssetNameMax];
  char version[kAssetVersionMax];
  // For kind=file: the source URL. For kind=app: empty (per-file URLs
  // live in the AssetFileEntry pool).
  char url[kAssetUrlMax];
  char sha256[kAssetSha256Max];   // empty if not provided / kind=app
  // For kind=file: target file path (e.g. "/doom1.wad").
  // For kind=app:  target directory (e.g. "/apps/zigmoji").
  char dest_path[kAssetPathMax];
  char description[kAssetDescMax];
  uint32_t size;                  // file: byte count; app: total bytes
  uint32_t min_free_bytes;        // 0 if not specified
  AssetKind kind;
  uint16_t firstFileIdx;          // app: index into the file pool
  uint16_t fileCount;             // app: number of files in this bundle
};

enum class AssetStatus : uint8_t {
  kNotInstalled,
  kInstalled,
  kUpdateAvailable,
  kFailed,           // last install attempt errored
};

enum class RegistryRefresh : uint8_t {
  kOk,
  kCooldownActive,
  kNetworkError,
  kParseError,
  kNotConfigured,    // asset_registry_url is empty
  kInstallInProgress,
};

enum class AssetInstallResult : uint8_t {
  kOk,
  kDestPathInvalid,
  kInsufficientSpace,
  kHttpError,
  kFsWriteError,
  kSha256Mismatch,
  kAborted,
  kRegistryBusy,   // registry refresh overlapped the install
};

struct AssetProgress {
  size_t bytesWritten;
  size_t totalBytes;
  bool done;
  AssetInstallResult result;  // valid when done=true
};

using AssetProgressCb = void (*)(const AssetProgress&, void* user);

namespace registry {

void begin();

// Daily-cadence trigger; cheap when nothing to do.
void tick();

// Manual refresh from the Asset Library screen. Synchronous.
RegistryRefresh refresh(bool ignoreCooldown);

// Spawn a one-shot Core 0 task that runs `refresh(ignoreCooldown)`
// off the GUI thread. Returns false if a refresh is already running
// (caller should poll `isRefreshing()`); true if the task launched
// (or if it returned synchronously because no WiFi was available).
// Safe to call repeatedly — coalesces while in flight.
bool beginRefreshAsync(bool ignoreCooldown);

// True while the background refresh task is running.
bool isRefreshing();

// True while install() / installAll() holds the registry pool.
bool isInstalling();

// Result of the most recent refresh (sync or async). Defaults to kOk
// before the first call.
RegistryRefresh lastRefreshResult();

size_t count();
const AssetEntry* at(size_t index);
const AssetEntry* findById(const char* id);

AssetStatus statusOf(const AssetEntry& entry);
const char* installedVersionOf(const AssetEntry& entry);

bool install(const AssetEntry& entry, AssetProgressCb cb, void* user);
bool remove(const AssetEntry& entry);

// Walks every registered asset and installs each one whose status is
// kNotInstalled or kUpdateAvailable. Already-installed assets are
// skipped. The callback is invoked per-file (file kind) or per-sub-
// file (app kind) as the install progresses; betweenAssetCb fires
// once per asset boundary so the UI can swap headlines.
using AssetBatchHeadlineCb =
    void (*)(const AssetEntry& entry, size_t indexInBatch,
             size_t totalInBatch, void* user);

struct InstallAllResult {
  size_t total;        // assets considered for install (excluding already-OK)
  size_t succeeded;
  size_t failed;
};

InstallAllResult installAll(AssetBatchHeadlineCb headlineCb,
                            AssetProgressCb progressCb,
                            void* user);

const char* lastErrorMessage();
time_t lastRefreshEpoch();

}  // namespace registry

}  // namespace ota
