#include "TextInputLayouts.h"

#include "BadgeEmoji.h"

namespace TextInputLayouts {

static_assert(kEmojiTotal == BadgeEmoji::kCount,
              "keyboard emoji count must match badge emoji font");

// QWERTY content grid — 3 letter/digit/symbol rows per layer (row 3
// is the shared action row, resolved at render/commit time through
// `actionAt()`, not stored here). Indexed [layer][row][col] with
// layer in {0=Lower, 1=Upper, 2=Digits, 3=Symbol}; Help bypasses
// this table. Keeping capitals in the Upper table (rather than
// case-transforming Lower) lets us use different secondary chars
// in each case (e.g. `' → "`, `, → !`, `. → ?`, `? → :`).
const char kKeyGridQwerty[4][3][10] = {
    // Lower
    {{'q','w','e','r','t','y','u','i','o','p'},
     {'a','s','d','f','g','h','j','k','l','\''},
     {'z','x','c','v','b','n','m',',','.','?'}},
    // Upper
    {{'Q','W','E','R','T','Y','U','I','O','P'},
     {'A','S','D','F','G','H','J','K','L','"'},
     {'Z','X','C','V','B','N','M','!','?',':'}},
    // Digits
    {{'1','2','3','4','5','6','7','8','9','0'},
     {'-','_','=','+','[',']','{','}','(',')'},
     {'/','\\','|',':',';',',','.','<','>','?'}},
    // Symbol
    {{'!','@','#','$','%','^','&','*','~','`'},
     {'+','-','=','*','/','\\','|','_','(',')'},
     {'<','>','[',']','{','}',';',':','"','\''}},
};

namespace {

uint8_t appendUtf8(uint32_t cp, char* out, uint8_t pos) {
    if (cp <= 0x7F) {
        out[pos++] = static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out[pos++] = static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
        out[pos++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out[pos++] = static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
        out[pos++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[pos++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out[pos++] = static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
        out[pos++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[pos++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[pos++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    return pos;
}

}  // namespace

void emojiUtf8Bytes(uint8_t idx, char out[5]) {
    const uint32_t cp = BadgeEmoji::codepoint(idx);
    const uint8_t pos = appendUtf8(cp, out, 0);
    out[pos] = '\0';
}

char emojiRenderChar(uint8_t idx) {
    return BadgeEmoji::fontCharForIndex(idx);
}

}  // namespace TextInputLayouts
