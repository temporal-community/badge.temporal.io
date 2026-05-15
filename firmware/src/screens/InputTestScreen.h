#pragma once
#include "Screen.h"

// ─── Input test screen ──────────────────────────────────────────────────────
// Visualizes joystick, tilt, and button state on a static background.
// Auto-pops back to its caller after 5 seconds of input idle.

class InputTestScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenInputTest; }
  bool showCursor() const override { return false; }

 private:
  uint32_t lastActivityMs_ = 0;
  uint8_t prevBtns_ = 0;
  bool prevTilt_ = false;
};
