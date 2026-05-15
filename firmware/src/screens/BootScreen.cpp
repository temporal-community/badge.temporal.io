#include "BootScreen.h"

#include <Arduino.h>
#include <cstring>

#include "../hardware/Inputs.h"
#include "../hardware/LEDmatrix.h"
#include "../hardware/oled.h"
#include "../ui/BadgeDisplay.h"
#include "../ui/GUI.h"

extern LEDmatrix badgeMatrix;

namespace {
#ifdef BADGE_HAS_LED_MATRIX
constexpr int   kCols = LED_MATRIX_WIDTH;
constexpr int   kRows = LED_MATRIX_HEIGHT;
#endif
}  // namespace

void BootScreen::onEnter(GUIManager& /*gui*/) {
  std::memset(stars_, 0, sizeof(stars_));
  startMs_          = millis();
  lastSparkleMs_    = 0;
  replaceRequested_ = false;
#ifdef BADGE_HAS_LED_MATRIX
  // Make sure no other animation is fighting us for the matrix during the
  // splash window. We drive setPixel() directly from render().
  badgeMatrix.stopAnimation();
  badgeMatrix.clear(0);
#endif
}

void BootScreen::onExit(GUIManager& /*gui*/) {
  clearSparkles();
}

void BootScreen::render(oled& d, GUIManager& gui) {
  const uint32_t nowMs = millis();
  tickSparkles(nowMs);

  d.setDrawColor(1);
  const uint8_t frameCount = replayBootFrameCount();
  const uint16_t frameMs = replayBootFrameMs();
  uint8_t frame = frameCount > 0
      ? static_cast<uint8_t>(((nowMs - startMs_) / frameMs) % frameCount)
      : 0;
  drawReplayBootFrame(d, frame);

  // One-shot replace at the end. Guard with the bool so
  // a slow render frame after the deadline doesn't fire replaceScreen()
  // twice (the second call would no-op but logs a warning).
  if (!replaceRequested_ && (nowMs - startMs_) >= kMaxAnimationMs) {
    replaceRequested_ = true;
    gui.replaceScreen(homeRoute());
  }
  // While replace is pending we want every frame to keep sparkling, so
  // ask GUIManager to redraw rather than relying on input edges.
  gui.requestRender();
}

void BootScreen::handleInput(const Inputs& inputs, int16_t /*cursorX*/,
                             int16_t /*cursorY*/, GUIManager& gui) {
  // Any button skips the animation.
  const Inputs::ButtonEdges& e = inputs.edges();
  if (replaceRequested_) return;
  if (e.aPressed || e.bPressed || e.xPressed || e.yPressed ||
      e.confirmPressed || e.cancelPressed ||
      e.upPressed || e.downPressed || e.leftPressed || e.rightPressed) {
    replaceRequested_ = true;
    gui.replaceScreen(homeRoute());
  }
}

ScreenId BootScreen::homeRoute() const {
  return kScreenMainMenu;
}

// ── Sparkle animator (port of LED_ANIM_STAR from the reference) ────────────

void BootScreen::tickSparkles(uint32_t nowMs) {
#ifndef BADGE_HAS_LED_MATRIX
  (void)nowMs;
  return;
#else
  if (lastSparkleMs_ == 0) lastSparkleMs_ = nowMs;
  if ((nowMs - lastSparkleMs_) < kSparkleTickMs) return;
  lastSparkleMs_ = nowMs;

  int activeCount = 0;
  for (int i = 0; i < kCellCount; ++i) {
    if (stars_[i].speed == 0) continue;
    activeCount++;

    int b = (int)stars_[i].bright + (int)stars_[i].speed;
    if (stars_[i].speed > 0 && b >= kSparkleMaxBrt) {
      b = kSparkleMaxBrt;
      stars_[i].speed = (int8_t)-stars_[i].speed;  // peaked → start fading
    } else if (stars_[i].speed < 0 && b <= 0) {
      b = 0;
      stars_[i].speed = 0;
      stars_[i].bright = 0;
      const uint8_t x = (uint8_t)(i % kCols);
      const uint8_t y = (uint8_t)(i / kCols);
      badgeMatrix.setPixel(x, y, 0);
      continue;
    }
    stars_[i].bright = (uint8_t)b;
    const uint8_t x = (uint8_t)(i % kCols);
    const uint8_t y = (uint8_t)(i / kCols);
    badgeMatrix.setPixel(x, y, stars_[i].bright);
  }

  // Maybe spawn a fresh star, capped at kSparkleMaxLive concurrent.
  if (activeCount < kSparkleMaxLive &&
      (int)random(100) < kSparkleSpawnPct) {
    for (int attempt = 0; attempt < 16; ++attempt) {
      int idx = (int)random(kCellCount);
      if (stars_[idx].speed == 0 && stars_[idx].bright == 0 &&
          starIsIsolated(idx)) {
        const int range = kSparkleSpeedMax - kSparkleSpeedMin + 1;
        stars_[idx].speed =
            (int8_t)(kSparkleSpeedMin + (int)random(range));
        break;
      }
    }
  }
#endif
}

bool BootScreen::starIsIsolated(int idx) const {
#ifndef BADGE_HAS_LED_MATRIX
  (void)idx;
  return false;
#else
  // 8-connected isolation: spawning into a cell adjacent to a live star
  // visually clumps. The reference enforces the same rule.
  const int r = idx / kCols;
  const int c = idx % kCols;
  for (int dr = -1; dr <= 1; ++dr) {
    for (int dc = -1; dc <= 1; ++dc) {
      if (dr == 0 && dc == 0) continue;
      const int nr = r + dr;
      const int nc = c + dc;
      if (nr < 0 || nr >= kRows || nc < 0 || nc >= kCols) continue;
      const int ni = nr * kCols + nc;
      if (stars_[ni].speed != 0 || stars_[ni].bright != 0) return false;
    }
  }
  return true;
#endif
}

void BootScreen::clearSparkles() {
#ifdef BADGE_HAS_LED_MATRIX
  std::memset(stars_, 0, sizeof(stars_));
  badgeMatrix.clear(0);
#endif
}
