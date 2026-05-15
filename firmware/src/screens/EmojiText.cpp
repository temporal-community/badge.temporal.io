#include "EmojiText.h"

#include "../hardware/oled.h"
#include "BadgeEmoji.h"

namespace EmojiText {

namespace {

constexpr uint8_t kEmojiGlyphW = 16;

bool decodeUtf8(const char* p, uint32_t& cp, uint8_t& bytes) {
    if (!p || !*p) return false;
    const uint8_t b0 = static_cast<uint8_t>(p[0]);
    if (b0 < 0x80) {
        cp = b0;
        bytes = 1;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0) {
        const uint8_t b1 = static_cast<uint8_t>(p[1]);
        if ((b1 & 0xC0) != 0x80) return false;
        cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) |
             static_cast<uint32_t>(b1 & 0x3F);
        bytes = 2;
        return true;
    }
    if ((b0 & 0xF0) == 0xE0) {
        const uint8_t b1 = static_cast<uint8_t>(p[1]);
        const uint8_t b2 = static_cast<uint8_t>(p[2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
        cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) |
             (static_cast<uint32_t>(b1 & 0x3F) << 6) |
             static_cast<uint32_t>(b2 & 0x3F);
        bytes = 3;
        return true;
    }
    if ((b0 & 0xF8) == 0xF0) {
        const uint8_t b1 = static_cast<uint8_t>(p[1]);
        const uint8_t b2 = static_cast<uint8_t>(p[2]);
        const uint8_t b3 = static_cast<uint8_t>(p[3]);
        if ((b1 & 0xC0) != 0x80 ||
            (b2 & 0xC0) != 0x80 ||
            (b3 & 0xC0) != 0x80) {
            return false;
        }
        cp = (static_cast<uint32_t>(b0 & 0x07) << 18) |
             (static_cast<uint32_t>(b1 & 0x3F) << 12) |
             (static_cast<uint32_t>(b2 & 0x3F) << 6) |
             static_cast<uint32_t>(b3 & 0x3F);
        bytes = 4;
        return true;
    }
    return false;
}

}  // namespace

uint8_t utf8CpBytes(uint8_t leadByte) {
    if (leadByte < 0x80) return 1;
    if ((leadByte & 0xE0) == 0xC0) return 2;
    if ((leadByte & 0xF0) == 0xE0) return 3;
    if ((leadByte & 0xF8) == 0xF0) return 4;
    return 1;
}

EmojiRenderInfo emojiRenderInfoFromUtf8(const char* p) {
    EmojiRenderInfo info;
    uint32_t cp = 0;
    uint8_t bytes = 0;
    if (!decodeUtf8(p, cp, bytes)) return info;

    char fontChar = 0;
    if (BadgeEmoji::fontCharForCodepoint(cp, fontChar)) {
        info.kind = EmojiRenderKind::Font;
        info.bytes = bytes;
        info.fontChar = fontChar;

        const char* next = p + bytes;
        while (true) {
            uint32_t nextCp = 0;
            uint8_t nextBytes = 0;
            if (!decodeUtf8(next, nextCp, nextBytes)) break;
            const bool variation = nextCp == 0xFE0E || nextCp == 0xFE0F;
            const bool skinTone = nextCp >= 0x1F3FB && nextCp <= 0x1F3FF;
            if (!variation && !skinTone) break;
            info.bytes = static_cast<uint8_t>(info.bytes + nextBytes);
            next += nextBytes;
        }
        return info;
    }
    return info;
}

char emojiRenderCharFromUtf8(const char* p) {
    const EmojiRenderInfo info = emojiRenderInfoFromUtf8(p);
    return info.kind == EmojiRenderKind::Font ? info.fontChar : 0;
}

bool stringHasEmoji(const char* s) {
    if (!s) return false;
    while (*s) {
        const uint8_t b = static_cast<uint8_t>(*s);
        const EmojiRenderInfo info = emojiRenderInfoFromUtf8(s);
        if (info.kind != EmojiRenderKind::None) {
            return true;
        }
        s += info.bytes ? info.bytes : utf8CpBytes(b);
    }
    return false;
}

int mixedLineWidth(oled& d, const char* s) {
    if (!s) return 0;
    int w = 0;
    char runBuf[64];
    int runLen = 0;
    auto flushRun = [&]() {
        if (runLen == 0) return;
        runBuf[runLen] = '\0';
        w += d.getStrWidth(runBuf);
        runLen = 0;
    };
    const char* p = s;
    while (*p) {
        const uint8_t b0 = static_cast<uint8_t>(*p);
        const uint8_t n  = utf8CpBytes(b0);
        const EmojiRenderInfo info = emojiRenderInfoFromUtf8(p);
        if (info.kind != EmojiRenderKind::None) {
            flushRun();
            w += kEmojiGlyphW;
            p += info.bytes;
        } else if (b0 < 0x80) {
            if (runLen >= static_cast<int>(sizeof(runBuf)) - 1) flushRun();
            runBuf[runLen++] = static_cast<char>(b0);
            p += 1;
        } else {
            flushRun();
            w += d.getStrWidth("?");
            p += n;
        }
    }
    flushRun();
    return w;
}

void drawMixedLine(oled& d, int x, int asciiBaseline,
                   int emojiBaseline, const char* s,
                   const uint8_t* asciiFont) {
    if (!s) return;
    d.setFont(asciiFont);
    char runBuf[64];
    int runLen = 0;
    auto flushRun = [&]() {
        if (runLen == 0) return;
        runBuf[runLen] = '\0';
        d.setFont(asciiFont);  // in case an emoji ran immediately before
        d.drawStr(x, asciiBaseline, runBuf);
        x += d.getStrWidth(runBuf);
        runLen = 0;
    };
    const char* p = s;
    while (*p) {
        const uint8_t b0 = static_cast<uint8_t>(*p);
        const uint8_t n  = utf8CpBytes(b0);
        const EmojiRenderInfo info = emojiRenderInfoFromUtf8(p);
        if (info.kind != EmojiRenderKind::None) {
            flushRun();
            d.setFontPreset(FONT_EMOJI);
            char buf[2] = {info.fontChar, '\0'};
            d.drawStr(x, emojiBaseline, buf);
            x += kEmojiGlyphW;
            p += info.bytes;
        } else if (b0 < 0x80) {
            if (runLen >= static_cast<int>(sizeof(runBuf)) - 1) flushRun();
            runBuf[runLen++] = static_cast<char>(b0);
            p += 1;
        } else {
            flushRun();
            d.setFont(asciiFont);
            d.drawStr(x, asciiBaseline, "?");
            x += d.getStrWidth("?");
            p += n;
        }
    }
    flushRun();
    d.setFont(asciiFont);
}

}  // namespace EmojiText
