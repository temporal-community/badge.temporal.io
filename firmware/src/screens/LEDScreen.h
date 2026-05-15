#pragma once

#include "JoyRamp.h"
#include "Screen.h"
#include "../led/LEDAppRuntime.h"

class LEDScreen : public Screen {
 public:
  LEDScreen();

  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX,
                   int16_t cursorY, GUIManager& gui) override;
  ScreenId id() const override { return kScreenMatrixApps; }
  bool showCursor() const override { return false; }

 private:
  enum class View : uint8_t {
    Carousel,
    LifeEditor,
    CustomEditor,
    Presets,
  };

  void enterCarousel();
  void enterEditor(LEDAppRuntime::Mode mode);
  void enterPresets();
  void cancelEditor();
  void saveEditor(GUIManager& gui);
  void loadPreset(uint8_t index);
  void moveMode(int8_t dir);
  // Total carousel slots: built-in modes + discovered Python matrix apps.
  uint8_t carouselCount() const;
  // True when carouselIndex points past the built-ins into a matrix app.
  bool isPythonAppIndex(uint8_t index) const;
  // 0..matrixAppCount-1 — caller must check isPythonAppIndex first.
  uint8_t matrixAppOffset(uint8_t index) const;
  void moveCursor(int8_t dx, int8_t dy);
  void drawGrid(oled& d, const uint8_t frame[8], int8_t cursorX,
                int8_t cursorY, int x, int y, bool showOffDots);
  void drawCarousel(oled& d);
  void drawEditor(oled& d);
  void drawPresets(oled& d);
  const char* presetName(uint8_t index) const;
  uint8_t presetCount() const;
  void presetFrame(uint8_t index, uint8_t out[8]);
  uint8_t randomByte();

  View view_ = View::Carousel;
  LEDAppRuntime::Mode selectedMode_ = LEDAppRuntime::Mode::Temporal;
  LEDAppRuntime::Mode editingMode_ = LEDAppRuntime::Mode::Life;
  uint8_t draft_[8] = {};
  uint8_t modeIndex_ = 0;
  uint8_t presetIndex_ = 0;
  uint8_t cursorX_ = 0;
  uint8_t cursorY_ = 0;
  bool committed_ = false;
  uint16_t delay_ = LEDAppRuntime::kDefaultDelay;
  uint8_t brightness_ = LEDAppRuntime::kDefaultBrightness;
  bool adjDelay_ = true;
  JoyRamp joyRamp_;
  uint32_t rng_ = 0xA341316C;
};
