#pragma once

#include <Arduino.h>

class oled;

// ════════════════════════════════════════════════════════════════════════
//  PROJECT RULE — never use bare button letters in user-facing copy.
// ════════════════════════════════════════════════════════════════════════
//
// The badge has Y / B / A / X face buttons but the labels Up/Right/Down/
// Left rotate with nametag flip, and "A confirms / B cancels" can be
// swapped via settings (kSwapConfirmCancel). Any literal "A:back" or
// "press X" in plain text is a confusing lie depending on the user's
// settings.
//
// ALWAYS route hint / status copy through `drawInlineHint(...)` — it
// substitutes glyphs for the tokens `X`, `A`, `B`, `Y`, `^`, `v`, `<`,
// `>`, `U/D`, `L/R`, `ALL`, `Confirm`, `Cancel`, `Back`, etc. — and the
// tokens auto-rotate with the active confirm/cancel mapping where
// applicable. NEVER call `oled::drawStr` with a button letter in the
// string.
//
// drawActionFooter / drawFooterActions / drawFooterHint do the right
// thing automatically; the trap is only when you reach for `drawStr`
// to format a status line by hand.
// ════════════════════════════════════════════════════════════════════════

namespace ButtonGlyphs {

enum class Button : uint8_t {
  Y = 0,
  B = 1,
  A = 2,
  X = 3,
  Up = Y,
  Right = B,
  Down = A,
  Left = X,
};

constexpr uint8_t kGlyphW = 10;
constexpr uint8_t kGlyphH = 10;
constexpr uint8_t kGap = 2;

void draw(oled& d, Button button, int x, int y);

// Composite cluster glyphs (all four / left+right / up+down) are
// surfaced via the inline-hint parser only — see ButtonGlyphs.cpp.
// The recognised tokens are `ALL`, `L/R` (alias `X/B`), and `U/D`
// (aliases `Y/A`, `^v`); each draws as a single 10×10 cluster picture
// with the relevant pads filled, instead of two side-by-side glyphs
// or an OR composition at draw time.
int drawHint(oled& d, int x, int baseline, Button button, const char* label);
int measureHint(oled& d, Button button, const char* label);

int drawInlineHint(oled& d, int x, int baseline, const char* text);
int drawInlineHintRight(oled& d, int rightX, int baseline, const char* text);
int measureInlineHint(oled& d, const char* text);
int drawInlineHintCompact(oled& d, int x, int baseline, const char* text);
int measureInlineHintCompact(oled& d, const char* text);

}  // namespace ButtonGlyphs
