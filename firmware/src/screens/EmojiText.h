#pragma once
#include <Arduino.h>

class oled;

// ─── UTF-8 / emoji rendering utilities ─────────────────────────────────────
//
// Shared by the text-input echo, the thread-detail bubble render, and
// the send-boop confirmation toast. Each needs to: scan a byte string,
// decode each codepoint, render ASCII with the caller's current font,
// and render the badge emoji table via FONT_EMOJI. The generated U8g2 font
// remaps supported emoji to printable chars, so we pass the remapped char
// through `drawStr` instead of asking `drawUTF8` to decode sparse Unicode.
//
// Critical implementation note for mixedLineWidth/drawMixedLine: width
// (and rendering) of contiguous ASCII runs MUST be computed with a
// single `getStrWidth` / `drawStr` call, NOT per-char summed. u8g2's
// fonts include inter-glyph advance in `drawStr("ABC")` that DOES NOT
// get summed correctly by three separate `drawStr("A") drawStr("B")
// drawStr("C")` calls (chars end up visually smushed together).
// Buffering runs and invoking drawStr once per run preserves spacing.

namespace EmojiText {

enum class EmojiRenderKind : uint8_t {
    None,
    Font,
};

struct EmojiRenderInfo {
    EmojiRenderKind kind = EmojiRenderKind::None;
    uint8_t bytes = 0;
    char fontChar = 0;
};

// Width of one UTF-8 codepoint leader byte (1..4 bytes total).
// Returns 1 for invalid leaders so callers advance past garbage.
uint8_t utf8CpBytes(uint8_t leadByte);

// Decode a native font-supported emoji codepoint at `p`.
EmojiRenderInfo emojiRenderInfoFromUtf8(const char* p);

// Decode a UTF-8 sequence at `p` to a FONT_EMOJI render char if it falls
// in the badge emoji table. Returns 0 otherwise.
char emojiRenderCharFromUtf8(const char* p);

// True if any native font-supported emoji appears in the string.
bool stringHasEmoji(const char* s);

// Pixel width of `s` when rendered with the caller's current ASCII font
// for ordinary codepoints and FONT_EMOJI for Emoticons-block emoji. The
// caller must have the ASCII font active on entry; this helper leaves
// that font active on exit.
int mixedLineWidth(oled& d, const char* s);

// Draw `s` with ASCII runs in `asciiFont` at `asciiBaseline` and emoji
// glyphs in FONT_EMOJI at `emojiBaseline`. Same run-buffering approach
// as mixedLineWidth.
void drawMixedLine(oled& d, int x, int asciiBaseline, int emojiBaseline,
                   const char* s, const uint8_t* asciiFont);

}  // namespace EmojiText
