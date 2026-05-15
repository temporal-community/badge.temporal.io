#include "LEDmatrix.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Haptics.h"
#include "IMU.h"
#include "Inputs.h"
#include "Power.h"

#ifdef BADGE_HAS_LED_MATRIX



// ═══════════════════════════════════════════════════════════════════════════════
//  Shared helpers
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// Builtin image table — single source of truth. Adding a builtin is
// one row plus the kImage* / kMask* declarations in LEDmatrixImages.h;
// previously each image needed an extra strcmp arm.
struct BuiltinImageRow {
  const char* id;
  const uint8_t* mask;
};

constexpr BuiltinImageRow kBuiltinImages[] = {
  { LEDmatrixImages::kImageSmiley,    LEDmatrixImages::kMaskSmiley    },
  { LEDmatrixImages::kImageHeart,     LEDmatrixImages::kMaskHeart     },
  { LEDmatrixImages::kImageArrowUp,   LEDmatrixImages::kMaskArrowUp   },
  { LEDmatrixImages::kImageArrowDown, LEDmatrixImages::kMaskArrowDown },
  { LEDmatrixImages::kImageXMark,     LEDmatrixImages::kMaskXMark     },
  { LEDmatrixImages::kImageDot,       LEDmatrixImages::kMaskDot       },
};

bool getBuiltinImageMask(const char *imageId, uint8_t outMask[LED_MATRIX_HEIGHT]) {
  if (imageId == nullptr || outMask == nullptr) {
    return false;
  }
  for (const BuiltinImageRow& row : kBuiltinImages) {
    if (strcmp(imageId, row.id) == 0) {
      memcpy(outMask, row.mask, LED_MATRIX_HEIGHT);
      return true;
    }
  }
  return false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
//  LEDmatrixAnimator
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
constexpr int8_t kCrosshairBaseOffset = -11;
constexpr int8_t kCrosshairJoystickRange = 8;
constexpr int8_t kTemporalLogoCenterOffset = -12;
constexpr uint8_t kDefaultBrightnessStep = 2;

constexpr uint8_t kIntGpFlashBrightness = 40;

enum class IntGpFlashSource : uint8_t {
  None,
  ButtonUp,
  ButtonDown,
  ButtonLeft,
  ButtonRight,
  ImuMotion
};

constexpr uint8_t kMaskArrowLeft[LED_MATRIX_HEIGHT] = {
    0b00011000,
    0b00110000,
    0b01100000,
    0b11111111,
    0b11111111,
    0b01100000,
    0b00110000,
    0b00011000,
};

constexpr uint8_t kMaskArrowRight[LED_MATRIX_HEIGHT] = {
    0b00011000,
    0b00001100,
    0b00000110,
    0b11111111,
    0b11111111,
    0b00000110,
    0b00001100,
    0b00011000,
};

IntGpFlashSource detectIntGpFlashSource(IMU &imu) {
  if (digitalRead(BUTTON_UP) == LOW) {
    return IntGpFlashSource::ButtonUp;
  }
  if (digitalRead(BUTTON_DOWN) == LOW) {
    return IntGpFlashSource::ButtonDown;
  }
  if (digitalRead(BUTTON_LEFT) == LOW) {
    return IntGpFlashSource::ButtonLeft;
  }
  if (digitalRead(BUTTON_RIGHT) == LOW) {
    return IntGpFlashSource::ButtonRight;
  }
  if (imu.isReady()) {
    return IntGpFlashSource::ImuMotion;
  }
  return IntGpFlashSource::None;
}

void renderImuDotFrame(LEDmatrix &matrix, IMU &imu) {
  uint8_t mask[LED_MATRIX_HEIGHT] = {};
  constexpr int32_t kTiltSaturationMg = 1000;
  const int32_t xMg =
      constrain(static_cast<int32_t>(imu.tiltXMg()), -kTiltSaturationMg, kTiltSaturationMg);
  const int32_t yMg =
      constrain(static_cast<int32_t>(imu.tiltYMg()), -kTiltSaturationMg, kTiltSaturationMg);

  // Pan an 8x8 viewport over the 32x32 Temporal logo so the glyph
  // "rolls" with tilt (marble-in-a-tray feel): tilting the badge
  // right makes the logo slide right on the display.  With the
  // drawing pipeline rotating masks 180° for the default (kUpright)
  // orientation, mask x=0 is the visual right edge, so the glyph
  // needs to live near mask x=0 — which means the viewport's left
  // column tracks the glyph's left column.  xOff INCREASES as xMg
  // increases (and same for yOff).
  constexpr int32_t kPanRange = 32 - LED_MATRIX_WIDTH;  // 24 px of travel
  const uint8_t xOff = static_cast<uint8_t>(
      map(xMg, -kTiltSaturationMg, kTiltSaturationMg, 0, kPanRange));
  const uint8_t yOff = static_cast<uint8_t>(
      map(yMg, -kTiltSaturationMg, kTiltSaturationMg, 0, kPanRange));

  // Slice an 8-column window out of each source row.  Bit 31 of the
  // source word is the leftmost pixel (same convention as the
  // kBitmapCrosshairCircle32 bitmap above), so the 8-pixel window
  // starting at column xOff lives in bits [31-xOff .. 24-xOff] —
  // shift right by (24 - xOff) and mask to 8 bits.
  for (uint8_t r = 0; r < LED_MATRIX_HEIGHT; r++) {
    const uint32_t srcRow = LEDmatrixImages::kTemporalLogo32x32[yOff + r];
    mask[r] = static_cast<uint8_t>((srcRow >> (24U - xOff)) & 0xFFU);
  }
  matrix.drawMaskHardware(mask, kIntGpFlashBrightness, 0);
}

void renderIntGpFlashFrame(LEDmatrix &matrix, IMU &imu, IntGpFlashSource source) {
  switch (source) {
    case IntGpFlashSource::ButtonUp:
      matrix.drawMaskHardware(LEDmatrixImages::kMaskArrowUp, kIntGpFlashBrightness, 0);
      return;
    case IntGpFlashSource::ButtonDown:
      matrix.drawMaskHardware(LEDmatrixImages::kMaskArrowDown, kIntGpFlashBrightness, 0);
      return;
    case IntGpFlashSource::ButtonLeft:
      matrix.drawMaskHardware(kMaskArrowLeft, kIntGpFlashBrightness, 0);
      return;
    case IntGpFlashSource::ButtonRight:
      matrix.drawMaskHardware(kMaskArrowRight, kIntGpFlashBrightness, 0);
      return;
    case IntGpFlashSource::ImuMotion:
      renderImuDotFrame(matrix, imu);
      return;
    case IntGpFlashSource::None:
    default:
      matrix.drawFullOnHardware(kIntGpFlashBrightness);
      return;
  }
}
}  // namespace

LEDmatrixAnimator::LEDmatrixAnimator() { reset(); }

bool LEDmatrixAnimator::isUserActive(const Inputs &inputs) {
  const Inputs::ButtonEdges &edges = inputs.edges();
  const Inputs::ButtonStates &buttons = inputs.buttons();
  if (edges.upPressed || edges.upReleased || edges.downPressed || edges.downReleased || edges.leftPressed ||
      edges.leftReleased || edges.rightPressed || edges.rightReleased) {
    return true;
  }
  if (buttons.up || buttons.down || buttons.left || buttons.right) {
    return true;
  }
  constexpr int kCenter = 2047;
  constexpr int kJoystickDeadband = 350;
  const int dx = abs(static_cast<int>(inputs.joyX()) - kCenter);
  const int dy = abs(static_cast<int>(inputs.joyY()) - kCenter);
  return dx > kJoystickDeadband || dy > kJoystickDeadband;
}

void LEDmatrixAnimator::serviceIntGpOverlay(LEDmatrix &matrix, IMU &imu, bool wakeOnlyMode,
                                            const Inputs &inputs, uint32_t nowMs) {
  if (wakeOnlyMode || !matrix.isInitialized()) {
    intGpOverlayActive_ = false;
    return;
  }

  const bool intLow = (digitalRead(INT_GP_PIN) == LOW);
  if (intLow) {
    renderIntGpFlashFrame(matrix, imu, detectIntGpFlashSource(imu));
    intGpOverlayActive_ = true;
    return;
  }

  if (intGpOverlayActive_) {
    matrix.refreshDisplayFromFramebuffer();
    intGpOverlayActive_ = false;
  }
}

void LEDmatrixAnimator::reset() {
  animationSource_ = AnimationSource::None;
  activeAnimation_ = DefaultAnimation::None;
  lastAnimationStepMs_ = 0;
  animationIntervalMs_ = 120;
  animationBrightness_ = 255;
  animationFrame_ = 0;

  trackedSourceType_ = TrackedSourceType::None;
  trackedImage_ = DefaultImage::Smiley;
  trackedAnimation_ = DefaultAnimation::None;
  trackedBitmapRows32_ = nullptr;
  trackedBitmapWidth_ = 0;
  trackedBitmapHeight_ = 0;
  trackedOffsetX_ = 0;
  trackedOffsetY_ = 0;
  trackedScale_ = 1;
  trackedOnBrightness_ = 180;
  trackedOffBrightness_ = 0;
  trackedFrameOverrideEnabled_ = false;
  trackedFrameOverride_ = 0;
  trackedAutoFrame_ = 0;
  trackedFrameValid_ = false;
  memset(trackedFrameMask_, 0, sizeof(trackedFrameMask_));
  trackedFrameOnBrightness_ = 0;
  trackedFrameOffBrightness_ = 0;
  temporalLogoLastStepMs_ = 0;

  intGpOverlayActive_ = false;
}

bool LEDmatrixAnimator::startDefaultAnimation(DefaultAnimation animation, uint16_t frameIntervalMs, uint8_t brightness) {
  animationSource_ = AnimationSource::Default;
  activeAnimation_ = animation;
  animationIntervalMs_ = frameIntervalMs;
  animationBrightness_ = brightness;
  animationFrame_ = 0;
  lastAnimationStepMs_ = 0;
  trackedSourceType_ = TrackedSourceType::None;
  trackedFrameValid_ = false;
  return true;
}

bool LEDmatrixAnimator::startTrackedImage(DefaultImage image, uint16_t frameIntervalMs) {
  beginTrackedSource(TrackedSourceType::Image, frameIntervalMs);
  trackedImage_ = image;
  return true;
}

bool LEDmatrixAnimator::startTrackedAnimation(DefaultAnimation animation, uint16_t frameIntervalMs) {
  beginTrackedSource(TrackedSourceType::Animation, frameIntervalMs);
  trackedAnimation_ = animation;
  return true;
}

bool LEDmatrixAnimator::startTrackedBitmap32(const uint32_t *rows, uint8_t width, uint8_t height, uint16_t frameIntervalMs) {
  if (rows == nullptr || width == 0 || height == 0 || width > 32 || height > 32) {
    return false;
  }

  beginTrackedSource(TrackedSourceType::Bitmap32, frameIntervalMs);
  trackedBitmapRows32_ = rows;
  trackedBitmapWidth_ = width;
  trackedBitmapHeight_ = height;
  return true;
}

bool LEDmatrixAnimator::startCrosshairBitmap32(uint8_t maxOnBrightness, uint16_t frameIntervalMs,
                                               uint8_t scale, uint8_t offBrightness) {
  const uint8_t startingBrightness = static_cast<uint8_t>(maxOnBrightness / 2);
  if (!startTrackedBitmap32(LEDmatrixImages::kBitmapCrosshairCircle32, 32, 32, frameIntervalMs)) {
    return false;
  }
  if (!setTrackedScale(scale)) {
    return false;
  }
  if (!setTrackedBrightness(startingBrightness, offBrightness)) {
    return false;
  }
  return setTrackedPosition(kCrosshairBaseOffset, kCrosshairBaseOffset);
}

bool LEDmatrixAnimator::startTemporalLogoBitmap32(uint8_t maxOnBrightness, uint16_t frameIntervalMs,
                                                   uint8_t scale, uint8_t offBrightness) {
  // Start centered; the LED service nudges this with tilt and eases it
  // back to center so the default state stays readable.
  const uint8_t startingBrightness = static_cast<uint8_t>(maxOnBrightness / 2);
  if (!startTrackedBitmap32(LEDmatrixImages::kTemporalLogo32x32, 32, 32, frameIntervalMs)) {
    return false;
  }
  if (!setTrackedScale(scale)) {
    return false;
  }
  if (!setTrackedBrightness(startingBrightness, offBrightness)) {
    return false;
  }
  temporalLogoLastStepMs_ = 0;
  return setTrackedPosition(kTemporalLogoCenterOffset, kTemporalLogoCenterOffset);
}

bool LEDmatrixAnimator::updateCrosshairFromJoystick(const Inputs &inputs) {
  const int8_t joyOffsetX = static_cast<int8_t>(
      map(constrain(inputs.joyX(), 0, 4095), 0, 4095, -kCrosshairJoystickRange, kCrosshairJoystickRange));
  const int8_t joyOffsetY = static_cast<int8_t>(
      map(constrain(inputs.joyY(), 0, 4095), 0, 4095, -kCrosshairJoystickRange, kCrosshairJoystickRange));
  const int8_t trackedX = static_cast<int8_t>(kCrosshairBaseOffset + joyOffsetX);
  const int8_t trackedY = static_cast<int8_t>(kCrosshairBaseOffset + joyOffsetY);

  if (trackedX == trackedOffsetX_ && trackedY == trackedOffsetY_) {
    return false;
  }
  return setTrackedPosition(trackedX, trackedY);
}

bool LEDmatrixAnimator::updateCrosshairFromAccel(float axMg, float ayMg) {
  constexpr float kDeadzoneMg = 55.f;
  constexpr float kSaturationMg = 480.f;

  auto tiltToOffset = [](float mg) -> int8_t {
    if (mg > -kDeadzoneMg && mg < kDeadzoneMg) {
      return 0;
    }
    const float clipped = constrain(mg, -kSaturationMg, kSaturationMg);
    const float sign = clipped >= 0.f ? 1.f : -1.f;
    const float mag = fabsf(clipped);
    const float span = kSaturationMg - kDeadzoneMg;
    const float t = (mag - kDeadzoneMg) / span;
    const int32_t off =
        static_cast<int32_t>(roundf(t * static_cast<float>(kCrosshairJoystickRange) * sign));
    return static_cast<int8_t>(constrain(off, -kCrosshairJoystickRange, kCrosshairJoystickRange));
  };

  const int8_t offX = tiltToOffset(axMg);
  const int8_t offY = tiltToOffset(ayMg);
  const int8_t trackedX = static_cast<int8_t>(kCrosshairBaseOffset + offX);
  const int8_t trackedY = static_cast<int8_t>(kCrosshairBaseOffset + offY);

  if (trackedX == trackedOffsetX_ && trackedY == trackedOffsetY_) {
    return false;
  }
  return setTrackedPosition(trackedX, trackedY);
}

bool LEDmatrixAnimator::updateCrosshairFromAccelAndJoystick(float axMg, float ayMg, const Inputs &inputs) {
  constexpr float kDeadzoneMg = 1.f;
  constexpr float kSaturationMg = 480.f;

  auto tiltToOffset = [&](float mg) -> int8_t {
    if (mg > -kDeadzoneMg && mg < kDeadzoneMg) {
      return 0;
    }
    const float clipped = constrain(mg, -kSaturationMg, kSaturationMg);
    const float sign = clipped >= 0.f ? 1.f : -1.f;
    const float mag = fabsf(clipped);
    const float span = kSaturationMg - kDeadzoneMg;
    const float t = (mag - kDeadzoneMg) / span;
    const int32_t off = static_cast<int32_t>(roundf(t * static_cast<float>(kCrosshairJoystickRange) * sign));
    return static_cast<int8_t>(constrain(off, -kCrosshairJoystickRange, kCrosshairJoystickRange));
  };

  const int8_t accelOffX = tiltToOffset(axMg);
  const int8_t accelOffY = tiltToOffset(ayMg);

  const int8_t joyOffsetX = static_cast<int8_t>(
      map(constrain(inputs.joyX(), 0, 4095), 0, 4095, -kCrosshairJoystickRange, kCrosshairJoystickRange));
  const int8_t joyOffsetY = static_cast<int8_t>(
      map(constrain(inputs.joyY(), 0, 4095), 0, 4095, -kCrosshairJoystickRange, kCrosshairJoystickRange));

  const int8_t sumOffX = static_cast<int8_t>(constrain(static_cast<int16_t>(accelOffX) + static_cast<int16_t>(joyOffsetX),
                                                        -kCrosshairJoystickRange, kCrosshairJoystickRange));
  const int8_t sumOffY = static_cast<int8_t>(constrain(static_cast<int16_t>(accelOffY) + static_cast<int16_t>(joyOffsetY),
                                                        -kCrosshairJoystickRange, kCrosshairJoystickRange));

  const int8_t trackedX = static_cast<int8_t>(kCrosshairBaseOffset + sumOffX);
  const int8_t trackedY = static_cast<int8_t>(kCrosshairBaseOffset + sumOffY);

  if (trackedX == trackedOffsetX_ && trackedY == trackedOffsetY_) {
    return false;
  }
  return setTrackedPosition(trackedX, trackedY);
}

bool LEDmatrixAnimator::updateTemporalLogoFromAccel(float axMg, float ayMg, uint32_t nowMs) {
  if (animationSource_ != AnimationSource::Tracked ||
      trackedSourceType_ != TrackedSourceType::Bitmap32 ||
      trackedBitmapRows32_ != LEDmatrixImages::kTemporalLogo32x32) {
    return false;
  }

  constexpr float kDeadzoneMg = 95.f;
  constexpr float kSaturationMg = 520.f;
  constexpr int8_t kTiltRange = 3;
  constexpr uint32_t kTiltStepMs = 55;
  constexpr uint32_t kCenterStepMs = 160;

  auto tiltToOffset = [](float mg) -> int8_t {
    if (mg > -kDeadzoneMg && mg < kDeadzoneMg) {
      return 0;
    }
    const float clipped = constrain(mg, -kSaturationMg, kSaturationMg);
    const float sign = clipped >= 0.f ? 1.f : -1.f;
    const float mag = fabsf(clipped);
    const float span = kSaturationMg - kDeadzoneMg;
    const float t = (mag - kDeadzoneMg) / span;
    const int32_t off = static_cast<int32_t>(roundf(t * static_cast<float>(kTiltRange) * sign));
    return static_cast<int8_t>(constrain(off, -kTiltRange, kTiltRange));
  };

  const int8_t offX = tiltToOffset(axMg);
  const int8_t offY = tiltToOffset(ayMg);
  const int8_t targetX = static_cast<int8_t>(kTemporalLogoCenterOffset + offX);
  const int8_t targetY = static_cast<int8_t>(kTemporalLogoCenterOffset + offY);
  if (targetX == trackedOffsetX_ && targetY == trackedOffsetY_) {
    return false;
  }

  const bool returningToCenter = offX == 0 && offY == 0;
  const uint32_t stepMs = returningToCenter ? kCenterStepMs : kTiltStepMs;
  if (temporalLogoLastStepMs_ != 0 && (nowMs - temporalLogoLastStepMs_) < stepMs) {
    return false;
  }
  temporalLogoLastStepMs_ = nowMs;

  auto stepToward = [](int8_t current, int8_t target) -> int8_t {
    if (current < target) {
      return static_cast<int8_t>(current + 1);
    }
    if (current > target) {
      return static_cast<int8_t>(current - 1);
    }
    return current;
  };

  return setTrackedPosition(stepToward(trackedOffsetX_, targetX),
                            stepToward(trackedOffsetY_, targetY));
}

bool LEDmatrixAnimator::setTrackedPosition(int8_t x, int8_t y) {
  if (animationSource_ != AnimationSource::Tracked) {
    return false;
  }
  trackedOffsetX_ = x;
  trackedOffsetY_ = y;
  return true;
}

bool LEDmatrixAnimator::setTrackedScale(uint8_t scale) {
  if (animationSource_ != AnimationSource::Tracked || scale == 0 || scale == trackedScale_) {
    return false;
  }

  uint8_t sourceWidth = LED_MATRIX_WIDTH;
  uint8_t sourceHeight = LED_MATRIX_HEIGHT;
  if (trackedSourceType_ == TrackedSourceType::Bitmap32 && trackedBitmapWidth_ > 0 && trackedBitmapHeight_ > 0) {
    sourceWidth = trackedBitmapWidth_;
    sourceHeight = trackedBitmapHeight_;
  }

  const int16_t centerX = static_cast<int16_t>(sourceWidth / 2);
  const int16_t centerY = static_cast<int16_t>(sourceHeight / 2);
  const int16_t dx = static_cast<int16_t>(trackedScale_) - static_cast<int16_t>(scale);
  const int16_t newOffsetX = static_cast<int16_t>(trackedOffsetX_) + (centerX * dx);
  const int16_t newOffsetY = static_cast<int16_t>(trackedOffsetY_) + (centerY * dx);

  if (newOffsetX < -128 || newOffsetX > 127 || newOffsetY < -128 || newOffsetY > 127) {
    return false;
  }

  trackedOffsetX_ = static_cast<int8_t>(newOffsetX);
  trackedOffsetY_ = static_cast<int8_t>(newOffsetY);
  trackedScale_ = scale;
  return true;
}

bool LEDmatrixAnimator::setTrackedBrightness(uint8_t onBrightness, uint8_t offBrightness) {
  if (animationSource_ != AnimationSource::Tracked) {
    return false;
  }
  trackedOnBrightness_ = onBrightness;
  trackedOffBrightness_ = offBrightness;
  return true;
}

bool LEDmatrixAnimator::setTrackedFrame(uint8_t frameIndex) {
  if (animationSource_ != AnimationSource::Tracked || trackedSourceType_ != TrackedSourceType::Animation) {
    return false;
  }
  trackedFrameOverrideEnabled_ = true;
  trackedFrameOverride_ = frameIndex;
  return true;
}

bool LEDmatrixAnimator::clearTrackedFrameOverride() {
  if (animationSource_ != AnimationSource::Tracked || trackedSourceType_ != TrackedSourceType::Animation) {
    return false;
  }
  trackedFrameOverrideEnabled_ = false;
  return true;
}

bool LEDmatrixAnimator::handleDirectionalButtons(const Inputs &inputs, uint8_t currentBrightness, uint8_t brightnessStep,
                                                 uint8_t minScale, uint8_t maxScale) {
  (void)minScale;
  (void)maxScale;
  if (animationSource_ != AnimationSource::Tracked) {
    return false;
  }

  if (brightnessStep == 0) {
    brightnessStep = kDefaultBrightnessStep;
  }


  const Inputs::ButtonEdges &edges = inputs.edges();
  bool changed = false;

  if (edges.upPressed) {
    const uint16_t nextBrightness = static_cast<uint16_t>(trackedOnBrightness_) + brightnessStep;
    trackedOnBrightness_ = static_cast<uint8_t>(nextBrightness > currentBrightness ? currentBrightness : nextBrightness);
    changed = true;
  }
  if (edges.downPressed) {
    trackedOnBrightness_ =
        static_cast<uint8_t>(trackedOnBrightness_ > brightnessStep ? trackedOnBrightness_ - brightnessStep : 0);
    changed = true;
  }
  if (edges.leftPressed) {
    Haptics::adjustStrength(-static_cast<int>(Haptics::kStrengthStep));
    changed = true;
  }
  if (edges.rightPressed) {
    Haptics::adjustStrength(static_cast<int>(Haptics::kStrengthStep));
    changed = true;
  }

  return changed;
}

bool LEDmatrixAnimator::stop() {
  animationSource_ = AnimationSource::None;
  activeAnimation_ = DefaultAnimation::None;
  animationFrame_ = 0;
  trackedSourceType_ = TrackedSourceType::None;
  trackedBitmapRows32_ = nullptr;
  trackedBitmapWidth_ = 0;
  trackedBitmapHeight_ = 0;
  trackedFrameValid_ = false;
  return true;
}

LEDmatrixAnimator::AnimationSource LEDmatrixAnimator::source() const { return animationSource_; }

void LEDmatrixAnimator::beginTrackedSource(TrackedSourceType sourceType, uint16_t frameIntervalMs) {
  animationSource_ = AnimationSource::Tracked;
  activeAnimation_ = DefaultAnimation::None;
  animationIntervalMs_ = frameIntervalMs;
  lastAnimationStepMs_ = 0;
  trackedSourceType_ = sourceType;
  trackedBitmapRows32_ = nullptr;
  trackedBitmapWidth_ = 0;
  trackedBitmapHeight_ = 0;
  trackedOffsetX_ = 0;
  trackedOffsetY_ = 0;
  trackedScale_ = 1;
  trackedOnBrightness_ = 255;
  trackedOffBrightness_ = 0;
  trackedFrameOverrideEnabled_ = false;
  trackedFrameOverride_ = 0;
  trackedAutoFrame_ = 0;
  trackedFrameValid_ = false;
  temporalLogoLastStepMs_ = 0;
}

LEDmatrixAnimator::TrackedSourceType LEDmatrixAnimator::trackedSourceType() const { return trackedSourceType_; }

uint16_t LEDmatrixAnimator::frameIntervalMs() const { return animationIntervalMs_; }

int8_t LEDmatrixAnimator::trackedOffsetX() const { return trackedOffsetX_; }

int8_t LEDmatrixAnimator::trackedOffsetY() const { return trackedOffsetY_; }

uint8_t LEDmatrixAnimator::trackedScale() const { return trackedScale_; }

uint8_t LEDmatrixAnimator::trackedOnBrightness() const { return trackedOnBrightness_; }

uint8_t LEDmatrixAnimator::trackedOffBrightness() const { return trackedOffBrightness_; }

bool LEDmatrixAnimator::trackedFrameOverrideEnabled() const { return trackedFrameOverrideEnabled_; }

uint8_t LEDmatrixAnimator::trackedFrameOverride() const { return trackedFrameOverride_; }

uint8_t LEDmatrixAnimator::trackedAutoFrame() const { return trackedAutoFrame_; }

LEDmatrixAnimator::DefaultImage LEDmatrixAnimator::trackedImage() const { return trackedImage_; }

LEDmatrixAnimator::DefaultAnimation LEDmatrixAnimator::trackedAnimation() const { return trackedAnimation_; }

const uint32_t *LEDmatrixAnimator::trackedBitmapRows32() const { return trackedBitmapRows32_; }

uint8_t LEDmatrixAnimator::trackedBitmapWidth() const { return trackedBitmapWidth_; }

uint8_t LEDmatrixAnimator::trackedBitmapHeight() const { return trackedBitmapHeight_; }

bool LEDmatrixAnimator::update(uint32_t nowMs, RenderFrame *outFrame, bool *outChanged) {
  if (outFrame == nullptr || outChanged == nullptr || animationSource_ == AnimationSource::None) {
    return false;
  }

  if (lastAnimationStepMs_ != 0 && (nowMs - lastAnimationStepMs_) < animationIntervalMs_) {
    *outChanged = false;
    return true;
  }
  lastAnimationStepMs_ = nowMs;

  if (animationSource_ == AnimationSource::Default) {
    memset(outFrame->mask, 0, LED_MATRIX_HEIGHT);
    switch (activeAnimation_) {
      case DefaultAnimation::Spinner:
      case DefaultAnimation::BlinkSmiley:
      case DefaultAnimation::PulseHeart:
        if (!loadAnimationFrameMask(activeAnimation_, animationFrame_, outFrame->mask)) {
          return false;
        }
        animationFrame_ = static_cast<uint8_t>((animationFrame_ + 1U) % frameCountForAnimation(activeAnimation_));
        outFrame->onBrightness = animationBrightness_;
        outFrame->offBrightness = 0;
        *outChanged = true;
        return true;
      case DefaultAnimation::None:
      default:
        return false;
    }
  }

  if (animationSource_ != AnimationSource::Tracked) {
    return false;
  }

  if (!renderTrackedFrame(outFrame->mask)) {
    return false;
  }
  outFrame->onBrightness = trackedOnBrightness_;
  outFrame->offBrightness = trackedOffBrightness_;

  const bool maskChanged = !trackedFrameValid_ || memcmp(trackedFrameMask_, outFrame->mask, LED_MATRIX_HEIGHT) != 0;
  const bool brightnessChanged =
      !trackedFrameValid_ || trackedFrameOnBrightness_ != outFrame->onBrightness ||
      trackedFrameOffBrightness_ != outFrame->offBrightness;
  *outChanged = maskChanged || brightnessChanged;

  if (*outChanged) {
    memcpy(trackedFrameMask_, outFrame->mask, LED_MATRIX_HEIGHT);
    trackedFrameOnBrightness_ = outFrame->onBrightness;
    trackedFrameOffBrightness_ = outFrame->offBrightness;
    trackedFrameValid_ = true;
  }

  return true;
}

bool LEDmatrixAnimator::renderTrackedFrame(uint8_t outMask[LED_MATRIX_HEIGHT]) {
  if (outMask == nullptr || trackedSourceType_ == TrackedSourceType::None) {
    return false;
  }

  uint8_t baseMask[LED_MATRIX_HEIGHT] = {};
  switch (trackedSourceType_) {
    case TrackedSourceType::Image:
      if (!loadBuiltinImageMask(trackedImage_, baseMask)) {
        return false;
      }
      break;
    case TrackedSourceType::Animation: {
      const uint8_t frameIndex = trackedFrameOverrideEnabled_ ? trackedFrameOverride_ : trackedAutoFrame_;
      if (!loadAnimationFrameMask(trackedAnimation_, frameIndex, baseMask)) {
        return false;
      }
      if (!trackedFrameOverrideEnabled_) {
        trackedAutoFrame_ = static_cast<uint8_t>((trackedAutoFrame_ + 1U) % frameCountForAnimation(trackedAnimation_));
      }
      break;
    }
    case TrackedSourceType::Bitmap32: {
      if (trackedBitmapRows32_ == nullptr || trackedBitmapWidth_ == 0 || trackedBitmapHeight_ == 0) {
        return false;
      }
      for (uint8_t row = 0; row < LED_MATRIX_HEIGHT; ++row) {
        outMask[row] = 0;
      }
      const uint8_t scale = trackedScale_ == 0 ? 1 : trackedScale_;
      for (uint8_t sy = 0; sy < trackedBitmapHeight_; ++sy) {
        const uint32_t rowBits = trackedBitmapRows32_[sy];
        for (uint8_t sx = 0; sx < trackedBitmapWidth_; ++sx) {
          const bool isOn = ((rowBits >> (31U - sx)) & 0x01U) != 0;
          if (!isOn) {
            continue;
          }
          for (uint8_t dy = 0; dy < scale; ++dy) {
            for (uint8_t dx = 0; dx < scale; ++dx) {
              const int16_t tx = static_cast<int16_t>(trackedOffsetX_) + (static_cast<int16_t>(sx) * scale) + dx;
              const int16_t ty = static_cast<int16_t>(trackedOffsetY_) + (static_cast<int16_t>(sy) * scale) + dy;
              if (tx < 0 || tx >= LED_MATRIX_WIDTH || ty < 0 || ty >= LED_MATRIX_HEIGHT) {
                continue;
              }
              outMask[ty] |= static_cast<uint8_t>(0x80U >> tx);
            }
          }
        }
      }
      return true;
    }
    case TrackedSourceType::None:
    default:
      return false;
  }

  applyTransform(baseMask, outMask);
  return true;
}

void LEDmatrixAnimator::applyTransform(const uint8_t inMask[LED_MATRIX_HEIGHT], uint8_t outMask[LED_MATRIX_HEIGHT]) const {
  for (uint8_t row = 0; row < LED_MATRIX_HEIGHT; ++row) {
    outMask[row] = 0;
  }

  const uint8_t scale = trackedScale_ == 0 ? 1 : trackedScale_;
  for (uint8_t sy = 0; sy < LED_MATRIX_HEIGHT; ++sy) {
    for (uint8_t sx = 0; sx < LED_MATRIX_WIDTH; ++sx) {
      const bool isOn = (inMask[sy] & (0x80U >> sx)) != 0;
      if (!isOn) {
        continue;
      }
      for (uint8_t dy = 0; dy < scale; ++dy) {
        for (uint8_t dx = 0; dx < scale; ++dx) {
          const int16_t tx = static_cast<int16_t>(trackedOffsetX_) + (static_cast<int16_t>(sx) * scale) + dx;
          const int16_t ty = static_cast<int16_t>(trackedOffsetY_) + (static_cast<int16_t>(sy) * scale) + dy;
          if (tx < 0 || tx >= LED_MATRIX_WIDTH || ty < 0 || ty >= LED_MATRIX_HEIGHT) {
            continue;
          }
          outMask[ty] |= static_cast<uint8_t>(0x80U >> tx);
        }
      }
    }
  }
}

bool LEDmatrixAnimator::loadBuiltinImageMask(DefaultImage image, uint8_t outMask[LED_MATRIX_HEIGHT]) const {
  if (outMask == nullptr) {
    return false;
  }
  switch (image) {
    case DefaultImage::Smiley:
      memcpy(outMask, LEDmatrixImages::kMaskSmiley, LED_MATRIX_HEIGHT);
      return true;
    case DefaultImage::Heart:
      memcpy(outMask, LEDmatrixImages::kMaskHeart, LED_MATRIX_HEIGHT);
      return true;
    case DefaultImage::ArrowUp:
      memcpy(outMask, LEDmatrixImages::kMaskArrowUp, LED_MATRIX_HEIGHT);
      return true;
    case DefaultImage::ArrowDown:
      memcpy(outMask, LEDmatrixImages::kMaskArrowDown, LED_MATRIX_HEIGHT);
      return true;
    case DefaultImage::XMark:
      memcpy(outMask, LEDmatrixImages::kMaskXMark, LED_MATRIX_HEIGHT);
      return true;
    case DefaultImage::Dot:
      memcpy(outMask, LEDmatrixImages::kMaskDot, LED_MATRIX_HEIGHT);
      return true;
    default:
      return false;
  }
}

bool LEDmatrixAnimator::loadAnimationFrameMask(DefaultAnimation animation, uint8_t frameIndex,
                                               uint8_t outMask[LED_MATRIX_HEIGHT]) const {
  if (outMask == nullptr) {
    return false;
  }
  switch (animation) {
    case DefaultAnimation::Spinner:
      memcpy(outMask, LEDmatrixImages::kAnimSpinner[frameIndex % LEDmatrixImages::kAnimSpinnerFrameCount],
             LED_MATRIX_HEIGHT);
      return true;
    case DefaultAnimation::BlinkSmiley:
      memcpy(outMask, LEDmatrixImages::kAnimBlinkSmiley[frameIndex % LEDmatrixImages::kAnimBlinkSmileyFrameCount],
             LED_MATRIX_HEIGHT);
      return true;
    case DefaultAnimation::PulseHeart:
      memcpy(outMask, LEDmatrixImages::kAnimPulseHeart[frameIndex % LEDmatrixImages::kAnimPulseHeartFrameCount],
             LED_MATRIX_HEIGHT);
      return true;
    case DefaultAnimation::None:
    default:
      return false;
  }
}

uint8_t LEDmatrixAnimator::frameCountForAnimation(DefaultAnimation animation) const {
  switch (animation) {
    case DefaultAnimation::Spinner:
      return LEDmatrixImages::kAnimSpinnerFrameCount;
    case DefaultAnimation::BlinkSmiley:
      return LEDmatrixImages::kAnimBlinkSmileyFrameCount;
    case DefaultAnimation::PulseHeart:
      return LEDmatrixImages::kAnimPulseHeartFrameCount;
    case DefaultAnimation::None:
    default:
      return 1;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LEDmatrix
// ═══════════════════════════════════════════════════════════════════════════════

LEDmatrix::LEDmatrix()
    : driver_(16, 9),
      framebuffer_{},
      initialized_(false),
      brightness_(kGlobalDefaultBrightness),
      animator_(),
      inputs_(nullptr),
      imu_(nullptr) {}

int LEDmatrix::init(uint8_t address) {
  pinMode(LED_MATRIX_ENABLE_PIN, OUTPUT);
  digitalWrite(LED_MATRIX_ENABLE_PIN, HIGH);

  pinMode(LED_MATRIX_INTB_PIN, INPUT_PULLUP);

  pinMode(LED_MATRIX_AUDIO_PIN, OUTPUT);
  digitalWrite(LED_MATRIX_AUDIO_PIN, LOW);

  delay(2);
  // Wire is initialized once in main.cpp setup(); do not re-init here.

  if (!driver_.begin(address, &Wire)) {
    initialized_ = false;
    return -1;
  }

  initialized_ = true;
  brightness_ = kGlobalDefaultBrightness;
  animator_.reset();
  clear(0);
  return 0;
}

bool LEDmatrix::isInitialized() const { return initialized_; }

void LEDmatrix::setBrightness(uint8_t brightness) {
  brightness_ = brightness;
  brightnessFadeTotal_ = 0;  // direct set cancels any in-flight ramp
  if (initialized_) {
    refreshDisplayFromFramebuffer();
  }
}

// ─── Brightness fade ────────────────────────────────────────────────────────
//
// Linear interpolation of the global brightness scale used by
// applyGlobalBrightness(). Each tick re-pushes the framebuffer at the new
// scale, which is cheap (9x9 driver writes) at 33 ms cadence.
void LEDmatrix::startBrightnessFade(uint8_t target, uint32_t durationMs) {
  brightnessFadeFrom_  = brightness_;
  brightnessFadeTarget_ = target;
  brightnessFadeTick_   = 0;
  uint32_t ticks = (durationMs + kBrightnessFadeTickMs - 1) / kBrightnessFadeTickMs;
  if (ticks == 0) ticks = 1;
  if (ticks > UINT16_MAX) ticks = UINT16_MAX;
  brightnessFadeTotal_ = (uint16_t)ticks;
}

void LEDmatrix::tickBrightnessFade() {
  if (brightnessFadeTotal_ == 0) return;

  brightnessFadeTick_++;
  uint8_t lvl;
  if (brightnessFadeTick_ >= brightnessFadeTotal_) {
    lvl = brightnessFadeTarget_;
    brightnessFadeTotal_ = 0;
  } else {
    const uint32_t elapsed   = brightnessFadeTick_;
    const uint32_t remaining = brightnessFadeTotal_ - elapsed;
    const uint32_t total     = brightnessFadeTotal_;
    lvl = (uint8_t)(((uint32_t)brightnessFadeFrom_ * remaining
                   + (uint32_t)brightnessFadeTarget_ * elapsed) / total);
  }

  if (lvl != brightness_) {
    brightness_ = lvl;
    if (initialized_) refreshDisplayFromFramebuffer();
  }
}

bool LEDmatrix::isBrightnessIdle() const { return brightnessFadeTotal_ == 0; }

void LEDmatrix::awaitBrightnessFade() {
  while (brightnessFadeTotal_ > 0) {
    tickBrightnessFade();
    delay(kBrightnessFadeTickMs);
  }
}

uint8_t LEDmatrix::getBrightness() const { return brightness_; }

bool LEDmatrix::clear(uint8_t brightness) { return fill(brightness); }

bool LEDmatrix::fill() { return fill(brightness_); }

bool LEDmatrix::fill(uint8_t brightness) {
  if (!initialized_) {
    return false;
  }

  for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < LED_MATRIX_WIDTH; ++x) {
      setPixel(x, y, brightness);
    }
  }

  return true;
}

bool LEDmatrix::setPixel(uint8_t x, uint8_t y, uint8_t brightness) {
  if (!initialized_ || !inBounds(x, y)) {
    return false;
  }

  framebuffer_[y][x] = normalizeBrightnessForStorage(brightness);
  // Masks/images are authored with y=0 at the visual top when the badge is
  // in its default (kUpright) orientation. The panel is mounted such that
  // this requires a 180° rotation, which we apply when flipped_ is false.
  // Holding the badge inverted (flipped_ = true) cancels that rotation
  // because the user's own inversion already flips what they see.
  const uint8_t fx = !flipped_ ? static_cast<uint8_t>((LED_MATRIX_WIDTH - 1) - x) : x;
  const uint8_t fy = !flipped_ ? static_cast<uint8_t>((LED_MATRIX_HEIGHT - 1) - y) : y;
  const uint8_t hwX = fy;
  const uint8_t hwY = static_cast<uint8_t>((LED_MATRIX_WIDTH - 1) - fx);
  driver_.drawPixel(hwX, hwY, applyGlobalBrightness(framebuffer_[y][x]));
  return true;
}

uint8_t LEDmatrix::getPixel(uint8_t x, uint8_t y) const {
  if (!inBounds(x, y)) {
    return 0;
  }

  return applyGlobalBrightness(framebuffer_[y][x]);
}

bool LEDmatrix::showCheckerboard(uint8_t offBrightness) {
  return showCheckerboard(brightness_, offBrightness);
}

bool LEDmatrix::showCheckerboard(uint8_t onBrightness, uint8_t offBrightness) {
  if (!initialized_) {
    return false;
  }

  for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < LED_MATRIX_WIDTH; ++x) {
      const bool onCell = ((x + y) & 0x01U) == 0;
      setPixel(x, y, onCell ? onBrightness : offBrightness);
    }
  }

  return true;
}

bool LEDmatrix::showImage(DefaultImage image, uint8_t onBrightness, uint8_t offBrightness) {
  return showImageById(defaultImageId(image), ImageSource::Builtin, onBrightness, offBrightness);
}

bool LEDmatrix::showImage(DefaultImage image, uint8_t offBrightness) {
  return showImage(image, brightness_, offBrightness);
}

bool LEDmatrix::showImageById(const char *imageId, ImageSource source, uint8_t offBrightness) {
  return showImageById(imageId, source, brightness_, offBrightness);
}

bool LEDmatrix::showImageById(const char *imageId, ImageSource source, uint8_t onBrightness, uint8_t offBrightness) {
  if (!initialized_) {
    return false;
  }

  stopAnimation();
  uint8_t mask[LED_MATRIX_HEIGHT] = {};
  if (!loadImageMask(imageId, source, mask)) {
    return false;
  }
  return drawMask(mask, onBrightness, offBrightness);
}

bool LEDmatrix::startAnimation(DefaultAnimation animation, uint16_t frameIntervalMs) {
  return startAnimation(animation, frameIntervalMs, brightness_);
}

bool LEDmatrix::startAnimation(DefaultAnimation animation, uint16_t frameIntervalMs, uint8_t brightness) {
  if (!initialized_) {
    return false;
  }
  animator_.startDefaultAnimation(animation, frameIntervalMs, brightness);
  return updateAnimation();
}

bool LEDmatrix::stopAnimation() {
  return animator_.stop();
}

bool LEDmatrix::updateAnimation() {
  if (!initialized_) {
    return false;
  }
  LEDmatrixAnimator::RenderFrame frame = {};
  bool changed = false;
  if (!animator_.update(millis(), &frame, &changed)) {
    return false;
  }
  if (!changed) {
    return true;
  }
  return drawMask(frame.mask, frame.onBrightness, frame.offBrightness);
}

LEDmatrix::AnimationSource LEDmatrix::getAnimationSource() const { return animator_.source(); }

LEDmatrixAnimator &LEDmatrix::animator() { return animator_; }

const LEDmatrixAnimator &LEDmatrix::animator() const { return animator_; }

void LEDmatrix::bindInputs(const Inputs* inputs) { inputs_ = inputs; }

void LEDmatrix::bindAccel(const IMU* imu) { imu_ = imu; }

void LEDmatrix::setFlipped(bool flipped) {
  if (flipped == flipped_) return;
  flipped_ = flipped;
  if (initialized_) {
    refreshDisplayFromFramebuffer();
  }
}


void LEDmatrix::setMicropythonMode(bool active) {
  if (active == micropythonMode_) {
    return;
  }
  micropythonMode_ = active;
  if (active) {
    stopAnimation();
    clear(0);
  } else {
    stopAnimation();
    clear(0);
  }
}

bool LEDmatrix::isMicropythonMode() const {
  return micropythonMode_;
}

void LEDmatrix::service() {
  if (!initialized_ || inputs_ == nullptr) {
    return;
  }
  const uint32_t nowMs = millis();
  if ((nowMs - lastServiceMs_) < Power::Policy::ledServiceIntervalMs) {
    return;
  }
  lastServiceMs_ = nowMs;

  if (brightnessFadeTotal_ > 0) {
    tickBrightnessFade();
  }

  const bool nativeTrackedAnimation =
      animator_.source() == LEDmatrixAnimator::AnimationSource::Tracked;
  if (!micropythonMode_ || nativeTrackedAnimation) {
    const bool temporalTrackedAnimation =
        nativeTrackedAnimation &&
        animator_.trackedSourceType() == LEDmatrixAnimator::TrackedSourceType::Bitmap32 &&
        animator_.trackedBitmapRows32() == LEDmatrixImages::kTemporalLogo32x32;

    if (faceDownHeartActive_) {
      faceDownHeartActive_ = false;
      animator_.startTemporalLogoBitmap32(getBrightness(),
                                           Power::Policy::kCrosshairFrameIntervalMs, 1, 0);
    }

    if (temporalTrackedAnimation) {
      if (imu_ != nullptr && imu_->isReady()) {
        // Polarity-swapped: the Temporal logo demo expects the tilt
        // axes to drive the logo opposite to how the IMU reports them
        // on this board's mounting orientation.
        animator_.updateTemporalLogoFromAccel(-imu_->tiltXMg(), -imu_->tiltYMg(), nowMs);
      } else {
        animator_.updateTemporalLogoFromAccel(0.f, 0.f, nowMs);
      }
    } else if (imu_ != nullptr && imu_->isReady()) {
      animator_.updateCrosshairFromAccelAndJoystick(imu_->tiltXMg(), imu_->tiltYMg(), *inputs_);
    } else {
      animator_.updateCrosshairFromJoystick(*inputs_);
    }
  }

  updateAnimation();
}

const char* LEDmatrix::name() const { return "LEDmatrix"; }

const char *LEDmatrix::defaultImageId(DefaultImage image) {
  switch (image) {
    case DefaultImage::Smiley:
      return LEDmatrixImages::kImageSmiley;
    case DefaultImage::Heart:
      return LEDmatrixImages::kImageHeart;
    case DefaultImage::ArrowUp:
      return LEDmatrixImages::kImageArrowUp;
    case DefaultImage::ArrowDown:
      return LEDmatrixImages::kImageArrowDown;
    case DefaultImage::XMark:
      return LEDmatrixImages::kImageXMark;
    case DefaultImage::Dot:
      return LEDmatrixImages::kImageDot;
    default:
      return nullptr;
  }
}

bool LEDmatrix::loadImageMask(const char *imageId, ImageSource source, uint8_t outMask[LED_MATRIX_HEIGHT]) const {
  if (imageId == nullptr || outMask == nullptr) {
    return false;
  }

  switch (source) {
    case ImageSource::Builtin:
      return getBuiltinImageMask(imageId, outMask);
    case ImageSource::Filesystem:
      return loadFilesystemImageMask(imageId, outMask);
    case ImageSource::Auto:
      if (getBuiltinImageMask(imageId, outMask)) {
        return true;
      }
      return loadFilesystemImageMask(imageId, outMask);
    default:
      return false;
  }
}

bool LEDmatrix::loadFilesystemImageMask(const char *imageId, uint8_t outMask[LED_MATRIX_HEIGHT]) const {
  (void)imageId;
  (void)outMask;
  return false;
}

bool LEDmatrix::drawMask(const uint8_t mask[LED_MATRIX_HEIGHT], uint8_t onBrightness, uint8_t offBrightness) {
  if (!initialized_) {
    return false;
  }

  for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < LED_MATRIX_WIDTH; ++x) {
      const bool on = (mask[y] & (0x80U >> x)) != 0;
      setPixel(x, y, on ? onBrightness : offBrightness);
    }
  }
  return true;
}

uint8_t LEDmatrix::normalizeBrightnessForStorage(uint8_t brightness) const {
  if (brightness == 0) {
    return 0;
  }
  if (brightness_ == 0) {
    return 255;
  }

  uint16_t normalized = static_cast<uint16_t>(brightness) * 255U / brightness_;
  if (normalized > 255U) {
    normalized = 255U;
  }
  return static_cast<uint8_t>(normalized);
}

uint8_t LEDmatrix::applyGlobalBrightness(uint8_t normalizedBrightness) const {
  return static_cast<uint8_t>((static_cast<uint16_t>(normalizedBrightness) * brightness_) / 255U);
}

void LEDmatrix::refreshDisplayFromFramebuffer() {
  if (!initialized_) {
    return;
  }
  for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < LED_MATRIX_WIDTH; ++x) {
      // Polarity must match setPixel: rotate 180° when flipped_ is false so
      // bitmaps authored in the natural y=0-at-top convention render upright
      // in the badge's default (kUpright) orientation.
      const uint8_t fx = !flipped_ ? static_cast<uint8_t>((LED_MATRIX_WIDTH - 1) - x) : x;
      const uint8_t fy = !flipped_ ? static_cast<uint8_t>((LED_MATRIX_HEIGHT - 1) - y) : y;
      const uint8_t hwX = fy;
      const uint8_t hwY = static_cast<uint8_t>((LED_MATRIX_WIDTH - 1) - fx);
      driver_.drawPixel(hwX, hwY, applyGlobalBrightness(framebuffer_[y][x]));
    }
  }
}

void LEDmatrix::blankHardware() {
  if (!initialized_) {
    return;
  }
  for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < LED_MATRIX_WIDTH; ++x) {
      driver_.drawPixel(y, static_cast<uint8_t>((LED_MATRIX_WIDTH - 1) - x), 0);
    }
  }
}

void LEDmatrix::drawFullOnHardware(uint8_t brightness) {
  static constexpr uint8_t kAllOnMask[LED_MATRIX_HEIGHT] = {
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  };
  drawMaskHardware(kAllOnMask, brightness, 0);
}

void LEDmatrix::drawMaskHardware(const uint8_t mask[LED_MATRIX_HEIGHT], uint8_t onBrightness,
                                 uint8_t offBrightness) {
  if (!initialized_) {
    return;
  }
  for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < LED_MATRIX_WIDTH; ++x) {
      const bool on = (mask[y] & (0x80U >> x)) != 0;
      // Polarity must match setPixel: rotate 180° when flipped_ is false so
      // direct-to-hardware masks share the same y=0-at-top convention as
      // the framebuffer path.
      const uint8_t fx = !flipped_ ? static_cast<uint8_t>((LED_MATRIX_WIDTH - 1) - x) : x;
      const uint8_t fy = !flipped_ ? static_cast<uint8_t>((LED_MATRIX_HEIGHT - 1) - y) : y;
      const uint8_t hwX = fy;
      const uint8_t hwY = static_cast<uint8_t>((LED_MATRIX_WIDTH - 1) - fx);
      driver_.drawPixel(hwX, hwY, on ? onBrightness : offBrightness);
    }
  }
}

bool LEDmatrix::inBounds(uint8_t x, uint8_t y) {
  return x < LED_MATRIX_WIDTH && y < LED_MATRIX_HEIGHT;
}

#endif  // BADGE_HAS_LED_MATRIX
