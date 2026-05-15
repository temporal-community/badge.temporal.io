// ScalePickerScreen — list available destination sizes for a sticker.
//
// For catalog stickers, dimensions come from `ImageScaler::availableScales`.
// For saved-anim stickers, they come from {native, native/2, native/4,
// native/8} filtered to ≥ 4 px on the smaller axis.

#pragma once

#include <cstdint>

#include "../Screen.h"
#include "StickerPickerScreen.h"

class ScalePickerScreen : public Screen {
 public:
  using Callback = void (*)(uint8_t scale, GUIManager& gui, void* user);

  // Configure with the same Result that StickerPickerScreen produced — we
  // need to know the source dimensions to compute valid scales.
  void configure(const StickerPickerScreen::Result& src, Callback cb, void* user);

  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenDrawScalePicker; }
  bool showCursor() const override { return false; }

 private:
  void rebuildOptions();

  static constexpr uint8_t kMaxOptions = 6;
  static constexpr uint16_t kJoyDeadband = 400;

  StickerPickerScreen::Result src_{};
  Callback cb_ = nullptr;
  void* user_ = nullptr;
  uint8_t options_[kMaxOptions] = {};
  uint8_t optionHeights_[kMaxOptions] = {};
  uint8_t optionCount_ = 0;
  uint8_t cursor_ = 0;
  uint32_t lastJoyNavMs_ = 0;
};

extern ScalePickerScreen sScalePicker;
