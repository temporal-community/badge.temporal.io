#pragma once
#include "JoyRamp.h"
#include "Screen.h"
#include "../infra/BadgeConfig.h"

class Config;

// ─── Settings screen ────────────────────────────────────────────────────────
//
// Settings are grouped into collapsible categories (Display, Haptics, LED,
// Input, etc.). Joystick Y scrolls; the physical Down button toggles the
// dropdown on the currently-focused group. Only one group is open at a
// time — moving the cursor outside the open group's rows auto-collapses it.
//
// Per-row interaction is two-mode:
//   • Nav mode  — cursor scrolls, A enters edit mode on the focused setting,
//                 B pops back to the parent menu.
//   • Edit mode — value on the right of the row is highlighted; joystick
//                 left/right changes it, A or B exits.
//
// While editing kLedBrightness, the entire LED matrix is lit at the
// current value via LEDAppRuntime::beginOverride/endOverride; on exit the
// user's animation resumes.

class SettingsScreen : public ListMenuScreen {
 public:
  SettingsScreen(ScreenId sid, Config* config);

  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  void onItemAdjust(uint8_t index, int8_t dir, GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  const char* hintText() const override;

  // Called by the static TextInputScreen submit trampoline.
  void onTextSubmit(const char* text);
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;

 private:
  uint8_t computeRowHeight(oled& d) const override;
  void drawItem(oled& d, uint8_t index, uint8_t y, uint8_t rowHeight,
                bool selected) const override;

  // Resolve the visible cursor index to either a group header, a
  // concrete SettingIndex, or an action row.
  struct RowRef {
    int8_t groupIdx    = -1;   // -1 if cursor doesn't resolve
    int8_t settingIdx  = -1;   // -1 means "this row is the group header"
    bool   isAction    = false;
    uint8_t actionId   = 0;
    const char* actionLabel = nullptr;
  };
  RowRef resolveRow(uint8_t cursor) const;

  void formatSettingValue(uint8_t settingIdx,
                          char* label, uint8_t labelSize,
                          char* value, uint8_t valueSize) const;

  void enterEditMode();
  void exitEditMode();
  void cycleValue(int8_t dir, GUIManager& gui);

  // Auto-collapse the currently-open group when the cursor leaves it.
  void maybeAutoCollapse();

  Config* config_;
  bool    editing_        = false;
  bool    editingLed_     = false;
  int8_t  activeGroup_    = -1;     // -1 = no group expanded
  JoyRamp joyRamp_;                 // ramped Y-scroll cadence
  JoyRamp joyAdjustRamp_;           // ramped X value-change cadence

  // Pending text-input action — set when a row pushes TextInputScreen
  // and consumed in the static onDone callback. Buffer is the scratch
  // area handed to the keyboard.
  uint8_t pendingAction_ = 0;
  // Sized to fit the longest WiFi password (63 chars + NUL) plus
  // headroom; SSIDs cap at 32, "HH:MM" at 5.
  char    inputBuf_[96]  = {};
};
