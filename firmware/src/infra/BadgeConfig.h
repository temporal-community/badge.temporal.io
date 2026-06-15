// BadgeConfig.h — Compatibility shim for modular conference-badge code.
// Maps compile-time constants from the original BadgeConfig.h to runtime
// Config getters (NVS / settings.txt) and CharlieDefines.h pin numbers.

#pragma once
#include <Arduino.h>
#include <stdint.h>

#include "hardware/HardwareConfig.h"
#include "Scheduler.h"
#include "RepoUrls.h"

// Optional WiFi credentials. The badge keeps a small list of saved
// networks; `wifiSsid()` / `wifiPass()` always reflect slot 0 (the
// preferred network) for any legacy caller that just wants "the"
// SSID. Connection attempts iterate every saved slot — see
// WiFiService::connect.
#define WIFI_SSID      badgeConfig.wifiSsid()
#define WIFI_PASS      badgeConfig.wifiPass()

// Timing
// Generous enough to cover WPA3 SAE handshakes and the occasional slow
// AP that takes its time replying to assoc-request. Earlier slot
// candidates use a shorter per-attempt budget (kPerAttemptTimeoutMs in
// WiFiService.cpp); only the *last* candidate gets the full timeout.
static const int WIFI_TIMEOUT_MS    = 30000;
static const int PAIRING_TIMEOUT_MS = 15000;
static const int POLL_INTERVAL_MS   = 2000;

// Pin aliases — map modular XIAO names to Charlie board GPIOs
#define BTN_UP     BUTTON_UP
#define BTN_DOWN   BUTTON_DOWN
#define BTN_LEFT   BUTTON_LEFT
#define BTN_RIGHT  BUTTON_RIGHT

// Joystick / display tuning
static const float JOY_DEADBAND        = 0.08f;
static const float MENU_NAV_THRESHOLD  = 0.5f;
static const bool  TILT_SHOWS_BADGE    = true;

// Messaging poll interval
#define MSG_POLL_INTERVAL_MS 5000

// Role number constants kept for nametag/UI compatibility.
#define ROLE_NUM_ATTENDEE  1
#define ROLE_NUM_STAFF     2
#define ROLE_NUM_VENDOR    3
#define ROLE_NUM_SPEAKER   4
#define ROLE_NUM_DEITY     5 // this is god mode, remove this from the production build


// ---------------------------------------------------------------------------
//  Setting indices and font-family constants shared by Config + FileBrowser.
// ---------------------------------------------------------------------------

enum SettingIndex : uint8_t {
  kLedBrightness,
  kHapticEnabled,
  kHapticStrength,
  kHapticFreqHz,
  kHapticPulseMs,
  kJoySensitivity,
  kJoyDeadzone,
  kFontFamily,
  kFontSize,
  kLightSleepSec,
  kDeepSleepSec,
  kAutoFlipEnable,
  kFlipUpThreshold,
  kFlipDownThreshold,
  kFlipDelayMs,
  kFlipButtons,
  kFlipJoystick,
  kOledOsc,
  kOledDiv,
  kOledMux,
  kOledPrecharge1,
  kOledPrecharge2,
  kOledContrast,

  // Dev-tuning settings (remove for production)
  kImuSmoothing,
  kImuInt1Threshold,
  kImuInt1Duration,
  kBtnDebounceMs,
  kRptInitialDelayMs,
  kRptFirstIntervalMs,
  kRptSecondDelayMs,
  kRptSecondIntervalMs,
  kLedServiceMs,
  kOledRefreshMs,
  kJoyPollMs,
  kSchHighDiv,
  kSchNormDiv,
  kSchLowDiv,
  kLoopDelayMs,
  kCpuIdleMhz,
  kCpuActiveMhz,
  kWifiCheckMs,

  kBoopIrInfo,
  kBoopInfoFields,
  kIrTxPowerPct,

  kNotifyIrEnable,

  // Confirm/cancel grammar. 0 = Xbox-style A confirm / B cancel.
  // 1 = Nintendo-style B confirm / A cancel (default).
  kSwapConfirmCancel,

  // Serial log gates — controlled via `DebugLog.h` macros
  // (LOG_IR / LOG_BOOP / LOG_NOTIFY / LOG_ZIGMOJI / LOG_IMU).
  // Defaults are OFF so the
  // serial console is quiet under normal use; flip any category
  // to 1 in settings.txt to re-enable its per-event spam while
  // debugging. Boot / init / error logs stay unconditional.
  kLogIr,       // BadgeIR frame-level events
  kLogBoop,     // BadgeBoops + BoopFeedback pairing-protocol events
  kLogNotify,   // Reserved legacy notification/debug category
  kLogZigmoji,  // Reserved legacy zigmoji/debug category
  kLogImu,      // IMU samples, orientation thresholds, flip transitions

  // Header-clock "artificial horizon" effect — when enabled, the centered
  // time pill drifts/tilts with the IMU. Off = static text rendering.
  kHorizonClock,

  // Master WiFi enable. When 0, the badge never auto-connects on boot
  // and explicit connect attempts (including badge.http_get/post from
  // MicroPython) bail immediately. Default 1; effectively forced off
  // when no SSID/password is configured.
  kWifiEnabled,

  // CREDITS launcher backend. 0 = run the native AboutCreditsScreen
  // (drawXBM directly out of AboutCredits.h, no Python interpreter
  // overhead). 1 = launch the MicroPython /apps/credits.py companion
  // (slower per-frame but lives on the editable filesystem so the
  // bitmap-walk code can be tweaked without reflashing). Both views
  // render the same headshot blob — see scripts/gen_credit_xbms.py.
  kCreditsUsePython,
};

extern const uint8_t kFontFamilyCount;
extern const char* const kFontFamilyNames[];
int8_t fontFamilyFromName(const char* name);

// ---------------------------------------------------------------------------
//  Config — persistent badge settings.
//  Primary storage: human-readable INI file on the FAT filesystem.
//  Fallback: NVS (Preferences) when the filesystem is not yet mounted.
//  On boot: loadFromNvs() gives early defaults, then loadFromFile() is
//  called after MicroPython mounts the FAT partition.  applyAll() pushes
//  values to hardware.  The FileBrowser edits values live and calls
//  saveToFile() on exit.
// ---------------------------------------------------------------------------

class Config {
    public:
     struct SettingDef {
       const char* key;
       const char* label;
       int32_t defaultValue;
       int32_t minValue;
       int32_t maxValue;
       int32_t step;
     };

     static const SettingDef kDefs[];
     static const uint8_t kCount;
     // Editable runtime settings. Not provisioned by StartupFiles; created on
     // first boot and then left as a normal user-editable FatFS file.
     static constexpr const char* kSettingsPath = "settings.txt";
     static constexpr const char* kDefaultTimezone =
         "PST8PDT,M3.2.0,M11.1.0";

     // Headroom for future settings.  MUST stay >= kCount (enforced by
     // static_assert in BadgeConfig.cpp) — a too-small array silently
     // truncates writes from set() / saveToNvs() and OOB-reads in
     // get() / apply() corrupt adjacent Config members (which is what
     // caused the "log_notify shows a huge number and crashes the
     // badge" bug in April 2026).
     static constexpr uint8_t kMaxSettings = 64;

     // ESP32 supports a discrete set of CPU clock frequencies. Settings indices
     // kCpuIdleMhz / kCpuActiveMhz are snapped to one of these values.
     static const int32_t kCpuValidMhz[];
     static const uint8_t kCpuValidMhzCount;

     Config();

     bool loadFromNvs();
     bool saveToNvs();

     bool loadFromFile();
     bool saveToFile();

     bool load();
     bool save();

     int32_t get(uint8_t index) const;
     void set(uint8_t index, int32_t value);

     // Compute the next value for `index` given current value and a direction.
     // Most settings step by `kDefs[index].step * magnitude` (clamped to range);
     // CPU frequency settings step through kCpuValidMhz[] by `magnitude` slots.
     static int32_t nextValue(uint8_t index, int32_t current, int8_t dir,
                              uint8_t magnitude = 1);

     void applyAll();
     void apply(uint8_t index);

     bool checkFileChanged();
     void snapshotFileStat();

     // Maximum number of saved WiFi networks. Stored as
     // `ui_wifi_ssid_<n>` / `ui_wifi_pwd_<n>` blobs in NVS (slot 0 is
     // the preferred network and is also exposed via `wifiSsid()` /
     // `wifiPass()` for any legacy caller that wants "the" SSID).
     // 4 keeps the NVS footprint small but covers home + work +
     // hotspot + venue, which is the realistic max we'd ever pre-load.
     static constexpr uint8_t kMaxWifiNetworks = 4;

     // Slot 0 accessors — kept for backwards compatibility with the
     // `WIFI_SSID` / `WIFI_PASS` macros. `wifiSsid()` returns the
     // preferred network, or an empty string when no network is
     // configured.
     const char* wifiSsid() const { return wifiSlots_[0].ssid; }
     const char* wifiPass() const { return wifiSlots_[0].pass; }
     // Per-slot accessors. `index` must be < kMaxWifiNetworks.
     // Out-of-range indices return empty strings rather than asserting
     // — keeps the screen-side code simple.
     const char* wifiSsidAt(uint8_t index) const;
     const char* wifiPassAt(uint8_t index) const;
     uint8_t wifiNetworkCount() const;  // # of slots with non-empty ssid
     bool wifiSlotConfigured(uint8_t index) const;
     bool wifiConfigured() const;       // any slot has ssid+pass
     // Master WiFi enable toggle (kWifiEnabled). Returns false when the
     // setting is off OR when no SSID/password are configured.
     bool wifiEnabled() const;
     // UI-entered WiFi credentials, slot 0. Convenience wrapper over
     // `setWifiCredentialsAt(0, ssid, pass)`. Passing nullptr for
     // either field leaves that field unchanged.
     void setWifiCredentials(const char* ssid, const char* pass);
     // Per-slot edit. `index` must be < kMaxWifiNetworks. Passing a
     // null SSID or password leaves that field alone; passing an empty
     // SSID clears the slot entirely (and removes the password blob).
     void setWifiCredentialsAt(uint8_t index, const char* ssid,
                               const char* pass);
     // Add a network to the first empty slot (or update the slot whose
     // SSID already matches). Returns the slot index used, or -1 when
     // the list is full.
     int8_t addWifiNetwork(const char* ssid, const char* pass);
     // Drop a slot and shift later slots up so there are no gaps.
     void removeWifiNetwork(uint8_t index);
     // Reorder slot `index` up (toward 0). No-op when at the top.
     void moveWifiNetworkUp(uint8_t index);
     // Reorder slot `index` down. No-op when already at the bottom.
     void moveWifiNetworkDown(uint8_t index);
     // Set the wall clock to today's date (or 2026-01-01 if no date is
     // available yet) at HH:MM:00 local time. Returns true on success.
     bool setManualTime(int hour24, int minute);
     const char* timezone() const { return timezone_; }
     void setTimezone(const char* value);
     void applyTimezone() const;

     /// `[nametag] nametag = default | <8-char hex id> | …/info.json`
     const char* nametagSetting() const { return nametagSetting_; }
     void setNametagSetting(const char* value);
     /// Load/clear `gNametagDoc` from `[nametag] nametag =` in settings.txt.
     void applyNametagSetting();

     /// `[ota] manifest_url = ...` — overrides the default GitHub
     /// release endpoint when set. Leave empty to use the default
     /// (`https://api.github.com/repos/<OTA_GITHUB_REPO>/releases/latest`).
     const char* otaManifestUrl() const { return otaManifestUrl_; }
     void setOtaManifestUrl(const char* value);

     /// `[ota] community_apps_url = ...` — JSON file enumerating
     /// installable Community Apps + assets (DOOM WAD, etc.). Empty
     /// disables the Community Apps screen.
     ///
     /// The legacy `asset_registry_url` setting is read transparently
     /// (and written into `communityAppsUrl_`) so settings.txt files
     /// from older builds keep working unmodified.
     const char* communityAppsUrl() const;
     void setCommunityAppsUrl(const char* value);

     // Backwards-compat aliases. Existing call sites (BadgeConfig.cpp,
     // GUI.cpp, DoomScreen.cpp, AssetRegistry.cpp) still call these;
     // they forward to the new community_apps_url field.
     const char* assetRegistryUrl() const { return communityAppsUrl(); }
     void setAssetRegistryUrl(const char* value) { setCommunityAppsUrl(value); }

   private:
    int32_t values_[kMaxSettings];

     static constexpr uint8_t kStringMaxLen = 128;
     // One saved WiFi network. Empty SSID = unused slot. Password is
     // the *plaintext* (decoded from the obfuscated NVS blob at load
     // time); we re-scramble per slot when persisting.
     struct WifiSlot {
       char ssid[kStringMaxLen] = "";
       char pass[kStringMaxLen] = "";
     };
     WifiSlot wifiSlots_[kMaxWifiNetworks];
     char timezone_[64] = "PST8PDT,M3.2.0,M11.1.0";
     char nametagSetting_[64] = "default";
     char otaManifestUrl_[160] = "";
     // The repo-root /registry/community_apps.json file is the v2
     // Community Apps registry. The previous /registry/registry.json
     // (v1) is kept in-tree for backwards compatibility; older firmware
     // out in the field still hits it via its own baked default URL.
     //
     // Why raw.githubusercontent.com and not jsDelivr? The repo (with
     // its MicroPython submodule + DOOM WAD + zigmoji frames) is over
     // jsDelivr's free 50 MB per-package limit, so jsdelivr replies
     // with a 403 "Package size exceeded" page. GitHub raw has a
     // 60 req/hr unauthenticated rate limit but the badge only fetches
     // once a day so this is fine.
     char communityAppsUrl_[160] = REPO_COMMUNITY_APPS_URL;

     uint32_t lastFileSize_ = 0;
     uint16_t lastFileDate_ = 0;
     uint16_t lastFileTime_ = 0;

     bool parseSettingsFile(const char* buf, uint16_t len);
     uint16_t formatSettingsFile(char* buf, uint16_t bufSize) const;
     void loadNetworkFromBuild();
     void loadStringsFromNvs();
     void saveStringsToNvs();
     void clearLegacyNetworkFromNvs();
     void persistWifiSlot(uint8_t index) const;
     void clearWifiSlotInNvs(uint8_t index) const;
   };

   extern Config badgeConfig;

   // ---------------------------------------------------------------------------
   //  ConfigWatcher — scheduler service that polls settings.txt for changes.
   //  When the file is modified externally (e.g. via USB/MicroPython), the
   //  watcher reloads and applies settings automatically.
   // ---------------------------------------------------------------------------

   class ConfigWatcher : public IService {
    public:
     void begin(Config* config);
     void service() override;
     const char* name() const override;

    private:
     Config* config_ = nullptr;
     uint32_t lastCheckMs_ = 0;
     static constexpr uint32_t kPollIntervalMs = 2000;
   };

   extern ConfigWatcher configWatcher;
