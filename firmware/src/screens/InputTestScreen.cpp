#include "InputTestScreen.h"

#include <cmath>
#include <cstdio>
#include <Arduino.h>

#include "../ui/GUI.h"
#include "../hardware/IMU.h"
#include "../hardware/Inputs.h"
#include "../ui/graphics.h"
#include "../hardware/oled.h"

extern Inputs inputs;

void InputTestScreen::onEnter(GUIManager& /*gui*/) {
  lastActivityMs_ = millis();
  prevBtns_ = 0;
  prevTilt_ = false;
}

void InputTestScreen::render(oled& d, GUIManager& /*gui*/) {
  extern IMU imu;
  d.setDrawColor(1);

  d.drawXBM(0, 0, Graphics_Base_width, Graphics_Base_height,
            Graphics_Base_bits);

  constexpr int kJoyCircleCx = 100;
  constexpr int kJoyCircleCy = 53;
  constexpr int kJoyCircleR = 6;
  constexpr int kJoyDeadband = 170;
  float nx = (static_cast<int>(inputs.joyX()) - 2047) / 2047.0f;
  float ny = (static_cast<int>(inputs.joyY()) - 2047) / 2047.0f;
  if (abs(static_cast<int>(inputs.joyX()) - 2047) < kJoyDeadband) nx = 0.0f;
  if (abs(static_cast<int>(inputs.joyY()) - 2047) < kJoyDeadband) ny = 0.0f;
  float px = nx * kJoyCircleR;
  float py = ny * kJoyCircleR;
  float dist = sqrtf(px * px + py * py);
  if (dist > kJoyCircleR) {
    px = px / dist * kJoyCircleR;
    py = py / dist * kJoyCircleR;
  }
  int jx = kJoyCircleCx + static_cast<int>(roundf(px));
  int jy = kJoyCircleCy + static_cast<int>(roundf(py));
  d.drawBox(jx - 1, jy - 1, 3, 3);

  bool curTilt = imu.isReady() && imu.isFaceDown();
  if (!curTilt)
    d.drawBox(84, 48, 4, 5);
  else
    d.drawBox(84, 54, 4, 5);

  const Inputs::ButtonStates& btns = inputs.buttons();
  struct { int x; int y; } btnPos[] = {
    {118, 48}, {118, 58}, {113, 53}, {123, 53},
  };
  if (btns.up)    d.drawBox(btnPos[0].x - 1, btnPos[0].y - 1, 3, 3);
  if (btns.down)  d.drawBox(btnPos[1].x - 1, btnPos[1].y - 1, 3, 3);
  if (btns.left)  d.drawBox(btnPos[2].x - 1, btnPos[2].y - 1, 3, 3);
  if (btns.right) d.drawBox(btnPos[3].x - 1, btnPos[3].y - 1, 3, 3);

  unsigned long elapsed = millis() - lastActivityMs_;
  int secsLeft = (int)((5000 - elapsed + 999) / 1000);
  if (secsLeft < 0) secsLeft = 0;

  char buf[4];
  snprintf(buf, sizeof(buf), "%d", secsLeft);
  d.setFontPreset(FONT_XLARGE);
  int tw = d.getStrWidth(buf);
  d.setDrawColor(0);
  d.drawBox(64 - tw / 2 - 2, 16, tw + 4, 34);
  d.setDrawColor(1);
  d.drawStr(64 - tw / 2, 48, buf);
}

void InputTestScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                  int16_t /*cy*/, GUIManager& gui) {
  extern IMU imu;

  const Inputs::ButtonStates& btns = inputs.buttons();
  uint8_t curBtns = 0;
  if (btns.up)    curBtns |= 1;
  if (btns.down)  curBtns |= 2;
  if (btns.left)  curBtns |= 4;
  if (btns.right) curBtns |= 8;

  bool curTilt = imu.isReady() && imu.isFaceDown();
  if (curBtns != prevBtns_ || curTilt != prevTilt_) {
    prevBtns_ = curBtns;
    prevTilt_ = curTilt;
    lastActivityMs_ = millis();
  }

  if (millis() - lastActivityMs_ >= 5000) {
    gui.popScreen();
  }
}
