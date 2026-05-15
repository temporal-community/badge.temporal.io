// BadgeOTA.h — Firmware OTA driven by GitHub Releases.
//
// Polls `https://api.github.com/repos/<owner>/<repo>/releases/latest` on a
// once-per-day cadence (state persisted in NVS namespace `badge_ota`),
// caches the latest tag + matching asset URL, and exposes:
//
//   - `updateAvailable()` for the status-bar glyph and home-tile label.
//   - `checkNow()` to refresh on demand from the Update screen.
//   - `installCached()` to stream the cached `.bin` into the inactive
//                      OTA slot via `Update.write` and reboot.
//
// The asset name the badge looks for is supplied at compile time via
// `OTA_ASSET_NAME` (default `firmware.bin`) so different
// environments / forks pick the right `.bin` from a multi-asset
// release. The repo to query is `OTA_GITHUB_REPO`
// (default `temporal-community/badge.temporal.io`).
//
// First-boot rollback: `markCurrentAppValidIfPending()` should be
// called once the GUI has ticked healthily for ~30 s after a fresh
// install. If that doesn't happen before the next reset, the
// bootloader auto-rolls back to the previous slot.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <time.h>

#ifndef OTA_GITHUB_REPO
#define OTA_GITHUB_REPO "temporal-community/badge.temporal.io"
#endif

#ifndef OTA_ASSET_NAME
#define OTA_ASSET_NAME "firmware.bin"
#endif

namespace ota {

enum class CheckResult : uint8_t {
  kOkUpToDate,        // tag == FIRMWARE_VERSION
  kOkNewerAvailable,  // tag > FIRMWARE_VERSION; cached
  kOkOlder,           // tag < FIRMWARE_VERSION (pre-release / downgrade)
  kNoMatchingAsset,   // release exists but no `firmware-<env>.bin`
  kNetworkError,      // wifi / http failure
  kParseError,        // JSON malformed
  kCooldownActive,    // 24h cooldown not elapsed and ignoreCooldown=false
};

enum class InstallResult : uint8_t {
  kOk,                // Update.end() succeeded; ESP.restart() pending
  kNoAssetCached,     // checkNow() never produced a downloadable asset
  kBatteryTooLow,     // < kMinBatteryPct and not on charger
  kWifiUnavailable,
  kHttpError,
  kUpdateBeginFailed, // Update.begin() rejected (slot too small, etc.)
  kWriteFailed,
  kEndFailed,
};

struct InstallProgress {
  size_t bytesWritten;
  size_t totalBytes;     // 0 if unknown
  bool done;
  InstallResult result;  // valid when done == true
};

using InstallProgressCb = void (*)(const InstallProgress&, void* user);

// Refuse to start an install below this charge percentage unless the
// charger is plugged in. A bricked badge is the only real risk and a
// dead battery mid-flash is the most likely cause.
constexpr uint8_t kMinBatteryPct = 30;

// Cooldown between automatic API checks. Manual "Check now" from the
// Update screen passes ignoreCooldown=true.
constexpr uint32_t kCheckCooldownSec = 24 * 60 * 60;

// Initialise from NVS cache. Call after Preferences is usable
// (post-nvs_flash_init in setup()).
void begin();

// Drives the daily-cadence trigger. Call from the Scheduler tick or
// from `WiFiService` after a successful connect. Cheap when nothing
// to do.
void tick();

// Synchronous check. Returns the parsed result; on success the
// internal cache is updated.
CheckResult checkNow(bool ignoreCooldown);

// True iff the cached `latest_tag` is newer than `FIRMWARE_VERSION`
// AND we have a matching asset URL on file.
bool updateAvailable();

// Cached info — empty strings when nothing is cached.
const char* latestKnownTag();
const char* latestKnownAssetUrl();
size_t latestKnownAssetSize();
time_t lastCheckEpoch();

// Last user-facing error message from checkNow / installCached. Empty
// after a successful call. Lifetime is until the next call.
const char* lastErrorMessage();

// Stream the cached asset URL into the inactive OTA slot. Calls
// `cb` periodically with progress (caller-owned, may be null). On
// success: callback fires with `done=true result=kOk`, then returns
// `kOk` and the badge should call `ESP.restart()` immediately.
InstallResult installCached(InstallProgressCb cb, void* user);

// Bootloader rollback safety. Called from main.cpp after the GUI is
// confirmed healthy on a fresh install. No-op if the running app is
// already marked valid.
void markCurrentAppValidIfPending();

// True when running from an OTA slot that is awaiting validation
// (i.e. this is the first boot after an install). Used by main.cpp
// to arm the 30s health timer.
bool runningPendingVerify();

// ── Storage / partition introspection ─────────────────────────────────────
//
// The 16 MB flash chip's `ffat` partition can be larger than the FAT
// volume currently formatted on it (e.g. after an OTA bump that ships
// a wider partition table). The badge will only see the FAT-reported
// volume size until it reformats. These helpers let the Firmware
// Update screen offer a one-tap "Expand storage" action when the gap
// is large enough to be worth surfacing.

// Bytes reserved for the `ffat` partition by the partition table.
// Returns 0 if the partition can't be found.
size_t ffatPartitionBytes();

// Bytes the currently-mounted FAT volume reports as its total
// capacity (used + free). Returns 0 if the FS is not mounted.
size_t ffatVolumeBytes();

// True iff partition >> volume by a meaningful margin (> 256 KB).
// Used to decide whether to surface the "Expand storage" affordance.
bool ffatExpansionAvailable();

// Reformats `ffat` and reboots. Synchronous; does NOT return on
// success. Wipes ALL user data on `/`. Caller must have already
// confirmed the destructive action with the user.
void reformatFfatAndReboot();

}  // namespace ota
