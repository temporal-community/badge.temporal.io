#ifndef POWER_H
#define POWER_H

#include <Arduino.h>
#include <stdint.h>

#include "HardwareConfig.h"
#include "../infra/Scheduler.h"

// ---------------------------------------------------------------------------
//  Power::Policy — compile-time knobs for sleep, CPU governor, and UI pacing.
// ---------------------------------------------------------------------------

namespace Power {

struct Policy {
  static constexpr uint32_t kLightSleepAfterNoMotionMs = 300000UL;   // 5 min — dim LEDs
  static constexpr uint32_t kDeepSleepAfterNoMotionMs = 1200000UL;   // 20 min — deep sleep
  static constexpr uint8_t kLedMatrixDefaultBrightness = 10;
  static constexpr uint8_t kLedMatrixDimBrightness = 3;              // idle-dim level
  static constexpr uint16_t kCrosshairFrameIntervalMs = 35;
  static constexpr uint32_t kCpuActivityHoldMs = 1500;
  static constexpr uint32_t kCpuSwitchMinIntervalMs = 250;
  static constexpr uint32_t kForceDeepSleepHoldMs = 5000;          // hold UP 5s to force deep sleep
  static constexpr uint32_t kUnpairedDeepSleepMs = 300000UL;       // 5 min deep sleep when unpaired

  // Runtime-tunable (dev settings, initialized to compile-time defaults)
  static uint16_t ledServiceIntervalMs;
  static uint16_t oledRefreshMs;
  static uint16_t joystickPollMs;
  static uint8_t schedulerHighDivisor;
  static uint8_t schedulerNormalDivisor;
  static uint8_t schedulerLowDivisor;
  static uint16_t loopDelayMs;
  static uint32_t cpuIdleMhz;
  static uint32_t cpuActiveMhz;

  // Number of ADC reads collected per service() tick during the active probe's
  // sampling phase. Batching multiple samples into one tick minimizes the
  // CE-high duty cycle (the BQ24079 is disabled while CE is high, so every
  // tick spent mid-probe is pure charging loss). See the duty-cycle comment
  // block in BatteryGauge::service() for the full rationale and tuning notes.
  static uint8_t probeSamplesPerTick;
};

bool wifiAllowed();
void applyBootRadioDefaults();
void applyLoopPacing();
void enterPerformanceMode();
void exitPerformanceMode();
void initCpuGovernor();
void noteActivity();
void updateCpuGovernor();

#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
struct ChargerTelemetry {
  bool available = false;
  uint8_t pgoodLevel = HIGH;
  uint8_t statLevel = HIGH;
  bool externalPowerPresent = false;
  bool charging = false;
  const char* state = "n/a";
};

void initChargerTelemetryPins();
ChargerTelemetry readChargerTelemetry();
void logChargerTelemetry(const ChargerTelemetry& telemetry);
void logChargerTelemetryIfDue(uint32_t intervalMs = 5000);
#endif

// RAII helper that silences the SoC brownout detector for the
// duration of a high-current window (TLS handshake, LED matrix
// burst, IR transmit). Saves the RTC_CNTL_BROWN_OUT_REG state on
// construction, zeroes it (disables BOD enable / interrupt /
// reset bits), and restores on destruction. Safe to nest — the
// register simply round-trips through the same value.
//
// Trade-off: a deep undervolt during the silenced window WILL NOT
// reset the chip. Keep windows short (well under a second) and
// only wrap real high-current bursts so a runaway crash on a
// dropping rail can't corrupt flash.
class BrownoutSilencer {
 public:
  BrownoutSilencer();
  ~BrownoutSilencer();
  BrownoutSilencer(const BrownoutSilencer&) = delete;
  BrownoutSilencer& operator=(const BrownoutSilencer&) = delete;

 private:
  uint32_t saved_;
};

}  // namespace Power

// ---------------------------------------------------------------------------
//  SleepService — light/deep sleep driven by IMU motion events.
//  Full implementation requires BADGE_HAS_SLEEP_SERVICE (Charlie board).
//  DELTA gets a stub that exposes the same API as no-ops.
// ---------------------------------------------------------------------------

class Inputs;
class IMU;
class LEDmatrix;
class oled;

#ifdef BADGE_HAS_SLEEP_SERVICE

class SleepService : public IService {
 public:
  volatile bool caffeine = false;

  void bindInputs(Inputs* inputs);
  void begin(IMU* imu, LEDmatrix* matrix, oled* display, uint8_t wakePin = INT_GP_PIN,
             uint32_t lightSleepAfterNoMotionMs = Power::Policy::kLightSleepAfterNoMotionMs,
             uint32_t deepSleepAfterNoMotionMs = Power::Policy::kDeepSleepAfterNoMotionMs);
  void processDeferredWake();
  void requestDeepSleep();
  void setLightSleepMs(uint32_t ms) { lightSleepAfterNoMotionMs_ = ms; }
  void setDeepSleepMs(uint32_t ms) { deepSleepAfterNoMotionMs_ = ms; }
  void service() override;
  const char* name() const override;

 private:
  void preparePeripheralsForDeepSleep();
  void enterDeepSleep();

  Inputs* inputs_ = nullptr;
  IMU* imu_ = nullptr;
  LEDmatrix* matrix_ = nullptr;
  oled* display_ = nullptr;
  uint8_t wakePin_ = INT_GP_PIN;
  uint32_t lightSleepAfterNoMotionMs_ = Power::Policy::kLightSleepAfterNoMotionMs;
  uint32_t deepSleepAfterNoMotionMs_ = Power::Policy::kDeepSleepAfterNoMotionMs;
  uint32_t lastMotionMs_ = 0;
  bool displayDimmed_ = false;
  uint8_t savedBrightness_ = 0;
  bool enabled_ = false;
};

#else
// ── Stub SleepService — no motion-based sleep on this hardware ───────────────

class SleepService : public IService {
 public:
  volatile bool caffeine = false;  // Set by UI code to defer sleep; no-op here
  void bindInputs(Inputs*) {}
  void begin(IMU*, LEDmatrix*, oled*, uint8_t = 0xFF,
             uint32_t = 0, uint32_t = 0) {}
  void processDeferredWake() {}
  void requestDeepSleep() {}
  void setLightSleepMs(uint32_t) {}
  void setDeepSleepMs(uint32_t) {}
  void service() override {}
  const char* name() const override { return "SleepService"; }
};

#endif  // BADGE_HAS_SLEEP_SERVICE

extern SleepService sleepService;

// ---------------------------------------------------------------------------
//  BatteryGauge — MAX17048 fuel gauge or ADC voltage divider.
// ---------------------------------------------------------------------------

#if defined(BADGE_HAS_BATTERY_GAUGE) && defined(BADGE_ECHO)
// ── ADC-based gauge — runs the shared BatteryAlgorithm on a resistor divider
//    plus a CE-pulse slope probe driven from the main CPU. Identical math to
//    a future ULP-RISC-V port via firmware/src/hardware/battery/BatteryAlgorithm.h.

#include "battery/BatteryAlgorithm.h"

// Three-band battery state is defined by BatteryAlgorithm.h:
//   BAT_THRESH_NORMIE, BAT_THRESH_DOM, BAT_THRESH_SUB.

class BatteryGauge : public IService {
 public:
  bool begin();
  void service() override;
  const char* name() const override;
  bool isReady() const { return ready_; }

  float stateOfChargePercent() const { return static_cast<float>(batteryPercent()); }
  float cellVoltage()          const { return batteryMilliVolts() / 1000.0f; }

  bool     batteryPresent()    const { return presence_.latched; }
  // O(1) field read post-boot — the service tick keeps cached_threshold_
  // current. Pre-ready callers (e.g. the WiFi gate during boot, before
  // the IIR filter is warm) get a fast median-based bootup path that
  // takes a handful of raw ADC reads inline and classifies directly.
  battery_threshold_t threshold();
  uint32_t filteredRawAdc()    const { return filter_.filtered_raw >> BAT_IIR_SHIFT; }
  uint32_t batteryMilliVolts() const { return filter_.bat_mv; }
  uint32_t batteryPercent()    const { return filter_.bat_pct; }
  uint32_t chargerFlags()      const { return flags_; }
  uint32_t rawAdc()            const { return filter_.adc_raw; }     // instantaneous, not redundant
  uint32_t rawAdcMin()         const { return rawAdcMin_; }
  uint32_t rawAdcMax()         const { return rawAdcMax_; }
  uint32_t sampleCount()       const { return sampleCount_; }



  // Compatibility shims for code that used to consume the HEAD branch's
  // boolean fields directly. Derived from chargerFlags().
  bool usbPresent() const { return (flags_ & MASK_PGOOD) != 0; }
  bool isCharging() const { return (flags_ & MASK_STAT_LOW) != 0; }
  uint8_t displayTens() const {
    uint32_t p = filter_.bat_pct > 99u ? 99u : filter_.bat_pct;
    return (uint8_t)(p / 10u);
  }
  uint8_t displayUnits() const {
    uint32_t p = filter_.bat_pct > 99u ? 99u : filter_.bat_pct;
    return (uint8_t)(p % 10u);
  }

 private:
  // ── Async probe state machine ──────────────────────────────────────
  // Replaces the old blocking runProbe() with a phase machine driven
  // by micros() deadlines. service() does at most Policy::probeSamplesPerTick
  // ADC reads per call during kProbeSampling; a full probe completes across
  // BAT_PROBE_SAMPLES sample deadlines plus the charger-resettle deadline.
  // The probe buffer is delivered to bat_classify_probe() exactly as
  // before — only the sample cadence changes. Net effect on timing:
  //   - BAT_PROBE_WINDOW_US is the configured sample window (50ms today).
  //   - BAT_PROBE_SAMPLES is 4 today, so the nominal inter-sample target is
  //     BAT_PROBE_WINDOW_US / (BAT_PROBE_SAMPLES - 1), about 16.7ms.
  //   - With probeSamplesPerTick > 1, samples within a batch busy-wait on
  //     micros() rather than yielding, to keep CE-high time bounded by the
  //     probe burst itself instead of the scheduler cadence. CE is released
  //     after the final sample and the classifier runs after the resettle
  //     deadline.
  enum ProbePhase : uint8_t {
    kProbeIdle = 0,
    kProbeSampling,   // CE high, collecting BAT_PROBE_SAMPLES with inter-sample gap
    kProbeResettle,   // CE released, waiting BQ resettle before classifier consumes
  };
  ProbePhase  probePhase_           = kProbeIdle;
  uint8_t     probeSampleIdx_       = 0;
  uint32_t    probeSamples_[BAT_PROBE_SAMPLES] = {0};
  uint32_t    probeNextSampleAtUs_  = 0;
  uint32_t    probeReadyAtUs_       = 0;

  void updateAdcStats(uint32_t raw);
  void logAdcTelemetryIfDue(uint32_t intervalMs = 1000);

  bat_filter_state_t filter_{};
  bat_presence_state_t presence_{};
  uint32_t flags_ = 0;
  uint8_t probeTickCounter_ = 0;
  bool chargerPgoodPrev_         = false;
  bool inUsbProbeDrainBurst_     = false;
  bool ready_ = false;
  uint32_t rawAdcMin_ = UINT32_MAX;
  uint32_t rawAdcMax_ = 0;
  uint32_t sampleCount_ = 0;
  uint32_t lastAdcTelemetryLogMs_ = 0;
  uint32_t last_reported_pct_ = UINT32_MAX;
  battery_threshold_t cached_threshold_ = BAT_THRESH_NORMIE;
};

#elif defined(BADGE_HAS_BATTERY_GAUGE)
#include <Adafruit_MAX1704X.h>

class BatteryGauge : public IService {
 public:
  bool begin();
  void service() override;
  const char* name() const override;
  bool isReady() const { return ready_; }
  float stateOfChargePercent() const { return socPercent_; }
  float cellVoltage() const { return cellVoltage_; }
  // No charger status pins on this hardware — surface the same accessors
  // as the echo gauge so OLEDLayout doesn't have to compile-time branch.
  bool batteryPresent() const { return ready_; }
  bool usbPresent()     const { return false; }
  bool isCharging()     const { return false; }
  uint8_t displayTens() const {
    int p = (socPercent_ > 99.f) ? 99 : (socPercent_ < 0.f ? 0 : (int)socPercent_);
    return (uint8_t)(p / 10);
  }
  uint8_t displayUnits() const {
    int p = (socPercent_ > 99.f) ? 99 : (socPercent_ < 0.f ? 0 : (int)socPercent_);
    return (uint8_t)(p % 10);
  }
 private:
  Adafruit_MAX17048 gauge_;
  bool ready_ = false;
  float socPercent_ = NAN;
  float cellVoltage_ = NAN;
  uint32_t last_reported_pct_ = 0;
  static constexpr float kBatteryVFull = 4.05f;
  static constexpr float kBatteryVEmpty = 2.9f;
  static constexpr float kBatteryVReset = 3.0f;
};

#else
// ── Stub BatteryGauge — always reports not-ready ─────────────────────────────

class BatteryGauge : public IService {
 public:
  bool begin() { return false; }
  void service() override {}
  const char* name() const override { return "BatteryGauge"; }
  bool isReady() const { return false; }
  float stateOfChargePercent() const { return 0.0f; }
  float cellVoltage() const { return 0.0f; }
  bool batteryPresent() const { return false; }
  bool usbPresent()     const { return false; }
  bool isCharging()     const { return false; }
  uint8_t displayTens()  const { return 0; }
  uint8_t displayUnits() const { return 0; }
};

#endif  // BADGE_HAS_BATTERY_GAUGE

#endif  // POWER_H