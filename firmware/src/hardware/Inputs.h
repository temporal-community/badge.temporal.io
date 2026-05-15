#ifndef INPUTS_H
#define INPUTS_H

#include <Arduino.h>
#include <stdint.h>
#include "HardwareConfig.h"
#include "IMU.h"
#include "../infra/Scheduler.h"

class Inputs : public IService {
 public:
  struct ButtonStates {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool y = false;
    bool a = false;
    bool x = false;
    bool b = false;
    bool confirm = false;
    bool cancel = false;
  };

  struct ButtonEdges {
    bool upPressed = false;
    bool upReleased = false;
    bool downPressed = false;
    bool downReleased = false;
    bool leftPressed = false;
    bool leftReleased = false;
    bool rightPressed = false;
    bool rightReleased = false;
    bool yPressed = false;
    bool yReleased = false;
    bool aPressed = false;
    bool aReleased = false;
    bool xPressed = false;
    bool xReleased = false;
    bool bPressed = false;
    bool bReleased = false;
    bool confirmPressed = false;
    bool confirmReleased = false;
    bool cancelPressed = false;
    bool cancelReleased = false;
  };

  void begin();
  void update();
  void suspendInterrupts();
  void resumeInterrupts();
  void resyncButtons();
  void service() override;
  const char* name() const override;

  uint16_t joyX() const;
  uint16_t joyY() const;

  /** Sample ADC immediately (same axis remap as periodic sampling; optionally
   * **without** the screen-space deadzone) — **never** applies IIR smoothing. */
  void readJoystickImmediate(uint16_t* outX, uint16_t* outY,
                             bool applyDeadzone = true) const;

  // 0..100%: controls joystick smoothing (lower = heavier smoothing, slower response).
  void setJoystickSensitivityPercent(uint8_t percent);
  uint8_t joystickSensitivityPercent() const { return joystickSensitivityPercent_; }

  // 0..30%: joystick deadzone around center. Values within this range snap to center (2047).
  void setJoystickDeadzonePercent(uint8_t percent);
  uint8_t joystickDeadzonePercent() const { return joystickDeadzonePercent_; }

  void setDebounceMs(uint8_t ms) { debounceMs_ = ms; }
  void setRepeatTiming(uint16_t initMs, uint16_t int1Ms, uint16_t dly2Ms, uint16_t int2Ms);

  void setOrientation(BadgeOrientation orient);
  void setFlipButtons(bool enable) { flipButtons_ = enable; }
  void setFlipJoystick(bool enable) { flipJoystick_ = enable; }
  void setConfirmCancelSwapped(bool enable);
  bool confirmCancelSwapped() const { return swapConfirmCancel_; }

  const ButtonStates& buttons() const;
  const ButtonEdges& edges() const;
  void clearEdges();
  void clearPressEdge(uint8_t buttonIndex);

  // Dev/test hook: synthesize a press edge for a logical button
  // (0=up, 1=down, 2=left, 3=right).  Does NOT touch the physical
  // debounce state — intentionally fire-and-forget: next call to
  // clearEdges() wipes it.  Meant ONLY for the serial test harness
  // driven via MicroPython `badge.dev("btn", ...)`.
  void injectPress(uint8_t buttonIndex);

  // 0=up, 1=down, 2=left, 3=right. Milliseconds held, or 0 if not pressed.
  uint32_t heldMs(uint8_t buttonIndex) const;

 private:
  enum ButtonIndex : uint8_t {
    kUp = 0,
    kDown = 1,
    kLeft = 2,
    kRight = 3,
    kButtonCount = 4
  };

  static Inputs* instance_;
  static void IRAM_ATTR onButtonUpISR();
  static void IRAM_ATTR onButtonDownISR();
  static void IRAM_ATTR onButtonLeftISR();
  static void IRAM_ATTR onButtonRightISR();

  uint8_t debounceMs_ = 20;
  uint16_t initialRepeatDelayMs_ = 325;
  uint16_t firstRepeatIntervalMs_ = 110;
  uint16_t secondRepeatDelayMs_ = 900;
  uint16_t secondRepeatIntervalMs_ = 55;
  static constexpr uint8_t kDefaultJoystickSensitivityPercent = 80;
  static constexpr uint8_t kDefaultJoystickDeadzonePercent = 8;
  static constexpr uint8_t kButtonPins[kButtonCount] = {
      BUTTON_UP,
      BUTTON_DOWN,
      BUTTON_LEFT,
      BUTTON_RIGHT,
  };

  uint16_t joyXRaw_ = 0;
  uint16_t joyYRaw_ = 0;
  float joyXSmoothed_ = 2047.0f;
  float joyYSmoothed_ = 2047.0f;
  uint8_t joystickSensitivityPercent_ = kDefaultJoystickSensitivityPercent;
  uint8_t joystickDeadzonePercent_ = kDefaultJoystickDeadzonePercent;

  ButtonStates buttonStates_;
  ButtonEdges buttonEdges_;

  volatile uint8_t pendingIrqMask_ = 0;
  uint8_t stableMask_ = 0;
  uint32_t lastDebounceMs_[kButtonCount] = {0, 0, 0, 0};
  uint32_t heldSinceMs_[kButtonCount] = {0, 0, 0, 0};
  uint32_t nextRepeatMs_[kButtonCount] = {0, 0, 0, 0};
  uint32_t lastAnalogSampleMs_ = 0;

  BadgeOrientation orientation_ = BadgeOrientation::kUpright;
  bool flipButtons_ = true;
  bool flipJoystick_ = true;
  bool swapConfirmCancel_ = false;


  ButtonIndex remapIndex(ButtonIndex physical) const;

  void markIrq(ButtonIndex idx);
  void refreshButtons(uint8_t pendingMask, uint32_t nowMs);
  void applyKeyRepeat(uint32_t nowMs);
  void setPressEdge(ButtonIndex idx);
  void updateSemanticAliases();

  // EMA smoothing: blends raw ADC toward smoothed value; alpha = sensitivityPercent/100.
  static uint16_t applyJoystickSensitivity(uint16_t raw, float& smoothed,
                                           uint8_t sensitivityPercent);
};

#endif
