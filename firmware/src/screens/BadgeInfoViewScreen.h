#pragma once
#include "Screen.h"

#include "../identity/BadgeInfo.h"

// ─── Badge info view + edit screen ──────────────────────────────────────────
// Lists the local badge's identity fields. Confirm on a row pushes a
// TextInputScreen pre-loaded with the current value. On submit we write
// the new value back to /badgeInfo.json and reload the running globals
// so the next boop transmits the updated field without a reboot.

class BadgeInfoViewScreen : public ListMenuScreen {
 public:
  BadgeInfoViewScreen();
  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  // Custom row layout: tiny 4x6 font for labels in a left column, regular
  // text font for values in a fixed value column so every editable row
  // lines up regardless of label length.
  void drawItem(oled& d, uint8_t index, uint8_t y, uint8_t rowHeight,
                bool selected) const override;
  bool navigableItems() const override { return true; }
  const char* hintText() const override { return "Edit:set Cancel:back"; }
  void onItemSelect(uint8_t index, GUIManager& gui) override;

  // Called by the TextInputScreen submit trampoline. Writes editBuf_ into
  // the field pointed to by editIndex_, persists, and re-applies globals.
  void onEditSubmit();

 private:
  // Index of the row currently being edited; valid only between
  // onItemSelect (push) and onEditSubmit (pop).
  uint8_t editIndex_ = 0;
  // Scratch buffer the keyboard writes into. Sized for the longest
  // editable field (bio, 128 bytes).
  char editBuf_[128] = {};
};
