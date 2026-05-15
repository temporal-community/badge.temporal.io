// TextInputLayouts.h — keyboard table + emoji range used by TextInputScreen.
//
// Split out so TextInputScreen.cpp focuses on render/input logic rather
// than carrying the static keyboard table. Emoji constants are inline
// constexpr so callers see the values directly.

#pragma once

#include <stdint.h>

namespace TextInputLayouts {

// QWERTY layout — 4 layers × 3 rows × 10 cols. Row 3 is the action row,
// resolved at render/commit time via TextInputScreen::actionAt(), not
// stored here.
extern const char kKeyGridQwerty[4][3][10];

// Emoji picker layout — 7 cols × 2 rows = 14 per page; 84 emoji total
// over 6 full pages. Each entry maps to the custom badge emoji font.
inline constexpr uint8_t  kEmojiCols           = 7;
inline constexpr uint8_t  kEmojiRows           = 2;
inline constexpr uint8_t  kEmojiPerPage        = kEmojiCols * kEmojiRows;   // 14
inline constexpr uint8_t  kEmojiTotal          = 84;
inline constexpr uint8_t  kEmojiPages          =
    (kEmojiTotal + kEmojiPerPage - 1) / kEmojiPerPage;                      // 6

// Sanity gate — single source of truth.
static_assert(kEmojiPerPage * kEmojiPages >= kEmojiTotal,
              "page count must cover the full emoji range");
static_assert(kEmojiPerPage * (kEmojiPages - 1) < kEmojiTotal,
              "page count must not over-allocate (off-by-one guard)");

// Write the UTF-8 encoding of the `idx`th emoji into `out`.
// `idx` must be in [0, kEmojiTotal).
void emojiUtf8Bytes(uint8_t idx, char out[5]);

// Return the remapped ASCII char used to render a font-backed emoji
// via drawStr with FONT_EMOJI.
char emojiRenderChar(uint8_t idx);

}  // namespace TextInputLayouts
