#ifndef LEDMATRIX_H
#define LEDMATRIX_H

#include "HardwareConfig.h"

#ifdef BADGE_HAS_LED_MATRIX
// ── Full IS31FL3731 LED matrix driver (Charlie board only) ────────────────────

#include <Adafruit_IS31FL3731.h>
#include <Arduino.h>
#include <Wire.h>

#include "LEDmatrixImages.h"
#include "../infra/Scheduler.h"

class Inputs;
class IMU;
class LEDmatrix;

class LEDmatrixAnimator {
 public:
  enum class DefaultImage : uint8_t {
    Smiley,
    Heart,
    ArrowUp,
    ArrowDown,
    XMark,
    Dot
  };

  enum class DefaultAnimation : uint8_t {
    None,
    Spinner,
    BlinkSmiley,
    PulseHeart
  };

  enum class AnimationSource : uint8_t {
    None,
    Default,
    Tracked
  };

  enum class TrackedSourceType : uint8_t {
    None,
    Image,
    Animation,
    Bitmap32
  };

  struct RenderFrame {
    uint8_t mask[LED_MATRIX_HEIGHT];
    uint8_t onBrightness;
    uint8_t offBrightness;
  };

  LEDmatrixAnimator();

  void reset();

  void serviceIntGpOverlay(LEDmatrix &matrix, IMU &imu, bool wakeOnlyMode, const Inputs &inputs,
                           uint32_t nowMs);

  static bool isUserActive(const Inputs &inputs);
  bool startDefaultAnimation(DefaultAnimation animation, uint16_t frameIntervalMs, uint8_t brightness);
  bool startTrackedImage(DefaultImage image, uint16_t frameIntervalMs);
  bool startTrackedAnimation(DefaultAnimation animation, uint16_t frameIntervalMs);
  bool startTrackedBitmap32(const uint32_t *rows, uint8_t width, uint8_t height, uint16_t frameIntervalMs);
  bool startCrosshairBitmap32(uint8_t maxOnBrightness, uint16_t frameIntervalMs = 20, uint8_t scale = 1,
                              uint8_t offBrightness = 0);
  bool startTemporalLogoBitmap32(uint8_t maxOnBrightness, uint16_t frameIntervalMs = 20, uint8_t scale = 1,
                                  uint8_t offBrightness = 0);
  bool updateCrosshairFromJoystick(const Inputs &inputs);
  bool updateCrosshairFromAccel(float axMg, float ayMg);
  bool updateCrosshairFromAccelAndJoystick(float axMg, float ayMg, const Inputs &inputs);
  bool updateTemporalLogoFromAccel(float axMg, float ayMg, uint32_t nowMs);
  bool setTrackedPosition(int8_t x, int8_t y);
  bool setTrackedScale(uint8_t scale);
  bool setTrackedBrightness(uint8_t onBrightness, uint8_t offBrightness);
  bool setTrackedFrame(uint8_t frameIndex);
  bool clearTrackedFrameOverride();
  bool handleDirectionalButtons(const Inputs &inputs, uint8_t maxOnBrightness = 254, uint8_t brightnessStep = 1,
                                uint8_t minScale = 1, uint8_t maxScale = 8);
  bool stop();

  AnimationSource source() const;
  TrackedSourceType trackedSourceType() const;
  uint16_t frameIntervalMs() const;
  int8_t trackedOffsetX() const;
  int8_t trackedOffsetY() const;
  uint8_t trackedScale() const;
  uint8_t trackedOnBrightness() const;
  uint8_t trackedOffBrightness() const;
  bool trackedFrameOverrideEnabled() const;
  uint8_t trackedFrameOverride() const;
  uint8_t trackedAutoFrame() const;
  DefaultImage trackedImage() const;
  DefaultAnimation trackedAnimation() const;
  const uint32_t *trackedBitmapRows32() const;
  uint8_t trackedBitmapWidth() const;
  uint8_t trackedBitmapHeight() const;
  bool update(uint32_t nowMs, RenderFrame *outFrame, bool *outChanged);

 private:
  void beginTrackedSource(TrackedSourceType sourceType, uint16_t frameIntervalMs);
  bool renderTrackedFrame(uint8_t outMask[LED_MATRIX_HEIGHT]);
  void applyTransform(const uint8_t inMask[LED_MATRIX_HEIGHT], uint8_t outMask[LED_MATRIX_HEIGHT]) const;
  bool loadBuiltinImageMask(DefaultImage image, uint8_t outMask[LED_MATRIX_HEIGHT]) const;
  bool loadAnimationFrameMask(DefaultAnimation animation, uint8_t frameIndex,
                              uint8_t outMask[LED_MATRIX_HEIGHT]) const;
  uint8_t frameCountForAnimation(DefaultAnimation animation) const;

  AnimationSource animationSource_;
  DefaultAnimation activeAnimation_;
  uint32_t lastAnimationStepMs_;
  uint16_t animationIntervalMs_;
  uint8_t animationBrightness_;
  uint8_t animationFrame_;

  TrackedSourceType trackedSourceType_;
  DefaultImage trackedImage_;
  DefaultAnimation trackedAnimation_;
  const uint32_t *trackedBitmapRows32_;
  uint8_t trackedBitmapWidth_;
  uint8_t trackedBitmapHeight_;
  int8_t trackedOffsetX_;
  int8_t trackedOffsetY_;
  uint8_t trackedScale_;
  uint8_t trackedOnBrightness_;
  uint8_t trackedOffBrightness_;
  bool trackedFrameOverrideEnabled_;
  uint8_t trackedFrameOverride_;
  uint8_t trackedAutoFrame_;
  bool trackedFrameValid_;
  uint8_t trackedFrameMask_[LED_MATRIX_HEIGHT];
  uint8_t trackedFrameOnBrightness_;
  uint8_t trackedFrameOffBrightness_;
  uint32_t temporalLogoLastStepMs_;

  bool intGpOverlayActive_;
};

class LEDmatrix : public IService {
 public:
  static constexpr uint8_t kGlobalDefaultBrightness = 5;

  using DefaultImage = LEDmatrixAnimator::DefaultImage;
  using DefaultAnimation = LEDmatrixAnimator::DefaultAnimation;

  enum class ImageSource : uint8_t {
    Auto,
    Builtin,
    Filesystem
  };

  using AnimationSource = LEDmatrixAnimator::AnimationSource;

  LEDmatrix();

  int init(uint8_t address = ISSI_ADDR_DEFAULT);
  bool isInitialized() const;
  void setBrightness(uint8_t brightness);
  uint8_t getBrightness() const;

  // Asynchronous brightness ramp. The IS31FL3731 has no global brightness
  // register, so this is per-pixel software scaling — visually identical
  // to a hardware fade. Tick advances from service(); use awaitBrightnessFade
  // to spin until done.
  void startBrightnessFade(uint8_t target, uint32_t durationMs);
  void tickBrightnessFade();
  bool isBrightnessIdle() const;
  void awaitBrightnessFade();

  bool clear(uint8_t brightness = 0);
  bool fill();
  bool fill(uint8_t brightness);
  bool setPixel(uint8_t x, uint8_t y, uint8_t brightness);
  uint8_t getPixel(uint8_t x, uint8_t y) const;

  // One-shot copy of the logical 8×8 image (after global brightness), same
  // values as getPixel().  Used by screenshot().  Accurate whenever the
  // visible frame was produced via setPixel / drawMask / clear / fill /
  // showImage paths that maintain framebuffer_.  drawMaskHardware is for
  // transient overlays only (e.g. intGp flash) and does not update
  // framebuffer_ — avoid screenshot during that cue if pixel-perfect match
  // to silicon matters.
  bool snapshotFramebufferDisplay(uint8_t out[LED_MATRIX_HEIGHT][LED_MATRIX_WIDTH]);

  bool showCheckerboard(uint8_t offBrightness = 0);
  bool showCheckerboard(uint8_t onBrightness, uint8_t offBrightness);
  bool showImage(DefaultImage image, uint8_t offBrightness = 0);
  bool showImage(DefaultImage image, uint8_t onBrightness, uint8_t offBrightness);
  bool showImageById(const char *imageId, ImageSource source = ImageSource::Auto, uint8_t offBrightness = 0);
  bool showImageById(const char *imageId, ImageSource source, uint8_t onBrightness, uint8_t offBrightness);
  bool startAnimation(DefaultAnimation animation, uint16_t frameIntervalMs = 120);
  bool startAnimation(DefaultAnimation animation, uint16_t frameIntervalMs, uint8_t brightness);
  bool stopAnimation();
  bool updateAnimation();
  AnimationSource getAnimationSource() const;
  LEDmatrixAnimator &animator();
  const LEDmatrixAnimator &animator() const;
  void bindInputs(const Inputs* inputs);
  void bindAccel(const IMU* imu);

  void setMicropythonMode(bool active);
  bool isMicropythonMode() const;

  void setFlipped(bool flipped);
  bool isFlipped() const { return flipped_; }

  void refreshDisplayFromFramebuffer();
  void blankHardware();
  void drawFullOnHardware(uint8_t brightness);
  void drawMaskHardware(const uint8_t mask[LED_MATRIX_HEIGHT], uint8_t onBrightness,
                        uint8_t offBrightness = 0);

  // Persistent full-frame update: each pixel goes through setPixel() so
  // framebuffer_ tracks the visible pattern.  Use this for matrix apps,
  // MicroPython led_set_frame, etc.  drawMaskHardware skips framebuffer_
  // (intGp overlay / blank-to-silicon shortcuts).
  bool drawMask(const uint8_t mask[LED_MATRIX_HEIGHT], uint8_t onBrightness,
                uint8_t offBrightness = 0);

  void service() override;
  const char* name() const override;

 private:
  static const char *defaultImageId(DefaultImage image);
  bool loadImageMask(const char *imageId, ImageSource source, uint8_t outMask[LED_MATRIX_HEIGHT]) const;
  bool loadFilesystemImageMask(const char *imageId, uint8_t outMask[LED_MATRIX_HEIGHT]) const;
  uint8_t normalizeBrightnessForStorage(uint8_t brightness) const;
  uint8_t applyGlobalBrightness(uint8_t normalizedBrightness) const;
  static bool inBounds(uint8_t x, uint8_t y);

  Adafruit_IS31FL3731 driver_;
  uint8_t framebuffer_[LED_MATRIX_HEIGHT][LED_MATRIX_WIDTH];
  bool initialized_;
  uint8_t brightness_;
  LEDmatrixAnimator animator_;
  const Inputs* inputs_;
  const IMU* imu_;
  bool faceDownHeartActive_ = false;
  bool micropythonMode_ = false;
  bool flipped_ = false;
  uint32_t lastServiceMs_ = 0;

  uint8_t  brightnessFadeFrom_  = 0;
  uint8_t  brightnessFadeTarget_ = 0;
  uint16_t brightnessFadeTick_  = 0;
  uint16_t brightnessFadeTotal_ = 0;   // 0 = idle
  static constexpr uint32_t kBrightnessFadeTickMs = 33;  // matches LED service cadence
};

#else
// ── Stub LEDmatrix — no hardware, same API as no-ops ─────────────────────────
#include <Arduino.h>
#include "../infra/Scheduler.h"

class Inputs;
class IMU;

class LEDmatrixAnimator {
 public:
  enum class DefaultAnimation : uint8_t { None, Spinner, BlinkSmiley, PulseHeart };
  static bool isUserActive(const Inputs &) { return false; }
  bool startCrosshairBitmap32(uint8_t, uint16_t, int, int) { return false; }
  bool startTemporalLogoBitmap32(uint8_t, uint16_t, int, int) { return false; }
  bool setTrackedBrightness(uint8_t, uint8_t) { return false; }
};

class LEDmatrix : public IService {
 public:
  using DefaultAnimation = LEDmatrixAnimator::DefaultAnimation;
  enum class ImageSource : uint8_t { Auto, Builtin, Filesystem };

  int init(uint8_t = 0) { return 0; }
  bool isInitialized() const { return false; }
  void service() override {}
  const char* name() const override { return "LEDmatrix"; }
  void setBrightness(uint8_t) {}
  uint8_t getBrightness() const { return 0; }
  void startBrightnessFade(uint8_t, uint32_t) {}
  void tickBrightnessFade() {}
  bool isBrightnessIdle() const { return true; }
  void awaitBrightnessFade() {}
  bool clear(uint8_t = 0) { return false; }
  bool fill() { return false; }
  bool fill(uint8_t) { return false; }
  bool setPixel(uint8_t, uint8_t, uint8_t) { return false; }
  uint8_t getPixel(uint8_t, uint8_t) const { return 0; }
  bool snapshotFramebufferDisplay(uint8_t[8][8]) { return false; }
  bool drawMask(const uint8_t*, uint8_t, uint8_t = 0) { return false; }
  bool showImageById(const char*, ImageSource = ImageSource::Auto, uint8_t = 0) { return false; }
  bool showImageById(const char*, ImageSource, uint8_t, uint8_t) { return false; }
  bool startAnimation(DefaultAnimation, uint16_t = 120) { return false; }
  bool startAnimation(DefaultAnimation, uint16_t, uint8_t) { return false; }
  bool stopAnimation() { return false; }
  bool updateAnimation() { return false; }
  void drawMaskHardware(const uint8_t*, uint8_t, uint8_t = 0) {}
  void bindInputs(const Inputs*) {}
  void bindAccel(const IMU*) {}
  void setMicropythonMode(bool) {}
  bool isMicropythonMode() const { return false; }
  void setFlipped(bool) {}
  bool isFlipped() const { return false; }
  LEDmatrixAnimator& animator() { static LEDmatrixAnimator a; return a; }
  const LEDmatrixAnimator& animator() const { static LEDmatrixAnimator a; return a; }
};

#endif  // BADGE_HAS_LED_MATRIX
#endif  // LEDMATRIX_H
