#pragma once

#include <stdint.h>

namespace BadgeEmoji {

inline constexpr uint8_t kCount = 84;
inline constexpr char kFirstFontChar = 0x20;

uint32_t codepoint(uint8_t index);
char fontCharForIndex(uint8_t index);
bool fontCharForCodepoint(uint32_t codepoint, char& out);

}  // namespace BadgeEmoji
