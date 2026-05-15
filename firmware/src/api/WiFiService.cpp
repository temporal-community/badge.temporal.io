#include "WiFiService.h"

#include "../identity/BadgeUID.h"
#include "../infra/BadgeConfig.h"
#include "../hardware/Power.h"

#include <WiFi.h>
#include <cstring>
#include <time.h>

#include "esp32-hal-cpu.h"
#include "esp_pm.h"
#include "esp_wifi.h"

WiFiService wifiService;

namespace {
esp_pm_lock_handle_t s_wifiPmLock = nullptr;
bool s_timeConfigured = false;
// Set by the WIFI_EVENT_STA_START arduino-esp32 callback. Polled by
// primeRadioForConnectCycle() so we don't fire WiFi.begin() before the
// IDF has actually started the station — that race produces the
// "WiFi.begin() returns success but status stays at WL_IDLE_STATUS"
// hang documented in arduino-esp32 issues #2501 and #12292.
volatile bool s_staStarted = false;
bool s_staEventHookInstalled = false;

void onStaStart(arduino_event_id_t /*event*/, arduino_event_info_t /*info*/) {
  s_staStarted = true;
}

void onStaStop(arduino_event_id_t /*event*/, arduino_event_info_t /*info*/) {
  s_staStarted = false;
}

constexpr const char* kNtpPrimary = "216.239.35.0";
constexpr const char* kNtpSecondary = "129.6.15.28";
constexpr time_t kValidUnixTime = 1700000000;

void configureClockOnce() {
  if (s_timeConfigured) return;
  badgeConfig.applyTimezone();
  configTzTime(badgeConfig.timezone(), kNtpPrimary, kNtpSecondary);
  s_timeConfigured = true;
}

bool clockLooksReady() {
  return time(nullptr) > kValidUnixTime;
}

}  // namespace

namespace {
void wifiAutoConnectTask(void* arg) {
  auto* svc = static_cast<WiFiService*>(arg);
  // Let setup() and the GUI settle before we monopolise the radio.
  vTaskDelay(pdMS_TO_TICKS(2500));
  if (svc != nullptr && badgeConfig.wifiEnabled()) {
    Serial.println("[WiFi] boot auto-connect attempt");
    svc->connect();
  }
  vTaskDelete(nullptr);
}
}  // namespace

void WiFiService::begin() {
  WiFi.mode(WIFI_OFF);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!s_wifiPmLock) {
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "wifi", &s_wifiPmLock);
  }
  if (badgeConfig.wifiEnabled()) {
    Serial.println("[WiFi] credentials present; scheduling boot auto-connect");
    xTaskCreatePinnedToCore(wifiAutoConnectTask, "wifi_auto", 4096, this,
                            tskIDLE_PRIORITY + 1, nullptr, 0);
  } else {
    Serial.println("[WiFi] explicit networking ready; auto-connect disabled");
  }
}

namespace {
// Per-attempt timeout. When several networks are saved we don't want
// the boot path to block WIFI_TIMEOUT_MS × N — keep individual attempts
// short enough that the worst-case full sweep still finishes within a
// reasonable budget. WPA3 SAE on a known-good network usually completes
// well under 10 s; un-attended boots can wait the full WIFI_TIMEOUT_MS
// only for the *last* candidate.
constexpr uint32_t kPerAttemptTimeoutMs = 30000;

// Bring the radio up in STA mode once per connect cycle. Subsequent
// per-slot attempts only call `WiFi.disconnect(false, true)` to wipe
// stale state — they do *not* deinit the driver. Toggling WIFI_OFF /
// WIFI_STA between every attempt is what produced the
//   "STA disconnect failed! 0x3001: ESP_ERR_WIFI_NOT_INIT"
// log noise plus the "status=0 stuck at WL_IDLE_STATUS" hang on both
// arduino-esp32 3.3.4 and 3.3.7: `disconnect(true)` deinits the driver,
// then the next `disconnect(true)` racing the re-init occasionally
// leaves the netif in a state where `WiFi.begin()` returns success but
// the connect request never reaches the AP. Init once, reuse, and only
// power the radio off explicitly at the end of the cycle.
void primeRadioForConnectCycle() {
  if (!s_staEventHookInstalled) {
    WiFi.onEvent(onStaStart, ARDUINO_EVENT_WIFI_STA_START);
    WiFi.onEvent(onStaStop, ARDUINO_EVENT_WIFI_STA_STOP);
    s_staEventHookInstalled = true;
  }

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  s_staStarted = false;
  WiFi.mode(WIFI_STA);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);

  // Wait for the WIFI_EVENT_STA_START event before letting begin() run.
  // arduino-esp32 3.x dispatches that event on a worker task, so it can
  // arrive several hundred ms after WiFi.mode() returns — especially on
  // a busy badge where Core 0 is also running the IR / scheduler loops.
  // Cap the wait at 2 s so a genuinely broken radio still surfaces
  // through the per-attempt timeout instead of stalling forever here.
  const uint32_t startMs = millis();
  while (!s_staStarted && (millis() - startMs) < 2000) {
    delay(20);
  }
  if (!s_staStarted) {
    Serial.println("[WiFi] STA_START event missed; proceeding anyway");
  }
}

// Cleanly power the radio down at the end of a failed connect cycle so
// we don't leave the AP scanning subsystem burning ~80 mA on a
// battery-powered badge. After `disconnect(true, true)` the driver is
// deinited; the next `connect()` will call `primeRadioForConnectCycle`
// again to bring it back up.
void shutDownRadio() {
  // disconnect(wifioff=true, eraseap=true) -> esp_wifi_disconnect +
  // esp_wifi_stop + esp_wifi_deinit + clears any stored AP record.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  s_staStarted = false;
}

bool tryConnectOnce(const char* ssid, const char* pass, uint32_t timeoutMs) {
  // Wipe any stale association/config without tearing the driver down.
  // disconnect(wifioff=false, eraseap=true). On the very first attempt
  // of a connect cycle this is a no-op (config is already empty), but
  // it costs nothing and saves us from dragging stale state into a
  // retry after a previous slot's failure.
  WiFi.disconnect(false, true);
  delay(50);

  Serial.printf("[WiFi] try ssid='%s' (len=%u, pwd_len=%u, timeout=%u ms)\n",
                ssid,
                static_cast<unsigned>(strlen(ssid)),
                static_cast<unsigned>(strlen(pass)),
                static_cast<unsigned>(timeoutMs));
  WiFi.begin(ssid, pass);

  const uint32_t startMs = millis();
  uint32_t lastKickMs = startMs;
  bool sawNonIdle = false;
  uint8_t kicks = 0;
  constexpr uint8_t kMaxKicks = 2;

  while (millis() - startMs < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    const IPAddress ip = WiFi.localIP();
    if (static_cast<uint32_t>(ip) != 0) return true;

    const wl_status_t st = WiFi.status();
    if (!sawNonIdle && st != WL_IDLE_STATUS && st != WL_NO_SHIELD) {
      sawNonIdle = true;
    }

    // Hard-fail status codes — bail out early so the next saved
    // network gets a real chance instead of waiting the full timeout.
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
      Serial.printf("[WiFi] short-circuit fail status=%d\n", st);
      return false;
    }

    // Status stuck at WL_IDLE_STATUS (0) for >3 s after begin() means
    // the begin() call raced the STA_START event and esp_wifi_connect
    // never actually fired. Kick begin() again. Caps at kMaxKicks so a
    // truly dead radio doesn't loop forever inside this attempt.
    if (!sawNonIdle && kicks < kMaxKicks &&
        (millis() - lastKickMs) > 3000) {
      Serial.printf("[WiFi] stuck-idle kick %u/%u status=%d\n",
                    static_cast<unsigned>(kicks + 1),
                    static_cast<unsigned>(kMaxKicks), st);
      WiFi.disconnect(false, false);
      delay(100);
      WiFi.begin(ssid, pass);
      lastKickMs = millis();
      ++kicks;
    }

    delay(100);
  }
  Serial.printf("[WiFi] attempt timed out status=%d\n", WiFi.status());
  return false;
}
}  // namespace

bool WiFiService::connect() {
  if (isConnected()) {
    noteConnectionOk();
    return true;
  }
  if (!badgeConfig.wifiEnabled()) {
    Serial.println("[WiFi] disabled in settings or not configured; connect skipped");
    noteConnectionFailed();
    return false;
  }
  if (!Power::wifiAllowed()) {
    Serial.println("[WiFi] blocked by power policy");
    noteConnectionFailed();
    return false;
  }

  const uint32_t prevMhz = getCpuFrequencyMhz();
  if (s_wifiPmLock) {
    esp_pm_lock_acquire(s_wifiPmLock);
  } else if (prevMhz < 160) {
    setCpuFrequencyMhz(160);
  }
  auto restoreCpu = [&]() {
    if (s_wifiPmLock) esp_pm_lock_release(s_wifiPmLock);
    else if (prevMhz < 160) setCpuFrequencyMhz(prevMhz);
  };

  // Walk saved networks in slot order. Slot 0 is "preferred" so a
  // user who only ever fills in one network gets the same single-
  // attempt behaviour as before; multiple slots fall through on
  // per-attempt timeout / hard-fail until one works or every slot
  // has been tried.
  const uint8_t total = badgeConfig.wifiNetworkCount();
  if (total == 0) {
    Serial.println("[WiFi] no saved networks configured");
    noteConnectionFailed();
    restoreCpu();
    return false;
  }

  primeRadioForConnectCycle();

  // Hostname must be set after the driver is up (WIFI_STA) but before
  // begin(); otherwise it gets ignored on the first DHCP exchange.
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "badge-%s", uid_hex);
  WiFi.setHostname(hostname);

  bool ok = false;
  uint8_t attempted = 0;
  for (uint8_t i = 0; i < Config::kMaxWifiNetworks && !ok; ++i) {
    const char* ssid = badgeConfig.wifiSsidAt(i);
    const char* pass = badgeConfig.wifiPassAt(i);
    if (!ssid || !ssid[0]) continue;
    ++attempted;
    // Last candidate gets the full timeout so a slow but valid AP
    // still has a chance even when earlier saved networks were
    // tried first.
    const uint32_t timeoutMs =
        (attempted >= total) ? WIFI_TIMEOUT_MS : kPerAttemptTimeoutMs;
    Serial.printf("[WiFi] explicit connect to slot %u (timeout=%u ms)\n",
                  static_cast<unsigned>(i),
                  static_cast<unsigned>(timeoutMs));
    ok = tryConnectOnce(ssid, pass ? pass : "", timeoutMs);
  }

  if (ok) {
    // Modem-sleep saves ~50 mA average on an associated link by
    // letting the radio nap between DTIM beacons. Safe to enable
    // after we've actually got an IP — earlier and the AP can drop
    // the association mid-handshake.
    WiFi.setSleep(true);
    configureClockOnce();
    noteConnectionOk();
    Serial.printf("[WiFi] connected ip=%s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    shutDownRadio();
    noteConnectionFailed();
    Serial.printf("[WiFi] connect failed after %u attempt%s — radio off\n",
                  static_cast<unsigned>(attempted),
                  attempted == 1 ? "" : "s");
  }

  restoreCpu();
  return ok;
}

void WiFiService::disconnect() {
  shutDownRadio();
  networkIndicatorActive_ = false;
}

bool WiFiService::isConnected() const {
  return WiFi.status() == WL_CONNECTED ||
         static_cast<uint32_t>(WiFi.localIP()) != 0;
}

int WiFiService::rssi() const {
  // WiFi.RSSI() returns 0 if the radio isn't currently associated, and
  // it briefly drops to 0 between management frames even on a stable
  // link. Cache the most recent non-zero reading so the status icon
  // doesn't flicker, but invalidate the cache when we know we're
  // disconnected (so we report "0 / no signal" promptly).
  if (!isConnected()) {
    lastRssi_ = 0;
    lastRssiSampleMs_ = 0;
    return 0;
  }
  const int raw = WiFi.RSSI();
  if (raw != 0) {
    lastRssi_ = static_cast<int8_t>(raw);
    lastRssiSampleMs_ = millis();
    return raw;
  }
  // Zero reading despite being associated — return the cached value if
  // it's recent enough (under 10 s), otherwise admit we don't know.
  if (lastRssiSampleMs_ != 0 && (millis() - lastRssiSampleMs_) < 10000) {
    return lastRssi_;
  }
  return 0;
}

uint8_t WiFiService::signalLevel() const {
  if (!isConnected()) return 0;
  const int r = rssi();
  if (r == 0) return 1;  // associated but no reading yet — show 1 bar
  if (r >= -55) return 3;
  if (r >= -65) return 2;
  return 1;
}

void WiFiService::setPhase(Phase p) {
  phase_ = p;
  phaseChangedMs_ = millis();
}

void WiFiService::dismissPhase() {
  if (phase_ == Phase::kConnected || phase_ == Phase::kFailed) {
    setPhase(Phase::kIdle);
    setPhaseStatus("");
  }
}

void WiFiService::setPhaseStatus(const char* s) {
  if (!s) s = "";
  std::strncpy(phaseStatusText_, s, sizeof(phaseStatusText_) - 1);
  phaseStatusText_[sizeof(phaseStatusText_) - 1] = '\0';
}

// File-scope so WiFiService::pollStaAssociation (outside unnamed namespace)
// can share wording with worker tasks inside the TU.
static const char* wifiPhaseLabelForWl(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:      return "Radio idle";
    case WL_NO_SSID_AVAIL:    return "Network not found";
    case WL_SCAN_COMPLETED:   return "Scan complete";
    case WL_CONNECTED:        return "Connected";
    case WL_CONNECT_FAILED:   return "Auth failed";
    case WL_CONNECTION_LOST:  return "Connection lost";
    case WL_DISCONNECTED:     return "Disconnected";
    case WL_NO_SHIELD:        return "Radio offline";
    default:                  return "Working...";
  }
}

namespace {

struct AsyncConnectArgs {
  WiFiService* svc;
  uint8_t slot;
};

void wifiSlotConnectTask(void* arg) {
  auto* args = static_cast<AsyncConnectArgs*>(arg);
  WiFiService* svc = args->svc;
  const uint8_t slot = args->slot;
  delete args;
  if (svc) svc->runSlotConnect(slot);
  vTaskDelete(nullptr);
}

struct SavedIterateArgs {
  WiFiService* svc;
};

void wifiSavedIterateTask(void* arg) {
  auto* args = static_cast<SavedIterateArgs*>(arg);
  WiFiService* svc = args->svc;
  delete args;
  if (svc) svc->runSavedNetworksConnect();
  vTaskDelete(nullptr);
}
}  // namespace

bool WiFiService::pollStaAssociation(uint32_t timeoutMs, const char* ssid,
                                   const char* pass) {
  const char* pwd = pass ? pass : "";
  const uint32_t startMs = millis();
  uint32_t lastKickMs = startMs;
  bool sawAuth = false;
  bool sawNonIdle = false;
  uint8_t kicks = 0;
  constexpr uint8_t kMaxKicks = 2;

  while (millis() - startMs < timeoutMs) {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED ||
        static_cast<uint32_t>(WiFi.localIP()) != 0) {
      return true;
    }
    if (!sawAuth && st != WL_IDLE_STATUS && st != WL_DISCONNECTED &&
        st != WL_NO_SHIELD) {
      sawAuth = true;
      setPhaseStatus(wifiPhaseLabelForWl(st));
      setPhase(Phase::kAuthenticating);
    }
    if (!sawNonIdle && st != WL_IDLE_STATUS && st != WL_NO_SHIELD) {
      sawNonIdle = true;
    }
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
      setPhaseStatus(wifiPhaseLabelForWl(st));
      setPhase(Phase::kFailed);
      return false;
    }
    if (!sawNonIdle && kicks < kMaxKicks &&
        (millis() - lastKickMs) > 3000) {
      Serial.printf("[WiFi] async stuck-idle kick %u/%u status=%d\n",
                    static_cast<unsigned>(kicks + 1),
                    static_cast<unsigned>(kMaxKicks), static_cast<int>(st));
      WiFi.disconnect(false, false);
      delay(100);
      WiFi.begin(ssid, pwd);
      lastKickMs = millis();
      ++kicks;
    }
    delay(150);
  }
  return false;
}

bool WiFiService::connectToSlotAsync(uint8_t slot) {
  if (asyncConnectInFlight_) return false;
  if (slot >= Config::kMaxWifiNetworks) return false;
  const char* ssid = badgeConfig.wifiSsidAt(slot);
  if (!ssid || !ssid[0]) return false;
  // The wifi_enabled toggle gates *boot* auto-connect; an explicit
  // Connect press from the WiFi screen still works regardless.
  if (!Power::wifiAllowed()) {
    setPhaseStatus("Blocked by power policy");
    setPhase(Phase::kFailed);
    return false;
  }
  asyncConnectInFlight_ = true;
  std::strncpy(phaseSsid_, ssid, sizeof(phaseSsid_) - 1);
  phaseSsid_[sizeof(phaseSsid_) - 1] = '\0';
  setPhaseStatus("Starting...");
  setPhase(Phase::kStarting);

  auto* args = new AsyncConnectArgs{this, slot};
  if (xTaskCreatePinnedToCore(wifiSlotConnectTask, "wifi_slot", 6144,
                              args, tskIDLE_PRIORITY + 1, nullptr,
                              0) != pdPASS) {
    delete args;
    asyncConnectInFlight_ = false;
    setPhaseStatus("Out of memory");
    setPhase(Phase::kFailed);
    return false;
  }
  return true;
}

bool WiFiService::connectSavedNetworksAsync() {
  if (asyncConnectInFlight_) return false;
  if (!Power::wifiAllowed()) {
    setPhaseStatus("Blocked by power policy");
    setPhase(Phase::kFailed);
    return false;
  }
  const uint8_t netCount = badgeConfig.wifiNetworkCount();
  if (netCount == 0) {
    setPhaseStatus("No saved networks");
    setPhase(Phase::kFailed);
    return false;
  }

  asyncConnectInFlight_ = true;
  phaseSsid_[0] = '\0';
  setPhaseStatus("Starting...");
  setPhase(Phase::kStarting);

  auto* args = new SavedIterateArgs{this};
  if (xTaskCreatePinnedToCore(wifiSavedIterateTask, "wifi_scan", 8192, args,
                              tskIDLE_PRIORITY + 1, nullptr,
                              0) != pdPASS) {
    delete args;
    asyncConnectInFlight_ = false;
    setPhaseStatus("Out of memory");
    setPhase(Phase::kFailed);
    return false;
  }
  return true;
}

void WiFiService::runSlotConnect(uint8_t slot) {
  const char* ssid = badgeConfig.wifiSsidAt(slot);
  const char* pass = badgeConfig.wifiPassAt(slot);
  if (!ssid || !ssid[0]) {
    setPhaseStatus("Empty slot");
    setPhase(Phase::kFailed);
    asyncConnectInFlight_ = false;
    return;
  }

  const uint32_t prevMhz = getCpuFrequencyMhz();
  if (s_wifiPmLock) {
    esp_pm_lock_acquire(s_wifiPmLock);
  } else if (prevMhz < 160) {
    setCpuFrequencyMhz(160);
  }
  auto restoreCpu = [&]() {
    if (s_wifiPmLock) esp_pm_lock_release(s_wifiPmLock);
    else if (prevMhz < 160) setCpuFrequencyMhz(prevMhz);
  };

  const bool wasConnected =
      (WiFi.status() == WL_CONNECTED) ||
      (static_cast<uint32_t>(WiFi.localIP()) != 0);
  String currentSsid = wasConnected ? WiFi.SSID() : String();
  if (wasConnected && currentSsid == ssid) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Already connected (%s)",
                  WiFi.localIP().toString().c_str());
    setPhaseStatus(buf);
    setPhase(Phase::kConnected);
    Serial.printf("[WiFi] async slot %u: already on '%s'\n",
                  static_cast<unsigned>(slot), ssid);
    noteConnectionOk();
    restoreCpu();
    asyncConnectInFlight_ = false;
    return;
  }

  if (wasConnected && currentSsid.length() > 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Disconnecting %.20s",
                  currentSsid.c_str());
    setPhaseStatus(buf);
    setPhase(Phase::kStarting);
    Serial.printf("[WiFi] switching from '%s' to '%s'\n",
                  currentSsid.c_str(), ssid);
  }

  primeRadioForConnectCycle();

  char hostname[32];
  snprintf(hostname, sizeof(hostname), "badge-%s", uid_hex);
  WiFi.setHostname(hostname);

  WiFi.disconnect(false, true);
  delay(50);

  char line[64];
  std::snprintf(line, sizeof(line), "Connecting to %.20s",
                phaseSsid_[0] ? phaseSsid_ : ssid);
  setPhaseStatus(line);
  setPhase(Phase::kAttempting);
  Serial.printf("[WiFi] async slot %u ssid='%s' (len=%u, pwd_len=%u)\n",
                static_cast<unsigned>(slot), ssid,
                static_cast<unsigned>(strlen(ssid)),
                static_cast<unsigned>(strlen(pass ? pass : "")));
  WiFi.begin(ssid, pass ? pass : "");

  const bool ok =
      pollStaAssociation(WIFI_TIMEOUT_MS, ssid, pass ? pass : "");

  if (ok) {
    WiFi.setSleep(true);
    configureClockOnce();
    noteConnectionOk();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Got IP %s",
                  WiFi.localIP().toString().c_str());
    setPhaseStatus(buf);
    setPhase(Phase::kConnected);
    Serial.printf("[WiFi] connected ip=%s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    shutDownRadio();
    noteConnectionFailed();
    if (phase_ != Phase::kFailed) {
      setPhaseStatus("Timed out");
    }
    setPhase(Phase::kFailed);
    Serial.printf("[WiFi] async slot %u failed — radio off\n",
                  static_cast<unsigned>(slot));
  }

  restoreCpu();
  asyncConnectInFlight_ = false;
}

void WiFiService::runSavedNetworksConnect() {
  const uint32_t prevMhz = getCpuFrequencyMhz();
  if (s_wifiPmLock) {
    esp_pm_lock_acquire(s_wifiPmLock);
  } else if (prevMhz < 160) {
    setCpuFrequencyMhz(160);
  }
  auto restoreCpu = [&]() {
    if (s_wifiPmLock) esp_pm_lock_release(s_wifiPmLock);
    else if (prevMhz < 160) setCpuFrequencyMhz(prevMhz);
  };

  primeRadioForConnectCycle();

  char hostname[32];
  snprintf(hostname, sizeof(hostname), "badge-%s", uid_hex);
  WiFi.setHostname(hostname);

  bool ok = false;
  uint8_t attempted = 0;
  const uint8_t total = badgeConfig.wifiNetworkCount();

  for (uint8_t i = 0; i < Config::kMaxWifiNetworks && !ok; ++i) {
    const char* ssid = badgeConfig.wifiSsidAt(i);
    const char* pass = badgeConfig.wifiPassAt(i);
    if (!ssid || !ssid[0]) continue;
    ++attempted;
    const uint32_t timeoutMs =
        (attempted >= total) ? WIFI_TIMEOUT_MS : kPerAttemptTimeoutMs;

    std::strncpy(phaseSsid_, ssid, sizeof(phaseSsid_) - 1);
    phaseSsid_[sizeof(phaseSsid_) - 1] = '\0';

    char line[64];
    snprintf(line, sizeof(line), "Connecting to %.20s", ssid);
    setPhaseStatus(line);
    setPhase(Phase::kAttempting);

    WiFi.disconnect(false, true);
    delay(50);
    WiFi.begin(ssid, pass ? pass : "");

    Serial.printf(
        "[WiFi] async iterate slot %u ssid='%s' timeout=%u ms\n",
        static_cast<unsigned>(i), ssid,
        static_cast<unsigned>(timeoutMs));
    ok = pollStaAssociation(timeoutMs, ssid, pass ? pass : "");
  }

  if (ok) {
    WiFi.setSleep(true);
    configureClockOnce();
    noteConnectionOk();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Got IP %s",
                  WiFi.localIP().toString().c_str());
    setPhaseStatus(buf);
    setPhase(Phase::kConnected);
    Serial.printf("[WiFi] iterate ok ip=%s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    shutDownRadio();
    noteConnectionFailed();
    if (phase_ != Phase::kFailed) {
      setPhaseStatus("Timed out");
    }
    setPhase(Phase::kFailed);
    Serial.printf("[WiFi] iterate failed — radio off\n");
  }

  restoreCpu();
  asyncConnectInFlight_ = false;
}

void WiFiService::refreshClockState() const {
  const time_t now = time(nullptr);
  if (now <= kValidUnixTime) return;

  clockEverReady_ = true;
  lastClockEpoch_ = static_cast<uint32_t>(now);
  lastClockSampleMs_ = millis();
}

void WiFiService::noteConnectionOk() {
  networkIndicatorActive_ = true;
  lastNetworkOkMs_ = millis();
  refreshClockState();
}

void WiFiService::noteConnectionFailed() {
  networkIndicatorActive_ = false;
  lastNetworkFailMs_ = millis();
}

void WiFiService::noteRequestOk() {
  noteConnectionOk();
}

void WiFiService::noteRequestFailed() {
  networkIndicatorActive_ = false;
  lastNetworkFailMs_ = millis();
}

bool WiFiService::clockReady() const {
  if (clockLooksReady()) refreshClockState();
  return clockEverReady_;
}

bool WiFiService::currentTime(time_t* out) const {
  if (clockLooksReady()) {
    const time_t now = time(nullptr);
    refreshClockState();
    if (out) *out = now;
    return true;
  }

  if (!clockEverReady_ || lastClockEpoch_ == 0) return false;
  if (out) {
    *out = static_cast<time_t>(
        lastClockEpoch_ + ((millis() - lastClockSampleMs_) / 1000UL));
  }
  return true;
}
