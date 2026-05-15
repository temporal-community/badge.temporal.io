#include "Inputs.h"
#include "Haptics.h"
#include "IMU.h"
#include "Power.h"

Inputs* Inputs::instance_ = nullptr;
constexpr uint8_t Inputs::kButtonPins[Inputs::kButtonCount];

void Inputs::setJoystickSensitivityPercent(uint8_t percent) {
  if (percent < 5) {
    percent = 5;
  } else if (percent > 100) {
    percent = 100;
  }
  joystickSensitivityPercent_ = percent;
}

void Inputs::setJoystickDeadzonePercent(uint8_t percent) {
  if (percent > 30) {
    percent = 30;
  }
  joystickDeadzonePercent_ = percent;
}

void Inputs::setRepeatTiming(uint16_t initMs, uint16_t int1Ms,
                             uint16_t dly2Ms, uint16_t int2Ms) {
  initialRepeatDelayMs_ = initMs;
  firstRepeatIntervalMs_ = int1Ms;
  secondRepeatDelayMs_ = dly2Ms;
  secondRepeatIntervalMs_ = int2Ms;
}

void Inputs::setOrientation(BadgeOrientation orient) {
  (void)orient;
  // Upside-down badge posture is passive display-only: nametag and
  // other rendered assets can flip, but interactive controls stay normal.
  orientation_ = BadgeOrientation::kUpright;
}

void Inputs::setConfirmCancelSwapped(bool enable) {
  swapConfirmCancel_ = enable;
  updateSemanticAliases();
}

Inputs::ButtonIndex Inputs::remapIndex(ButtonIndex physical) const {
  if (orientation_ != BadgeOrientation::kInverted || !flipButtons_) return physical;
  switch (physical) {
    case kUp:    return kDown;
    case kDown:  return kUp;
    case kLeft:  return kRight;
    case kRight: return kLeft;
    default:     return physical;
  }
}

uint16_t Inputs::applyJoystickSensitivity(uint16_t raw, float& smoothed,
                                          uint8_t sensitivityPercent) {
  const float alpha = static_cast<float>(sensitivityPercent) / 100.0f;
  smoothed += alpha * (static_cast<float>(raw) - smoothed);
  const int32_t out = static_cast<int32_t>(smoothed + 0.5f);
  if (out < 0) return 0;
  if (out > 4095) return 4095;
  return static_cast<uint16_t>(out);
}

namespace {
// Median-of-3 filter for analogRead.
//
// On the echo board JOY_X (GPIO 13) and JOY_Y (GPIO 12) sit on ADC2,
// which the ESP32-S3 shares with the WiFi peripheral. When WiFi briefly
// owns ADC2 — DHCP renewal, beacon listening, OTA polls, the daily
// registry refresh, etc. — a single `analogRead()` on those pins
// returns 0 or stale data. That manifested as occasional spurious
// "UP" cursor moves on the home menu (every minute or so, matching
// WiFi keepalive cadence), because user-visible Y is sourced from
// JOY_X after the 90° axis rotation.
//
// Sampling three times in quick succession and taking the median lets
// a single corrupted read be dropped as the outlier — the other two
// reads bracket the true value. Total cost is ~30 µs per axis, well
// under the joystick poll budget.
uint16_t analogReadMedian3(uint8_t pin) {
  const int a = analogRead(pin);
  const int b = analogRead(pin);
  const int c = analogRead(pin);
  int hi = a > b ? a : b;
  int lo = a < b ? a : b;
  int mid;
  if (c > hi) mid = hi;
  else if (c < lo) mid = lo;
  else mid = c;
  if (mid < 0) mid = 0;
  if (mid > 4095) mid = 4095;
  return static_cast<uint16_t>(mid);
}
}  // namespace

void Inputs::begin() {
  instance_ = this;
  // return;

  pinMode(JOY_X, INPUT);
  pinMode(JOY_Y, INPUT);

  for (uint8_t i = 0; i < kButtonCount; ++i) {
    pinMode(kButtonPins[i], INPUT_PULLUP);
  }

  attachInterrupt(digitalPinToInterrupt(BUTTON_UP), onButtonUpISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_DOWN), onButtonDownISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_LEFT), onButtonLeftISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_RIGHT), onButtonRightISR, CHANGE);

  joyXSmoothed_ = static_cast<float>(analogRead(JOY_X));
  joyYSmoothed_ = static_cast<float>(analogRead(JOY_Y));
  joyXRaw_ = static_cast<uint16_t>(joyXSmoothed_);
  joyYRaw_ = static_cast<uint16_t>(joyYSmoothed_);

  uint8_t sampledMask = 0;
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    const bool pressed = digitalRead(kButtonPins[i]) == LOW;
    if (pressed) {
      sampledMask |= (1U << i);
    }
  }
  stableMask_ = sampledMask;
  buttonStates_.up = (sampledMask & (1U << kUp)) != 0;
  buttonStates_.down = (sampledMask & (1U << kDown)) != 0;
  buttonStates_.left = (sampledMask & (1U << kLeft)) != 0;
  buttonStates_.right = (sampledMask & (1U << kRight)) != 0;
  buttonStates_.y = buttonStates_.up;
  buttonStates_.a = buttonStates_.down;
  buttonStates_.x = buttonStates_.left;
  buttonStates_.b = buttonStates_.right;
  updateSemanticAliases();
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    if ((sampledMask & (1U << i)) != 0) {
      heldSinceMs_[i] = nowMs;
      nextRepeatMs_[i] = nowMs + initialRepeatDelayMs_;
    }
  }
}

void Inputs::suspendInterrupts() {
  detachInterrupt(digitalPinToInterrupt(BUTTON_UP));
  detachInterrupt(digitalPinToInterrupt(BUTTON_DOWN));
  detachInterrupt(digitalPinToInterrupt(BUTTON_LEFT));
  detachInterrupt(digitalPinToInterrupt(BUTTON_RIGHT));
}

void Inputs::resumeInterrupts() {
  attachInterrupt(digitalPinToInterrupt(BUTTON_UP), onButtonUpISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_DOWN), onButtonDownISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_LEFT), onButtonLeftISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUTTON_RIGHT), onButtonRightISR, CHANGE);
}

void Inputs::resyncButtons() {
  noInterrupts();
  pendingIrqMask_ = 0;
  interrupts();

  uint8_t sampledMask = 0;
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    const bool pressed = digitalRead(kButtonPins[i]) == LOW;
    if (pressed) sampledMask |= (1U << i);
    heldSinceMs_[i] = 0;
    nextRepeatMs_[i] = 0;
    lastDebounceMs_[i] = 0;
  }

  stableMask_ = sampledMask;
  buttonStates_ = {};
  buttonEdges_ = {};

  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    const bool pressed = (sampledMask & (1U << i)) != 0;
    if (!pressed) continue;

    const ButtonIndex logical = remapIndex(static_cast<ButtonIndex>(i));
    switch (logical) {
      case kUp:
        buttonStates_.up = true;
        buttonStates_.y = true;
        break;
      case kDown:
        buttonStates_.down = true;
        buttonStates_.a = true;
        break;
      case kLeft:
        buttonStates_.left = true;
        buttonStates_.x = true;
        break;
      case kRight:
        buttonStates_.right = true;
        buttonStates_.b = true;
        break;
      default:
        break;
    }
    heldSinceMs_[i] = nowMs;
    nextRepeatMs_[i] = nowMs + initialRepeatDelayMs_;
  }

  updateSemanticAliases();
}

void Inputs::update() {
  service();
}

void Inputs::service() {
  const uint32_t nowMs = millis();
  if ((nowMs - lastAnalogSampleMs_) >= Power::Policy::joystickPollMs) {
    uint16_t physicalX =
        applyJoystickSensitivity(analogReadMedian3(JOY_X),
                                 joyXSmoothed_, joystickSensitivityPercent_);
    uint16_t physicalY =
        applyJoystickSensitivity(analogReadMedian3(JOY_Y),
                                 joyYSmoothed_, joystickSensitivityPercent_);

    if (joystickDeadzonePercent_ > 0) {
      const int16_t dzRadius = static_cast<int16_t>(2047 * joystickDeadzonePercent_ / 100);
      if (abs(static_cast<int16_t>(physicalX) - 2047) < dzRadius) physicalX = 2047;
      if (abs(static_cast<int16_t>(physicalY) - 2047) < dzRadius) physicalY = 2047;
    }

    // The board-mounted joystick is rotated 90 degrees relative to the
    // display. Normalize it once, then apply the existing orientation flip
    // below so all callers see screen-space X/Y.
    joyXRaw_ = 4095 - physicalY;
    joyYRaw_ = physicalX;

    if (orientation_ == BadgeOrientation::kInverted && flipJoystick_) {
      joyXRaw_ = 4095 - joyXRaw_;
      joyYRaw_ = 4095 - joyYRaw_;
    }

    lastAnalogSampleMs_ = nowMs;
  }

  uint8_t pendingMask = 0;
  noInterrupts();
  pendingMask = pendingIrqMask_;
  pendingIrqMask_ = 0;
  interrupts();

  if (pendingMask != 0) {
    refreshButtons(pendingMask, nowMs);
  }

  applyKeyRepeat(nowMs);
}

const char* Inputs::name() const { return "Inputs"; }

uint16_t Inputs::joyX() const { return joyXRaw_; }

uint16_t Inputs::joyY() const { return joyYRaw_; }

void Inputs::readJoystickImmediate(uint16_t* outX, uint16_t* outY,
                                   bool applyDeadzone) const {
  if (!outX || !outY) return;
  uint16_t physicalX = analogReadMedian3(JOY_X);
  uint16_t physicalY = analogReadMedian3(JOY_Y);

  uint16_t jx = static_cast<uint16_t>(4095 - static_cast<int>(physicalY));
  uint16_t jy = physicalX;
  if (orientation_ == BadgeOrientation::kInverted && flipJoystick_) {
    jx = static_cast<uint16_t>(4095 - static_cast<int>(jx));
    jy = static_cast<uint16_t>(4095 - static_cast<int>(jy));
  }

  if (applyDeadzone && joystickDeadzonePercent_ > 0) {
    const int16_t dzRadius =
        static_cast<int16_t>(2047 * joystickDeadzonePercent_ / 100);
    if (abs(static_cast<int16_t>(jx) - 2047) < dzRadius) jx = 2047;
    if (abs(static_cast<int16_t>(jy) - 2047) < dzRadius) jy = 2047;
  }
  *outX = jx;
  *outY = jy;
}

const Inputs::ButtonStates& Inputs::buttons() const { return buttonStates_; }

const Inputs::ButtonEdges& Inputs::edges() const { return buttonEdges_; }

void Inputs::clearEdges() { buttonEdges_ = {}; }

void Inputs::clearPressEdge(uint8_t buttonIndex) {
  switch (static_cast<ButtonIndex>(buttonIndex)) {
    case kUp:
      buttonEdges_.upPressed = false;
      buttonEdges_.yPressed = false;
      break;
    case kDown:
      buttonEdges_.downPressed = false;
      buttonEdges_.aPressed = false;
      break;
    case kLeft:
      buttonEdges_.leftPressed = false;
      buttonEdges_.xPressed = false;
      break;
    case kRight:
      buttonEdges_.rightPressed = false;
      buttonEdges_.bPressed = false;
      break;
    default: break;
  }
  updateSemanticAliases();
}

void Inputs::injectPress(uint8_t buttonIndex) {
  if (buttonIndex >= kButtonCount) return;
  // Set the edge bit directly — GUIManager consumes buttonEdges_
  // on its next tick.  We intentionally skip the physical state
  // update so this doesn't interfere with real button debounce or
  // key-repeat machinery.
  switch (static_cast<ButtonIndex>(buttonIndex)) {
    case kUp:
      buttonEdges_.upPressed = true;
      buttonEdges_.yPressed = true;
      break;
    case kDown:
      buttonEdges_.downPressed = true;
      buttonEdges_.aPressed = true;
      break;
    case kLeft:
      buttonEdges_.leftPressed = true;
      buttonEdges_.xPressed = true;
      break;
    case kRight:
      buttonEdges_.rightPressed = true;
      buttonEdges_.bPressed = true;
      break;
    default: break;
  }
  updateSemanticAliases();
}

uint32_t Inputs::heldMs(uint8_t buttonIndex) const {
  if (buttonIndex >= kButtonCount) {
    return 0;
  }
  const ButtonIndex physical = remapIndex(static_cast<ButtonIndex>(buttonIndex));
  const uint8_t bit = static_cast<uint8_t>(1U << physical);

  if ((stableMask_ & bit) == 0) {
    return 0;
  }
  const uint32_t nowMs = millis();
  if (nowMs < heldSinceMs_[physical]) {
    return 0;
  }
  return nowMs - heldSinceMs_[physical];
}

void IRAM_ATTR Inputs::onButtonUpISR() {
  if (instance_ != nullptr) {
    instance_->markIrq(kUp);
  }
}

void IRAM_ATTR Inputs::onButtonDownISR() {
  if (instance_ != nullptr) {
    instance_->markIrq(kDown);
  }
}

void IRAM_ATTR Inputs::onButtonLeftISR() {
  if (instance_ != nullptr) {
    instance_->markIrq(kLeft);
  }
}

void IRAM_ATTR Inputs::onButtonRightISR() {
  if (instance_ != nullptr) {
    instance_->markIrq(kRight);
  }
}

void IRAM_ATTR Inputs::markIrq(ButtonIndex idx) {
  pendingIrqMask_ |= (1U << idx);
}

void Inputs::refreshButtons(uint8_t pendingMask, uint32_t nowMs) {
  uint8_t sampledMask = stableMask_;
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    if ((pendingMask & (1U << i)) == 0) {
      continue;
    }

    const bool pressedNow = digitalRead(kButtonPins[i]) == LOW;
    if (pressedNow) {
      sampledMask |= (1U << i);
    } else {
      sampledMask &= static_cast<uint8_t>(~(1U << i));
    }
  }

  for (uint8_t i = 0; i < kButtonCount; ++i) {
    const uint8_t bit = (1U << i);
    const bool wasPressed = (stableMask_ & bit) != 0;
    const bool isPressed = (sampledMask & bit) != 0;
    if (wasPressed == isPressed) {
      continue;
    }

    if ((nowMs - lastDebounceMs_[i]) < debounceMs_) {
      continue;
    }

    lastDebounceMs_[i] = nowMs;
    stableMask_ ^= bit;
    if (isPressed) {
      heldSinceMs_[i] = nowMs;
      nextRepeatMs_[i] = nowMs + initialRepeatDelayMs_;
    } else {
      heldSinceMs_[i] = 0;
      nextRepeatMs_[i] = 0;
    }

    const ButtonIndex logical = remapIndex(static_cast<ButtonIndex>(i));
    switch (logical) {
      case kUp:
        buttonStates_.up = isPressed;
        buttonStates_.y = isPressed;
        buttonEdges_.upPressed = isPressed;
        buttonEdges_.upReleased = !isPressed;
        buttonEdges_.yPressed = isPressed;
        buttonEdges_.yReleased = !isPressed;
        break;
      case kDown:
        buttonStates_.down = isPressed;
        buttonStates_.a = isPressed;
        buttonEdges_.downPressed = isPressed;
        buttonEdges_.downReleased = !isPressed;
        buttonEdges_.aPressed = isPressed;
        buttonEdges_.aReleased = !isPressed;
        break;
      case kLeft:
        buttonStates_.left = isPressed;
        buttonStates_.x = isPressed;
        buttonEdges_.leftPressed = isPressed;
        buttonEdges_.leftReleased = !isPressed;
        buttonEdges_.xPressed = isPressed;
        buttonEdges_.xReleased = !isPressed;
        break;
      case kRight:
        buttonStates_.right = isPressed;
        buttonStates_.b = isPressed;
        buttonEdges_.rightPressed = isPressed;
        buttonEdges_.rightReleased = !isPressed;
        buttonEdges_.bPressed = isPressed;
        buttonEdges_.bReleased = !isPressed;
        break;
      default:
        break;
    }

    if (isPressed && Haptics::buttonFeedbackEnabled() && !Haptics::motorTestIsActive()) {
      Haptics::shortPulse();
    }
    updateSemanticAliases();
  }
}

void Inputs::applyKeyRepeat(uint32_t nowMs) {
  constexpr uint8_t kAllButtonsMask = (1U << kUp) | (1U << kDown)
                                    | (1U << kLeft) | (1U << kRight);
  if ((stableMask_ & kAllButtonsMask) == kAllButtonsMask) return;

  for (uint8_t i = 0; i < kButtonCount; ++i) {
    const uint8_t bit = (1U << i);
    if ((stableMask_ & bit) == 0) {
      continue;
    }

    if (nextRepeatMs_[i] == 0 || nowMs < nextRepeatMs_[i]) {
      continue;
    }

    setPressEdge(remapIndex(static_cast<ButtonIndex>(i)));

    if (Haptics::buttonFeedbackEnabled() && !Haptics::motorTestIsActive()
        && Haptics::steadyDuty() == 0) {
      // const uint8_t repeatStr =
      //     (static_cast<uint16_t>(Haptics::strength()) * Haptics::repeatStrengthPercent()) / 100;
      // Haptics::shortPulse(repeatStr);
      Haptics::shortPulse();
    }

    const uint32_t heldForMs = nowMs - heldSinceMs_[i];
    const bool useSecondRate = heldForMs >= secondRepeatDelayMs_;
    const uint16_t intervalMs =
        useSecondRate ? secondRepeatIntervalMs_ : firstRepeatIntervalMs_;
    nextRepeatMs_[i] = nowMs + intervalMs;
  }
}

void Inputs::setPressEdge(ButtonIndex idx) {
  switch (idx) {
    case kUp:
      buttonEdges_.upPressed = true;
      buttonEdges_.yPressed = true;
      break;
    case kDown:
      buttonEdges_.downPressed = true;
      buttonEdges_.aPressed = true;
      break;
    case kLeft:
      buttonEdges_.leftPressed = true;
      buttonEdges_.xPressed = true;
      break;
    case kRight:
      buttonEdges_.rightPressed = true;
      buttonEdges_.bPressed = true;
      break;
    default:
      break;
  }
  updateSemanticAliases();
}

void Inputs::updateSemanticAliases() {
  if (swapConfirmCancel_) {
    buttonStates_.confirm = buttonStates_.b;
    buttonStates_.cancel = buttonStates_.a;
    buttonEdges_.confirmPressed = buttonEdges_.bPressed;
    buttonEdges_.confirmReleased = buttonEdges_.bReleased;
    buttonEdges_.cancelPressed = buttonEdges_.aPressed;
    buttonEdges_.cancelReleased = buttonEdges_.aReleased;
  } else {
    buttonStates_.confirm = buttonStates_.a;
    buttonStates_.cancel = buttonStates_.b;
    buttonEdges_.confirmPressed = buttonEdges_.aPressed;
    buttonEdges_.confirmReleased = buttonEdges_.aReleased;
    buttonEdges_.cancelPressed = buttonEdges_.bPressed;
    buttonEdges_.cancelReleased = buttonEdges_.bReleased;
  }
}
