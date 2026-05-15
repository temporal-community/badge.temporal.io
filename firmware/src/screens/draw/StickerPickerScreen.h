// StickerPickerScreen — list every catalog image and saved animation
// (excluding the one currently being edited) so the user can pick one to
// place. On select, fires `onPicked` with the chosen catalog index OR
// saved-anim id, and pops back. Caller pushes ScalePickerScreen next.

#pragma once

#include <cstdint>
#include <vector>

#include "../Screen.h"
#include "AnimDoc.h"

class StickerPickerScreen : public Screen {
 public:
  enum class PickKind : uint8_t { Catalog, SavedAnim };

  struct Result {
    PickKind kind;
    int16_t  catalogIdx;
    char     savedAnimId[draw::kAnimIdLen + 1];
  };

  using Callback = void (*)(const Result& r, GUIManager& gui, void* user);

  void configure(const char* excludeAnimId, Callback cb, void* user);

  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenDrawStickerPicker; }
  bool showCursor() const override { return false; }

 private:
  void rebuildEntries();
  uint8_t total() const;
  void moveCursor(int8_t delta);

  static constexpr uint8_t kVisibleRows = 5;
  static constexpr uint8_t kRowHeight = 11;
  static constexpr uint8_t kTopY = 9;
  static constexpr uint16_t kJoyDeadband = 400;

  Callback cb_ = nullptr;
  void* user_ = nullptr;
  char excludeId_[draw::kAnimIdLen + 1] = {};

  uint8_t catalogCount_ = 0;
  std::vector<draw::AnimSummary> savedEntries_;
  uint8_t cursor_ = 0;
  uint8_t scroll_ = 0;
  uint32_t lastJoyNavMs_ = 0;
};

extern StickerPickerScreen sStickerPicker;
