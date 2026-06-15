#include "TextInputScreen.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../ui/GUI.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/OLEDLayout.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "EmojiText.h"
#include "TextInputLayouts.h"

using TextInputLayouts::kKeyGridQwerty;
using TextInputLayouts::kEmojiCols;
using TextInputLayouts::kEmojiRows;
using TextInputLayouts::kEmojiPerPage;
using TextInputLayouts::kEmojiPages;
using TextInputLayouts::kEmojiTotal;
using TextInputLayouts::emojiRenderChar;
using TextInputLayouts::emojiUtf8Bytes;

// ─── Local helpers (file-scope) ─────────────────────────────────────────────

static void drawFrame(oled& d, int x, int y, int w, int h) {
    d.drawHLine(x, y, w);
    d.drawHLine(x, y + h - 1, w);
    d.drawVLine(x, y, h);
    d.drawVLine(x + w - 1, y, h);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TextInputScreen — on-screen keyboard
// ═══════════════════════════════════════════════════════════════════════════════
//
// Keyboard layout (kKeyGridQwerty), emoji constants, and the
// emojiUtf8Bytes / emojiRenderChar helpers live in TextInputLayouts.{h,cpp}.

namespace {

uint16_t nextDisplayGlyphOffset(const char* buf, uint16_t len, uint16_t pos) {
    if (!buf || pos >= len) return len;
    const EmojiText::EmojiRenderInfo info =
        EmojiText::emojiRenderInfoFromUtf8(buf + pos);
    if (info.bytes > 0) {
        uint16_t next = static_cast<uint16_t>(pos + info.bytes);
        return next <= len ? next : len;
    }
    const uint8_t b0 = static_cast<uint8_t>(buf[pos]);
    uint16_t next = static_cast<uint16_t>(pos + EmojiText::utf8CpBytes(b0));
    return next <= len ? next : static_cast<uint16_t>(pos + 1);
}

uint16_t previousDisplayGlyphOffset(const char* buf, uint16_t len,
                                    uint16_t pos) {
    if (!buf || pos == 0) return 0;
    uint16_t cur = 0;
    uint16_t prev = 0;
    while (cur < pos && cur < len) {
        prev = cur;
        const uint16_t next = nextDisplayGlyphOffset(buf, len, cur);
        if (next >= pos || next <= cur) return prev;
        cur = next;
    }
    return prev;
}

}  // namespace

void TextInputScreen::configure(const char* title, char* buffer,
                                uint16_t capacity, Submit onDone, void* user) {
    // Copy the title — never hold the caller's pointer. WifiScreen
    // builds the password title in a stack buffer and the pointer-only
    // version left `title_` dangling once the caller returned, which
    // surfaced as garbage characters in the keyboard's header.
    if (title) {
        std::strncpy(title_, title, sizeof(title_) - 1);
        title_[sizeof(title_) - 1] = '\0';
    } else {
        title_[0] = '\0';
    }
    buf_ = buffer;
    cap_ = capacity;
    len_ = buffer ? static_cast<uint16_t>(strlen(buffer)) : 0;
    cursorPos_ = len_;  // append mode by default
    onDone_ = onDone;
    user_ = user;
}

void TextInputScreen::onEnter(GUIManager& /*gui*/) {
    layer_ = Layer::Lower;
    // `mode_` NOT reset — sticky Grid/Mouse toggle across entries.
    shift_ = ShiftState::None;
    shiftTapMs_ = 0;
    upWasHeld_ = false;
    upHoldFired_ = false;
    gridInTextCursor_ = false;
    // Start the selector on the space cell — most-common first action
    // when the keyboard opens is typing text, and space between words
    // is the most-clicked key once someone starts. Space is the middle
    // pair on the action row (row 3, cols 4-5 — snap even).
    gridRow_ = 3;
    gridCol_ = 4;
    if (buf_) {
        len_ = static_cast<uint16_t>(strlen(buf_));
        cursorPos_ = len_;  // every entry starts in append mode
    }
}

void TextInputScreen::commitChar(char c) {
    // Route through the byte path so inline-insert semantics are
    // shared between single-byte keys (content rows) and multi-byte
    // emoji (commitBytes).
    char s[2] = {c, '\0'};
    commitBytes(s);
}

void TextInputScreen::commitBytes(const char* bytes) {
    if (!buf_ || !bytes) return;
    const uint16_t n = static_cast<uint16_t>(strlen(bytes));
    if (n == 0) return;
    // Leave one byte for NUL terminator. Silently drop on overflow
    // rather than corrupting the buffer.
    if (static_cast<uint32_t>(len_) + n >= cap_) return;
    if (cursorPos_ > len_) cursorPos_ = len_;
    // Make room at `cursorPos_` by shifting the tail right.
    // `memmove` handles overlapping ranges; `memcpy` would UB here.
    memmove(buf_ + cursorPos_ + n, buf_ + cursorPos_,
            static_cast<size_t>(len_ - cursorPos_));
    memcpy(buf_ + cursorPos_, bytes, n);
    cursorPos_ += n;
    len_ += n;
    buf_[len_] = '\0';
}

void TextInputScreen::backspace() {
    if (!buf_ || cursorPos_ == 0) return;
    // Scan by display glyph so one Backspace removes one visible
    // native emoji rather than a raw byte fragment.
    const uint16_t start = previousDisplayGlyphOffset(buf_, len_, cursorPos_);
    const uint16_t glyphBytes = static_cast<uint16_t>(cursorPos_ - start);
    // Shift the tail left to close the hole.
    memmove(buf_ + start, buf_ + cursorPos_,
            static_cast<size_t>(len_ - cursorPos_));
    cursorPos_ = start;
    len_ = static_cast<uint16_t>(len_ - glyphBytes);
    buf_[len_] = '\0';
}

void TextInputScreen::cursorMoveLeft() {
    if (!buf_ || cursorPos_ == 0) return;
    cursorPos_ = previousDisplayGlyphOffset(buf_, len_, cursorPos_);
}

void TextInputScreen::cursorMoveRight() {
    if (!buf_ || cursorPos_ >= len_) return;
    cursorPos_ = nextDisplayGlyphOffset(buf_, len_, cursorPos_);
}

void TextInputScreen::cycleLayer() {
    // Forward walk through text layers, emoji pages, and Help.
    const uint8_t n = static_cast<uint8_t>(Layer::kCount);
    layer_ = static_cast<Layer>(
        (static_cast<uint8_t>(layer_) + 1) % n);
}

void TextInputScreen::cycleEmojiPage() {
    if (!isEmojiLayer()) return;
    const uint8_t first = static_cast<uint8_t>(Layer::Emoji1);
    const uint8_t page = emojiPage();
    const uint8_t nextPage = static_cast<uint8_t>((page + 1) % kEmojiPages);
    layer_ = static_cast<Layer>(first + nextPage);
}

char TextInputScreen::applyShift(char c) {
    // Only letters care about shift. Digits / symbols are case-agnostic.
    if (c >= 'a' && c <= 'z' && shift_ != ShiftState::None) {
        c = static_cast<char>(c - 'a' + 'A');
        if (shift_ == ShiftState::OneShot) {
            shift_ = ShiftState::None;  // consume one-shot
        }
    } else if (c >= 'A' && c <= 'Z' && shift_ == ShiftState::None) {
        // Layer table stores uppercase in the Upper layer; if we're
        // there without active shift (shouldn't normally happen, but
        // defensive), leave the char as-is.
    }
    return c;
}

TextInputScreen::KeyAction TextInputScreen::actionAt(uint8_t row,
                                                    uint8_t col) const {
    // Only row 3 is the action row. Rows 0..2 are always plain-character
    // cells from `kKeyGridQwerty`.
    if (row != 3) return KeyAction::None;
    // Bottom row = 5 double-width keys. Each action spans 2 adjacent
    // lattice cells (24 px wide) so the row visually chunks into 5
    // big keys instead of 10 cramped ones. The pair index is col/2:
    //   pair 0 (cols 0..1): Shift
    //   pair 1 (cols 2..3): Emoji
    //   pair 2 (cols 4..5): Space
    //   pair 3 (cols 6..7): Backspace
    //   pair 4 (cols 8..9): Submit
    // Layer-cycle lives on Y (per the input contract), so the
    // `123/abc` toggle from the old single-width layout is gone.
    switch (col / 2) {
        case 0: return KeyAction::Shift;
        case 1: return KeyAction::Emoji;
        case 2: return KeyAction::Space;
        case 3: return KeyAction::Backspace;
        default: return KeyAction::Submit;  // pair 4 (cols 8, 9)
    }
}

void TextInputScreen::doAction(KeyAction action, GUIManager& gui) {
    uint32_t now = millis();
    switch (action) {
        case KeyAction::None:
            break;
        case KeyAction::Shift: {
            if (isEmojiLayer()) {
                shift_ = ShiftState::None;
                cycleEmojiPage();
                Haptics::shortPulse();
                break;
            }
            // Tap cycles None → OneShot → Locked → None. Two taps
            // within 400 ms skip straight to Locked, which is the
            // "double-tap = caps-lock" convention.
            const bool fastTap = (now - shiftTapMs_) < 400;
            shiftTapMs_ = now;
            if (shift_ == ShiftState::None) {
                shift_ = fastTap ? ShiftState::Locked : ShiftState::OneShot;
            } else if (shift_ == ShiftState::OneShot) {
                shift_ = ShiftState::Locked;
            } else {
                shift_ = ShiftState::None;
            }
            // Also flip the underlying content layer so Upper/Lower
            // glyphs render correctly when shift is non-None.
            if (shift_ == ShiftState::None && layer_ == Layer::Upper) {
                layer_ = Layer::Lower;
            } else if (shift_ != ShiftState::None && layer_ == Layer::Lower) {
                layer_ = Layer::Upper;
            }
            Haptics::shortPulse();
            break;
        }
        case KeyAction::LayerCycle: {
            // 123/abc button: flip between letter-track and digit-track.
            // Letters (Lower/Upper) → Digits; Digits/Symbol → Lower.
            if (layer_ == Layer::Lower || layer_ == Layer::Upper) {
                layer_ = Layer::Digits;
                shift_ = ShiftState::None;
            } else {
                layer_ = (shift_ != ShiftState::None) ? Layer::Upper : Layer::Lower;
            }
            Haptics::shortPulse();
            break;
        }
        case KeyAction::Emoji:
            // Toggle emoji mode. From any text layer → jump to the
            // first emoji page (Emoji1). From any of the emoji
            // pages → back to Lower. Y short-press pages through
            // the emoji layers like any other cycle transition.
            if (isEmojiLayer()) {
                layer_ = Layer::Lower;
                shift_ = ShiftState::None;
            } else {
                layer_ = Layer::Emoji1;
                // Clamp selector into the 8×2 emoji region so the
                // highlight doesn't land on an out-of-bounds cell
                // on first paint after the switch.
                if (gridRow_ >= kEmojiRows && gridRow_ != 3) {
                    gridRow_ = 0;
                }
                if (gridCol_ >= kEmojiCols && gridRow_ != 3) {
                    gridCol_ = static_cast<uint8_t>(kEmojiCols - 1);
                }
            }
            Haptics::shortPulse();
            break;
        case KeyAction::Space:
            commitChar(' ');
            Haptics::shortPulse();
            break;
        case KeyAction::Backspace:
            backspace();
            Haptics::shortPulse();
            break;
        case KeyAction::Submit:
            if (onDone_) onDone_(buf_, user_);
            gui.popScreen();
            Haptics::shortPulse();
            break;
    }
}

void TextInputScreen::render(oled& d, GUIManager& /*gui*/) {
    renderQwerty(d);
}

void TextInputScreen::drawTypedBufferEcho(oled& d) {
    if (!buf_) return;
    // The echo renders two glyph classes inline:
    //   * ASCII — FONT_SMALL at baseline y=18 (current echo baseline,
    //     matches the 10-px vertical budget between the y=8 divider
    //     and the y=24 keyboard top).
    //   * Emoji — FONT_EMOJI at baseline y=22. Emoji glyphs extend
    //     up to y=6 (just below the title baseline) and down to y=22.
    //     Tighter than the ASCII band but the only way to display
    //     them inline without restructuring the whole screen.
    //
    // Single-pass walk: we advance a byte pointer through buf_
    // from `showStart`, decode each codepoint, draw it with the
    // right font, and track both the running pixel-x and the
    // cursor's x-position (caretX) in the same sweep.
    constexpr uint8_t kMaxVisible = 22;           // byte cap for visible window
    constexpr int     kAsciiBaseline = 18;
    constexpr int     kEmojiBaseline = 22;
    constexpr int     kAsciiGlyphW   = 6;         // FONT_SMALL cell width
    constexpr int     kEmojiGlyphW   = 16;        // unifont emoticons cell
    constexpr int     kMaxPixelX     = 128;

    // Slide the visible window so the cursor stays onscreen.
    uint16_t showStart;
    if (len_ <= kMaxVisible) {
        showStart = 0;
    } else if (cursorPos_ <= kMaxVisible / 2) {
        showStart = 0;
    } else if (cursorPos_ >= len_ - kMaxVisible / 2) {
        showStart = static_cast<uint16_t>(len_ - kMaxVisible);
    } else {
        showStart = static_cast<uint16_t>(cursorPos_ - kMaxVisible / 2);
    }
    // Snap showStart back to a display-glyph boundary so we never
    // start inside a UTF-8 codepoint.
    if (showStart > 0) {
        uint16_t scan = 0;
        uint16_t prev = 0;
        while (scan < showStart && scan < len_) {
            prev = scan;
            const uint16_t next = nextDisplayGlyphOffset(buf_, len_, scan);
            if (next > showStart || next <= scan) break;
            scan = next;
        }
        if (scan != showStart) showStart = prev;
    }

    int x = 0;
    int caretX = 0;
    bool caretResolved = false;
    uint16_t p = showStart;

    d.setFontPreset(FONT_SMALL);
    while (p < len_) {
        // Remember caret position the moment we cross it, before
        // advancing x for this glyph. Covers both cursor-before-
        // glyph and cursor-after-last-glyph cases.
        if (!caretResolved && p == cursorPos_) {
            caretX = x;
            caretResolved = true;
        }
        if (x >= kMaxPixelX) break;

        const uint8_t b0 = static_cast<uint8_t>(buf_[p]);
        if (b0 < 0x80) {
            char s[2] = {static_cast<char>(b0), '\0'};
            d.setFontPreset(FONT_SMALL);
            d.drawStr(x, kAsciiBaseline, s);
            x += kAsciiGlyphW;
            p += 1;
        } else {
            const EmojiText::EmojiRenderInfo emoji =
                EmojiText::emojiRenderInfoFromUtf8(buf_ + p);
            if (emoji.kind == EmojiText::EmojiRenderKind::Font) {
                char s[2] = {emoji.fontChar, '\0'};
                d.setFontPreset(FONT_EMOJI);
                d.drawStr(x, kEmojiBaseline, s);
                x += kEmojiGlyphW;
                p = nextDisplayGlyphOffset(buf_, len_, p);
            } else {
                // UTF-8 outside the supported emoji set — render
                // a placeholder so the user sees something opaque.
                d.setFontPreset(FONT_SMALL);
                d.drawStr(x, kAsciiBaseline, "?");
                x += kAsciiGlyphW;
                if ((b0 & 0xE0) == 0xC0 && p + 1 < len_) p += 2;
                else if ((b0 & 0xF0) == 0xE0 && p + 2 < len_) p += 3;
                else if ((b0 & 0xF8) == 0xF0 && p + 3 < len_) p += 4;
                else p += 1;  // malformed — salvage one byte
            }
        }
    }
    if (!caretResolved) caretX = x;  // cursor at end of buffer

    // Blink caret at ~3 Hz. Use the ASCII line band so the caret
    // stays where the user expects even when emoji runs bump the
    // vertical rendering.
    const bool caretVisible = ((millis() / 333) % 2) == 0;
    if (caretVisible) {
        int cx = caretX;
        if (cx > 126) cx = 126;
        d.drawVLine(cx, 9, 11);
    }
    d.setFontPreset(FONT_SMALL);  // leave caller with a known font
}

void TextInputScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                  int16_t /*cy*/, GUIManager& gui) {
    handleInputQwerty(inputs, gui);
}

// ─── Keyboard layout ────────────────────────────────────────────────────────
//
// Phone-style 10×4 grid with 3 letter/digit rows and 1 persistent action
// row (shift, 123/abc toggle, emoji stub, space ×4, backspace, submit ×2).
// Button minimal: B=exit, A=commit/action, Y short=cycle-layer +
// long=mouse-toggle. All verbs are on-screen so
// users don't need to memorize arcane button bindings.

void TextInputScreen::renderQwerty(oled& d) {
    d.setDrawColor(1);
    d.setFontPreset(FONT_TINY);

    // Layer tag with inline shift-state indicator.
    char layerTagBuf[16];
    const char* layerTag = "abc";
    switch (layer_) {
        case Layer::Lower:  layerTag = "abc"; break;
        case Layer::Upper:  layerTag = "ABC"; break;
        case Layer::Digits: layerTag = "123"; break;
        case Layer::Symbol: layerTag = "!@#"; break;
        case Layer::Emoji1:
        case Layer::Emoji2:
        case Layer::Emoji3:
        case Layer::Emoji4:
        case Layer::Emoji5:
        case Layer::Emoji6:
            snprintf(layerTagBuf, sizeof(layerTagBuf), "emj %u/%u",
                     static_cast<unsigned>(emojiPage() + 1),
                     static_cast<unsigned>(kEmojiPages));
            layerTag = layerTagBuf;
            break;
        case Layer::Help:   layerTag = "?";   break;
        default: break;
    }
    // Prepend shift indicator to the layer tag so drawHeader handles
    // the full right-side string in one pass.
    char rightBuf[20];
    if (shift_ == ShiftState::OneShot) {
        snprintf(rightBuf, sizeof(rightBuf), "'%s", layerTag);
    } else if (shift_ == ShiftState::Locked) {
        snprintf(rightBuf, sizeof(rightBuf), "\"%s", layerTag);
    } else {
        snprintf(rightBuf, sizeof(rightBuf), "%s", layerTag);
    }
    (void)rightBuf;
    OLEDLayout::drawNameStatusHeader(d, title_);

    // Typed-buffer echo — shared helper draws the FONT_SMALL text
    // and a blinking caret at `cursorPos_`.
    if (buf_) {
        drawTypedBufferEcho(d);
        d.setFontPreset(FONT_TINY);
    }

    const uint8_t kbY = 24;
    const uint8_t cellW = 12;
    const uint8_t cellH = 10;

    if (layer_ == Layer::Help) {
        // Help content — Qwerty button semantics + mouse-above-kbd
        // text-cursor mode + emoji layer reachable via `emoji` cell.
        static const char* kHelpLines[] = {
            "X:delete Y:cycle",
            "Cancel:exit Confirm:select",
            "Hold Y:mouse",
            "mouse above edits",
            "emoji shft:next",
        };
        int y = 26;
        for (const char* line : kHelpLines) {
            ButtonGlyphs::drawInlineHintCompact(d, 2, y, line);
            y += 9;
        }
        return;
    }

    const bool isEmoji = isEmojiLayer();
    const uint8_t actionRowY = kbY + 3 * cellH;

    // Content area has NO outer frame or inter-cell lattice. The
    // only horizontal line between the content grid and the action
    // row is the HLine seam drawn below, which doubles as the top
    // edge of the action row's frame. Keeps the visual minimal
    // without losing the visible action-row container.
    const uint8_t seamY = kbY + 3 * cellH;
    d.drawHLine(4, seamY, cellW * kGridCols);

    if (isEmoji) {
        // Emoji layer: 7×2 cells at 17×15. 17 = 16-wide unifont
        // glyph + 1 px right-spacing, which is the "breathing room"
        // between adjacent emoji. 7×17 = 119 px grid width, 1 px
        // narrower than the 120 px action row below (minor visual
        // asymmetry at the right edge; swap to 7×17 + left margin
        // if tighter alignment matters). Pages are backed by the
        // curated emoji table in TextInputLayouts.cpp; `emojiPage()`
        // returns the current page in [0, kEmojiPages). Out-of-range
        // slots render as empty.
        constexpr uint8_t ecellW = 17;
        constexpr uint8_t ecellH = 15;
        d.setFontPreset(FONT_EMOJI);
        const uint8_t pageBase = static_cast<uint8_t>(emojiPage() * kEmojiPerPage);
        for (uint8_t r = 0; r < kEmojiRows; r++) {
            for (uint8_t c = 0; c < kEmojiCols; c++) {
                const int x = 4 + c * ecellW;
                const int y = kbY + r * ecellH;
                const uint8_t idx = static_cast<uint8_t>(pageBase + r * kEmojiCols + c);
                if (idx >= kEmojiTotal) continue;
                // Glyph drawn at cell's left edge — its 16-wide
                // footprint ends at x+15, leaving pixel x+16 empty
                // for the 1-px spacing to the next cell.
                char glyph[2] = {emojiRenderChar(idx), '\0'};
                d.drawStr(x, y + ecellH - 1, glyph);
            }
        }
        d.setFontPreset(FONT_TINY);
    } else {
        // Text content rows — FONT_SMALL glyphs with no inter-cell
        // separators. Letters appear to "float" in their 12×10
        // regions; selection highlight reveals the cell grid.
        uint8_t layerIdx = static_cast<uint8_t>(layer_);
        if (layerIdx > 3) layerIdx = 0;
        d.setFontPreset(FONT_SMALL);
        for (uint8_t r = 0; r < 3; r++) {
            for (uint8_t c = 0; c < kGridCols; c++) {
                int x = c * cellW + 4;
                int y = kbY + r * cellH;
                char ch = kKeyGridQwerty[layerIdx][r][c];
                char label[2] = {ch, '\0'};
                d.drawStr(x + 3, y + cellH - 1, label);
            }
        }
        d.setFontPreset(FONT_TINY);
    }

    // Shared action-row pair separators. Render the row-3 VLines
    // regardless of text-vs-emoji layer above, plus the top HLine
    // of the action row (the shared border between content and
    // actions). Placed AFTER the content grid so the action strip
    // visually attaches to either kind of grid above.
    // Action row frame + pair separators. The frame's top edge IS
    // the seam line drawn above by the content-area pass, so the
    // two overlap at y = actionRowY and read as a single 1-px line
    // between the letters and the bottom row.
    drawFrame(d, 4, actionRowY, cellW * kGridCols, cellH);
    for (uint8_t pair = 1; pair < 5; ++pair) {
        d.drawVLine(4 + pair * 2 * cellW, actionRowY, cellH);
    }

    // Action row: 5 double-width pairs. Iterate pair 0..4, draw the
    // label centered across the 24-px pair span. FONT_TINY baseline
    // also shifts down 1 px (`actionY + cellH - 2` vs `... - 3`).
    const int actionY = actionRowY;  // alias for readability
    const uint8_t kPairCount = 5;
    const uint8_t pairWidth = cellW * 2;  // 24 px
    for (uint8_t pair = 0; pair < kPairCount; ++pair) {
        const int pairX = 4 + pair * pairWidth;
        const char* label = "";
        switch (actionAt(3, pair * 2)) {
            case KeyAction::Shift:
                label = isEmojiLayer()                  ? "next"
                      : (shift_ == ShiftState::Locked)  ? "CAPS"
                      : (shift_ == ShiftState::OneShot) ? "Shft"
                                                        : "shft";
                break;
            case KeyAction::Space:
                label = "      ";  // wide underscore, reads as space bar
                break;
            case KeyAction::Emoji:
                // On any emoji page the same cell toggles back to
                // letters, so flip the label to "abc" to signal
                // "exit emoji mode" rather than "go to emoji".
                label = isEmojiLayer() ? "abc" : "emoji";
                break;
            case KeyAction::Backspace:
                label = "del";
                break;
            case KeyAction::Submit:
                label = "send";
                break;
            default:
                break;
        }
        if (label[0] != '\0') {
            int w = d.getStrWidth(label);
            int lx = pairX + (pairWidth - w) / 2;
            d.drawStr(lx, actionY + cellH - 2, label);
        }
    }

    // Selection XOR highlight. Three geometries to handle:
    //   1. Text content rows (0..2, non-emoji): 12×10 cells, single.
    //   2. Emoji content rows (0..1): 15×15 cells on an 8-col grid.
    //   3. Action row 3 (both layers): pair-snapped 24×10 cells.
    // Mouse-above-keyboard (text-cursor mode) suppresses the
    // highlight entirely — caller sets hiRow to 0xFF to skip.
    uint8_t hiCol = 0;
    uint8_t hiRow = 0;
    bool highlightSuppressed = false;
    if (gridInTextCursor_) {
        // Grid-mode text-cursor: selector lifted into composer;
        // keyboard has no active cell until user drops back via
        // joystick-down.
        highlightSuppressed = true;
    } else if (mode_ == Mode::Grid) {
        hiCol = gridCol_;
        hiRow = gridRow_;
    } else {
        if (mouseY_ < kbY) {
            // Mouse-mode text-cursor — don't highlight any key. The
            // caret in the typed-buffer echo is the visible cursor.
            highlightSuppressed = true;
        } else {
            int hc;
            int hr;
            if (isEmoji && mouseY_ < kbY + 30.0f) {
                // 7×2 emoji grid at 17×15.
                hc = static_cast<int>((mouseX_ - 4.0f) / 17.0f);
                hr = static_cast<int>((mouseY_ - kbY) / 15.0f);
                if (hc < 0) hc = 0;
                if (hc >= kEmojiCols) hc = kEmojiCols - 1;
                if (hr < 0) hr = 0;
                if (hr >= kEmojiRows) hr = kEmojiRows - 1;
            } else {
                // Text grid or action row: 12×10 cells, 10 cols, rows 0..3
                hc = static_cast<int>((mouseX_ - 4.0f) / static_cast<float>(cellW));
                hr = static_cast<int>((mouseY_ - kbY) / static_cast<float>(cellH));
                if (hc < 0) hc = 0;
                if (hc >= kGridCols) hc = kGridCols - 1;
                if (hr < 0) hr = 0;
                if (hr >= kGridRows) hr = kGridRows - 1;
            }
            hiCol = static_cast<uint8_t>(hc);
            hiRow = static_cast<uint8_t>(hr);
        }
    }
    if (!highlightSuppressed) {
        // Selection XOR box, one px larger on left/top/bottom than
        // the pre-existing inset. Original box was at
        // (sx+1, sy+1, w-1, h-1) — a 1-px inset on every side —
        // which left the glyph appearing slightly shifted (more
        // empty space on the right than the left). New box sits
        // at (sx, sy, w_right_preserved, h+1), extending the
        // highlight 1 px further in every direction EXCEPT right,
        // which already had the natural "extra pixel" the user
        // liked. Result: glyph reads centered inside a slightly
        // larger highlight.
        int sx;
        int sy;
        int boxW;
        int boxH;
        if (isEmoji && hiRow < kEmojiRows) {
            // Emoji: 17×15 cell → 17×16 XOR box. Covers the full
            // cell width (including the 1-px gap column on the
            // right of the glyph) and extends 1 px below the cell.
            // Minor overlap into row 1 / the seam line is accepted
            // for the symmetric "slightly bigger" look.
            sx   = hiCol * 17 + 4;
            sy   = kbY + hiRow * 15;
            boxW = 17;
            boxH = 15 + 1;
        } else if (hiRow == 3) {
            // Action-row pair: 24×10 → 24×11. +1 on top / left /
            // bottom; right edge where it was.
            const uint8_t pairCol = hiCol & ~uint8_t{1};
            sx   = pairCol * cellW + 4;
            sy   = kbY + 3 * cellH;
            boxW = pairWidth;
            boxH = cellH + 1;
        } else {
            // Text content row: 12×10 → 12×11. Glyph drawn at
            // (sx+3) sits visually centered inside this 12-wide
            // box that reaches sx+11 (same right edge as before).
            sx   = hiCol * cellW + 4;
            sy   = kbY + hiRow * cellH;
            boxW = cellW;
            boxH = cellH + 1;
        }
        d.setDrawColor(2);
        d.drawBox(sx, sy, boxW, boxH);
        d.setDrawColor(1);
    }

    if (mode_ == Mode::Mouse) {
        int mx = static_cast<int>(mouseX_);
        int my = static_cast<int>(mouseY_);
        d.setDrawColor(2);
        d.drawBox(mx - 1, my - 1, 3, 3);
        d.setDrawColor(1);
    }
}

void TextInputScreen::handleInputQwerty(const Inputs& inputs, GUIManager& gui) {
    const Inputs::ButtonEdges& e = inputs.edges();
    const uint32_t now = millis();
    const bool onHelpLayer = (layer_ == Layer::Help);

    // UP: short = cycle forward; long (>= 800 ms) = toggle mouse.
    // Same `heldMs()` repeat-immune detection as the Legacy handler.
    const uint32_t upHeldMs = inputs.heldMs(0);
    const bool upNowHeld = (upHeldMs > 0);
    if (upNowHeld && !upHoldFired_ && !onHelpLayer && upHeldMs >= 800) {
        mode_ = (mode_ == Mode::Grid) ? Mode::Mouse : Mode::Grid;
        Haptics::shortPulse();
        upHoldFired_ = true;
    }
    if (upWasHeld_ && !upNowHeld) {
        if (!upHoldFired_) {
            cycleLayer();
        }
        upHoldFired_ = false;
    }
    upWasHeld_ = upNowHeld;

    // Semantic cancel exits without firing `onDone_`, so the caller's
    // buffer retains whatever the user typed — that's the "draft
    // preservation" the plan calls for. Callers that don't want to
    // keep drafts can clear the buffer before the next push.
    if (e.cancelPressed) {
        gui.popScreen();
        return;
    }

    // LEFT (= X) is a one-button shortcut to backspace. The help
    // text already documents this — wires it up in either Grid or
    // Mouse mode without disturbing selector/cell state. Auto-repeat
    // from `applyKeyRepeat` lets a held LEFT chew through a buffer
    // without forcing the user to dance over to the `del` action key.
    if (e.leftPressed && !onHelpLayer) {
        backspace();
        Haptics::shortPulse();
        return;
    }

    const bool onEmoji = isEmojiLayer();
    // Keyboard top Y — shared with `renderQwerty`. Duplicated here
    // rather than hoisted to a class constant so the render keeps
    // its locality; at 24 px it's the tightest layout that leaves
    // room for the title + typed-buffer echo above.
    constexpr uint8_t kbY = 24;

    // Semantic confirm commits the cell under the selector (Grid or Mouse) —
    // OR, when the selector is in the composer area (mouse above
    // keyboard OR grid-text-cursor), place the caret. Text-cursor
    // mode hover doesn't track the caret; it waits for an A
    // click (Mouse) or is already step-by-step (Grid).
    const bool inTextCursor =
        (mode_ == Mode::Mouse && mouseY_ < kbY) || gridInTextCursor_;
    const bool mouseInTextCursor =
        (mode_ == Mode::Mouse) && (mouseY_ < kbY);
    if (e.confirmPressed && !onHelpLayer && mouseInTextCursor) {
        // Click-to-place the caret. Same window / width math as
        // the echo's `drawTypedBufferEcho` uses so the click
        // position maps to the same visible glyph the user sees
        // under the cursor dot.
        constexpr uint8_t kMaxVisible = 22;
        uint16_t showStart;
        if (len_ <= kMaxVisible) {
            showStart = 0;
        } else if (cursorPos_ <= kMaxVisible / 2) {
            showStart = 0;
        } else if (cursorPos_ >= len_ - kMaxVisible / 2) {
            showStart = static_cast<uint16_t>(len_ - kMaxVisible);
        } else {
            showStart = static_cast<uint16_t>(cursorPos_ - kMaxVisible / 2);
        }
        // FONT_SMALL glyph is 6 px wide. Add 0.5 for round-to-nearest
        // so a click exactly between two glyph cells biases to the
        // closer side rather than always rounding down.
        constexpr float kGlyphWidthPx = 6.0f;
        int col = static_cast<int>(mouseX_ / kGlyphWidthPx + 0.5f);
        int pos = static_cast<int>(showStart) + col;
        if (pos < 0) pos = 0;
        if (pos > static_cast<int>(len_)) pos = static_cast<int>(len_);
        cursorPos_ = static_cast<uint16_t>(pos);
        Haptics::shortPulse();
        return;
    }
    if (e.confirmPressed && !onHelpLayer && !inTextCursor) {
        uint8_t cellR = gridRow_;
        uint8_t cellC = gridCol_;

        // Derive hovered cell from mouse when in Mouse mode. Emoji
        // content rows use 15-wide cells and 2 rows; everything else
        // uses 12×10.
        if (mode_ == Mode::Mouse && mouseY_ >= kbY) {
            if (onEmoji && mouseY_ < kbY + 30.0f) {
                int hc = static_cast<int>((mouseX_ - 4.0f) / 17.0f);
                int hr = static_cast<int>((mouseY_ - kbY) / 15.0f);
                if (hc < 0) hc = 0;
                if (hc >= kEmojiCols) hc = kEmojiCols - 1;
                if (hr < 0) hr = 0;
                if (hr >= kEmojiRows) hr = kEmojiRows - 1;
                cellR = static_cast<uint8_t>(hr);
                cellC = static_cast<uint8_t>(hc);
            } else {
                int hc = static_cast<int>((mouseX_ - 4.0f) / 12.0f);
                int hr = static_cast<int>((mouseY_ - 24.0f) / 10.0f);
                if (hc < 0) hc = 0;
                if (hc >= kGridCols) hc = kGridCols - 1;
                if (hr < 0) hr = 0;
                if (hr >= kGridRows) hr = kGridRows - 1;
                cellR = static_cast<uint8_t>(hr);
                cellC = static_cast<uint8_t>(hc);
            }
        }

        const KeyAction act = actionAt(cellR, cellC);
        if (act == KeyAction::None) {
            if (onEmoji && cellR < kEmojiRows) {
                // Commit an emoji glyph (multi-byte UTF-8). The
                // flat emoji index combines page + on-screen slot;
                // page is derived from `layer_` via `emojiPage()`.
                const uint8_t pageBase =
                    static_cast<uint8_t>(emojiPage() * kEmojiPerPage);
                const uint8_t idx = static_cast<uint8_t>(
                    pageBase + cellR * kEmojiCols + cellC);
                if (idx < kEmojiTotal) {
                    char utf8[5];
                    emojiUtf8Bytes(idx, utf8);
                    commitBytes(utf8);
                    Haptics::shortPulse();
                }
            } else {
                // Text letter / digit / symbol commit from grid.
                uint8_t layerIdx = static_cast<uint8_t>(layer_);
                if (layerIdx <= 3 && cellR < 3) {
                    char ch = kKeyGridQwerty[layerIdx][cellR][cellC];
                    ch = applyShift(ch);
                    // OneShot consumed → drop back from Upper to Lower.
                    if (shift_ == ShiftState::None && layer_ == Layer::Upper) {
                        layer_ = Layer::Lower;
                    }
                    commitChar(ch);
                    Haptics::shortPulse();
                }
            }
        } else {
            doAction(act, gui);
        }
    }

    if (onHelpLayer) return;

    // Joystick nav (Grid vs Mouse).
    //   - Row 3 is 5 double-width pairs → step 2 cols, snap even.
    //   - Emoji layer rows 0..1 have 8 cols (instead of 10) → clamp.
    //   - Emoji layer skips row 2 (dead zone between emoji grid and
    //     the action row); up/down goes 0 ↔ 1 ↔ 3.
    //   - Mouse mode expands `kMouseMinY` into the typed-buffer echo
    //     region so moving the joystick up above the keyboard drops
    //     into text-cursor mode (`mouseY_ < kbY`), where joystick X
    //     drives `cursorPos_` for mid-string editing.
    if (mode_ == Mode::Grid) {
        int16_t joyX = static_cast<int16_t>(inputs.joyX()) - 2047;
        int16_t joyY = static_cast<int16_t>(inputs.joyY()) - 2047;
        if ((abs(joyX) > kJoyDeadband || abs(joyY) > kJoyDeadband)
            && (now - lastJoyMs_ >= 150)) {
            lastJoyMs_ = now;
            // Grid-mode text-cursor: pushing UP at row 0 lifts the
            // selector off the keyboard into the composer. In that
            // state, horizontal joystick steps the caret through
            // the buffer one UTF-8 glyph at a time; joystick-down
            // drops back to grid row 0. Keyboard highlight is
            // suppressed by the render while in this state.
            if (gridInTextCursor_) {
                if (abs(joyX) > abs(joyY)) {
                    if (joyX > 0) cursorMoveRight();
                    else           cursorMoveLeft();
                } else {
                    if (joyY > 0) {
                        // Down → return to grid row 0.
                        gridInTextCursor_ = false;
                        gridRow_ = 0;
                    }
                    // Joystick up while in text-cursor is a no-op
                    // (we're already "above" the keyboard).
                }
            } else if (abs(joyX) > abs(joyY)) {
                // Horizontal step depends on current row geometry.
                // Emoji pages are separate layers now, so paging is
                // handled via Y short-press (cycleLayer), not
                // joystick-edge wrap. The
                // horizontal joystick simply cycles within the row.
                uint8_t step;
                uint8_t colCount;
                if (gridRow_ == 3) {
                    step = 2;  // action row pair-step
                    colCount = kGridCols;
                } else if (onEmoji) {
                    step = 1;  // emoji content row is 8 cols
                    colCount = kEmojiCols;
                } else {
                    step = 1;  // text row 10 cols
                    colCount = kGridCols;
                }
                if (joyX > 0) {
                    const uint8_t next =
                        static_cast<uint8_t>(gridCol_ + step);
                    if (next >= colCount) {
                        // Don't wrap right off Send (action row pair
                        // 4 = cols 8/9). Send is the most-used action
                        // so the selector is sticky there rather than
                        // rolling around to Shift on the left edge.
                        // Other rows still wrap normally so users can
                        // cycle through letters without backing up.
                        if (gridRow_ != 3) {
                            gridCol_ = 0;
                        }
                    } else {
                        gridCol_ = next;
                    }
                } else {
                    // Leftward wrap is preserved on every row,
                    // including from Shift → Send: wrapping *into*
                    // Send is desirable, only wrapping *out of* Send
                    // is suppressed.
                    gridCol_ = static_cast<uint8_t>(
                        (gridCol_ + colCount - step) % colCount);
                }
            } else {
                // Vertical step. Emoji pages skip row 2 (dead zone
                // between the 2-row emoji grid and the action row).
                // Text layers at row 0 pushed UP lift into the
                // composer text-cursor instead of wrapping to row 3.
                // The bottom row (row 3, action row) is sticky on the
                // way down — no wrap back to row 0 — so users can't
                // accidentally fall off Send / Backspace into a
                // letter row. Top → bottom wrap is still allowed on
                // emoji layers because the dead-row geometry already
                // makes "up at row 0 → row 3" the natural step.
                const bool down = (joyY > 0);
                if (onEmoji) {
                    if (down) {
                        // 0 → 1 → 3 → 3 (sticky at row 3).
                        gridRow_ = (gridRow_ == 0) ? 1
                                 : (gridRow_ == 1) ? 3 : 3;
                    } else {
                        gridRow_ = (gridRow_ == 0) ? 3
                                 : (gridRow_ == 1) ? 0 : 1;
                    }
                } else if (!down && gridRow_ == 0) {
                    // Text layer row 0, joystick up → text-cursor.
                    gridInTextCursor_ = true;
                } else if (down && gridRow_ == 3) {
                    // Action row + down: stay put. (no-op)
                } else {
                    gridRow_ = static_cast<uint8_t>(
                        (gridRow_ + (down ? 1 : kGridRows - 1))
                        % kGridRows);
                }
                // Row-change fix-ups: snap col to even on row 3;
                // clamp col to emoji range when landing on an
                // emoji content row.
                if (gridRow_ == 3) {
                    gridCol_ &= ~uint8_t{1};
                } else if (onEmoji && gridCol_ >= kEmojiCols) {
                    gridCol_ = static_cast<uint8_t>(kEmojiCols - 1);
                }
            }
        }
    } else {
        // Absolute mouse with 1.5 expo. The mapping ranges are
        // intentionally oversized (extend past the physical screen)
        // so ~80% stick deflection reaches the screen edge. This
        // guarantees corners (e.g. Submit at bottom-right) are
        // reachable even when the thumbstick gate prevents
        // simultaneous full deflection on both axes. The clamp
        // below snaps the cursor to the usable screen region.
        constexpr float kJoyCenter    = 2047.0f;
        constexpr float kJoyHalfRange = 2047.0f;
        constexpr float kClampMinX    = 4.0f;
        constexpr float kClampMaxX    = 123.0f;
        constexpr float kClampMinY    = 10.0f;
        constexpr float kClampMaxY    = 63.0f;
        // Overshoot: mapping target extends ~20% past the clamp
        // bounds so 80% deflection reaches the physical edge.
        constexpr float kMapMinX      = -10.0f;
        constexpr float kMapMaxX      = 137.0f;
        constexpr float kMapMinY      = 0.0f;
        constexpr float kMapMaxY      = 73.0f;
        constexpr float kMouseRestY   = 43.5f;
        constexpr float kCenterX      = (kMapMinX + kMapMaxX) * 0.5f;
        constexpr float kHalfRangeX   = (kMapMaxX - kMapMinX) * 0.5f;
        constexpr float kRangeYUp     = kMouseRestY - kMapMinY;
        constexpr float kRangeYDown   = kMapMaxY - kMouseRestY;
        constexpr float kMouseExpo    = 1.5f;
        float dx = (static_cast<float>(inputs.joyX()) - kJoyCenter) / kJoyHalfRange;
        float dy = (static_cast<float>(inputs.joyY()) - kJoyCenter) / kJoyHalfRange;
        if (dx < -1.0f) dx = -1.0f;
        if (dx >  1.0f) dx =  1.0f;
        if (dy < -1.0f) dy = -1.0f;
        if (dy >  1.0f) dy =  1.0f;
        const float fx = (dx < 0.0f ? -1.0f : 1.0f) * powf(fabsf(dx), kMouseExpo);
        const float fy = (dy < 0.0f ? -1.0f : 1.0f) * powf(fabsf(dy), kMouseExpo);
        mouseX_ = kCenterX + fx * kHalfRangeX;
        mouseY_ = kMouseRestY + fy * (fy < 0.0f ? kRangeYUp : kRangeYDown);
        if (mouseX_ < kClampMinX) mouseX_ = kClampMinX;
        if (mouseX_ > kClampMaxX) mouseX_ = kClampMaxX;
        if (mouseY_ < kClampMinY) mouseY_ = kClampMinY;
        if (mouseY_ > kClampMaxY) mouseY_ = kClampMaxY;

        if (mouseY_ < kbY) {
            // Text-cursor mode: hover only. `cursorPos_` is NOT
            // updated here — we wait for semantic confirm (handled
            // above) so hovering doesn't reposition the caret as
            // the user is still aiming. The 3x3 dot cursor shows
            // the hover position; the blinking caret line in the
            // typed-buffer echo stays put at the last committed
            // position until the user clicks.
            //
            // gridCol_/gridRow_ are also left untouched so moving
            // the mouse back down into the keyboard resumes the
            // key-highlight where it was last.
        } else {
            // Mouse inside the keyboard area — same cell-snap logic
            // as before, with emoji-aware geometry on Emoji layer.
            int hc;
            int hr;
            if (onEmoji && mouseY_ < kbY + 30.0f) {
                hc = static_cast<int>((mouseX_ - 4.0f) / 17.0f);
                hr = static_cast<int>((mouseY_ - kbY) / 15.0f);
                if (hc < 0) hc = 0;
                if (hc >= kEmojiCols) hc = kEmojiCols - 1;
                if (hr < 0) hr = 0;
                if (hr >= kEmojiRows) hr = kEmojiRows - 1;
            } else {
                hc = static_cast<int>((mouseX_ - 4.0f) / 12.0f);
                hr = static_cast<int>((mouseY_ - 24.0f) / 10.0f);
                if (hc < 0) hc = 0;
                if (hc >= kGridCols) hc = kGridCols - 1;
                if (hr < 0) hr = 0;
                if (hr >= kGridRows) hr = kGridRows - 1;
                if (hr == 3) hc &= ~1;  // snap pair for row 3
            }
            gridCol_ = static_cast<uint8_t>(hc);
            gridRow_ = static_cast<uint8_t>(hr);
        }
    }
}
