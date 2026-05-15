#include "IMU.h"

#ifdef BADGE_HAS_IMU

#include <Wire.h>

#include "../infra/DebugLog.h"
#include "lis2dh12_reg.h"

namespace {
constexpr uint32_t kImuDebugIntervalMs = 250;

// Latched by the GPIO ISR; consumed in IMU::service().
volatile bool g_int1Fired = false;
volatile uint32_t g_int1IsrMs = 0;

void IRAM_ATTR imuInt1Isr() {
  g_int1Fired = true;
  g_int1IsrMs = millis();
}

bool imuSerialDebugEnabled() {
  return badgeConfig.get(kLogImu) != 0;
}

const char* orientationName(BadgeOrientation orientation) {
  switch (orientation) {
    case BadgeOrientation::kUpright:
      return "upright";
    case BadgeOrientation::kInverted:
      return "inverted";
    case BadgeOrientation::kUnknown:
    default:
      return "unknown";
  }
}

// LIS2DH12 INT1_THS LSB at ±2g full-scale = 16 mg.
uint8_t mgToInt1Threshold(float mg) {
  const float v = fabsf(mg) / 16.f;
  if (v < 1.f) return 1;
  if (v > 127.f) return 127;
  return static_cast<uint8_t>(v + 0.5f);
}

// INT1_DURATION ticks scale with ODR (one tick = 1/ODR seconds).
uint8_t msToInt1Duration(uint32_t ms, uint16_t odrHz) {
  uint32_t ticks = (ms * odrHz) / 1000U;
  if (ticks > 127U) ticks = 127U;
  return static_cast<uint8_t>(ticks);
}
}  // namespace


bool IMU::begin(uint8_t i2cAddress) {
  // Wire is initialized once in main.cpp setup(); do not re-init here.
  ready_ = sensor_.begin(i2cAddress, Wire);
  if (!ready_) {
    tiltXMg_ = 0.f;
    tiltYMg_ = 0.f;
    accelZMg_ = 0.f;
    faceDown_ = false;
    motionEventLatched_ = false;
    haveSample_ = false;
    if (imuSerialDebugEnabled()) {
      Serial.printf("[IMU] begin failed addr=0x%02x\n", i2cAddress);
    }
    return false;
  }

  sensor_.setDataRate(LIS2DH12_ODR_200Hz);
  static_assert(kOdrHz == 200, "msToInt1Duration assumes ODR matches kOdrHz");

  sensor_.setIntPolarity(LOW);
  sensor_.setInt1IA1(true);
  sensor_.setInt1Latch(false);
  lis2dh12_int1_pin_notification_mode_set(&sensor_.dev_ctx, LIS2DH12_INT1_PULSED);

  // Orientation needs the gravity DC component, so the high-pass filter
  // must be off for the INT1 generator (it was on for high-G motion).
  lis2dh12_high_pass_int_conf_set(&sensor_.dev_ctx, LIS2DH12_DISC_FROM_INT_GENERATOR);

  programInt1_();

  pinMode(INT_GP_PIN, INPUT_PULLUP);
  g_int1Fired = false;
  attachInterrupt(digitalPinToInterrupt(INT_GP_PIN), imuInt1Isr, FALLING);

  motionEventLatched_ = false;
  haveSample_ = false;
  if (imuSerialDebugEnabled()) {
    Serial.printf("[IMU] begin OK addr=0x%02x odr=50Hz int_pin=%d ths=%u dur=%u\n",
                  i2cAddress, INT_GP_PIN, int1Threshold_, int1Duration_);
  }
  return true;
}

void IMU::programInt1_() {
  // 6D movement recognition on the X axis. The badge's "inversion" axis
  // is X (current sw flip code reads -rawX as tiltY): rawX > +ths ⇒ upright,
  // rawX < -ths ⇒ inverted. XH face = upright, XL face = inverted.
  lis2dh12_int1_cfg_t cfg = {};
  cfg.aoi  = 1;  // 6D movement recognition (with _6d=1)
  cfg._6d  = 1;
  cfg.xlie = 1;
  cfg.xhie = 1;
  cfg.ylie = 0;
  cfg.yhie = 0;
  cfg.zlie = 0;
  cfg.zhie = 0;
  lis2dh12_int1_gen_conf_set(&sensor_.dev_ctx, &cfg);

  sensor_.setInt1Threshold(int1Threshold_);
  sensor_.setInt1Duration(int1Duration_);

  // Drain any pending source bits so the next ISR is a real transition.
  lis2dh12_int1_src_t src;
  lis2dh12_int1_gen_source_get(&sensor_.dev_ctx, &src);
}

bool IMU::int1Fired() const {
  return g_int1Fired;
}

void IMU::handleInt1_() {
  const uint32_t handleMs = millis();
  const uint32_t isrMs = g_int1IsrMs;

  lis2dh12_int1_src_t src;
  if (lis2dh12_int1_gen_source_get(&sensor_.dev_ctx, &src) != 0) {
    // Serial.printf("[FLIP] hw->handle dt=%lu ms (read INT1_SRC failed)\n",
                  // static_cast<unsigned long>(handleMs - isrMs));
    return;
  }
  // Serial.printf("[FLIP] hw->handle dt=%lu ms src ia=%u xh=%u xl=%u yh=%u yl=%u zh=%u zl=%u\n",
                // static_cast<unsigned long>(handleMs - isrMs),
                // src.ia, src.xh, src.xl, src.yh, src.yl, src.zh, src.zl);
  if (!src.ia) return;

  motionEventLatched_ = true;

  if (!flipEnabled_) return;

  BadgeOrientation target = orientation_;
  if (src.xh) target = BadgeOrientation::kUpright;
  else if (src.xl) target = BadgeOrientation::kInverted;
  else return;

  if (target != orientation_) {
    orientation_ = target;
    flipChanged_ = true;
    flipChangedAtMs_ = handleMs;
    Serial.printf("[FLIP] orientation -> %s @ t=%lu (since isr=%lu ms)\n",
                  orientationName(orientation_),
                  static_cast<unsigned long>(handleMs),
                  static_cast<unsigned long>(handleMs - isrMs));
  }
}

void IMU::service() {
  const uint32_t now = millis();
  static uint32_t lastDebugMs = 0;

  if (!ready_) {
    if (imuSerialDebugEnabled() && now - lastDebugMs >= 1000) {
      lastDebugMs = now;
      Serial.println("[IMU] not ready");
    }
    return;
  }

  if (g_int1Fired) {
    g_int1Fired = false;
    handleInt1_();
  }

  if (!sensor_.available()) {
    if (imuSerialDebugEnabled() && now - lastDebugMs >= 1000) {
      lastDebugMs = now;
      Serial.printf("[IMU] no sample orient=%s have=%d motion=%d\n",
                    orientationName(orientation_), haveSample_ ? 1 : 0,
                    motionEventLatched_ ? 1 : 0);
    }
    return;
  }

  sensor_.parseAccelData();
  const float rawX = sensor_.getX();
  const float rawY = sensor_.getY();
  const float rawZ = sensor_.getZ();

  const float sx = kNegateScreenX ? -rawX : rawX;
  const float sy = kNegateScreenY ? -rawY : rawY;
  accelZMg_ = rawZ;

  if (!faceDown_) {
    const bool enter =
        kFaceDownZIsNegative ? (rawZ < -kFaceDownThresholdMgIn) : (rawZ > kFaceDownThresholdMgIn);
    if (enter) {
      faceDown_ = true;
    }
  } else {
    const bool exit =
        kFaceDownZIsNegative ? (rawZ > -kFaceDownThresholdMgOut) : (rawZ < kFaceDownThresholdMgOut);
    if (exit) {
      faceDown_ = false;
    }
  }

  if (!haveSample_) {
    tiltXMg_ = sy;
    tiltYMg_ = sx;
    haveSample_ = true;
  } else {
    tiltXMg_ = (1.f - smoothing_) * tiltXMg_ + smoothing_ * sy;
    tiltYMg_ = (1.f - smoothing_) * tiltYMg_ + smoothing_ * sx;
  }

  if (imuSerialDebugEnabled() && now - lastDebugMs >= kImuDebugIntervalMs) {
    lastDebugMs = now;
    Serial.printf("[IMU] raw_mg=(%.0f,%.0f,%.0f) screen=(%.0f,%.0f) tilt=(%.0f,%.0f) z=%.0f face=%d orient=%s flip_en=%d changed=%d\n",
                  rawX, rawY, rawZ, sx, sy, tiltXMg_, tiltYMg_, accelZMg_,
                  faceDown_ ? 1 : 0, orientationName(orientation_),
                  flipEnabled_ ? 1 : 0, flipChanged_ ? 1 : 0);
  }
}

void IMU::setFlipConfig(bool enable, float flipUpMg, float flipDownMg,
                        uint32_t holdMs) {
  flipEnabled_ = enable;
  flipUpMg_ = flipUpMg;
  flipDownMg_ = flipDownMg;
  flipHoldMs_ = holdMs;
  // 6D uses one symmetric threshold; pick the larger magnitude so neither
  // direction triggers spuriously.
  const float mg = fmaxf(fabsf(flipUpMg_), fabsf(flipDownMg_));
  int1Threshold_ = mgToInt1Threshold(mg);
  int1Duration_ = msToInt1Duration(flipHoldMs_, kOdrHz);
  if (imuSerialDebugEnabled()) {
    Serial.printf("[IMU] flip config enable=%d up=%.0f down=%.0f hold=%lu ths=%u dur=%u\n",
                  enable ? 1 : 0, flipUpMg_, flipDownMg_,
                  static_cast<unsigned long>(flipHoldMs_),
                  int1Threshold_, int1Duration_);
  }
  if (ready_) {
    sensor_.setInt1Threshold(int1Threshold_);
    sensor_.setInt1Duration(int1Duration_);
  }
  if (!enable) {
    orientation_ = BadgeOrientation::kUpright;
    flipChanged_ = true;
  }
}

void IMU::checkInitialOrientation() {
  if (!ready_ || !flipEnabled_) return;

  // Seed the orientation from a single accel sample so the GUI knows which
  // way is up before the first flip event arrives.
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (sensor_.available()) {
      sensor_.parseAccelData();
      const float rawX = sensor_.getX();
      const float ths = static_cast<float>(int1Threshold_) * 16.f;
      if (rawX > ths) {
        orientation_ = BadgeOrientation::kUpright;
      } else if (rawX < -ths) {
        orientation_ = BadgeOrientation::kInverted;
      } else {
        orientation_ = BadgeOrientation::kUpright;
      }
      flipChanged_ = true;
      if (imuSerialDebugEnabled()) {
        Serial.printf("[IMU] initial orientation %s rawX=%.0f ths=%.0f\n",
                      orientationName(orientation_), rawX, ths);
      }
      return;
    }
    delay(5);
  }

  orientation_ = BadgeOrientation::kUpright;
  flipChanged_ = true;
  if (imuSerialDebugEnabled()) {
    Serial.printf("[IMU] initial orientation forced %s (no sample)\n",
                  orientationName(orientation_));
  }
}

void IMU::setSmoothing(uint8_t percent) {
  smoothing_ = static_cast<float>(percent) / 100.f;
}

void IMU::setInt1Config(uint8_t threshold, uint8_t duration) {
  int1Threshold_ = threshold;
  int1Duration_ = duration;
  if (ready_) {
    sensor_.setInt1Threshold(threshold);
    sensor_.setInt1Duration(duration);
  }
}

bool IMU::consumeMotionEvent() {
  const bool hadMotionEvent = motionEventLatched_;
  motionEventLatched_ = false;
  return hadMotionEvent;
}

const char* IMU::name() const { return "IMU"; }

#endif  // BADGE_HAS_IMU
