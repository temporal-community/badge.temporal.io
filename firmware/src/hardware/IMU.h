#ifndef IMU_H
#define IMU_H

#include "HardwareConfig.h"
#include "../infra/Scheduler.h"

enum class BadgeOrientation : int8_t {
  kUnknown  = -1,
  kInverted =  0,
  kUpright  =  1,
};

#ifdef BADGE_HAS_IMU
// ── Full LIS2DH12 driver (Charlie board only) ─────────────────────────────────
#include <Arduino.h>
#include <SparkFun_LIS2DH12.h>

class IMU : public IService {
 public:
  bool begin(uint8_t i2cAddress = LIS2DH12_I2C_ADDRESS);
  void service() override;
  const char* name() const override;
  bool isReady() const { return ready_; }
  float tiltXMg() const { return tiltXMg_; }
  float tiltYMg() const { return tiltYMg_; }
  float accelZMg() const { return accelZMg_; }
  bool isFaceDown() const { return faceDown_; }
  bool int1Fired() const;
  bool consumeMotionEvent();

  BadgeOrientation getOrientation() const { return orientation_; }
  bool flipChanged() const { return flipChanged_; }
  uint32_t flipChangedAtMs() const { return flipChangedAtMs_; }
  void clearFlipChanged() { flipChanged_ = false; }
  void setFlipConfig(bool enable, float flipUpMg, float flipDownMg,
                     uint32_t holdMs);
  void checkInitialOrientation();

  void setSmoothing(uint8_t percent);
  void setInt1Config(uint8_t threshold, uint8_t duration);

 private:
  SPARKFUN_LIS2DH12 sensor_;
  bool ready_ = false;
  float tiltXMg_ = 0.f, tiltYMg_ = 0.f;
  bool haveSample_ = false, faceDown_ = false;
  bool motionEventLatched_ = false;
  float accelZMg_ = 0.f;
  float smoothing_ = 0.88f;
  static constexpr bool kNegateScreenY = false, kNegateScreenX = true, kNegateScreenZ = true;
  static constexpr bool kFaceDownZIsNegative = false;
  static constexpr float kFaceDownThresholdMgIn = 750.f, kFaceDownThresholdMgOut = 600.f;
  static constexpr uint16_t kOdrHz = 200;
  uint8_t int1Threshold_ = 12, int1Duration_ = 2;

  bool flipEnabled_ = true;
  float flipUpMg_ = -200.f;
  float flipDownMg_ = 200.f;
  uint32_t flipHoldMs_ = 150;
  BadgeOrientation orientation_ = BadgeOrientation::kUnknown;
  bool flipChanged_ = false;
  uint32_t flipChangedAtMs_ = 0;

  void programInt1_();
  void handleInt1_();
};

#else
// ── Stub IMU — no hardware, all returns are safe no-ops ──────────────────────
#include <Arduino.h>

class IMU : public IService {
 public:
  bool begin(uint8_t = 0) { return false; }
  void service() override {}
  const char* name() const override { return "IMU"; }
  bool isReady() const { return false; }
  float tiltXMg() const { return 0.f; }
  float tiltYMg() const { return 0.f; }
  float accelZMg() const { return 0.f; }
  bool isFaceDown() const { return false; }
  bool int1Fired() const { return false; }
  bool consumeMotionEvent() { return false; }
  BadgeOrientation getOrientation() const { return BadgeOrientation::kUpright; }
  bool flipChanged() const { return false; }
  uint32_t flipChangedAtMs() const { return 0; }
  void clearFlipChanged() {}
  void setFlipConfig(bool, float, float, uint32_t) {}
  void checkInitialOrientation() {}
  void setSmoothing(uint8_t) {}
  void setInt1Config(uint8_t, uint8_t) {}
};

#endif  // BADGE_HAS_IMU
#endif  // IMU_H
