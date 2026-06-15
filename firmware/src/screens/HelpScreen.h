#pragma once
#include "Screen.h"

// ─── Scrollable Help / Docs screen ─────────────────────────────────────────
//
// One vertically-scrolling page of badge tips and links. Sections are a
// mix of headings, plain-text lines, button-glyph hint rows (parsed by
// ButtonGlyphs::drawInlineHint — tokens like "Y X A B", "L/R", "v"
// auto-render as glyphs), and inline QR codes for the three docs URLs:
//
//   docs.jumperless.org/badge-developer-guide/  (canonical)
//   ide.jumperless.org                          (online MicroPython IDE)
//   badge.temporal.io                           (mirror, may lag)
//
// Joystick up/down scrolls the page; cancel pops back to the menu.

class HelpScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenHelp; }
  bool showCursor() const override { return false; }
  ScreenAccess access() const override { return ScreenAccess::kAny; }

 private:
  // Three QR codes are pre-generated on first entry and cached for the
  // life of the screen instance. ricmoo/QRCode silently truncates on
  // overflow, so we pick the version explicitly from a length-indexed
  // capacity table (see HelpScreen.cpp). 64×64 max scaled bitmap →
  // worst-case 8 bytes/row × 64 rows = 512 bytes.
  static constexpr uint8_t kQrCount = 3;
  static constexpr uint16_t kQrBitmapBytes = 512;
  static constexpr uint16_t kQrWorkBytes = 160;

  struct Qr {
    uint8_t bits[kQrBitmapBytes] = {};
    uint8_t pixels = 0;  // square: width == height
  };

  void generateAll();
  bool generateOne(const char* url, Qr& out);

  Qr qrs_[kQrCount];
  uint8_t qrWork_[kQrWorkBytes] = {};
  bool qrsReady_ = false;

  // Vertical scroll, in pixels into the content area (below the
  // sticky header). Clamped to [0, contentHeight - viewportH] each tick.
  int16_t scrollPx_ = 0;
  uint32_t lastScrollMs_ = 0;
};
