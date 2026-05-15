#ifndef WIFISERVICE_H
#define WIFISERVICE_H

#include <Arduino.h>
#include <time.h>

#include "../infra/Scheduler.h"

class WiFiService : public IService {
 public:
  void begin();

  // Explicit-only networking with one exception: if WiFi is enabled in
  // settings AND credentials are configured, begin() schedules a
  // single auto-connect attempt shortly after boot. Subsequent
  // connect() calls are still on-demand (e.g. badge.http_get/post).
  bool connect();
  void disconnect();
  bool isConnected() const;

  // Phase reported by the async connect path. Drawn by `WifiScreen`
  // as a popup that updates as we walk through the handshake.
  enum class Phase : uint8_t {
    kIdle,            // not currently attempting
    kStarting,        // disconnecting + radio bring-up
    kAttempting,      // WiFi.begin() issued; waiting for any status
    kAuthenticating,  // got past idle/scan; still negotiating
    kConnected,       // WL_CONNECTED + IP assigned
    kFailed,          // attempt finished without a connection
  };

  // Kick off a connect attempt against a single saved slot in the
  // background. Returns false if a connect is already in flight or the
  // slot is empty/disabled. Phase + status string update as the
  // attempt walks through start → attempt → auth → got-ip / fail.
  bool connectToSlotAsync(uint8_t slot);

  // Like connect() slot iteration, but async with the same Phase/overlay
  // updates as connectToSlotAsync (for WiFi screen "Connect Now").
  bool connectSavedNetworksAsync();

  // Worker entry; do not call directly from UI code. Public only so
  // the FreeRTOS task entry point in the .cpp can forward to it.
  void runSlotConnect(uint8_t slot);
  void runSavedNetworksConnect();

  Phase phase() const { return phase_; }
  // Human-readable single-line status (e.g. "Connecting to MyAP",
  // "Got IP 192.168.4.42", "Bad password"). Stable pointer; safe to
  // render directly. Empty string when phase is kIdle.
  const char* phaseStatusText() const { return phaseStatusText_; }
  const char* phaseSsid() const { return phaseSsid_; }
  // Snapshot of millis() when phase last transitioned. Lets the UI
  // auto-dismiss the popup a few seconds after a terminal phase.
  uint32_t phaseChangedMs() const { return phaseChangedMs_; }

  // UI ack: drops the overlay-driving phase back to kIdle, but only
  // when the current phase is terminal (Connected/Failed). Active
  // attempts are not interruptible — calling this mid-handshake is a
  // no-op so the user can't accidentally hide a connect they meant to
  // wait on.
  void dismissPhase();

  bool networkIndicatorActive() const { return networkIndicatorActive_; }
  bool hasEverConnected() const {
    return lastNetworkOkMs_ != 0 || clockEverReady_;
  }
  bool clockReady() const;
  bool currentTime(time_t* out) const;
  bool isAutoSyncInProgress() const { return false; }
  void requestImmediateSync() {}

  // Last-known signal strength in dBm. 0 when never connected. Refreshed
  // implicitly when callers ask for `signalLevel()` so the status header
  // doesn't need its own polling task.
  int rssi() const;
  // 0..3 bars derived from rssi(); 0 only when the radio has no
  // association, i.e. a "down" indicator. Drawn by OLEDLayout.
  uint8_t signalLevel() const;

  void noteConnectionOk();
  void noteConnectionFailed();
  void noteRequestOk();
  void noteRequestFailed();

  void service() override {}
  const char* name() const override { return "Network"; }

  void setCheckIntervalMs(uint32_t ms) { checkIntervalMs_ = ms; }
  void resetConnectionBackoff() {}
  void armForReconnect() {}

  void pushForeground() {}
  void popForeground() {}
  bool requestMessageFetch() { return false; }
  bool requestZigmojiFetch() { return false; }
  bool foregroundFetchInProgress() const { return false; }

 private:
  uint32_t checkIntervalMs_ = 10000;
  volatile bool networkIndicatorActive_ = false;
  volatile uint32_t lastNetworkOkMs_ = 0;
  volatile uint32_t lastNetworkFailMs_ = 0;
  mutable volatile bool clockEverReady_ = false;
  mutable volatile uint32_t lastClockEpoch_ = 0;
  mutable volatile uint32_t lastClockSampleMs_ = 0;
  // Cached RSSI from the last successful poll. WiFi.RSSI() can spike
  // briefly to 0 between management frames; caching avoids the icon
  // dropping to 1 bar every couple of seconds on a steady link.
  mutable volatile int8_t lastRssi_ = 0;
  mutable volatile uint32_t lastRssiSampleMs_ = 0;
  void refreshClockState() const;

  // Async connect plumbing — touched from a Core 0 worker task and read
  // from the main-loop UI on Core 1. Single-byte/uint32 stores with
  // `volatile` are safe enough on ESP32 for this producer-consumer
  // pattern; we never need the values to be perfectly atomic, only
  // monotonically progressing.
  volatile Phase phase_ = Phase::kIdle;
  volatile uint32_t phaseChangedMs_ = 0;
  volatile bool asyncConnectInFlight_ = false;
  // Static buffers so the UI can read pointers without the worker
  // holding a lock. Truncated to a sane single-line popup width.
  char phaseSsid_[33] = {};
  char phaseStatusText_[64] = {};
  void setPhase(Phase p);
  void setPhaseStatus(const char* s);
  bool pollStaAssociation(uint32_t timeoutMs, const char* ssid,
                          const char* pass);
};

extern WiFiService wifiService;

#endif
