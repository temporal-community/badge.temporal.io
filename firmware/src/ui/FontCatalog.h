// FontCatalog.h — u8g2 font tables consumed by oled.cpp.
//
// Split out so the U8G2 wrapper TU isn't dominated by ~150 lines of
// table data. The grid (`kFontGrid[family][size]`) is the canonical
// source for `setFontFamilyAndSlot`; the flat catalog (`kReplayFonts`)
// drives `setFont(name)` and the MicroPython `badge.display.set_font`
// binding.

#pragma once

#include <stdint.h>

// kFontFamilyCount is also declared in BadgeConfig.h alongside the
// human-readable kFontFamilyNames[] used by SettingsScreen. Both files
// hard-code 10 — a compile-time mismatch would crash on the first
// kFontGrid access. The same 10 here is the load-bearing constant for
// the font grid; if you change one, change the other.
inline constexpr uint8_t kFontGridFamilyCount = 10;
inline constexpr uint8_t kSizeCount           = 10;

struct ReplayFont {
    const uint8_t* font;
    const char*    name;
};

extern const char* const   kSizeLabels[kSizeCount];
extern const uint8_t*      kFontGrid[kFontGridFamilyCount][kSizeCount];
extern const ReplayFont    kReplayFonts[];
extern const int           kReplayFontCount;

const char* fontDisplayName(uint8_t family, uint8_t size);
