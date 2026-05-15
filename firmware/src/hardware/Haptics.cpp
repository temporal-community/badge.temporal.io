#include "Haptics.h"

#include "HardwareConfig.h"
#include "Inputs.h"
#include "../infra/Scheduler.h"
#include "oled.h"
#include <cstdio>
#include <driver/mcpwm_prelude.h>

#ifdef BADGE_HAS_HAPTICS

extern Scheduler scheduler;

namespace {

/// 1 MHz ticks (1 µs), same scale as ESP-IDF `mcpwm_servo_control` — PWM freq = kMcpwmResolutionHz / period_ticks.
constexpr uint32_t kMcpwmResolutionHz = 1000000;

uint32_t pwmFreqHz_ = Haptics::kDefaultPwmFreqHz;
uint32_t configuredPwmFreqHz_ = Haptics::kDefaultPwmFreqHz;
uint32_t pulseEndMs_ = 0;
bool mcpwmReady_ = false;
uint8_t steadyDuty255_ = 0;
uint8_t strength255_ = Haptics::kDefaultStrength;
uint16_t clickPulseMs_ = Haptics::kDefaultPulseMs;
uint8_t repeatStrengthPercent_ = Haptics::kDefaultRepeatStrengthPercent;
bool hapticsEnabled_ = true;
bool buttonFeedbackEnabled_ = true;
bool pulseFreqOverridden_ = false;
bool coilTonePlaying_ = false;
uint32_t coilToneEndMs_ = 0;

mcpwm_timer_handle_t mcpwmTimer_ = nullptr;
mcpwm_oper_handle_t mcpwmOper_ = nullptr;
mcpwm_cmpr_handle_t mcpwmCmpr_ = nullptr;
mcpwm_gen_handle_t mcpwmGen_ = nullptr;
uint32_t mcpwmPeriodTicks_ = 0;

uint32_t maxHardwareDuty() { return 255U; }

uint32_t duty255ToHardware(uint8_t duty255) {
  return (static_cast<uint32_t>(duty255) * maxHardwareDuty()) / 255U;
}

void ensureGpioOutput() { pinMode(MOTOR_PIN, OUTPUT); }

void applyDigitalLevel(bool high) {
  ensureGpioOutput();
  digitalWrite(MOTOR_PIN, high ? HIGH : LOW);
}

void destroyMcpwm() {
  if (mcpwmTimer_) {
    mcpwm_timer_start_stop(mcpwmTimer_, MCPWM_TIMER_STOP_EMPTY);
    mcpwm_timer_disable(mcpwmTimer_);
  }
  if (mcpwmGen_) {
    mcpwm_del_generator(mcpwmGen_);
    mcpwmGen_ = nullptr;
  }
  if (mcpwmCmpr_) {
    mcpwm_del_comparator(mcpwmCmpr_);
    mcpwmCmpr_ = nullptr;
  }
  if (mcpwmOper_) {
    mcpwm_del_operator(mcpwmOper_);
    mcpwmOper_ = nullptr;
  }
  if (mcpwmTimer_) {
    mcpwm_del_timer(mcpwmTimer_);
    mcpwmTimer_ = nullptr;
  }
  mcpwmPeriodTicks_ = 0;
  mcpwmReady_ = false;
}

bool buildMcpwm() {
  uint32_t target_resolution = kMcpwmResolutionHz;
  uint32_t ticks = target_resolution / pwmFreqHz_;
  if (ticks > 65000) {
    target_resolution = pwmFreqHz_ * 65000;
    if (target_resolution < 1000) target_resolution = 1000;
    ticks = target_resolution / pwmFreqHz_;
  }
  mcpwmPeriodTicks_ = ticks;
  if (mcpwmPeriodTicks_ < 4) {
    mcpwmPeriodTicks_ = 4;
  }

  mcpwm_timer_config_t timer_config = {
      .group_id = 0,
      .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
      .resolution_hz = target_resolution,
      .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
      .period_ticks = mcpwmPeriodTicks_,
  };
  if (mcpwm_new_timer(&timer_config, &mcpwmTimer_) != ESP_OK) {
    return false;
  }

  mcpwm_operator_config_t operator_config = {
      .group_id = 0,
  };
  if (mcpwm_new_operator(&operator_config, &mcpwmOper_) != ESP_OK) {
    destroyMcpwm();
    return false;
  }
  if (mcpwm_operator_connect_timer(mcpwmOper_, mcpwmTimer_) != ESP_OK) {
    destroyMcpwm();
    return false;
  }

  mcpwm_comparator_config_t comparator_config = {};
  comparator_config.flags.update_cmp_on_tez = 1;
  if (mcpwm_new_comparator(mcpwmOper_, &comparator_config, &mcpwmCmpr_) != ESP_OK) {
    destroyMcpwm();
    return false;
  }

  mcpwm_generator_config_t generator_config = {
      .gen_gpio_num = MOTOR_PIN,
  };
  if (mcpwm_new_generator(mcpwmOper_, &generator_config, &mcpwmGen_) != ESP_OK) {
    destroyMcpwm();
    return false;
  }

  if (mcpwm_generator_set_action_on_timer_event(
          mcpwmGen_, MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY,
                                                  MCPWM_GEN_ACTION_HIGH)) != ESP_OK) {
    destroyMcpwm();
    return false;
  }
  if (mcpwm_generator_set_action_on_compare_event(
          mcpwmGen_, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, mcpwmCmpr_, MCPWM_GEN_ACTION_LOW)) !=
      ESP_OK) {
    destroyMcpwm();
    return false;
  }

  // Hold output low until a non-zero duty (compare==0 with EMPTY→HIGH is ambiguous).
  if (mcpwm_generator_set_force_level(mcpwmGen_, 0, true) != ESP_OK) {
    destroyMcpwm();
    return false;
  }

  if (mcpwm_timer_enable(mcpwmTimer_) != ESP_OK) {
    destroyMcpwm();
    return false;
  }
  if (mcpwm_timer_start_stop(mcpwmTimer_, MCPWM_TIMER_START_NO_STOP) != ESP_OK) {
    destroyMcpwm();
    return false;
  }

  mcpwmReady_ = true;
  return true;
}

bool tryInitMcpwm() {
  if (mcpwmReady_) {
    return true;
  }
  return buildMcpwm();
}

void applyMcpwmDuty(uint32_t hwDuty) {
  // hwDuty 0..255 (same scale as former LEDC 8-bit path)
  if (hwDuty == 0) {
    mcpwm_generator_set_force_level(mcpwmGen_, 0, true);
    return;
  }
  mcpwm_generator_set_force_level(mcpwmGen_, -1, true);
  uint32_t cmp = (hwDuty * mcpwmPeriodTicks_) / 255U;
  if (cmp > mcpwmPeriodTicks_) {
    cmp = mcpwmPeriodTicks_;
  }
  mcpwm_comparator_set_compare_value(mcpwmCmpr_, cmp);
}

void applyHardwareDuty(uint32_t hwDuty) {
  if (tryInitMcpwm()) {
    applyMcpwmDuty(hwDuty);
    return;
  }
  applyDigitalLevel(hwDuty > 0);
}

bool pulseActive(uint32_t nowMs) {
  return pulseEndMs_ != 0 && static_cast<int32_t>(nowMs - pulseEndMs_) < 0;
}

void updateImuState(bool motorActive) {
  static bool wasMotorActive = false;
  if (motorActive != wasMotorActive) {
    scheduler.setServiceState("IMU", !motorActive);
    wasMotorActive = motorActive;
  }
}

}  // namespace

void Haptics::begin() {
  steadyDuty255_ = 0;
  pulseEndMs_ = 0;
  strength255_ = Haptics::kDefaultStrength;
  clickPulseMs_ = Haptics::kDefaultPulseMs;
  configuredPwmFreqHz_ = Haptics::kDefaultPwmFreqHz;
  pwmFreqHz_ = configuredPwmFreqHz_;
  repeatStrengthPercent_ = Haptics::kDefaultRepeatStrengthPercent;
  hapticsEnabled_ = true;
  buttonFeedbackEnabled_ = true;
  pulseFreqOverridden_ = false;
  coilTonePlaying_ = false;
  coilToneEndMs_ = 0;
  destroyMcpwm();
  ensureGpioOutput();
  digitalWrite(MOTOR_PIN, LOW);
  if (tryInitMcpwm()) {
    applyMcpwmDuty(0);
  }
}

void Haptics::checkPulseEnd() {
  if (!hapticsEnabled_) {
    if (pulseEndMs_ != 0 || steadyDuty255_ > 0 || coilTonePlaying_) {
      off();
    }
    return;
  }

  const uint32_t nowMs = millis();
  if (pulseEndMs_ != 0 && static_cast<int32_t>(nowMs - pulseEndMs_) >= 0) {
    pulseEndMs_ = 0;
    applyHardwareDuty(duty255ToHardware(steadyDuty255_));
    if (pulseFreqOverridden_) {
      pulseFreqOverridden_ = false;
      if (pwmFreqHz_ != configuredPwmFreqHz_) {
        pwmFreqHz_ = configuredPwmFreqHz_;
        destroyMcpwm();
        ensureGpioOutput();
        buildMcpwm();
        applyHardwareDuty(duty255ToHardware(steadyDuty255_));
      }
    }
  }
  const bool motorOn = pulseActive(nowMs) || steadyDuty255_ > 0 || coilTonePlaying_;
  updateImuState(motorOn);
}

void Haptics::shortPulse(int16_t strengthOverride, int16_t durationMsOverride, int32_t freqHzOverride) {
  if (!hapticsEnabled_) return;

  // If a caller has set a steady duty (e.g. drawing haptics), don't let
  // button-press pulses stomp on the custom frequency / duty.
  if (steadyDuty255_ > 0) return;

  if (coilTonePlaying_) {
    CoilTone::noTone();
  }

  const uint8_t str = (strengthOverride < 0)
      ? strength255_
      : static_cast<uint8_t>(strengthOverride > 255 ? 255 : strengthOverride);
  const uint16_t dur = (durationMsOverride < 0)
      ? clickPulseMs_
      : static_cast<uint16_t>(durationMsOverride);

  if (str == 0 || dur == 0) return;

  if (freqHzOverride >= 0) {
    const uint32_t freq = static_cast<uint32_t>(freqHzOverride);
    if (freq != pwmFreqHz_) {
      setPwmFrequency(freq);
      pulseFreqOverridden_ = true;
    }
  } else if (pulseFreqOverridden_ || pwmFreqHz_ != configuredPwmFreqHz_) {
    setPwmFrequency(configuredPwmFreqHz_);
    pulseFreqOverridden_ = false;
  }

  const uint32_t nowMs = millis();
  pulseEndMs_ = nowMs + dur;
  applyHardwareDuty(duty255ToHardware(str));
  updateImuState(true);
}

bool Haptics::enabled() { return hapticsEnabled_; }

void Haptics::setEnabled(bool enabled) {
  if (hapticsEnabled_ == enabled) return;
  hapticsEnabled_ = enabled;
  if (!hapticsEnabled_) {
    off();
  }
}

uint8_t Haptics::strength() { return strength255_; }

void Haptics::setStrength(uint8_t strength255) { strength255_ = strength255; }

void Haptics::adjustStrength(int delta) {
  int v = static_cast<int>(strength255_) + delta;
  if (v < 0) {
    v = 0;
  } else if (v > 255) {
    v = 255;
  }
  strength255_ = static_cast<uint8_t>(v);
}

uint16_t Haptics::clickPulseDuration() { return clickPulseMs_; }

void Haptics::setClickPulseDuration(uint16_t ms) { clickPulseMs_ = ms; }

void Haptics::pulse(uint16_t durationMs, uint32_t nowMs) {
  if (!hapticsEnabled_) return;
  if (durationMs == 0) {
    return;
  }
  pulseEndMs_ = nowMs + durationMs;
  applyHardwareDuty(duty255ToHardware(strength255_));
}

void Haptics::setDuty(uint8_t duty255) {
  steadyDuty255_ = duty255;
  if (!hapticsEnabled_) {
    applyHardwareDuty(0);
    updateImuState(false);
    return;
  }
  const uint32_t nowMs = millis();
  if (!pulseActive(nowMs)) {
    applyHardwareDuty(duty255ToHardware(steadyDuty255_));
  }
}

void Haptics::setPwmFrequency(uint32_t freqHz) {
  if (freqHz < 1u) {
    freqHz = 1u;
  }
  if (freqHz > 250000u) {
    freqHz = 250000u;
  }
  pwmFreqHz_ = freqHz;

  const uint32_t nowMs = millis();
  const bool midPulse = pulseActive(nowMs);

  destroyMcpwm();
  ensureGpioOutput();

  if (!buildMcpwm()) {
    if (midPulse) {
      applyDigitalLevel(true);
    } else {
      applyHardwareDuty(duty255ToHardware(steadyDuty255_));
    }
    return;
  }

  if (midPulse) {
    applyMcpwmDuty(duty255ToHardware(strength255_));
  } else {
    applyMcpwmDuty(duty255ToHardware(steadyDuty255_));
  }
}

uint32_t Haptics::pwmFrequencyHz() { return pwmFreqHz_; }

uint8_t Haptics::steadyDuty() { return steadyDuty255_; }

void Haptics::off() {
  if (coilTonePlaying_) {
    coilTonePlaying_ = false;
    coilToneEndMs_ = 0;
  }
  steadyDuty255_ = 0;
  pulseEndMs_ = 0;
  pulseFreqOverridden_ = false;
  if (mcpwmReady_) {
    applyMcpwmDuty(0);
  } else {
    ensureGpioOutput();
    digitalWrite(MOTOR_PIN, LOW);
  }
  updateImuState(false);
}

namespace {

enum class MotorTestStep : uint8_t {
  Intro,
  ConfigStrength,
  ConfigPwmFreq,
  ConfigPulseMs,
  PulseShort,
  PulseLong,
  SteadyMid,
  FreqLow,
  FreqHigh,
  PulseAtStrength,
  Done,
};

bool motorTestActive_ = false;
MotorTestStep motorTestStep_ = MotorTestStep::Intro;
uint32_t motorTestDebounceUntilMs_ = 0;
uint32_t motorTestUpDownHoldStartMs_ = 0;
uint32_t motorTestSteadyClearMs_ = 0;

MotorTestStep motorTestNextStep(MotorTestStep s) {
  if (s == MotorTestStep::Done) {
    return MotorTestStep::Intro;
  }
  return static_cast<MotorTestStep>(static_cast<uint8_t>(s) + 1U);
}

void motorTestLeaveStep(MotorTestStep s) {
  switch (s) {
    case MotorTestStep::SteadyMid:
      motorTestSteadyClearMs_ = 0;
      Haptics::setDuty(0);
      break;
    default:
      break;
  }
}

void motorTestEnterStep(MotorTestStep s, uint32_t nowMs) {
  motorTestSteadyClearMs_ = 0;
  switch (s) {
    case MotorTestStep::Intro:
    case MotorTestStep::ConfigPulseMs:
      Haptics::off();
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      break;
    case MotorTestStep::ConfigStrength:
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      Haptics::setDuty(Haptics::strength());
      break;
    case MotorTestStep::ConfigPwmFreq:
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      Haptics::setDuty(Haptics::strength());
      break;
    case MotorTestStep::PulseShort:
      Haptics::off();
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      Haptics::pulse(Haptics::clickPulseDuration(), nowMs);
      break;
    case MotorTestStep::PulseLong:
      Haptics::off();
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      Haptics::pulse(Haptics::clickPulseDuration() * 4, nowMs);
      break;
    case MotorTestStep::SteadyMid:
      Haptics::off();
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      Haptics::setDuty(Haptics::strength() / 2);
      motorTestSteadyClearMs_ = nowMs + 900;
      break;
    case MotorTestStep::FreqLow:
      Haptics::off();
      Haptics::setPwmFrequency(80);
      Haptics::pulse(120, nowMs);
      break;
    case MotorTestStep::FreqHigh:
      Haptics::off();
      Haptics::setPwmFrequency(220);
      Haptics::pulse(120, nowMs);
      break;
    case MotorTestStep::PulseAtStrength:
      Haptics::off();
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      Haptics::pulse(100, nowMs);
      break;
    case MotorTestStep::Done:
      Haptics::setPwmFrequency(configuredPwmFreqHz_);
      Haptics::off();
      break;
    default:
      break;
  }
}

void motorTestDrawScreen(oled &d, MotorTestStep step) {
  d.clearDisplay();
  d.setCursor(0, 0);
  d.setTextSize(1);

  d.println("HAPTIC TEST");

  switch (step) {
    case MotorTestStep::Intro:
      d.println("Walkthrough");
      d.println("Y/A: prev/next");
      d.println("Hold Y+A: exit");
      break;
    case MotorTestStep::ConfigStrength: {
      d.println("Set Strength:");
      char line[24];
      std::snprintf(line, sizeof(line), "X %u B", static_cast<unsigned>(Haptics::strength()));
      d.println(line);
      break;
    }
    case MotorTestStep::ConfigPwmFreq: {
      d.println("Set PWM Freq:");
      char line[24];
      std::snprintf(line, sizeof(line), "X %u Hz B", static_cast<unsigned>(configuredPwmFreqHz_));
      d.println(line);
      break;
    }
    case MotorTestStep::ConfigPulseMs: {
      d.println("Click Pulse Duration:");
      char line[24];
      std::snprintf(line, sizeof(line), "X %u ms B", static_cast<unsigned>(Haptics::clickPulseDuration()));
      d.println(line);
      break;
    }
    case MotorTestStep::PulseShort: {
      char line[24];
      std::snprintf(line, sizeof(line), "Pulse %ums", static_cast<unsigned>(Haptics::clickPulseDuration()));
      d.println(line);
      d.println("at active str");
      break;
    }
    case MotorTestStep::PulseLong: {
      char line[24];
      std::snprintf(line, sizeof(line), "Pulse %ums", static_cast<unsigned>(Haptics::clickPulseDuration() * 4));
      d.println(line);
      d.println("at active str");
      break;
    }
    case MotorTestStep::SteadyMid: {
      char line[24];
      std::snprintf(line, sizeof(line), "Steady %u/255", static_cast<unsigned>(Haptics::strength() / 2));
      d.println(line);
      d.println("900ms then off");
      break;
    }
    case MotorTestStep::FreqLow:
      d.println("PWM 80 Hz pulse");
      d.println("low carrier");
      break;
    case MotorTestStep::FreqHigh:
      d.println("PWM 220 Hz pulse");
      d.println("high carrier");
      break;
    case MotorTestStep::PulseAtStrength: {
      char line[24];
      std::snprintf(line, sizeof(line), "str=%u X/B adj", static_cast<unsigned>(Haptics::strength()));
      d.println("Pulse @ strength");
      d.println(line);
      break;
    }
    case MotorTestStep::Done:
      d.println(" ");
      d.println("Btn: restart");
      break;
    default:
      break;
  }

  d.display();
}

}  // namespace

void Haptics::motorTestService(oled &display, const Inputs &inputs, uint32_t nowMs) {
  const Inputs::ButtonStates &b = inputs.buttons();

  if (b.y && b.a) {
    if (motorTestUpDownHoldStartMs_ == 0) {
      motorTestUpDownHoldStartMs_ = nowMs;
    } else if (nowMs - motorTestUpDownHoldStartMs_ >= 800) {
      if (motorTestActive_) {
        motorTestLeaveStep(motorTestStep_);
        motorTestActive_ = false;
        Haptics::setPwmFrequency(configuredPwmFreqHz_);
        Haptics::off();
        scheduler.setServiceState("OLED", true);
        motorTestUpDownHoldStartMs_ = 0;
        return;
      }
      motorTestActive_ = true;
      motorTestStep_ = MotorTestStep::Intro;
      motorTestDebounceUntilMs_ = nowMs + 450;
      motorTestSteadyClearMs_ = 0;
      motorTestEnterStep(MotorTestStep::Intro, nowMs);
      scheduler.setServiceState("OLED", false);
      motorTestUpDownHoldStartMs_ = 0;
    }
  } else {
    motorTestUpDownHoldStartMs_ = 0;
  }

  if (!motorTestActive_) {
    return;
  }

  if (motorTestSteadyClearMs_ != 0 && static_cast<int32_t>(nowMs - motorTestSteadyClearMs_) >= 0) {
    Haptics::setDuty(0);
    motorTestSteadyClearMs_ = 0;
  }

  const Inputs::ButtonEdges &e = inputs.edges();
  const bool debouncing =
      motorTestDebounceUntilMs_ != 0 && static_cast<int32_t>(nowMs - motorTestDebounceUntilMs_) < 0;
  if (!debouncing && !(b.y && b.a)) {
    bool handledNav = false;

    if (motorTestStep_ == MotorTestStep::ConfigStrength) {
      if (e.xPressed) {
        Haptics::adjustStrength(-Haptics::kStrengthStep);
        Haptics::setDuty(Haptics::strength());
        handledNav = true;
      }
      if (e.bPressed) {
        Haptics::adjustStrength(Haptics::kStrengthStep);
        Haptics::setDuty(Haptics::strength());
        handledNav = true;
      }
    } else if (motorTestStep_ == MotorTestStep::ConfigPwmFreq) {
      if (e.xPressed) {
        uint32_t step = configuredPwmFreqHz_ > 10000 ? 1000 : (configuredPwmFreqHz_ > 1000 ? 500 : (configuredPwmFreqHz_ > 100 ? 50 : 10));
        configuredPwmFreqHz_ = configuredPwmFreqHz_ > step ? configuredPwmFreqHz_ - step : 1;
        Haptics::setPwmFrequency(configuredPwmFreqHz_);
        handledNav = true;
      }
      if (e.bPressed) {
        uint32_t step = configuredPwmFreqHz_ >= 10000 ? 1000 : (configuredPwmFreqHz_ >= 1000 ? 500 : (configuredPwmFreqHz_ >= 100 ? 50 : 10));
        configuredPwmFreqHz_ += step;
        if (configuredPwmFreqHz_ > 250000) configuredPwmFreqHz_ = 250000;
        Haptics::setPwmFrequency(configuredPwmFreqHz_);
        handledNav = true;
      }
    } else if (motorTestStep_ == MotorTestStep::ConfigPulseMs) {
      if (e.xPressed) {
        Haptics::setClickPulseDuration(Haptics::clickPulseDuration() > 5 ? Haptics::clickPulseDuration() - 5 : 5);
        handledNav = true; Haptics::pulse(Haptics::clickPulseDuration(), nowMs);
      }
      if (e.bPressed) {
        Haptics::setClickPulseDuration(Haptics::clickPulseDuration() + 5);
        handledNav = true; Haptics::pulse(Haptics::clickPulseDuration(), nowMs);
      }
    }

    if (handledNav) {
      motorTestDebounceUntilMs_ = nowMs + 100;
    } else if (e.yPressed || e.aPressed || e.xPressed || e.bPressed) {
      // Step navigation logic: A/B = next, X/Y = previous.
      motorTestLeaveStep(motorTestStep_);
      if (e.aPressed || e.bPressed) {
        motorTestStep_ = motorTestNextStep(motorTestStep_);
      } else if (e.yPressed || e.xPressed) {
        if (motorTestStep_ == MotorTestStep::Intro) {
          motorTestStep_ = MotorTestStep::Done;
        } else {
          motorTestStep_ = static_cast<MotorTestStep>(static_cast<uint8_t>(motorTestStep_) - 1U);
        }
      }
      motorTestEnterStep(motorTestStep_, nowMs);
      motorTestDebounceUntilMs_ = nowMs + 280;
    }
  }

  motorTestDrawScreen(display, motorTestStep_);
}

bool Haptics::motorTestIsActive() { return motorTestActive_; }

uint8_t Haptics::repeatStrengthPercent() { return repeatStrengthPercent_; }

void Haptics::setRepeatStrengthPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  repeatStrengthPercent_ = percent;
}

bool Haptics::buttonFeedbackEnabled() { return buttonFeedbackEnabled_; }

void Haptics::setButtonFeedbackEnabled(bool enabled) { buttonFeedbackEnabled_ = enabled; }

// ---------------------------------------------------------------------------
// CoilTone — audible tones from the motor coil at very low duty
// ---------------------------------------------------------------------------

void CoilTone::tone(uint32_t freqHz, uint16_t durationMs, uint8_t duty) {
  if (!Haptics::enabled()) return;

  pulseEndMs_ = 0;
  pulseFreqOverridden_ = false;

  Haptics::setPwmFrequency(freqHz);
  coilTonePlaying_ = true;
  coilToneEndMs_ = (durationMs > 0) ? millis() + durationMs : 0;
  applyHardwareDuty(duty255ToHardware(duty));
  updateImuState(true);
}

void CoilTone::noTone() {
  if (!coilTonePlaying_) return;
  coilTonePlaying_ = false;
  coilToneEndMs_ = 0;
  applyHardwareDuty(0);
  if (pwmFreqHz_ != configuredPwmFreqHz_) {
    Haptics::setPwmFrequency(configuredPwmFreqHz_);
  }
  updateImuState(steadyDuty255_ > 0);
}

bool CoilTone::isPlaying() { return coilTonePlaying_; }

void CoilTone::checkToneEnd() {
  if (!coilTonePlaying_ || coilToneEndMs_ == 0) return;
  if (static_cast<int32_t>(millis() - coilToneEndMs_) >= 0) {
    noTone();
  }
}

// ---------------------------------------------------------------------------
// HapticsService — scheduled housekeeping
// ---------------------------------------------------------------------------

extern bool gGuiActive;

void HapticsService::service() {
  Haptics::checkPulseEnd();
  CoilTone::checkToneEnd();
  if (display_ && inputs_ && !gGuiActive) {
    Haptics::motorTestService(*display_, *inputs_, millis());
  }
}

#endif  // BADGE_HAS_HAPTICS
