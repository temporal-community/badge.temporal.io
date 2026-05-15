#pragma once
#include <stdint.h>

#include "Screen.h"

// ─── HexViewScreen ──────────────────────────────────────────────────────────
//
// Read-only hex+ASCII viewer for binary / unknown files. Streams the
// file in a 1 KB PSRAM sliding window keyed off the current scroll
// offset so files of any size open instantly.
//
// Layout per row at FONT_TINY (~21 cols × ~6 body rows):
//   "OOOOO HH HH HH HH HH HH HH HH AAAAAAAA"
//   ^      ^                       ^
//   offset hex bytes (8 per row)   ASCII gutter

class HexViewScreen : public Screen {
 public:
  HexViewScreen();
  ~HexViewScreen() override;

  void loadFile(const char* path);

  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenHexView; }
  bool showCursor() const override { return false; }

 private:
  static constexpr uint16_t kBytesPerRow = 8;
  static constexpr uint16_t kWindowBytes = 1024;
  static constexpr uint8_t kVisibleRows = 6;

  char path_[96] = {};
  uint32_t fileSize_ = 0;

  // Sliding-window byte buffer (PSRAM). Spans
  // [windowStart_, windowStart_ + windowLen_).
  uint8_t* window_ = nullptr;
  uint32_t windowStart_ = 0;
  uint32_t windowLen_ = 0;

  // First-byte offset visible on the top row.
  uint32_t scrollOffset_ = 0;

  uint32_t lastJoyMs_ = 0;

  bool ensureWindow();
  // Make sure [start, start + bytesPerRow * kVisibleRows) is inside
  // window_; reload from disk if not.
  void refillWindow(uint32_t start);
};
