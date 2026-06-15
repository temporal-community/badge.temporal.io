#include "BadgeConfig.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <time.h>

#include "BuildWifiConfig.h"
#include "../api/WiFiService.h"
#include "../identity/BadgeUID.h"
#include "hardware/Haptics.h"
#include "hardware/IMU.h"
#include "hardware/Inputs.h"
#include "hardware/LEDmatrix.h"
#include "hardware/Power.h"
#include "ir/BadgeIR.h"
#include "DebugLog.h"
#include "Filesystem.h"
#include "PsramAllocator.h"
#include "hardware/oled.h"
#include "../BadgeGlobals.h"
#include "../screens/draw/AnimDoc.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

extern oled badgeDisplay;
extern LEDmatrix badgeMatrix;
extern Inputs inputs;
extern SleepService sleepService;
extern IMU imu;
extern Scheduler scheduler;

// ═══════════════════════════════════════════════════════════════════════════════
//  Font-family constants (shared with FileBrowser via BadgeConfig.h)
// ═══════════════════════════════════════════════════════════════════════════════

const uint8_t kFontFamilyCount = 10;
const char* const kFontFamilyNames[] = {
    "Default", "Profont", "Fixed", "Courier",
    "Helvetica", "NCenR", "Boutique", "Terminus",
    "Spleen", "Times"
};

int8_t fontFamilyFromName(const char* name) {
  for (uint8_t i = 0; i < kFontFamilyCount; ++i) {
    if (strcasecmp(name, kFontFamilyNames[i]) == 0) return static_cast<int8_t>(i);
  }
  return -1;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Config — file-backed settings (INI format on FAT filesystem)
// ═══════════════════════════════════════════════════════════════════════════════

  const Config::SettingDef Config::kDefs[] = {
      {"led_bright",  "LED Bright",  10,   1,   254,  1},
      {"haptic_en",   "Haptics",     1,    0,     1,  1},
      {"haptic_str",  "Haptic Str",  110,  0,   255,  5},
      {"haptic_freq", "Haptic Hz",   80,   10,  1000, 10},
      {"haptic_ms",   "Pulse ms",    35,   5,   500,  5},
      {"joy_sens",    "Joy Sens %",  80,   5,   100,  5},
      {"joy_dead",  "Joy Dead %",    12,    1,    254,   1},
      {"font_fam",    "Font",        8,    0,   9,    1},
      {"font_size",   "Font Size",   4,    0,   9,    1},
      {"lsleep_s",    "LightSlp s",  300,  0,  6000,  30},
      {"dsleep_s",    "DeepSlp s",   1200, 30, 36000, 60},
      {"flip_en",     "Auto Flip",   1,    0,      1,     1},
      {"flip_up",     "Flip Up mG",  -100, -950,   950,     50},
      {"flip_dn",     "Flip Dn mG",  750,  -950,      950,   50},
      {"flip_ms",     "Flip Delay",  0,  0,      30000,  50},
      {"flip_btn",    "Flip Btns",   1,    0,      1,     1},
      {"flip_joy",    "Flip Joy",    1,    0,      1,     1},
      {"oled_osc",    "OLED Osc",    15,    0,     15,     1},
      {"oled_div",    "OLED Div",    1,    1,     16,     1},
      {"oled_mux",    "OLED Mux",   64,   16,     64,     1},
      {"oled_pre1",   "PreChg P1",   1,    1,     15,     1},
      {"oled_pre2",   "PreChg P2",   15,    1,     15,     1},
      {"oled_ctr",    "Contrast",  255,    0,    255,     5},

      // Dev-tuning settings
      {"imu_smooth",  "IMU Smooth%", 18,   0,    99,    1},
      {"imu_thr",     "IMU Thr",     10,   0,    127,   1},
      {"imu_dur",     "IMU Dur",     0,    0,    127,   1},
      {"btn_dbnc",    "Debounce ms", 20,   5,    100,   5},
      {"rpt_init",    "RptInit ms",  325,  50,   1000,  25},
      {"rpt_int1",    "RptInt1 ms",  110,  20,   500,   10},
      {"rpt_dly2",    "RptDly2 ms",  900,  100,  3000,  50},
      {"rpt_int2",    "RptInt2 ms",  55,   10,   250,   5},
      {"led_intv",    "LED Intv ms", 25,   5,    200,   5},
      {"oled_rfsh",   "OLED Rfsh ms",100,  16,   500,   5},
      {"joy_poll",    "Joy Poll ms", 20,   5,    100,   5},
      {"sch_high",    "Sch High",    1,    1,    10,    1},
      {"sch_norm",    "Sch Normal",  4,    1,    30,    1},
      {"sch_low",     "Sch Low",     30,   1,    100,   1},
      {"loop_ms",     "Loop ms",     8,    1,    50,    1},
      // CPU freqs snap to kCpuValidMhz[]; step is unused for these indices.
      {"cpu_idle",    "CPU Idle",    80,   20,   240,   1},
      {"cpu_actv",    "CPU Active",  160,  20,   240,   1},
      {"wifi_chk",    "WiFi Chk ms", 30000,1000, 600000, 1000},

     // Boop / IR settings
     {"boop_ir_info","Boop Info",    1,    0,      1,     1},
     {"boop_fields", "Info Fields",  0x1FF,0,   0x1FF,    1},
     {"ir_tx_pw",    "IR TxPow %",  10,    1,     50,     1},

     // Notifications
     {"notify_ir",   "NotifIR",      1,    0,        1,      1},

     {"swap_ok",     "Confirm Btn",  1,    0,        1,      1},

     // Debug-log gates. Default OFF so serial is quiet; flip to 1
     // per category to re-enable per-event spam during debugging.
     {"log_ir",      "Log IR",       0,    0,        1,      1},
     {"log_boop",    "Log Boop",     0,    0,        1,      1},
     {"log_notify",  "Log Notify",   0,    0,        1,      1},
     {"log_zigmoji", "Log Zigmoji",  0,    0,        1,      1},
     {"log_imu",     "Log IMU",      0,    0,        1,      1},

     // UI toggles
     {"horiz_clk",   "Horizon Clock",1,    0,        1,      1},

     // Networking master switch. 0 = WiFi never auto-connects and explicit
     // connect attempts (incl. MicroPython badge.http_get/post) bail.
     {"wifi_en",     "WiFi",         1,    0,        1,      1},

     // CREDITS launcher backend. 0 = native AboutCreditsScreen,
     // 1 = MicroPython /apps/credits.py. Default 0 because the
     // native path has no Python warm-up cost on entry.
     {"creds_py",    "Credits Py",   0,    0,        1,      1},
};
  const uint8_t Config::kCount = sizeof(Config::kDefs) / sizeof(Config::kDefs[0]);

  // Guardrail: adding a setting without resizing values_[] corrupts
  // adjacent Config members at runtime (UB under
  // -Waggressive-loop-optimizations; observed live as "log_notify
  // shows a huge number and crashes when I change it").  If this ever
  // fires, bump Config::kMaxSettings in BadgeConfig.h.
  static_assert(sizeof(Config::kDefs) / sizeof(Config::kDefs[0])
                    <= Config::kMaxSettings,
                "kMaxSettings too small; bump Config::kMaxSettings");

  // ESP32 supported CPU frequencies (ascending).  setCpuFrequencyMhz() rejects
  // anything else, so all paths that write kCpuIdleMhz / kCpuActiveMhz must
  // snap into this set.
  const int32_t Config::kCpuValidMhz[] = {20, 40, 80, 160, 240};
  const uint8_t Config::kCpuValidMhzCount =
      sizeof(Config::kCpuValidMhz) / sizeof(Config::kCpuValidMhz[0]);

  Config badgeConfig;

  namespace {
  constexpr const char* kNvsNamespace = "badge_cfg";
  constexpr uint16_t kSettingsBufSize = 5120;
  Preferences gPrefs;

  bool isCpuMhzIndex(uint8_t index) {
    return index == kCpuIdleMhz || index == kCpuActiveMhz;
  }

  uint8_t nearestCpuMhzSlot(int32_t v) {
    uint8_t best = 0;
    int32_t bestDist = labs(static_cast<long>(v) -
                            static_cast<long>(Config::kCpuValidMhz[0]));
    for (uint8_t i = 1; i < Config::kCpuValidMhzCount; ++i) {
      int32_t d = labs(static_cast<long>(v) -
                       static_cast<long>(Config::kCpuValidMhz[i]));
      if (d < bestDist) {
        bestDist = d;
        best = i;
      }
    }
    return best;
  }

  // ─── WiFi-password obfuscation ──────────────────────────────────────────────
  //
  // We store the WiFi password in NVS as a byte blob XORed against a
  // 32-byte key derived from the eFuse MAC plus a fixed salt. This is
  // *not* real encryption — anyone who can read flash AND knows our
  // scheme can recover it — but it prevents the password from showing
  // up in a casual `nvs_dump` / strings(1) of the partition, and the
  // key is per-device so a leaked blob from one badge can't trivially
  // be replayed against another. Avoid logging plaintext passwords.

  constexpr uint8_t kWifiPwdSalt[16] = {
      0x4f, 0xa1, 0x37, 0xe8, 0x12, 0x9c, 0x60, 0xb5,
      0xd4, 0x73, 0x2b, 0xee, 0x55, 0x88, 0x1f, 0xca,
  };

  // The scramble key is derived from the eFuse MAC (`uid[]`). If we
  // ever reach this path before `read_uid()` has populated `uid[]`,
  // the resulting key is garbage (all-zeros XOR salt) and either
  // (a) writes an undecodable blob to NVS, or (b) decodes a perfectly
  // good blob into garbage. Both modes silently break auto-connect.
  // This guard refuses to scramble with a zero UID and logs loudly so
  // the boot-order regression is obvious in the serial log.
  bool wifiPwdKeyReady() {
    for (uint8_t i = 0; i < UID_SIZE; ++i) {
      if (uid[i] != 0) return true;
    }
    return false;
  }

  void buildWifiPwdKey(uint8_t out[32]) {
    for (uint8_t i = 0; i < 32; ++i) {
      out[i] = static_cast<uint8_t>(uid[i % UID_SIZE] ^
                                    kWifiPwdSalt[i % sizeof(kWifiPwdSalt)] ^
                                    (i * 0x9D));
    }
  }

  bool scrambleWifiPwd(const char* in, uint8_t* out, size_t len) {
    if (!wifiPwdKeyReady()) {
      Serial.println(
          "[Config] WiFi pwd scramble skipped: read_uid() has not run "
          "yet — refusing to write an undecodable blob to NVS");
      return false;
    }
    uint8_t key[32];
    buildWifiPwdKey(key);
    for (size_t i = 0; i < len; ++i) {
      out[i] = static_cast<uint8_t>(in[i]) ^ key[i % sizeof(key)];
    }
    return true;
  }

  bool unscrambleWifiPwd(const uint8_t* in, char* out, size_t len) {
    if (!wifiPwdKeyReady()) {
      Serial.println(
          "[Config] WiFi pwd unscramble skipped: read_uid() has not run "
          "yet — saved password would decode to garbage");
      out[0] = '\0';
      return false;
    }
    uint8_t key[32];
    buildWifiPwdKey(key);
    for (size_t i = 0; i < len; ++i) {
      out[i] = static_cast<char>(in[i] ^ key[i % sizeof(key)]);
    }
    out[len] = '\0';
    return true;
  }

  void trimInPlace(char* s) {
    char* start = s;
    while (*start == ' ' || *start == '\t') ++start;
    if (*start == '\0') { s[0] = '\0'; return; }
    char* end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) --end;
    size_t len = static_cast<size_t>(end - start + 1);
    if (start != s) memmove(s, start, len);
    s[len] = '\0';
  }

  void migrateFlipDelayDefault(int32_t values[]) {
    // We tried both historic defaults in the field: 0 ms was too twitchy,
    // 3000 ms made nametag mode feel broken. Keep explicit custom values,
    // but bring those defaults to the current 100 ms target.
    if (values[kFlipDelayMs] == 0 || values[kFlipDelayMs] == 3000) {
      values[kFlipDelayMs] = Config::kDefs[kFlipDelayMs].defaultValue;
    }
  }

  bool settingsContainsLegacyNetworkSecrets(const char* buf, uint16_t len) {
    char line[80];
    const char* p = buf;
    const char* end = buf + len;
    char section[24] = "";

    while (p < end) {
      const char* lineEnd = p;
      while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
      size_t lineLen = static_cast<size_t>(lineEnd - p);
      if (lineLen >= sizeof(line)) lineLen = sizeof(line) - 1;
      memcpy(line, p, lineLen);
      line[lineLen] = '\0';
      p = (lineEnd < end) ? lineEnd + 1 : end;

      trimInPlace(line);
      if (line[0] == '\0' || line[0] == '#' ||
          (line[0] == '/' && line[1] == '/')) {
        continue;
      }

      if (line[0] == '[') {
        char* rb = strchr(line, ']');
        if (rb) {
          *rb = '\0';
          strncpy(section, line + 1, sizeof(section) - 1);
          section[sizeof(section) - 1] = '\0';
        }
        continue;
      }

      if (strcmp(section, "wifi") != 0) continue;
      char* eq = strchr(line, '=');
      if (!eq) continue;
      *eq = '\0';
      char* key = line;
      trimInPlace(key);
      if (strcmp(key, "ssid") == 0 || strcmp(key, "pass") == 0 ||
          strcmp(key, "server") == 0 ||
          strcmp(key, "ep_badge") == 0 || strcmp(key, "ep_boops") == 0) {
        return true;
      }
    }
    return false;
  }

  bool settingsContainsTimezone(const char* buf, uint16_t len) {
    char line[96];
    const char* p = buf;
    const char* end = buf + len;
    char section[24] = "";

    while (p < end) {
      const char* lineEnd = p;
      while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
      size_t lineLen = static_cast<size_t>(lineEnd - p);
      if (lineLen >= sizeof(line)) lineLen = sizeof(line) - 1;
      memcpy(line, p, lineLen);
      line[lineLen] = '\0';
      p = (lineEnd < end) ? lineEnd + 1 : end;

      trimInPlace(line);
      if (line[0] == '\0' || line[0] == '#' ||
          (line[0] == '/' && line[1] == '/')) {
        continue;
      }

      if (line[0] == '[') {
        char* rb = strchr(line, ']');
        if (rb) {
          *rb = '\0';
          strncpy(section, line + 1, sizeof(section) - 1);
          section[sizeof(section) - 1] = '\0';
        }
        continue;
      }

      if (strcmp(section, "time") != 0) continue;
      char* eq = strchr(line, '=');
      if (!eq) continue;
      *eq = '\0';
      char* key = line;
      trimInPlace(key);
      if (strcmp(key, "timezone") == 0) return true;
    }
    return false;
  }
  }  // namespace
  
  Config::Config() {
    for (uint8_t i = 0; i < kCount && i < kMaxSettings; ++i) {
      values_[i] = kDefs[i].defaultValue;
    }
    loadNetworkFromBuild();
  }
  
  // ─── NVS (fallback for early boot before FAT is mounted) ─────────────────────
  
  bool Config::loadFromNvs() {
    // The WiFi password unscramble derives its key from the eFuse MAC
    // (`uid[]`). Calling load() before `read_uid()` produces a key of
    // zeros and silently corrupts the in-memory password, so the
    // first connect attempt fails with an "incorrect password" code
    // and the user assumes their saved credentials were lost. Refuse
    // to load early and shout about it.
    bool uidPopulated = false;
    for (uint8_t i = 0; i < UID_SIZE; ++i) {
      if (uid[i] != 0) { uidPopulated = true; break; }
    }
    if (!uidPopulated) {
      Serial.println(
          "[Config] loadFromNvs() called before read_uid() — WiFi password "
          "would decode to garbage. Caller must invoke read_uid() first.");
    }
    if (!gPrefs.begin(kNvsNamespace, true)) {
      Serial.println("Config: NVS open (readonly) failed, using defaults");
      return false;
    }
    for (uint8_t i = 0; i < kCount; ++i) {
      set(i, gPrefs.getInt(kDefs[i].key, kDefs[i].defaultValue));
    }
    gPrefs.end();
    loadStringsFromNvs();
    clearLegacyNetworkFromNvs();
    migrateFlipDelayDefault(values_);
    applyTimezone();
    DBG("Config: loaded from NVS\n");
    return true;
  }
  
  bool Config::saveToNvs() {
    if (!gPrefs.begin(kNvsNamespace, false)) {
      Serial.println("Config: NVS open (readwrite) failed");
      return false;
    }
    for (uint8_t i = 0; i < kCount; ++i) {
      gPrefs.putInt(kDefs[i].key, values_[i]);
    }
    gPrefs.end();
    saveStringsToNvs();
    DBG("Config: saved to NVS\n");
    return true;
  }

  namespace {
  // NVS key helpers. Keys cap at 15 chars, so the templates below
  // (`ui_wifi_ssid_<n>`, `ui_wifi_pwd_<n>`) work for n=0..9.
  void formatWifiSlotKey(char* out, size_t cap, const char* prefix,
                         uint8_t index) {
    snprintf(out, cap, "%s%u", prefix, static_cast<unsigned>(index));
  }
  }  // namespace

  void Config::loadNetworkFromBuild() {
    // Build-time credentials always seed slot 0; runtime values from
    // NVS overwrite them in `loadStringsFromNvs()`.
    BuildWifiConfig::decodeSsid(wifiSlots_[0].ssid, kStringMaxLen);
    BuildWifiConfig::decodePass(wifiSlots_[0].pass, kStringMaxLen);
    for (uint8_t i = 1; i < kMaxWifiNetworks; ++i) {
      wifiSlots_[i].ssid[0] = '\0';
      wifiSlots_[i].pass[0] = '\0';
    }
  }

  void Config::persistWifiSlot(uint8_t index) const {
    if (index >= kMaxWifiNetworks) return;
    Preferences p;
    if (!p.begin(kNvsNamespace, false)) return;
    char ssidKey[16];
    char pwdKey[16];
    formatWifiSlotKey(ssidKey, sizeof(ssidKey), "ui_wifi_ssid_", index);
    formatWifiSlotKey(pwdKey, sizeof(pwdKey), "ui_wifi_pwd_", index);

    const WifiSlot& slot = wifiSlots_[index];
    const size_t slen = strlen(slot.ssid);
    if (slen == 0) {
      if (p.isKey(ssidKey)) p.remove(ssidKey);
      if (p.isKey(pwdKey))  p.remove(pwdKey);
    } else {
      p.putString(ssidKey, slot.ssid);
      const size_t plen = strlen(slot.pass);
      if (plen == 0) {
        if (p.isKey(pwdKey)) p.remove(pwdKey);
      } else {
        uint8_t scrambled[kStringMaxLen];
        if (scrambleWifiPwd(slot.pass, scrambled, plen)) {
          p.putBytes(pwdKey, scrambled, plen);
        } else {
          // UID not ready — leave the existing NVS blob alone rather
          // than overwrite it with a key that won't round-trip.
          if (p.isKey(pwdKey)) {
            Serial.println(
                "[Config] preserving existing WiFi pwd blob (UID not ready)");
          }
        }
      }
    }
    p.end();
  }

  void Config::clearWifiSlotInNvs(uint8_t index) const {
    if (index >= kMaxWifiNetworks) return;
    Preferences p;
    if (!p.begin(kNvsNamespace, false)) return;
    char ssidKey[16];
    char pwdKey[16];
    formatWifiSlotKey(ssidKey, sizeof(ssidKey), "ui_wifi_ssid_", index);
    formatWifiSlotKey(pwdKey, sizeof(pwdKey), "ui_wifi_pwd_", index);
    if (p.isKey(ssidKey)) p.remove(ssidKey);
    if (p.isKey(pwdKey))  p.remove(pwdKey);
    p.end();
  }

  void Config::setWifiCredentials(const char* ssid, const char* pass) {
    setWifiCredentialsAt(0, ssid, pass);
  }

  void Config::setWifiCredentialsAt(uint8_t index, const char* ssid,
                                    const char* pass) {
    if (index >= kMaxWifiNetworks) return;
    WifiSlot& slot = wifiSlots_[index];
    if (ssid) {
      strncpy(slot.ssid, ssid, kStringMaxLen - 1);
      slot.ssid[kStringMaxLen - 1] = '\0';
      // Empty SSID means "clear the slot" — also drop the password so
      // we don't leak credentials into a "phantom" disabled slot.
      if (slot.ssid[0] == '\0') slot.pass[0] = '\0';
    }
    if (pass) {
      strncpy(slot.pass, pass, kStringMaxLen - 1);
      slot.pass[kStringMaxLen - 1] = '\0';
    }
    persistWifiSlot(index);
  }

  int8_t Config::addWifiNetwork(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0]) return -1;
    // Reuse the existing slot for the same SSID — keeps the order
    // stable for users updating just the password.
    for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
      if (wifiSlots_[i].ssid[0] != '\0' &&
          strcmp(wifiSlots_[i].ssid, ssid) == 0) {
        setWifiCredentialsAt(i, ssid, pass);
        return static_cast<int8_t>(i);
      }
    }
    for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
      if (wifiSlots_[i].ssid[0] == '\0') {
        setWifiCredentialsAt(i, ssid, pass);
        return static_cast<int8_t>(i);
      }
    }
    return -1;
  }

  void Config::removeWifiNetwork(uint8_t index) {
    if (index >= kMaxWifiNetworks) return;
    // Shift later slots up so the saved-network list stays gap-free —
    // the connect path iterates from slot 0 and stops at the first
    // empty SSID.
    for (uint8_t i = index; i + 1 < kMaxWifiNetworks; ++i) {
      wifiSlots_[i] = wifiSlots_[i + 1];
    }
    wifiSlots_[kMaxWifiNetworks - 1].ssid[0] = '\0';
    wifiSlots_[kMaxWifiNetworks - 1].pass[0] = '\0';
    for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
      persistWifiSlot(i);
    }
  }

  void Config::moveWifiNetworkUp(uint8_t index) {
    if (index == 0 || index >= kMaxWifiNetworks) return;
    WifiSlot tmp = wifiSlots_[index - 1];
    wifiSlots_[index - 1] = wifiSlots_[index];
    wifiSlots_[index] = tmp;
    persistWifiSlot(index - 1);
    persistWifiSlot(index);
  }

  void Config::moveWifiNetworkDown(uint8_t index) {
    if (index + 1 >= kMaxWifiNetworks) return;
    WifiSlot tmp = wifiSlots_[index + 1];
    wifiSlots_[index + 1] = wifiSlots_[index];
    wifiSlots_[index] = tmp;
    persistWifiSlot(index);
    persistWifiSlot(index + 1);
  }

  const char* Config::wifiSsidAt(uint8_t index) const {
    if (index >= kMaxWifiNetworks) return "";
    return wifiSlots_[index].ssid;
  }

  const char* Config::wifiPassAt(uint8_t index) const {
    if (index >= kMaxWifiNetworks) return "";
    return wifiSlots_[index].pass;
  }

  bool Config::wifiSlotConfigured(uint8_t index) const {
    if (index >= kMaxWifiNetworks) return false;
    return wifiSlots_[index].ssid[0] != '\0' &&
           wifiSlots_[index].pass[0] != '\0';
  }

  uint8_t Config::wifiNetworkCount() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
      if (wifiSlots_[i].ssid[0] != '\0') ++n;
    }
    return n;
  }

  bool Config::wifiEnabled() const {
    if (kWifiEnabled >= kCount) return wifiConfigured();
    return values_[kWifiEnabled] != 0 && wifiConfigured();
  }

  bool Config::setManualTime(int hour24, int minute) {
    if (hour24 < 0 || hour24 > 23 || minute < 0 || minute > 59) return false;

    // Build a tm in local time. Use the existing wall-clock date if the
    // RTC has any valid value; otherwise fall back to a sane stand-in
    // so the badge boots with a believable date.
    time_t now = time(nullptr);
    struct tm local = {};
    if (now > 1700000000) {
      localtime_r(&now, &local);
    } else {
      local.tm_year = 2026 - 1900;
      local.tm_mon  = 0;   // Jan
      local.tm_mday = 1;
    }
    local.tm_hour = hour24;
    local.tm_min  = minute;
    local.tm_sec  = 0;
    local.tm_isdst = -1;   // let the resolver decide

    applyTimezone();       // ensure TZ is set so mktime() reads as local
    const time_t epoch = mktime(&local);
    if (epoch <= 0) return false;
    struct timeval tv = {epoch, 0};
    return settimeofday(&tv, nullptr) == 0;
  }

  void Config::setTimezone(const char* value) {
    if (!value || !value[0]) {
      strncpy(timezone_, kDefaultTimezone, sizeof(timezone_) - 1);
      timezone_[sizeof(timezone_) - 1] = '\0';
      return;
    }

    char tmp[sizeof(timezone_)];
    strncpy(tmp, value, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    trimInPlace(tmp);
    if (tmp[0] == '\0') {
      strncpy(timezone_, kDefaultTimezone, sizeof(timezone_) - 1);
    } else {
      strncpy(timezone_, tmp, sizeof(timezone_) - 1);
    }
    timezone_[sizeof(timezone_) - 1] = '\0';
  }

  void Config::applyTimezone() const {
    const char* tz = timezone_[0] ? timezone_ : kDefaultTimezone;
    setenv("TZ", tz, 1);
    tzset();
    DBG("Config: timezone set to %s\n", tz);
  }

  void Config::setOtaManifestUrl(const char* value) {
    if (!value) value = "";
    char tmp[sizeof(otaManifestUrl_)];
    strncpy(tmp, value, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    trimInPlace(tmp);
    strncpy(otaManifestUrl_, tmp, sizeof(otaManifestUrl_) - 1);
    otaManifestUrl_[sizeof(otaManifestUrl_) - 1] = '\0';
  }

  // Belt-and-suspenders: if a stale URL ends up in communityAppsUrl_
  // (a settings.txt write that predates the v1->v2 schema, a jsDelivr
  // URL that 403s past the 50 MB package limit, or a partial parse),
  // patch it on every read. The cost is a few strstr calls per
  // access; AssetRegistry only reads at refresh time so this is not
  // hot.
  const char* Config::communityAppsUrl() const {
    static constexpr const char kV2Url[] = REPO_COMMUNITY_APPS_URL;
    // Auto-upgrade jsDelivr URLs (any path) to GitHub raw — the repo
    // is over jsDelivr's 50 MB free-tier limit and the CDN replies
    // with a 403 "Package size exceeded" HTML page that AssetRegistry
    // can't parse.
    if (strstr(communityAppsUrl_, "cdn.jsdelivr.net")) {
      return kV2Url;
    }
    // Auto-upgrade legacy v1 URL to v2.
    if (strstr(communityAppsUrl_, "Temporal-Replay-26-Badge") &&
        strstr(communityAppsUrl_, "/registry/registry.json")) {
      return kV2Url;
    }
    return communityAppsUrl_;
  }

  // TEMPORARY: ignore whatever is in settings.txt and always use the
  // baked-in default. The settings.txt parser was silently truncating
  // long URLs (line buffer was 80 chars), and we don't want stale
  // values in NVS/disk fighting us while the Community Apps screen
  // is still being stabilised. Pinning to the firmware-baked URL
  // means every boot starts from a known-good state.
  void Config::setCommunityAppsUrl(const char* value) {
    static constexpr const char kDefaultCommunityAppsUrl[] =
        REPO_COMMUNITY_APPS_URL;
    (void)value;
    strncpy(communityAppsUrl_, kDefaultCommunityAppsUrl,
            sizeof(communityAppsUrl_) - 1);
    communityAppsUrl_[sizeof(communityAppsUrl_) - 1] = '\0';
  }

  void Config::setNametagSetting(const char* value) {
    if (!value || !value[0]) {
      strncpy(nametagSetting_, "default", sizeof(nametagSetting_));
      nametagSetting_[sizeof(nametagSetting_) - 1] = '\0';
      return;
    }
    strncpy(nametagSetting_, value, sizeof(nametagSetting_) - 1);
    nametagSetting_[sizeof(nametagSetting_) - 1] = '\0';
  }

  void Config::applyNametagSetting() {
    char aid[draw::kAnimIdLen + 1];
    const draw::NametagSettingParse parse =
        draw::parseNametagSetting(nametagSetting_, aid);

    auto clearNametag = []() {
      gCustomNametagEnabled = false;
      gNametagAnimId[0] = '\0';
      unloadNametagAnimationDoc();
    };

    if (parse == draw::NametagSettingParse::AnimId) {
      auto* doc = new draw::AnimDoc();
      if (draw::load(aid, *doc) && !doc->frames.empty()) {
        adoptNametagAnimationDoc(aid, doc);
        return;
      }
      draw::freeAll(*doc);
      delete doc;
      DBG("[Nametag] settings anim '%s' missing — disabling custom\n", aid);
      clearNametag();
      return;
    }

    if (parse == draw::NametagSettingParse::Invalid) {
      DBG("[Nametag] invalid nametag setting \"%s\"\n", nametagSetting_);
      clearNametag();
      return;
    }

    clearNametag();
  }
  
  void Config::loadStringsFromNvs() {
    loadNetworkFromBuild();
    // UI-entered credentials take precedence over build-baked values
    // when present. Modern layout: per-slot keys
    // `ui_wifi_ssid_<n>` (string) + `ui_wifi_pwd_<n>` (scrambled
    // bytes), one pair per slot 0..kMaxWifiNetworks-1. Legacy single-
    // network layout (`ui_wifi_ssid` + `ui_wifi_pwd`/`ui_wifi_pass`)
    // is migrated forward into slot 0 and the old keys removed.
    bool migratedFromLegacy = false;
    char legacySsid[kStringMaxLen] = "";
    char legacyPass[kStringMaxLen] = "";
    bool sawAnySlot = false;
    {
      Preferences p;
      if (!p.begin(kNvsNamespace, true)) return;
      for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
        char ssidKey[16];
        char pwdKey[16];
        formatWifiSlotKey(ssidKey, sizeof(ssidKey), "ui_wifi_ssid_", i);
        formatWifiSlotKey(pwdKey, sizeof(pwdKey), "ui_wifi_pwd_", i);
        if (p.isKey(ssidKey)) {
          // The slot was explicitly configured (even if empty), so wipe
          // out the build-baked seed for this slot before loading.
          wifiSlots_[i].ssid[0] = '\0';
          wifiSlots_[i].pass[0] = '\0';
          p.getString(ssidKey, wifiSlots_[i].ssid, kStringMaxLen);
          if (p.isKey(pwdKey)) {
            const size_t blen = p.getBytesLength(pwdKey);
            if (blen > 0 && blen < kStringMaxLen) {
              uint8_t buf[kStringMaxLen];
              if (p.getBytes(pwdKey, buf, blen) == blen) {
                unscrambleWifiPwd(buf, wifiSlots_[i].pass, blen);
              }
            }
          }
          sawAnySlot = true;
        }
      }
      // Legacy single-network layout. Migrate into slot 0 if no
      // modern slot is set yet.
      if (!sawAnySlot && p.isKey("ui_wifi_ssid")) {
        p.getString("ui_wifi_ssid", legacySsid, kStringMaxLen);
        if (p.isKey("ui_wifi_pwd")) {
          const size_t blen = p.getBytesLength("ui_wifi_pwd");
          if (blen > 0 && blen < kStringMaxLen) {
            uint8_t buf[kStringMaxLen];
            if (p.getBytes("ui_wifi_pwd", buf, blen) == blen) {
              unscrambleWifiPwd(buf, legacyPass, blen);
            }
          }
        } else if (p.isKey("ui_wifi_pass")) {
          p.getString("ui_wifi_pass", legacyPass, kStringMaxLen);
        }
        migratedFromLegacy = true;
      }
      p.end();
    }

    if (migratedFromLegacy) {
      DBG("Config: migrating legacy WiFi credentials into slot 0\n");
      // setWifiCredentialsAt persists the new slot and the next clean
      // boot will see the modern keys. We then drop the legacy keys
      // explicitly so they don't reappear on subsequent migrations.
      setWifiCredentialsAt(0, legacySsid, legacyPass);
      Preferences p;
      if (p.begin(kNvsNamespace, false)) {
        if (p.isKey("ui_wifi_ssid")) p.remove("ui_wifi_ssid");
        if (p.isKey("ui_wifi_pwd"))  p.remove("ui_wifi_pwd");
        if (p.isKey("ui_wifi_pass")) p.remove("ui_wifi_pass");
        p.end();
      }
    }
  }
  
  void Config::saveStringsToNvs() {
    Preferences p;
    if (!p.begin(kNvsNamespace, false)) return;
    if (p.isKey("wifi_ssid")) p.remove("wifi_ssid");
    if (p.isKey("wifi_pass")) p.remove("wifi_pass");
    if (p.isKey("server_url")) p.remove("server_url");
    if (p.isKey("ep_badge")) p.remove("ep_badge");
    if (p.isKey("ep_boops")) p.remove("ep_boops");
    p.end();
  }

  void Config::clearLegacyNetworkFromNvs() {
    Preferences p;
    if (!p.begin(kNvsNamespace, false)) return;
    bool removed = false;
    if (p.isKey("wifi_ssid")) {
      p.remove("wifi_ssid");
      removed = true;
    }
    if (p.isKey("wifi_pass")) {
      p.remove("wifi_pass");
      removed = true;
    }
    if (p.isKey("server_url")) {
      p.remove("server_url");
      removed = true;
    }
    if (p.isKey("ep_badge")) {
      p.remove("ep_badge");
      removed = true;
    }
    if (p.isKey("ep_boops")) {
      p.remove("ep_boops");
      removed = true;
    }
    p.end();
    if (removed) {
      DBG("Config: removed legacy network secrets from NVS\n");
    }
  }
  
  // ─── File-based INI parsing ──────────────────────────────────────────────────
  
  bool Config::parseSettingsFile(const char* buf, uint16_t len) {
    // 80 was too small — URL-valued keys (asset_registry_url,
    // ota.manifest_url) routinely exceed 80 chars and were getting
    // silently truncated mid-host, which made the legacy-URL migration
    // and the URL itself useless. Sized to fit the longest URL field
    // (assetRegistryUrl_[160]) plus key, equals, and slack.
    char line[256];
    const char* p = buf;
    const char* end = buf + len;
    char section[24] = "";
    uint8_t matched = 0;
  
    while (p < end) {
      const char* lineEnd = p;
      while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
      size_t lineLen = static_cast<size_t>(lineEnd - p);
      if (lineLen >= sizeof(line)) lineLen = sizeof(line) - 1;
      memcpy(line, p, lineLen);
      line[lineLen] = '\0';
      p = (lineEnd < end) ? lineEnd + 1 : end;
  
      trimInPlace(line);
      if (line[0] == '\0' || line[0] == '#' || (line[0] == '/' && line[1] == '/')) continue;
  
      if (line[0] == '[') {
        char* rb = strchr(line, ']');
        if (rb) {
          *rb = '\0';
          strncpy(section, line + 1, sizeof(section) - 1);
          section[sizeof(section) - 1] = '\0';
        }
        continue;
      }
  
      char* eq = strchr(line, '=');
      if (!eq) continue;
      *eq = '\0';
      char* key = line;
      char* val = eq + 1;
      trimInPlace(key);
      trimInPlace(val);
      size_t valLen = strlen(val);
      if (valLen > 0 && val[valLen - 1] == ';') val[valLen - 1] = '\0';
      trimInPlace(val);
  
      if (strcmp(section, "nametag") == 0) {
        if (strcmp(key, "nametag") == 0) {
          strncpy(nametagSetting_, val, sizeof(nametagSetting_) - 1);
          nametagSetting_[sizeof(nametagSetting_) - 1] = '\0';
          matched++;
        }
        continue;
      }

      if (strcmp(section, "time") == 0) {
        if (strcmp(key, "timezone") == 0) {
          setTimezone(val);
          matched++;
        }
        continue;
      }

      if (strcmp(section, "ota") == 0) {
        if (strcmp(key, "manifest_url") == 0) {
          setOtaManifestUrl(val);
          matched++;
        } else if (strcmp(key, "community_apps_url") == 0 ||
                   strcmp(key, "asset_registry_url") == 0) {
          // Legacy `asset_registry_url` is read transparently so old
          // settings.txt files keep working — both names map to the
          // same field.
          setCommunityAppsUrl(val);
          matched++;
        }
        continue;
      }

      if (strcmp(section, "wifi") == 0) {
        // Legacy network-secret keys are intentionally ignored — they
        // used to live in this file and the parser just drops them on
        // the floor (the legacy-cleanup pass rewrites the file).
        if (strcmp(key, "ssid") == 0 || strcmp(key, "pass") == 0 ||
            strcmp(key, "server") == 0 ||
            strcmp(key, "ep_badge") == 0 || strcmp(key, "ep_boops") == 0) {
          continue;
        }
        // Fall through so kDefs lookup can match `wifi_en` (and any
        // future per-section toggle) below.
      }
  
      for (uint8_t i = 0; i < kCount; ++i) {
        if (strcmp(key, kDefs[i].key) == 0) {
          int32_t v;
          if (i == kFontFamily) {
            int8_t idx = fontFamilyFromName(val);
            v = (idx >= 0) ? idx : static_cast<int32_t>(atoi(val));
          } else {
            v = static_cast<int32_t>(atoi(val));
          }
          set(i, v);
          matched++;
          break;
        }
      }
    }
  
    DBG("Config: parsed %u settings from file\n", matched);
    return matched > 0;
  }
  
  // ─── File-based INI formatting ───────────────────────────────────────────────
  //
  // Produces a human-readable settings.txt like:
  //
  //   # Replay Badge Settings
  //   # Edit values and reboot to apply.
  //   # Lines starting with # are comments.
  //
  //   [display]
  //   led_bright = 10;        # 1..254
  //
  //   [haptics]
  //   haptic_str = 155;       # 0..255
  //   ...
  
  uint16_t Config::formatSettingsFile(char* buf, uint16_t bufSize) const {
    int pos = 0;
    auto room = [&]() -> size_t {
      return (pos < bufSize) ? static_cast<size_t>(bufSize - pos) : 0;
    };

  

    pos += snprintf(buf + pos, room(),
        "# Replay Badge Settings\n\n");
  
    pos += snprintf(buf + pos, room(), "[display]\n");
    pos += snprintf(buf + pos, room(), "led_bright = %ld;        # %ld..%ld\n",
        (long)values_[kLedBrightness], (long)kDefs[kLedBrightness].minValue, (long)kDefs[kLedBrightness].maxValue);
    pos += snprintf(buf + pos, room(), "\n");

  
    pos += snprintf(buf + pos, room(), "[haptics]\n");
    pos += snprintf(buf + pos, room(), "haptic_en = %ld;        # 0=off, 1=on\n",
        (long)values_[kHapticEnabled]);
    pos += snprintf(buf + pos, room(), "haptic_str = %ld;       # %ld..%ld\n",
        (long)values_[kHapticStrength], (long)kDefs[kHapticStrength].minValue, (long)kDefs[kHapticStrength].maxValue);
    pos += snprintf(buf + pos, room(), "haptic_freq = %ld;       # %ld..%ld Hz\n",
        (long)values_[kHapticFreqHz], (long)kDefs[kHapticFreqHz].minValue, (long)kDefs[kHapticFreqHz].maxValue);
    pos += snprintf(buf + pos, room(), "haptic_ms = %ld;         # %ld..%ld ms\n\n",
        (long)values_[kHapticPulseMs], (long)kDefs[kHapticPulseMs].minValue, (long)kDefs[kHapticPulseMs].maxValue);
  
    pos += snprintf(buf + pos, room(), "[input]\n");
    pos += snprintf(buf + pos, room(), "joy_sens = %ld;          # %ld..%ld %%\n",
        (long)values_[kJoySensitivity], (long)kDefs[kJoySensitivity].minValue, (long)kDefs[kJoySensitivity].maxValue);
    pos += snprintf(buf + pos, room(), "joy_dead = %ld;           # %ld..%ld %%\n",
        (long)values_[kJoyDeadzone], (long)kDefs[kJoyDeadzone].minValue, (long)kDefs[kJoyDeadzone].maxValue);
    pos += snprintf(buf + pos, room(), "swap_ok = %ld;            # 0=normal confirm/cancel, 1=swapped\n\n",
        (long)values_[kSwapConfirmCancel]);

  
    pos += snprintf(buf + pos, room(), "[power]\n");
    pos += snprintf(buf + pos, room(), "lsleep_s = %ld;          # %ld..%ld seconds until light sleep\n",
        (long)values_[kLightSleepSec], (long)kDefs[kLightSleepSec].minValue, (long)kDefs[kLightSleepSec].maxValue);
    pos += snprintf(buf + pos, room(), "dsleep_s = %ld;          # %ld..%ld seconds until deep sleep\n\n",
        (long)values_[kDeepSleepSec], (long)kDefs[kDeepSleepSec].minValue, (long)kDefs[kDeepSleepSec].maxValue);
  
    pos += snprintf(buf + pos, room(), "[orientation]\n");
    pos += snprintf(buf + pos, room(), "# Show nametag mode when badge is rotated 180 degrees\n");
    pos += snprintf(buf + pos, room(), "# tiltY: -1000 mG = upright, +1000 mG = upside-down\n");
    pos += snprintf(buf + pos, room(), "flip_en = %ld;            # 0=off, 1=on\n",
        (long)values_[kAutoFlipEnable]);
    pos += snprintf(buf + pos, room(), "flip_up = %ld;         # %ld..%ld mG threshold to un-flip\n",
        (long)values_[kFlipUpThreshold], (long)kDefs[kFlipUpThreshold].minValue, (long)kDefs[kFlipUpThreshold].maxValue);
    pos += snprintf(buf + pos, room(), "flip_dn = %ld;          # %ld..%ld mG threshold to flip\n",
        (long)values_[kFlipDownThreshold], (long)kDefs[kFlipDownThreshold].minValue, (long)kDefs[kFlipDownThreshold].maxValue);
    pos += snprintf(buf + pos, room(), "flip_ms = %ld;            # %ld..%ld ms hold before flip\n\n",
        (long)values_[kFlipDelayMs], (long)kDefs[kFlipDelayMs].minValue, (long)kDefs[kFlipDelayMs].maxValue);

    pos += snprintf(buf + pos, room(), "[oled]\n");
    pos += snprintf(buf + pos, room(), "# Display timing (SSD1309) - tune to reduce tearing\n");
    pos += snprintf(buf + pos, room(), "# Frame rate = Fosc / (D * K * MUX), K=pre1+pre2+65\n");
    pos += snprintf(buf + pos, room(), "oled_osc = %ld;          # %ld..%ld oscillator frequency\n",
        (long)values_[kOledOsc], (long)kDefs[kOledOsc].minValue, (long)kDefs[kOledOsc].maxValue);
    pos += snprintf(buf + pos, room(), "oled_div = %ld;           # %ld..%ld clock divide ratio\n",
        (long)values_[kOledDiv], (long)kDefs[kOledDiv].minValue, (long)kDefs[kOledDiv].maxValue);
    pos += snprintf(buf + pos, room(), "oled_mux = %ld;          # %ld..%ld multiplex ratio\n",
        (long)values_[kOledMux], (long)kDefs[kOledMux].minValue, (long)kDefs[kOledMux].maxValue);
    pos += snprintf(buf + pos, room(), "oled_pre1 = %ld;          # %ld..%ld phase 1 precharge DCLKs\n",
        (long)values_[kOledPrecharge1], (long)kDefs[kOledPrecharge1].minValue, (long)kDefs[kOledPrecharge1].maxValue);
    pos += snprintf(buf + pos, room(), "oled_pre2 = %ld;         # %ld..%ld phase 2 precharge DCLKs\n",
        (long)values_[kOledPrecharge2], (long)kDefs[kOledPrecharge2].minValue, (long)kDefs[kOledPrecharge2].maxValue);
    pos += snprintf(buf + pos, room(), "oled_ctr = %ld;         # %ld..%ld contrast (0x81)\n\n",
        (long)values_[kOledContrast], (long)kDefs[kOledContrast].minValue, (long)kDefs[kOledContrast].maxValue);

    pos += snprintf(buf + pos, room(), "[imu]\n");
    pos += snprintf(buf + pos, room(), "# EMA smoothing as percent (88 = 0.88 alpha)\n");
    pos += snprintf(buf + pos, room(), "imu_smooth = %ld;     # %ld..%ld %%\n",
        (long)values_[kImuSmoothing], (long)kDefs[kImuSmoothing].minValue, (long)kDefs[kImuSmoothing].maxValue);
    pos += snprintf(buf + pos, room(), "imu_thr = %ld;        # %ld..%ld INT1 wake threshold\n",
        (long)values_[kImuInt1Threshold], (long)kDefs[kImuInt1Threshold].minValue, (long)kDefs[kImuInt1Threshold].maxValue);
    pos += snprintf(buf + pos, room(), "imu_dur = %ld;        # %ld..%ld INT1 duration\n\n",
        (long)values_[kImuInt1Duration], (long)kDefs[kImuInt1Duration].minValue, (long)kDefs[kImuInt1Duration].maxValue);

    pos += snprintf(buf + pos, room(), "[buttons]\n");
    pos += snprintf(buf + pos, room(), "btn_dbnc = %ld;       # %ld..%ld ms debounce\n",
        (long)values_[kBtnDebounceMs], (long)kDefs[kBtnDebounceMs].minValue, (long)kDefs[kBtnDebounceMs].maxValue);
    pos += snprintf(buf + pos, room(), "rpt_init = %ld;      # %ld..%ld ms initial repeat delay\n",
        (long)values_[kRptInitialDelayMs], (long)kDefs[kRptInitialDelayMs].minValue, (long)kDefs[kRptInitialDelayMs].maxValue);
    pos += snprintf(buf + pos, room(), "rpt_int1 = %ld;      # %ld..%ld ms first repeat interval\n",
        (long)values_[kRptFirstIntervalMs], (long)kDefs[kRptFirstIntervalMs].minValue, (long)kDefs[kRptFirstIntervalMs].maxValue);
    pos += snprintf(buf + pos, room(), "rpt_dly2 = %ld;      # %ld..%ld ms second repeat delay\n",
        (long)values_[kRptSecondDelayMs], (long)kDefs[kRptSecondDelayMs].minValue, (long)kDefs[kRptSecondDelayMs].maxValue);
    pos += snprintf(buf + pos, room(), "rpt_int2 = %ld;       # %ld..%ld ms second repeat interval\n\n",
        (long)values_[kRptSecondIntervalMs], (long)kDefs[kRptSecondIntervalMs].minValue, (long)kDefs[kRptSecondIntervalMs].maxValue);

    pos += snprintf(buf + pos, room(), "[timing]\n");
    pos += snprintf(buf + pos, room(), "# Scheduler and loop pacing — affects power & responsiveness\n");
    pos += snprintf(buf + pos, room(), "led_intv = %ld;       # %ld..%ld ms LED service interval\n",
        (long)values_[kLedServiceMs], (long)kDefs[kLedServiceMs].minValue, (long)kDefs[kLedServiceMs].maxValue);
    pos += snprintf(buf + pos, room(), "oled_rfsh = %ld;     # %ld..%ld ms OLED refresh interval\n",
        (long)values_[kOledRefreshMs], (long)kDefs[kOledRefreshMs].minValue, (long)kDefs[kOledRefreshMs].maxValue);
    pos += snprintf(buf + pos, room(), "joy_poll = %ld;       # %ld..%ld ms joystick poll interval\n",
        (long)values_[kJoyPollMs], (long)kDefs[kJoyPollMs].minValue, (long)kDefs[kJoyPollMs].maxValue);
    pos += snprintf(buf + pos, room(), "sch_high = %ld;       # %ld..%ld scheduler high-priority divisor\n",
        (long)values_[kSchHighDiv], (long)kDefs[kSchHighDiv].minValue, (long)kDefs[kSchHighDiv].maxValue);
    pos += snprintf(buf + pos, room(), "sch_norm = %ld;       # %ld..%ld scheduler normal-priority divisor\n",
        (long)values_[kSchNormDiv], (long)kDefs[kSchNormDiv].minValue, (long)kDefs[kSchNormDiv].maxValue);
    pos += snprintf(buf + pos, room(), "sch_low = %ld;       # %ld..%ld scheduler low-priority divisor\n",
        (long)values_[kSchLowDiv], (long)kDefs[kSchLowDiv].minValue, (long)kDefs[kSchLowDiv].maxValue);
    pos += snprintf(buf + pos, room(), "loop_ms = %ld;        # %ld..%ld ms main loop delay\n",
        (long)values_[kLoopDelayMs], (long)kDefs[kLoopDelayMs].minValue, (long)kDefs[kLoopDelayMs].maxValue);
    pos += snprintf(buf + pos, room(), "cpu_idle = %ld;       # MHz CPU idle frequency (20, 40, 80, 160, 240)\n",
        (long)values_[kCpuIdleMhz]);
    pos += snprintf(buf + pos, room(), "cpu_actv = %ld;      # MHz CPU active frequency (20, 40, 80, 160, 240)\n\n",
        (long)values_[kCpuActiveMhz]);

    pos += snprintf(buf + pos, room(), "[nametag]\n");
    pos += snprintf(buf + pos, room(),
        "# default = built-in text nametag; or 8-char composer hex id (folder under /composer)\n");
    pos += snprintf(buf + pos, room(),
        "# Example: nametag = a1b2c3d4   or   nametag = /composer/a1b2c3d4/info.json\n");
    pos += snprintf(buf + pos, room(), "nametag = %s\n\n", nametagSetting_);

    pos += snprintf(buf + pos, room(), "[time]\n");
    pos += snprintf(buf + pos, room(), "# POSIX TZ string. Default is Pacific time with DST.\n");
    pos += snprintf(buf + pos, room(), "timezone = %s\n\n", timezone_[0] ? timezone_ : kDefaultTimezone);

    pos += snprintf(buf + pos, room(), "[ota]\n");
    pos += snprintf(buf + pos, room(),
        "# Firmware OTA: leave empty to use the default GitHub Releases\n");
    pos += snprintf(buf + pos, room(),
        "# endpoint baked into the build. Set to override (advanced).\n");
    pos += snprintf(buf + pos, room(), "manifest_url = %s\n", otaManifestUrl_);
    pos += snprintf(buf + pos, room(),
        "# Community Apps: URL of a registry JSON describing installable\n");
    pos += snprintf(buf + pos, room(),
        "# apps + assets (DOOM WAD, etc.). Empty disables the Community\n");
    pos += snprintf(buf + pos, room(),
        "# Apps screen. (Legacy alias: asset_registry_url.)\n");
    pos += snprintf(buf + pos, room(),
        "community_apps_url = %s\n\n", communityAppsUrl_);

    pos += snprintf(buf + pos, room(), "[wifi]\n");
    pos += snprintf(buf + pos, room(), "# Master WiFi enable. When 0, the badge never connects on boot and\n");
    pos += snprintf(buf + pos, room(), "# explicit connect attempts (incl. badge.http_get/post) bail out.\n");
    pos += snprintf(buf + pos, room(), "# When 1 *and* an SSID/password are configured (via Settings ->\n");
    pos += snprintf(buf + pos, room(), "# WiFi), the badge will auto-connect once at boot and reuse the\n");
    pos += snprintf(buf + pos, room(), "# connection for any subsequent network calls. Credentials are\n");
    pos += snprintf(buf + pos, room(), "# stored in NVS (password obfuscated per-device); we never write\n");
    pos += snprintf(buf + pos, room(), "# SSID/password to this file.\n");
    pos += snprintf(buf + pos, room(), "wifi_en = %ld;          # 0=off, 1=on\n\n",
        (long)values_[kWifiEnabled]);

  pos += snprintf(buf + pos, room(), "[boop]\n");
  pos += snprintf(buf + pos, room(), "# IR identity exchange during offline boops\n");
  pos += snprintf(buf + pos, room(), "boop_ir_info = %ld;   # 0=off, 1=exchange identity over IR when offline\n",
      (long)values_[kBoopIrInfo]);
  pos += snprintf(buf + pos, room(), "boop_fields = %ld;   # 0x%03lX bitmask of field tags to transmit (bits 0..8)\n",
      (long)values_[kBoopInfoFields], (long)values_[kBoopInfoFields]);
  pos += snprintf(buf + pos, room(), "ir_tx_pw = %ld;       # %ld..%ld %% TX power\n\n",
      (long)values_[kIrTxPowerPct], (long)kDefs[kIrTxPowerPct].minValue, (long)kDefs[kIrTxPowerPct].maxValue);

  pos += snprintf(buf + pos, room(), "[log]\n");
  pos += snprintf(buf + pos, room(), "# Per-category serial debug spam. 0=quiet (default), 1=verbose.\n");
  pos += snprintf(buf + pos, room(), "# Boot / init / error logs are always on regardless.\n");
  pos += snprintf(buf + pos, room(), "log_ir     = %ld;       # BadgeIR frame-level events\n",
      (long)values_[kLogIr]);
  pos += snprintf(buf + pos, room(), "log_boop   = %ld;       # BadgeBoops + BoopFeedback (beacon/retx/field TX+RX)\n",
      (long)values_[kLogBoop]);
  pos += snprintf(buf + pos, room(), "log_notify = %ld;       # legacy notification debug gate (unused)\n",
      (long)values_[kLogNotify]);
  pos += snprintf(buf + pos, room(), "log_zigmoji = %ld;      # legacy zigmoji debug gate (unused)\n",
      (long)values_[kLogZigmoji]);
  pos += snprintf(buf + pos, room(), "log_imu    = %ld;       # IMU samples, thresholds, and flip transitions\n",
      (long)values_[kLogImu]);

    if (pos >= bufSize) {
      Serial.printf("Config: settings file truncated (%d > %u)\n", pos, bufSize);
      pos = bufSize - 1;
      buf[pos] = '\0';
    }
    return static_cast<uint16_t>(pos);
  }
  
  // ─── File I/O via oofatfs ────────────────────────────────────────────────────
  
  bool Config::loadFromFile() {
    Filesystem::IOLock fsLock;  // serialise vs other journal writers
    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) {
      Serial.println("Config: FAT not mounted, cannot load settings file");
      return false;
    }

    FIL fil;
    FRESULT res = f_open(fs, &fil, kSettingsPath, FA_READ);
    if (res != FR_OK) {
      Serial.println("Config: settings.txt not found, creating with defaults");
      saveToFile();
      return false;
    }

    char* buf = static_cast<char*>(BadgeMemory::allocPreferPsram(kSettingsBufSize));
    if (buf == nullptr) {
      Serial.println("Config: malloc failed for settings read");
      f_close(&fil);
      return false;
    }
  
    UINT bytesRead = 0;
    f_read(&fil, buf, kSettingsBufSize - 1, &bytesRead);
    f_close(&fil);
    buf[bytesRead] = '\0';
  
    const uint16_t settingsLen = static_cast<uint16_t>(bytesRead);
    const bool hadLegacyNetworkSecrets =
        settingsContainsLegacyNetworkSecrets(buf, settingsLen);
    const bool hadTimezone = settingsContainsTimezone(buf, settingsLen);
    bool ok = parseSettingsFile(buf, settingsLen);
    free(buf);
  
    // Always snapshot the on-disk stat after a successful read, even
    // when no recognized settings landed (e.g. an empty file or one
    // that only contained legacy keys we now drop). Without this the
    // watcher's lastFileSize_/Date/Time stay at their constructor
    // zeros and `checkFileChanged()` returns true on every poll,
    // spamming "settings.txt changed, reloading" forever.
    snapshotFileStat();

    if (ok) {
      const bool migratedFlipDelay =
          (values_[kFlipDelayMs] == 0 || values_[kFlipDelayMs] == 3000);
      migrateFlipDelayDefault(values_);
      applyTimezone();
      DBG("Config: loaded from settings.txt\n");
      DBG("Config: community_apps_url = %s (firmware default; "
          "settings.txt value ignored for now)\n",
          communityAppsUrl_);
      if (hadLegacyNetworkSecrets) {
        DBG("Config: removing legacy network secrets from settings.txt\n");
        saveToFile();
      } else if (!hadTimezone) {
        DBG("Config: adding default timezone to settings.txt\n");
        saveToFile();
      } else if (migratedFlipDelay) {
        DBG("Config: migrated flip_ms default to 100\n");
        saveToFile();
      }
    } else if (hadLegacyNetworkSecrets) {
      DBG("Config: replacing legacy network-only settings.txt\n");
      saveToFile();
    }
    return ok;
  }
  
  bool Config::saveToFile() {
    Filesystem::IOLock fsLock;  // serialise vs other journal writers
    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) {
      Serial.println("Config: FAT not mounted, falling back to NVS");
      return saveToNvs();
    }
  
    char* buf = static_cast<char*>(BadgeMemory::allocPreferPsram(kSettingsBufSize));
    if (buf == nullptr) {
      Serial.println("Config: malloc failed for settings write");
      return false;
    }
  
    uint16_t len = formatSettingsFile(buf, kSettingsBufSize);
  
    f_unlink(fs, kSettingsPath);
  
    FIL fil;
    FRESULT res = f_open(fs, &fil, kSettingsPath, FA_WRITE | FA_CREATE_NEW);
    if (res != FR_OK) {
      Serial.printf("Config: f_open for write failed (%d)\n", res);
      free(buf);
      return false;
    }
  
    UINT bytesWritten = 0;
    res = f_write(&fil, buf, len, &bytesWritten);
    f_sync(&fil);
    f_close(&fil);
    free(buf);
  
    if (res != FR_OK || bytesWritten != len) {
      Serial.printf("Config: write failed (res=%d wrote=%u/%u)\n", res, bytesWritten, len);
      return false;
    }
  
    snapshotFileStat();
    DBG("Config: saved to settings.txt\n");
    saveToNvs();
    return true;
  }
  
  // ─── Unified load/save (used by the rest of the codebase) ────────────────────
  
  bool Config::load() {
    loadFromNvs();
    return true;
  }
  
  bool Config::save() {
    FATFS* fs = replay_get_fatfs();
    if (fs != nullptr) {
      return saveToFile();
    }
    return saveToNvs();
  }
  
  // ─── Accessors & hardware apply ──────────────────────────────────────────────

  bool Config::wifiConfigured() const {
    for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
      if (wifiSlots_[i].ssid[0] != '\0' && wifiSlots_[i].pass[0] != '\0') {
        return true;
      }
    }
    return false;
  }

  int32_t Config::get(uint8_t index) const {
    if (index >= kCount || index >= kMaxSettings) {
      return 0;
    }
    return values_[index];
  }
  
  void Config::set(uint8_t index, int32_t value) {
    if (index >= kCount || index >= kMaxSettings) {
      return;
    }
    if (isCpuMhzIndex(index)) {
      value = kCpuValidMhz[nearestCpuMhzSlot(value)];
    } else {
      if (value < kDefs[index].minValue) {
        value = kDefs[index].minValue;
      }
      if (value > kDefs[index].maxValue) {
        value = kDefs[index].maxValue;
      }
    }
    values_[index] = value;
  }

  int32_t Config::nextValue(uint8_t index, int32_t current, int8_t dir,
                            uint8_t magnitude) {
    if (index >= kCount || dir == 0 || magnitude == 0) {
      return current;
    }
    if (isCpuMhzIndex(index)) {
      int16_t slot = static_cast<int16_t>(nearestCpuMhzSlot(current));
      slot += (dir > 0 ? 1 : -1) * static_cast<int16_t>(magnitude);
      if (slot < 0) slot = 0;
      if (slot >= static_cast<int16_t>(kCpuValidMhzCount)) {
        slot = static_cast<int16_t>(kCpuValidMhzCount - 1);
      }
      return kCpuValidMhz[slot];
    }
    int32_t result = current + static_cast<int32_t>(dir) *
                                   static_cast<int32_t>(magnitude) *
                                   kDefs[index].step;
    if (result < kDefs[index].minValue) result = kDefs[index].minValue;
    if (result > kDefs[index].maxValue) result = kDefs[index].maxValue;
    return result;
  }
  
  void Config::apply(uint8_t index) {
    if (index >= kCount || index >= kMaxSettings) {
      return;
    }
    const int32_t v = values_[index];
    switch (index) {
      case kLedBrightness:
        badgeMatrix.setBrightness(static_cast<uint8_t>(v));
        break;
      case kHapticEnabled:
        Haptics::setEnabled(v != 0);
        break;
      case kHapticStrength:
        Haptics::setStrength(static_cast<uint8_t>(v));
        break;
      case kHapticFreqHz:
        Haptics::setPwmFrequency(static_cast<uint32_t>(v));
        break;
      case kHapticPulseMs:
        Haptics::setClickPulseDuration(static_cast<uint16_t>(v));
        break;
      case kJoySensitivity:
        inputs.setJoystickSensitivityPercent(static_cast<uint8_t>(v));
        break;
      case kJoyDeadzone:
        inputs.setJoystickDeadzonePercent(static_cast<uint8_t>(v));
        break;
      case kSwapConfirmCancel:
        inputs.setConfirmCancelSwapped(v != 0);
        break;
      case kFontFamily:
        badgeDisplay.setFontPreset(FONT_TINY);
        break;
      case kFontSize:
        badgeDisplay.setFontPreset(FONT_TINY);
        break;
      case kLightSleepSec:
        sleepService.setLightSleepMs(static_cast<uint32_t>(v) * 1000UL);
        break;
      case kDeepSleepSec:
        sleepService.setDeepSleepMs(static_cast<uint32_t>(v) * 1000UL);
        break;
      case kAutoFlipEnable:
      case kFlipUpThreshold:
      case kFlipDownThreshold:
      case kFlipDelayMs:
        imu.setFlipConfig(
            values_[kAutoFlipEnable] != 0,
            static_cast<float>(values_[kFlipUpThreshold]),
            static_cast<float>(values_[kFlipDownThreshold]),
            static_cast<uint32_t>(values_[kFlipDelayMs]));
        break;
      case kFlipButtons:
        inputs.setFlipButtons(values_[kFlipButtons] != 0);
        break;
      case kFlipJoystick:
        inputs.setFlipJoystick(values_[kFlipJoystick] != 0);
        break;
      case kOledOsc:
      case kOledDiv:
        badgeDisplay.setClockDivOsc(
            static_cast<uint8_t>(values_[kOledOsc]),
            static_cast<uint8_t>(values_[kOledDiv]));
        break;
      case kOledMux:
        badgeDisplay.setMuxRatio(static_cast<uint8_t>(v));
        break;
      case kOledPrecharge1:
      case kOledPrecharge2:
        badgeDisplay.setPrecharge(
            static_cast<uint8_t>(values_[kOledPrecharge1]),
            static_cast<uint8_t>(values_[kOledPrecharge2]));
        break;
      case kOledContrast:
        badgeDisplay.setContrast(static_cast<uint8_t>(v));
        break;

      case kImuSmoothing:
        imu.setSmoothing(static_cast<uint8_t>(v));
        break;
      case kImuInt1Threshold:
      case kImuInt1Duration:
        imu.setInt1Config(
            static_cast<uint8_t>(values_[kImuInt1Threshold]),
            static_cast<uint8_t>(values_[kImuInt1Duration]));
        break;
      case kBtnDebounceMs:
        inputs.setDebounceMs(static_cast<uint8_t>(v));
        break;
      case kRptInitialDelayMs:
      case kRptFirstIntervalMs:
      case kRptSecondDelayMs:
      case kRptSecondIntervalMs:
        inputs.setRepeatTiming(
            static_cast<uint16_t>(values_[kRptInitialDelayMs]),
            static_cast<uint16_t>(values_[kRptFirstIntervalMs]),
            static_cast<uint16_t>(values_[kRptSecondDelayMs]),
            static_cast<uint16_t>(values_[kRptSecondIntervalMs]));
        break;
      case kLedServiceMs:
        Power::Policy::ledServiceIntervalMs = static_cast<uint16_t>(v);
        break;
      case kOledRefreshMs:
        Power::Policy::oledRefreshMs = static_cast<uint16_t>(v);
        break;
      case kJoyPollMs:
        Power::Policy::joystickPollMs = static_cast<uint16_t>(v);
        break;
      case kSchHighDiv:
      case kSchNormDiv:
      case kSchLowDiv:
        Power::Policy::schedulerHighDivisor = static_cast<uint8_t>(values_[kSchHighDiv]);
        Power::Policy::schedulerNormalDivisor = static_cast<uint8_t>(values_[kSchNormDiv]);
        Power::Policy::schedulerLowDivisor = static_cast<uint8_t>(values_[kSchLowDiv]);
        scheduler.setExecutionDivisors(
            Power::Policy::schedulerHighDivisor,
            Power::Policy::schedulerNormalDivisor,
            Power::Policy::schedulerLowDivisor);
        break;
      case kLoopDelayMs:
        Power::Policy::loopDelayMs = static_cast<uint16_t>(v);
        break;
      case kCpuIdleMhz:
        Power::Policy::cpuIdleMhz = static_cast<uint32_t>(v);
        break;
      case kCpuActiveMhz:
        Power::Policy::cpuActiveMhz = static_cast<uint32_t>(v);
        break;
      case kWifiCheckMs:
        break;
      case kIrTxPowerPct:
        irSetTxPower(static_cast<int>(values_[kIrTxPowerPct]));
        break;
      case kBoopIrInfo:
      case kBoopInfoFields:
        break;
      case kNotifyIrEnable:
        break;
      case kWifiEnabled:
        // Toggling off should sever any live connection immediately;
        // toggling on does nothing here — the user can press the
        // Connect action or reboot to retry the boot auto-connect.
        if (values_[kWifiEnabled] == 0 && wifiService.isConnected()) {
          DBG("[WiFi] disabled in settings — disconnecting\n");
          wifiService.disconnect();
        }
        break;
      default:
        break;
    }
  }
  
  void Config::applyAll() {
    for (uint8_t i = 0; i < kCount; ++i) {
      apply(i);
    }
    // Re-apply the flip thresholds after the loop. The IMU shares one
    // INT1 threshold register between motion-event detection
    // (kImuInt1Threshold) and 6D orientation flip detection
    // (kFlipUp/DownThreshold). Because kImuInt1Threshold is later in
    // the SettingIndex enum than the flip indices, the bare-loop pass
    // above ends with INT1 stuck at the dev-tuning motion threshold
    // (default 10) — too low for a clean flip and the symptom users
    // hit as "flip thresholds don't kick in until I open Settings and
    // touch them". Re-applying flip last guarantees INT1 reflects the
    // user's flip-up/down magnitudes after every load.
    apply(kFlipUpThreshold);
    applyTimezone();
    applyNametagSetting();
    DBG("Config: applied all settings\n");
  }
  
  // ─── File-change detection ───────────────────────────────────────────────────
  
  void Config::snapshotFileStat() {
    Filesystem::IOLock fsLock;
    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) return;
    FILINFO fno;
    if (f_stat(fs, kSettingsPath, &fno) == FR_OK) {
      lastFileSize_ = static_cast<uint32_t>(fno.fsize);
      lastFileDate_ = fno.fdate;
      lastFileTime_ = fno.ftime;
    }
  }
  
  bool Config::checkFileChanged() {
    Filesystem::IOLock fsLock;
    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) return false;
    FILINFO fno;
    if (f_stat(fs, kSettingsPath, &fno) != FR_OK) return false;
    return static_cast<uint32_t>(fno.fsize) != lastFileSize_
        || fno.fdate != lastFileDate_
        || fno.ftime != lastFileTime_;
  }
  
  // ─── ConfigWatcher service ───────────────────────────────────────────────────
  
  ConfigWatcher configWatcher;
  
  void ConfigWatcher::begin(Config* config) {
    config_ = config;
    lastCheckMs_ = millis();
  }

  const char* ConfigWatcher::name() const { return "ConfigWatcher"; }

  void ConfigWatcher::service() {
    if (config_ == nullptr) return;
    const uint32_t now = millis();
    if (now - lastCheckMs_ < kPollIntervalMs) return;
    lastCheckMs_ = now;

    if (!config_->checkFileChanged()) return;

    DBG("ConfigWatcher: settings.txt changed, reloading\n");
    const bool ok = config_->loadFromFile();
    // loadFromFile() now snapshots file stat unconditionally so the
    // watcher won't re-trigger when the file parses to 0 keys, but we
    // also snapshot here as a safety net in case loadFromFile bailed
    // out early (FS unmounted, malloc failure, …) without snapshotting.
    config_->snapshotFileStat();
    if (ok) {
      config_->applyAll();
    }
  }
