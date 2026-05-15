#pragma once
//
// MenuOrderScreen — manual reorder UI for the main grid menu.
//
// Lists every item currently in the main menu (curated + AppRegistry-
// discovered) in the order the rebuilder produced. The user picks a
// row with `X`, moves it up/down with the joystick, and drops it again
// with `X`. `A`/Confirm saves the new ordering to NVS via
// MenuOrderStore and triggers a main-menu rebuild. `B`/Back cancels
// without persisting.
//
// The screen takes its own snapshot of labels at onEnter() so the live
// menu state can change underneath us (e.g. badge.rescan_apps())
// without stale pointers.

#include "Screen.h"

#include "JoyRamp.h"

class MenuOrderScreen : public Screen {
 public:
  MenuOrderScreen();

  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenMenuOrder; }
  bool showCursor() const override { return false; }

 private:
  static constexpr uint8_t kMaxItems = 48;
  static constexpr uint8_t kLabelCap = 20;

  void snapshotFromLiveMenu();
  void moveCursor(int8_t dir);
  void swap(uint8_t a, uint8_t b);
  void commitAndExit(GUIManager& gui);

  char labels_[kMaxItems][kLabelCap];
  uint8_t count_ = 0;
  uint8_t cursor_ = 0;
  uint8_t scroll_ = 0;
  bool picked_ = false;
  uint32_t lastJoyMs_ = 0;
};
