#pragma once

#include <stdint.h>

#include "../hardware/HardwareConfig.h"
#include "Screen.h"

// ─── BootScreen ────────────────────────────────────────────────────────────
//
// Splash screen pushed first by GUIManager. Plays the Replay boot frames while
// running a random per-pixel sparkle animation on the LED matrix (ported from
// the reference firmware's LED_ANIM_STAR loop). It routes home after
// kMaxAnimationMs, or immediately when the user presses any button.
//
// Cursor is suppressed.

class BootScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenBoot; }
  bool showCursor() const override { return false; }
  ScreenAccess access() const override { return ScreenAccess::kAny; }

 private:
  static constexpr uint32_t kMaxAnimationMs  = 5000;
  static constexpr uint32_t kSparkleTickMs   = 33;     // ~30 fps
#ifdef BADGE_HAS_LED_MATRIX
  static constexpr int      kCellCount       = LED_MATRIX_WIDTH * LED_MATRIX_HEIGHT;
#else
  static constexpr int      kCellCount       = 1;       // dummy storage
#endif
  static constexpr uint8_t  kSparkleMaxBrt   = 50;     // brightness cap
  static constexpr uint8_t  kSparkleMaxLive  = 12;
  static constexpr uint8_t  kSparkleSpawnPct = 25;
  static constexpr int8_t   kSparkleSpeedMin = 4;
  static constexpr int8_t   kSparkleSpeedMax = 16;

  struct Star {
    uint8_t bright;
    int8_t  speed;     // 0 = idle, +ve = brightening, -ve = fading
  };
  Star stars_[kCellCount] = {};

  uint32_t startMs_           = 0;
  uint32_t lastSparkleMs_     = 0;
  bool     replaceRequested_  = false;

  void tickSparkles(uint32_t nowMs);
  bool starIsIsolated(int idx) const;
  void clearSparkles();
  ScreenId homeRoute() const;
};
