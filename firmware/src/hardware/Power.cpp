#include "Power.h"

#include "../api/WiFiService.h"
#include "../infra/DebugLog.h"
#include <Wire.h>

#ifdef BADGE_HAS_SLEEP_SERVICE
#include "IMU.h"
#include "Inputs.h"
#include "LEDmatrix.h"
#include "../BadgeGlobals.h"
#endif
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp32-hal-cpu.h"
#include "esp_bt.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "oled.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "esp_log.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  Power namespace — radio defaults, loop pacing, CPU governor
// ═══════════════════════════════════════════════════════════════════════════════

namespace Power {

uint16_t Policy::ledServiceIntervalMs = 25;
uint16_t Policy::oledRefreshMs = 100;
uint16_t Policy::joystickPollMs = 20;
uint8_t  Policy::schedulerHighDivisor = 1;
uint8_t  Policy::schedulerNormalDivisor = 4;
uint8_t  Policy::schedulerLowDivisor = 30;
uint16_t Policy::loopDelayMs = 8;
uint32_t Policy::cpuIdleMhz = 80;
uint32_t Policy::cpuActiveMhz = 160;
uint8_t  Policy::probeSamplesPerTick = 3;  // batch ADC reads per tick to minimize CE-high duty cycle

namespace {
bool gPmActive = false;
uint32_t gLastActivityMs = 0;
uint32_t gLastCpuSwitchMs = 0;
uint32_t gCurrentCpuMhz = 0;
uint8_t gPerformanceDepth = 0;

#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
bool gChargerTelemetryPinsReady = false;
uint32_t gLastChargerTelemetryLogMs = 0;
uint32_t gLastBatteryFullSerialMs = 0;
bool gWasCharging = false;
#endif

bool configurePm(uint32_t minMhz, uint32_t maxMhz, bool lightSleep) {
  if (!gPmActive) return false;
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = maxMhz;
  pm.min_freq_mhz = minMhz;
  pm.light_sleep_enable = lightSleep;
  const esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
    ESP_LOGI("POWER","PM: configure failed (0x%x)\n", err);
    return false;
  }
  return true;
}
}  // namespace

#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
void initChargerTelemetryPins() {
  if (gChargerTelemetryPinsReady) return;
  pinMode(CHG_GOOD_PIN, INPUT_PULLUP);
  pinMode(CHG_STAT_PIN, INPUT_PULLUP);
  gChargerTelemetryPinsReady = true;
}

ChargerTelemetry readChargerTelemetry() {
  initChargerTelemetryPins();

  ChargerTelemetry telemetry;
  telemetry.available = true;
  telemetry.pgoodLevel = digitalRead(CHG_GOOD_PIN) == LOW ? LOW : HIGH;
  telemetry.statLevel = digitalRead(CHG_STAT_PIN) == LOW ? LOW : HIGH;

  // BQ24079 status outputs are active-low/open-drain:
  // PGOOD low means external power is present; STAT low means charging.
  telemetry.externalPowerPresent = telemetry.pgoodLevel == LOW;
  telemetry.charging = telemetry.externalPowerPresent && telemetry.statLevel == LOW;
  if (!telemetry.externalPowerPresent) {
    telemetry.state = "battery";
  } else if (telemetry.charging) {
    telemetry.state = "charging";
  } else {
    telemetry.state = "external";
  }
  return telemetry;
}

void logChargerTelemetry(const ChargerTelemetry& telemetry) {
  if (!telemetry.available) {
    ESP_LOGI("POWER","Power: charger telemetry unavailable\n");
    return;
  }
  ESP_LOGI("POWER","Power: charger pgood=%u stat=%u external=%d charging=%d state=%s\n",
            (unsigned)telemetry.pgoodLevel,
            (unsigned)telemetry.statLevel,
            telemetry.externalPowerPresent ? 1 : 0,
            telemetry.charging ? 1 : 0,
            telemetry.state);
}

void logChargerTelemetryIfDue(uint32_t intervalMs) {
  const uint32_t now = millis();
  if (gLastChargerTelemetryLogMs != 0 &&
      now - gLastChargerTelemetryLogMs < intervalMs) {
    return;
  }
  gLastChargerTelemetryLogMs = now;
  const ChargerTelemetry telemetry = readChargerTelemetry();
  logChargerTelemetry(telemetry);

  if (gWasCharging && !telemetry.charging && telemetry.externalPowerPresent) {
    constexpr uint32_t kBatteryFullLogIntervalMs = 5 * 60 * 1000;
    if (gLastBatteryFullSerialMs == 0 ||
        (uint32_t)(now - gLastBatteryFullSerialMs) >= kBatteryFullLogIntervalMs) {
      gLastBatteryFullSerialMs = now;
      // Serial.println("[POWER] BATTERY_FULL");
      // ESP_LOGI("POWER", "Charge complete — battery full");
    }
  }
  gWasCharging = telemetry.charging;
}
#endif

void applyBootRadioDefaults() {
  // WiFi stays available for explicit MicroPython networking helpers only.
  // BLE controller memory is INTENTIONALLY NOT released — IDF's
  // esp_bt_mem_release(BLE) permanently relinquishes the controller's
  // static .bss/.data regions to the heap, after which any later
  // esp_bt_controller_init() fails inside btdm_controller_init and
  // the cleanup path NULL-derefs in semphr_delete_wrapper. Keeping
  // BLE reserved costs ~30 KB internal DRAM but is the only way the
  // venue proximity scanner can come up on demand. Releasing only
  // the classic-BT footprint reclaims unused BR/EDR memory safely.
  esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
}

void applyLoopPacing() {
  delay(Policy::loopDelayMs);
}

void enterPerformanceMode() {
  noteActivity();
  if (gPerformanceDepth < 255) {
    gPerformanceDepth++;
  }
  if (gPerformanceDepth != 1) return;

  if (configurePm(240, 240, false)) {
    ESP_LOGI("POWER","PM: performance mode enabled (240 MHz, no light sleep)\n");
  }
}

void exitPerformanceMode() {
  if (gPerformanceDepth == 0) return;
  gPerformanceDepth--;
  if (gPerformanceDepth != 0) return;

  if (configurePm(Policy::cpuIdleMhz, Policy::cpuActiveMhz, true)) {
    ESP_LOGI("POWER","PM: restored policy (%lu-%lu MHz, light sleep enabled)\n",
              (unsigned long)Policy::cpuIdleMhz,
              (unsigned long)Policy::cpuActiveMhz);
  }
  noteActivity();
}

void initCpuGovernor() {
  gLastActivityMs = millis();
  gLastCpuSwitchMs = 0;
  gCurrentCpuMhz = getCpuFrequencyMhz();

  // Prefer ESP-IDF power management with automatic light sleep.
  // Requires CONFIG_PM_ENABLE + CONFIG_FREERTOS_USE_TICKLESS_IDLE in the IDF
  // build; when they're missing, esp_pm_configure() returns
  // ESP_ERR_NOT_SUPPORTED and we must NOT fall through to manual CPU
  // scaling: setCpuFrequencyMhz() reprograms the APB clock, which can
  // corrupt in-flight I2C transactions on the new IDF 5.x driver (see
  // esp-idf#15734, arduino-esp32#11374). Pin the CPU at whatever f_cpu
  // selected at boot instead.
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = Policy::cpuActiveMhz;
  pm.min_freq_mhz = Policy::cpuIdleMhz;
  pm.light_sleep_enable = true;
  esp_err_t err = esp_pm_configure(&pm);
  if (err == ESP_OK) {
    gPmActive = true;
    ESP_LOGI("POWER","PM: auto light sleep enabled (%lu-%lu MHz)\n",
              (unsigned long)Policy::cpuIdleMhz,
              (unsigned long)Policy::cpuActiveMhz);
    return;
  }
  ESP_LOGI("POWER","PM: unavailable (0x%x) - CPU pinned at %lu MHz\n",
            err, (unsigned long)gCurrentCpuMhz);
}

void noteActivity() {
  gLastActivityMs = millis();
}

void updateCpuGovernor() {
  // No-op: either ESP-IDF PM is driving the frequency (when CONFIG_PM_ENABLE
  // is set), or we've chosen to leave the CPU at its boot frequency. Manual
  // setCpuFrequencyMhz() toggling has been removed because it reprograms the
  // APB clock and triggers ESP_ERR_INVALID_STATE on the I2C master driver
  // when a transaction is in flight.
}

// ── BrownoutSilencer ──────────────────────────────────────────────
// Save the master enable + interrupt enable + reset enable bits of
// RTC_CNTL_BROWN_OUT_REG and clear them; restore on destruct. Three
// bits, not the whole register — writing 0 to the entire register
// also zeroes the trip-level / wait / select fields, which on this
// SoC we observed leaves the detector in a state where the
// interrupt still fires immediately on the first WiFi TX burst.
// The IDF's brownout_hal_intr_enable(false) only touches INT_ENA;
// we additionally clear RST_ENA (no reset) and ENA (analog
// comparator off) for the duration of the silenced window.
//
// Also stamps the SPI-flash brownout-reset gate
// (RTC_CNTL_BROWN_OUT_CLOSE_FLASH_ENA) — the flash brownout reset
// is a separate path that bypasses the BOD ISR entirely, and was
// the residual offender in the field.
BrownoutSilencer::BrownoutSilencer() {
  saved_ = REG_READ(RTC_CNTL_BROWN_OUT_REG);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_INT_ENA);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_RST_ENA);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_CLOSE_FLASH_ENA);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
  // ESP_LOGI("POWER","[BOD] silenced (was 0x%08x, now 0x%08x)\n",
  //               static_cast<unsigned>(saved_),
  //               static_cast<unsigned>(REG_READ(RTC_CNTL_BROWN_OUT_REG)));
}

BrownoutSilencer::~BrownoutSilencer() {
  // Restore the exact prior register state — including any wait /
  // select fields the framework configured at boot.
  REG_WRITE(RTC_CNTL_BROWN_OUT_REG, saved_);
  // Serial.println("[BOD] restored");
}

#if !(defined(BADGE_HAS_BATTERY_GAUGE) && defined(BADGE_ECHO) && defined(BADGE_HAS_SLEEP_SERVICE))
// Non-echo builds (or echo without sleep service) don't have the BQ24079
// brownout problem; the gate is unconditionally open.
bool wifiAllowed() { return true; }
#endif

}  // namespace Power

#ifdef BADGE_HAS_SLEEP_SERVICE

// ═══════════════════════════════════════════════════════════════════════════════
//  SleepService — light/deep sleep driven by IMU motion events
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
bool isLineStableHigh(uint8_t pin, uint8_t samples = 6, uint8_t sampleDelayMs = 2) {
  for (uint8_t i = 0; i < samples; ++i) {
    if (digitalRead(pin) == LOW) {
      return false;
    }
    delay(sampleDelayMs);
  }
  return true;
}

// USB host sends SOF packets every 1ms, incrementing this counter.
// If the counter changed since last check, a USB host is connected.
bool isUsbConnected() {
  static uint32_t prevFrame = UINT32_MAX;
  uint32_t frame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FFU;
  bool connected = (frame != prevFrame);
  prevFrame = frame;
  return connected;
}
}  // namespace

void SleepService::bindInputs(Inputs* inputs) { inputs_ = inputs; }

void SleepService::begin(IMU* imu, LEDmatrix* matrix, oled* display, uint8_t wakePin,
                         uint32_t lightSleepAfterNoMotionMs, uint32_t deepSleepAfterNoMotionMs) {
  imu_ = imu;
  matrix_ = matrix;
  display_ = display;
  wakePin_ = wakePin;
  lightSleepAfterNoMotionMs_ = lightSleepAfterNoMotionMs;
  deepSleepAfterNoMotionMs_ = deepSleepAfterNoMotionMs;
  lastMotionMs_ = millis();
  caffeine = false;

  pinMode(wakePin_, INPUT_PULLUP);
  ESP_LOGI("POWER","Wake cause: %d\n", esp_sleep_get_wakeup_cause());

  enabled_ = (imu_ != nullptr && imu_->isReady());
  if (!enabled_) {
    ESP_LOGI("POWER","SleepService disabled (no IMU)\n");
  }
}

void SleepService::service() {
  if (!enabled_) return;

  if (imu_->consumeMotionEvent()) {
    caffeine = true;
  }

  // Force deep sleep: hold UP for 5 seconds (skip if USB connected for dev safety)
  if (inputs_ && inputs_->heldMs(0) >= Power::Policy::kForceDeepSleepHoldMs) {
    ESP_LOGI("POWER","Force deep sleep (UP held 5s)\n");
    enterDeepSleep();
    return;
  }

  bool anyButton = inputs_ &&
      (inputs_->buttons().up || inputs_->buttons().down ||
       inputs_->buttons().left || inputs_->buttons().right);


  if (caffeine || anyButton || Serial || isUsbConnected()) {
    caffeine = false;
    lastMotionMs_ = millis();
    if (displayDimmed_) {
      if (matrix_) {
        matrix_->setBrightness(savedBrightness_);
      }
      displayDimmed_ = false;
    }
    return;
  }

  const uint32_t idleMs = millis() - lastMotionMs_;

  // Unpaired badges (QR screen) deep sleep much sooner to save battery
  // on flashed badges waiting to be handed out.
  const uint32_t effectiveDeepSleepMs =
      (badgeState == BADGE_UNPAIRED) ? Power::Policy::kUnpairedDeepSleepMs
                                    : deepSleepAfterNoMotionMs_;

  if (idleMs >= effectiveDeepSleepMs) {
    enterDeepSleep();
    return;
  }


  // Dim LED matrix after idle timeout; ESP PM auto-light-sleep handles
  // CPU sleep while WiFi modem sleep keeps the connection alive.
  if (idleMs >= lightSleepAfterNoMotionMs_ && !displayDimmed_) {
    ESP_LOGI("POWER","Dimming LED matrix (%lu ms idle)\n", (unsigned long)idleMs);
    if (matrix_) {
      savedBrightness_ = matrix_->getBrightness();
      matrix_->setBrightness(Power::Policy::kLedMatrixDimBrightness);
    }
    displayDimmed_ = true;
  }
}

const char* SleepService::name() const { return "SleepService"; }


void SleepService::processDeferredWake() {}

void SleepService::requestDeepSleep() {
  if (!enabled_) {
    ESP_LOGI("POWER", "Deep sleep requested while motion sleep service is disabled\n");
    pinMode(wakePin_, INPUT_PULLUP);
  }
  ESP_LOGI("POWER","Deep sleep requested from UI\n");
  enterDeepSleep();
}

void SleepService::preparePeripheralsForDeepSleep() {
  if (matrix_ != nullptr) {
    matrix_->blankHardware();
  }
  if (display_ != nullptr) {
    display_->clearDisplay();
    display_->display();
    display_->setPowerSave(true);
    // delay(2);
  }
}

void SleepService::enterDeepSleep() {
   ESP_LOGI("POWER","Deep sleep idle reached (%lu ms)\n", (unsigned long)(millis() - lastMotionMs_));

  preparePeripheralsForDeepSleep();

  // Build EXT1 mask: EXT0 only observes a single RTC pin; on Echo / Charlie the
  // face buttons diode-OR onto INT_PWR_PIN (and may share MCU_SLEEP_PIN) while
  // INT_GP_PIN is the accelerometer IRQ. Wake must include every net that pulls
  // low on user activity or we never leave deep sleep until IMU twitch.
  uint64_t ext1_mask = (1ULL << wakePin_);

#if defined(INT_PWR_PIN)
  if (static_cast<int>(wakePin_) != INT_PWR_PIN) {
    ext1_mask |= (1ULL << static_cast<unsigned>(INT_PWR_PIN));
  }
#endif

#if defined(MCU_SLEEP_PIN)
#  if defined(SYSOFF_PIN) && (MCU_SLEEP_PIN == SYSOFF_PIN)
  // Boot holds SYSOFF as OUTPUT LOW; release so the diode button bus can idle
  // high (pull-up) and pulse low without fighting a driven low.
  pinMode(SYSOFF_PIN, INPUT_PULLUP);
#  endif
  if (static_cast<int>(wakePin_) != MCU_SLEEP_PIN
#  if defined(INT_PWR_PIN)
      && MCU_SLEEP_PIN != INT_PWR_PIN
#  endif
      ) {
    ext1_mask |= (1ULL << static_cast<unsigned>(MCU_SLEEP_PIN));
  }
#endif

#if defined(INT_PWR_PIN)
  // If a wake line is already low when EXT1 armed (e.g. user still pressing a
  // button after "force sleep" hold), deep sleep exits immediately — wait a
  // short window for release before configuring RTC.
  {
    constexpr uint16_t kMaxWaitMs = 400;
    const uint32_t waitStartMs = millis();
    while (((millis() - waitStartMs) < kMaxWaitMs)
           && (digitalRead(static_cast<uint8_t>(INT_PWR_PIN)) == LOW)) {
      delay(5);
    }
    if (digitalRead(static_cast<uint8_t>(INT_PWR_PIN)) == LOW) {
      ESP_LOGI("POWER",
               "Wake arm: INT_PWR_PIN still LOW after %u ms; sleep may bounce\n",
               static_cast<unsigned>(kMaxWaitMs));
    }
  }
#endif

  for (unsigned bit = 0; bit < 64u; ++bit) {
    if (((ext1_mask >> bit) & 1ULL) == 0) {
      continue;
    }
    if (!isLineStableHigh(static_cast<uint8_t>(bit), 6, 2)) {
      ESP_LOGI("POWER",
               "Deep sleep pre-arm warning; RTC wake GPIO %u not stably high, continuing\n",
               static_cast<unsigned>(bit));
    }
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  for (unsigned bit = 0; bit < 64u; ++bit) {
    if (((ext1_mask >> bit) & 1ULL) == 0) {
      continue;
    }
    const gpio_num_t g = static_cast<gpio_num_t>(bit);
    rtc_gpio_init(g);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(g);
    rtc_gpio_pulldown_dis(g);
  }

  const esp_err_t wakeCfg =
      esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  if (wakeCfg != ESP_OK) {
    ESP_LOGI("POWER", "Wake config failed EXT1 mask=0x%016llx err=%d\n",
             static_cast<unsigned long long>(ext1_mask), static_cast<int>(wakeCfg));
    return;
  }

  for (unsigned bit = 0; bit < 64u; ++bit) {
    if (((ext1_mask >> bit) & 1ULL) == 0) {
      continue;
    }
    if (!isLineStableHigh(static_cast<uint8_t>(bit), 4, 1)) {
      ESP_LOGI("POWER",
               "Deep sleep arm warning; GPIO %u dipped low during arm, continuing\n",
               static_cast<unsigned>(bit));
    }
  }

  if (display_ != nullptr) {
    display_->clearDisplay();
    display_->display();
    display_->setPowerSave(true);
    delay(20);
  }

  ESP_LOGI("POWER", "Sleeping; EXT1 wake mask=0x%016llx (ANY LOW)\n",
           static_cast<unsigned long long>(ext1_mask));
  Serial.flush();
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  BatteryGauge
// ═══════════════════════════════════════════════════════════════════════════════

#if defined(BADGE_HAS_BATTERY_GAUGE) && defined(BADGE_ECHO)
// ── ADC voltage-divider gauge — main-CPU adapter for BatteryAlgorithm ───────
//
// All math (median, IIR, presence debounce, probe classification)
// lives in firmware/src/hardware/battery/BatteryAlgorithm.h and is shared
// with a future ULP-RISC-V port. This file owns only the four HAL primitives:
//   - ADC reads via analogRead(BATT_VOLTAGE_PIN)
//   - CE drive via digitalWrite(CE_PIN, ...)
//   - PGOOD / STAT reads via digitalRead
//   - micros() deadlines (no blocking delays in service())

bool BatteryGauge::begin() {
  analogSetAttenuation(ADC_11db);
  pinMode(BATT_VOLTAGE_PIN, INPUT);

  // CE drives the BQ24079 enable line during the active presence probe.
  // Idle low = charger enabled.
  pinMode(CE_PIN, OUTPUT);
  digitalWrite(CE_PIN, LOW);

#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
  Power::initChargerTelemetryPins();
#endif

  bat_presence_init(&presence_);
  bat_filter_zero(&filter_);
  flags_ = 0;
  probeTickCounter_ = 0;
  probePhase_ = kProbeIdle;
  probeSampleIdx_ = 0;

  ready_ = true;
  service();
  // begin() fires service() once: with USB attached that usually arms kProbeSampling
  // with CE asserted. We are not registered on the cooperative scheduler yet, so crank
  // the probe FSM to kProbeIdle here (bounded); otherwise charger stays gated and the
  // first open-circuit sample waits until many main-loop iterations (~sch_low divisor).
  for (unsigned k = 0; k < 64u && probePhase_ != kProbeIdle; ++k) {
    service();
  }
  chargerPgoodPrev_ = (digitalRead(CHG_GOOD_PIN) == LOW);
  return true;
}

namespace {

// Inter-sample gap and BQ resettle window — were inline delays inside the
// previous blocking runProbe(). The phase machine in service() arms
// micros() deadlines from these.
static constexpr uint32_t kProbeInterSampleUs = BAT_PROBE_WINDOW_US / (BAT_PROBE_SAMPLES - 1);
static constexpr uint32_t kProbeResettleUs    = 2000;

// HAL: take one ADC read per service tick and expose a rolling BAT_SAMPLE_N
// window for median-of-N filtering. This keeps BatteryGauge::service()
// cooperative; adc_spread below reflects recent-window drift/noise rather
// than an instantaneous burst.
void collectMedianWindow(uint32_t* samples) {
  static uint32_t history[BAT_SAMPLE_N];
  static uint8_t  next_index   = 0;
  static bool     initialized  = false;

  const uint32_t sample = (uint32_t)analogRead(BATT_VOLTAGE_PIN);

  if (!initialized) {
    for (uint8_t i = 0; i < BAT_SAMPLE_N; i++) {
      history[i] = sample;
    }
    initialized = true;
    next_index  = 1 % BAT_SAMPLE_N;
  } else {
    history[next_index] = sample;
    next_index = (uint8_t)((next_index + 1) % BAT_SAMPLE_N);
  }

  for (uint8_t i = 0; i < BAT_SAMPLE_N; i++) {
    samples[i] = history[(next_index + i) % BAT_SAMPLE_N];
  }
}

}  // namespace

void BatteryGauge::service() {
  // ── Read charger status pins ──
  const bool pgood    = digitalRead(CHG_GOOD_PIN) == LOW;
  const bool stat_raw = digitalRead(CHG_STAT_PIN) == LOW;
  const bool stat_low = pgood && stat_raw;

  // First open-circuit sample after VBUS attaches: can't wait sch_low divisor
  // ticks—the probe burst would dribble across many main-loop passes. Bump
  // counter and finish the CE-high FSM in this call chain (recursive, bounded).
  bool usb_charge_rise = false;
  if (!inUsbProbeDrainBurst_) {
    usb_charge_rise = (pgood && !chargerPgoodPrev_);
    if (usb_charge_rise) {
      probeTickCounter_ = 0;
    }
    chargerPgoodPrev_ = pgood;
  }

  // ── Active probe schedule (only meaningful with USB present) ──
  // Phase machine driven by micros() deadlines. service() takes at most
  // Policy::probeSamplesPerTick ADC reads per call during kProbeSampling.
  // A verdict is delivered ONLY on the tick that drains the resettle
  // phase; every other tick during the probe runs with verdict=-1,
  // have_probe_sample=false, which is identical to "no probe scheduled
  // this tick" — the presence/filter pipeline below already handles that
  // case correctly.
  //
  // ── Probe duty cycle ──────────────────────────────────────────────
  // CE is held high for the entire duration from kProbeSampling entry
  // through the end of kProbeResettle. While CE is high the BQ24079 is
  // disabled and the cell is not charging, so every wall-clock millisecond
  // spent in those phases is pure charging loss when USB is present.
  //
  // The dominant cost is NOT the probe burst itself (BAT_PROBE_WINDOW_US,
  // hundreds of µs) — it's the inter-tick gaps. With probeSamplesPerTick=1,
  // the state machine takes one ADC read per service() call and yields,
  // stretching the burst across (BAT_PROBE_SAMPLES - 1) scheduler periods.
  // At a ~25 ms tick cadence and 5 samples, that's ~100 ms of CE-high time
  // per probe instead of the ~hundreds of µs the burst actually requires.
  //
  // Policy::probeSamplesPerTick batches multiple reads into a single tick,
  // busy-waiting on micros() between samples within the batch. The spin is
  // intentional: yielding mid-batch defeats the purpose because CE stays
  // high across the yield. The inter-sample gap is bounded by
  // kProbeInterSampleUs (tens of µs), so the spin is short relative to the
  // cost of an additional CE-high tick.
  //
  // Default of 3 lands near the knee of the duty-cycle curve for typical
  // BAT_PROBE_SAMPLES values: most of the win, with worst-case service()
  // time still well under one scheduler period. Setting it to
  // BAT_PROBE_SAMPLES collapses the entire probe into one tick — fine if
  // service() has the budget, but no further duty-cycle benefit beyond
  // covering the final inter-tick gap.
  //
  // DO NOT "optimize" the busy-wait inside kProbeSampling into a yield.
  // That reverts to the per-tick behavior and quietly multiplies CE-high
  // time by BAT_PROBE_SAMPLES.
  int8_t   probe_verdict     = -1;
  uint32_t probe_sample      = 0;
  bool     have_probe_sample = false;

  if (!pgood) {
    // USB removed — abort any in-flight probe so we re-arm cleanly on
    // the next pgood edge. Make sure CE is back low (charger enabled).
    if (probePhase_ != kProbeIdle) {
      digitalWrite(CE_PIN, LOW);
      probePhase_     = kProbeIdle;
      probeSampleIdx_ = 0;
    }
    probeTickCounter_ = 0;
  } else {
    const uint32_t now_us = micros();
    switch (probePhase_) {
      case kProbeIdle:
        if (probeTickCounter_ == 0) {
          // Kick off a fresh probe: drive CE high, take sample 0
          // immediately, schedule sample 1.
          digitalWrite(CE_PIN, HIGH);
          probeSamples_[0]     = (uint32_t)analogRead(BATT_VOLTAGE_PIN);
          probeSampleIdx_      = 1;
          probeNextSampleAtUs_ = now_us + kProbeInterSampleUs;
          probePhase_          = kProbeSampling;
        }
        // Counter advances only on idle-visit ticks, so the probe period
        // is measured between probe *completions*, not starts. Net effect:
        // the BQ24079 charges more (lower CE-high duty cycle) than the
        // original synchronous design.
        probeTickCounter_ = (probeTickCounter_ + 1) % BAT_PROBE_PERIOD_TICKS;
        break;

      case kProbeSampling: {
        // Collect up to Policy::probeSamplesPerTick ADC reads this tick. The
        // whole point of batching is to minimize CE-high time — every yield
        // back to the scheduler with CE still high is pure charging loss,
        // since the BQ24079 is disabled until the batch (and resettle)
        // finishes. Spin on the inter-sample gap rather than yielding; the
        // gap is bounded by kProbeInterSampleUs.
        uint8_t budget = Power::Policy::probeSamplesPerTick;
        if (budget == 0) budget = 1;  // 0 would stall the probe forever with CE high

        while (budget-- > 0) {
          // Spin until the inter-sample deadline. Bounded by
          // kProbeInterSampleUs (tens of µs) — small compared to the cost
          // of an extra CE-high tick. Signed compare against the deadline
          // so micros() wraparound (~71 minutes) doesn't strand us mid-probe.
          while ((int32_t)(micros() - probeNextSampleAtUs_) < 0) {
            // tight spin
          }

          probeSamples_[probeSampleIdx_] = (uint32_t)analogRead(BATT_VOLTAGE_PIN);
          probeSampleIdx_++;

          if (probeSampleIdx_ >= BAT_PROBE_SAMPLES) {
            // Batch finished the whole probe — release CE immediately and
            // start the resettle window.
            digitalWrite(CE_PIN, LOW);
            probeReadyAtUs_ = micros() + kProbeResettleUs;
            probePhase_     = kProbeResettle;
            break;
          }
          probeNextSampleAtUs_ = micros() + kProbeInterSampleUs;
        }
        break;
      }

      case kProbeResettle:
        if ((int32_t)(now_us - probeReadyAtUs_) >= 0) {
          // Probe complete — classify and feed the rest of the pipeline
          // exactly as the old synchronous path did.
          probe_verdict     = bat_classify_probe(probeSamples_,
                                                 BAT_PROBE_SAMPLES,
                                                 &probe_sample);
          have_probe_sample = true;
          probePhase_       = kProbeIdle;
          probeSampleIdx_   = 0;
        }
        break;
    }
  }

  // ── Sampling path ──
  uint32_t raw        = 0;
  uint32_t adc_spread = 0;
  bool     have_raw   = false;

  if (!pgood) {
    // BQ passes the cell straight through; analogRead IS cell voltage.
    // On USB (below: pgood) do NOT substitute this path — SOC must come from
    // the CE-high probe burst (charger paused while sampling).
    uint32_t samples[BAT_SAMPLE_N];
    collectMedianWindow(samples);
    bat_sort_small(samples, BAT_SAMPLE_N);
    raw        = bat_median_pair(samples, BAT_SAMPLE_N);
    adc_spread = samples[BAT_SAMPLE_N - 1] - samples[0];
    have_raw   = true;
  } else if (have_probe_sample) {
    // BQ regulates VBAT to non-cell values between probes — only the probe
    // window is trustworthy. Use its median as the raw reading for this tick.
    raw        = probe_sample;
    have_raw   = true;
    adc_spread = 0;
  }

  // ── Presence state machine ──
  bat_presence_inputs_t in;
  in.pgood         = pgood;
  in.stat_raw      = stat_raw;
  in.have_raw      = have_raw;
  in.raw           = raw;
  in.adc_spread    = adc_spread;
  in.probe_verdict = probe_verdict;

  bat_presence_outputs_t out;
  bat_presence_update(&presence_, &in, &out);

  // ── Edge-triggered reset on present->absent ──
  // Without this the host would see stale "last known-good" raw/filtered/pct
  // long after a battery has been removed, with only the bat_present flag
  // indicating they're meaningless.
  if (out.edge_present_to_absent) {
    bat_filter_zero(&filter_);
    last_reported_pct_ = UINT32_MAX;
  }

  const bool trustworthy_tick = bat_sample_trustworthy(pgood,
                                                         have_raw,
                                                         have_probe_sample,
                                                         out.target_present,
                                                         probe_verdict);

  // ── IIR + pct update (trustworthy samples refine gauge state) ────────────
  if (trustworthy_tick) {
    bat_filter_update(&filter_, raw);
    updateAdcStats(raw);

    uint32_t raw_pct = filter_.bat_pct;
    const bool clamp_seeded = (last_reported_pct_ != UINT32_MAX);
    if (clamp_seeded && pgood && raw_pct < last_reported_pct_) {
      filter_.bat_pct = last_reported_pct_;  // charging: don't drop
    } else if (clamp_seeded && !pgood &&
               raw_pct > last_reported_pct_) {
      filter_.bat_pct = last_reported_pct_;  // discharging: don't rise
    }
  } else if (have_raw && raw >= (uint32_t)BAT_RAW_MIN) {
    // Rough live estimate whenever we have a divider sample (USB: between
    // probes; refine on next trustworthy CE-off burst).
    filter_.adc_raw = raw;
    filter_.bat_mv  = bat_adc_to_mv(raw);
    uint32_t est    = bat_raw_to_pct(raw);
    if (est > filter_.bat_pct) filter_.bat_pct = est;
  }

  bat_filter_apply_mv_soc_floor(&filter_);

  if (trustworthy_tick) {
    last_reported_pct_ = filter_.bat_pct;
  }

  flags_ = bat_compose_flags(pgood, stat_raw, stat_low, out.latched);

  // ── Threshold classification (telemetry / cached band) ────────────────────
  // WiFi is not blocked on SUB; this state is retained for diagnostics.
  battery_threshold_t next = bat_classify_threshold(filter_.bat_pct);
  bool entering_sub_no_usb = (cached_threshold_ != BAT_THRESH_SUB &&
                              next == BAT_THRESH_SUB && !pgood);
  bool leaving_sub = (cached_threshold_ == BAT_THRESH_SUB &&
                      (next != BAT_THRESH_SUB || pgood));
  if (entering_sub_no_usb) {
    // Serial.println("[Power] battery SUB and no USB — disconnecting WiFi");
    // wifiService.disconnect();
  } else if (leaving_sub) {
    // Serial.println("[Power] battery recovered or USB present — re-arming WiFi");
    // wifiService.armForReconnect();
  }
  cached_threshold_ = next;

// #if defined(BADGE_POWER_TELEMETRY)
  logAdcTelemetryIfDue();
// #if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
  Power::logChargerTelemetryIfDue();
// #endif
// #endif

  if (usb_charge_rise) {
    inUsbProbeDrainBurst_ = true;
    for (unsigned k = 0; k < 64u && probePhase_ != kProbeIdle; ++k) {
      service();
    }
    inUsbProbeDrainBurst_ = false;
  }
}

void BatteryGauge::updateAdcStats(uint32_t raw) {
  if (sampleCount_ == 0) {
    rawAdcMin_ = raw;
    rawAdcMax_ = raw;
  } else {
    if (raw < rawAdcMin_) rawAdcMin_ = raw;
    if (raw > rawAdcMax_) rawAdcMax_ = raw;
  }
  if (sampleCount_ < UINT32_MAX) {
    sampleCount_++;
  }
}

void BatteryGauge::logAdcTelemetryIfDue(uint32_t intervalMs) {
  const uint32_t now = millis();
  if (lastAdcTelemetryLogMs_ != 0 &&
      now - lastAdcTelemetryLogMs_ < intervalMs) {
    return;
  }
  lastAdcTelemetryLogMs_ = now;
  const uint32_t raw_min = (sampleCount_ == 0) ? 0 : rawAdcMin_;
  const uint32_t raw_max = (sampleCount_ == 0) ? 0 : rawAdcMax_;
  ESP_LOGI("POWER","Power: battery raw=%lu filtered=%lu mv=%lu pct=%lu present=%d pgood=%d stat_raw=%d stat_low=%d raw_min=%lu raw_max=%lu samples=%lu\n",
            (unsigned long)filter_.adc_raw,
            (unsigned long)(filter_.filtered_raw >> BAT_IIR_SHIFT),
            (unsigned long)filter_.bat_mv,
            (unsigned long)filter_.bat_pct,
            batteryPresent() ? 1 : 0,
            (flags_ & MASK_PGOOD) ? 1 : 0,
            (flags_ & MASK_STAT_RAW) ? 1 : 0,
            (flags_ & MASK_STAT_LOW) ? 1 : 0,
            (unsigned long)raw_min,
            (unsigned long)raw_max,
            (unsigned long)sampleCount_);
}

const char* BatteryGauge::name() const { return "BatteryGauge"; }

battery_threshold_t BatteryGauge::threshold() {
  // Post-ready: the service tick already updated cached_threshold_.
  if (ready_) return cached_threshold_;

  // Pre-ready bootup path: no IIR, no probe history. Take BAT_SAMPLE_N raw
  // ADC reads in a tight loop, classify directly off the median. Speed
  // first; the only goal is "is the cell so low we'll brown out the rail
  // when the radio kicks on".
  analogSetAttenuation(ADC_11db);
  pinMode(BATT_VOLTAGE_PIN, INPUT);
  uint32_t samples[BAT_SAMPLE_N];
  for (uint8_t i = 0; i < BAT_SAMPLE_N; i++) {
    samples[i] = (uint32_t)analogRead(BATT_VOLTAGE_PIN);
  }
  return bat_bootup_threshold(samples, BAT_SAMPLE_N);
}

extern BatteryGauge batteryGauge;

namespace Power {
bool wifiAllowed() {
  return true;
}
}  // namespace Power

#elif defined(BADGE_HAS_BATTERY_GAUGE)
// ── MAX17048 I2C fuel gauge ──────────────────────────────────────────────────

bool BatteryGauge::begin() {
  ready_ = gauge_.begin(&Wire);
  if (!ready_) {
    socPercent_ = NAN;
    cellVoltage_ = NAN;
    return false;
  }

  gauge_.setResetVoltage(kBatteryVReset);
  gauge_.setAlertVoltages(kBatteryVEmpty, kBatteryVFull);
  gauge_.quickStart();

  delay(10);

  service();
  return true;
}

void BatteryGauge::service() {
  socPercent_ = gauge_.cellPercent();
  cellVoltage_ = gauge_.cellVoltage();
}

const char* BatteryGauge::name() const { return "BatteryGauge"; }
#endif  // BADGE_HAS_BATTERY_GAUGE
#endif  // BADGE_HAS_SLEEP_SERVICE