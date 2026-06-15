#ifndef HAPTICS_H
#define HAPTICS_H

#include "HardwareConfig.h"

#ifdef BADGE_HAS_HAPTICS
// ── Full haptics driver (Charlie board only) ─────────────────────────────────
#include <Arduino.h>
#include <stdint.h>
#include "../infra/Scheduler.h"

class oled;
class Inputs;

/// Vibration motor on MOTOR_PIN (ESP-IDF MCPWM + timed bursts; digital fallback if init fails).
namespace Haptics {

constexpr uint16_t kDefaultPulseMs = 35;
constexpr uint32_t kDefaultPwmFreqHz = 80;
constexpr uint8_t kDefaultStrength = 155;
constexpr uint8_t kStrengthStep = 5;
constexpr uint8_t kDefaultRepeatStrengthPercent = 40;

void begin();

/// Master switch for the vibration motor. When disabled, all pulses, steady
/// duty, and coil tones are suppressed.
bool enabled();
void setEnabled(bool enabled);

/// Fire-and-forget haptic pulse.  Pass -1 for any parameter to use the
/// configured default (strength / duration / frequency).
void shortPulse(int16_t strength = -1, int16_t durationMs = -1, int32_t freqHz = -1);

/// Call periodically (e.g. from the main loop) to auto-stop expired pulses
/// and keep IMU suspend state consistent.
void checkPulseEnd();

/// Peak level for `pulse()` / button clicks (0..255).
uint8_t strength();
void setStrength(uint8_t strength255);
void adjustStrength(int delta);

/// Click event duration (ms).
uint16_t clickPulseDuration();
void setClickPulseDuration(uint16_t ms);

/// Timed burst at the stored strength(). Prefer shortPulse() for new code.
void pulse(uint16_t durationMs, uint32_t nowMs);

/// Steady PWM intensity 0..255 (0 = off). Does not apply during an active `pulse` until it finishes.
void setDuty(uint8_t duty255);

/// Carrier frequency for PWM (typical ERM range ~50–300 Hz). Rebuilds MCPWM timer period.
void setPwmFrequency(uint32_t freqHz);

uint32_t pwmFrequencyHz();
uint8_t steadyDuty();

/// Clear pulse schedule and set output off.
void off();

/// Strength multiplier for auto-repeat button presses (0..100 %).
uint8_t repeatStrengthPercent();
void setRepeatStrengthPercent(uint8_t percent);

/// Master switch for automatic button-press haptic feedback.
bool buttonFeedbackEnabled();
void setButtonFeedbackEnabled(bool enabled);

/// Interactive demo on the OLED. Hold **UP+DOWN** ~800ms to enter/exit.
void motorTestService(oled &display, const Inputs &inputs, uint32_t nowMs);
bool motorTestIsActive();

}  // namespace Haptics

/// Thin scheduler wrapper — runs checkPulseEnd / CoilTone::checkToneEnd /
/// motor-test each frame.  All real haptic work is done via Haptics:: calls.
class HapticsService : public IService {
 public:
  const char* name() const override { return "Haptics"; }
  void service() override;

  void bindDisplay(oled* display) { display_ = display; }
  void bindInputs(const Inputs* inputs) { inputs_ = inputs; }

 private:
  oled* display_ = nullptr;
  const Inputs* inputs_ = nullptr;
};

/// Audible tones from the motor coil at very low duty cycle.
/// At duty ~30/255 the coil doesn't spin but vibrates audibly at the PWM
/// frequency.  API mirrors Arduino tone()/noTone().
class CoilTone {
 public:
  static constexpr uint8_t kDefaultDuty = 30;

  /// Start playing.  durationMs == 0  →  play until noTone().
  static void tone(uint32_t freqHz, uint16_t durationMs = 0, uint8_t duty = kDefaultDuty);
  static void noTone();
  static bool isPlaying();
  /// Auto-stop expired timed tones.  Call from the main loop.
  static void checkToneEnd();
};



#else
// ── Stub Haptics — no hardware, all calls are no-ops ─────────────────────────
#include <Arduino.h>
#include "../infra/Scheduler.h"

class oled;
class Inputs;

namespace Haptics {
  constexpr uint8_t kStrengthStep = 0;
  inline void begin() {}
  inline bool enabled() { return false; }
  inline void setEnabled(bool) {}
  inline void shortPulse(int16_t = -1, int16_t = -1, int32_t = -1) {}
  inline void checkPulseEnd() {}
  inline uint8_t strength() { return 0; }
  inline void setStrength(uint8_t) {}
  inline void adjustStrength(int) {}
  inline uint16_t clickPulseDuration() { return 0; }
  inline void setClickPulseDuration(uint16_t) {}
  inline void pulse(uint16_t, uint32_t) {}
  inline void setDuty(uint8_t) {}
  inline void setPwmFrequency(uint32_t) {}
  inline uint32_t pwmFrequencyHz() { return 0; }
  inline uint8_t steadyDuty() { return 0; }
  inline void off() {}
  inline uint8_t repeatStrengthPercent() { return 0; }
  inline void setRepeatStrengthPercent(uint8_t) {}
  inline bool buttonFeedbackEnabled() { return false; }
  inline void setButtonFeedbackEnabled(bool) {}
  inline void motorTestService(oled&, const Inputs&, uint32_t) {}
  inline bool motorTestIsActive() { return false; }
}

class HapticsService : public IService {
 public:
  const char* name() const override { return "Haptics"; }
  void service() override {}
  void bindDisplay(oled*) {}
  void bindInputs(const Inputs*) {}
};

class CoilTone {
 public:
  static constexpr uint8_t kDefaultDuty = 30;
  static void tone(uint32_t, uint16_t = 0, uint8_t = kDefaultDuty) {}
  static void noTone() {}
  static bool isPlaying() { return false; }
  static void checkToneEnd() {}
};

#endif  // BADGE_HAS_HAPTICS

#endif
