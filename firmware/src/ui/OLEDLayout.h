#pragma once

#include <Arduino.h>

class oled;

namespace OLEDLayout {

constexpr uint8_t kScreenW = 128;
constexpr uint8_t kScreenH = 64;
constexpr uint8_t kFooterTopY = 54;
constexpr uint8_t kFooterTextTopY = kFooterTopY + 1;
// Sits on the same row as the glyph top so the rule reads as a
// continuous baseline that the chip glyphs poke up through. The
// per-glyph pixel knockout in `ButtonGlyphs::drawHint` adds a 1-px
// break on either side of every glyph so the rule cleanly breaks
// rather than visually merging into the chip outline.
constexpr uint8_t kFooterRuleY = 54;
constexpr uint8_t kFooterBaseY = 63;  // baseline flush with screen bottom (kScreenH-1)
// Text-only footer baseline. One pixel above kFooterBaseY so footer
// labels read with breathing room above the screen edge — button-glyph
// callers (drawHint / drawActions) keep using kFooterBaseY so the
// glyphs themselves stay flush with the bottom.
constexpr uint8_t kFooterTextBaseY = kFooterBaseY - 1;
constexpr uint8_t kFooterUpperBaseY = 53;
constexpr uint8_t kHeaderRuleY = 8;
constexpr uint8_t kContentTopY = kHeaderRuleY + 2;

void fitText(oled& d, char* text, size_t cap, int maxW);
void drawHeader(oled& d, const char* title, const char* right = nullptr);
void drawStatusHeader(oled& d, const char* title);
void drawNameStatusHeader(oled& d, const char* name);
void drawNetworkIndicator(oled& d, int x, int y, bool busy = false);
void drawHeaderRule(oled& d, uint8_t y = kHeaderRuleY);

// ── Footers ────────────────────────────────────────────────────────
// Three semantic footer styles. Pick the one that matches the screen's
// purpose — never mix and match across screens, since the rule height
// shifts between game and the others.
//
//   nav   — Plain horizontal rule (no bookend stars). `text` is drawn
//           left-aligned at x=0, baseline kFooterBaseY (FONT_TINY).
//           Caller may then drop arrow chips on top of the rule (e.g.
//           the forward-arrow used by Map drill-down). Pass nullptr
//           for text-only screens that draw their own custom footer.
//
//   action — Plain rule with scrolling text on the left and the active
//            confirm-button glyph/action on the right. Use when a
//            description may need marquee room but the screen also has
//            one obvious primary action (home menu, settings).
//
//   star   — Rule + 7×7 star1 glyph at both screen edges. Text band
//            has 8 px padding each side (text never enters x=0..7 or
//            x=120..127). `text` centres in the band when it fits and
//            marquee-scrolls when it overflows. Use for purely
//            informational footers.
//
//   game   — Action-row rule, sits 4 px above the standard rule so the
//            10-px button-glyph row (drawFooterActions / drawFooterHint)
//            clears it. No stars. Caller follows up with
//            drawFooterActions / drawFooterHint to fill the chips.
//            Use for screens that surface the four face buttons
//            (list menus, schedule, file viewer, on-badge games).
// nav   — Plain rule + left-aligned text. Pass `actionLabel` to
//         additionally surface a button-glyph chip on the right
//         (uses the swap-aware confirm button + a 1-px vertical
//         divider, same chrome as drawActionFooter). When text is
//         long enough to collide with the chip, the helper clips
//         the text to the chip's left boundary.
void drawNavFooter(oled& d, const char* text = nullptr,
                   const char* actionLabel = nullptr);
void drawActionFooter(oled& d, const char* text,
                      const char* actionLabel = "select");
void drawStarFooter(oled& d, const char* text);

// Variant of drawStarFooter that omits the horizontal divider rule
// between the bookend stars. Used by the Map app and its sub-scenes
// where vector floor plans paint right up to the footer band — the
// rule would slice through the room outlines and read as a stray
// horizontal stroke. Stars + centred (or marquee) text identical to
// the standard star footer.
void drawStarFooterNoLine(oled& d, const char* text);
void drawGameFooter(oled& d, int x = 0, int w = kScreenW);

void clearFooter(oled& d);
void setFooterClip(oled& d);

// ── Modal chrome ───────────────────────────────────────────────────
// Shared frame + title-strip renderer. Paints the rounded box,
// inverted title strip with corner-pixel knockout, optional right-
// aligned subhead (1-px vertical divider + small text), the
// scrolling title text, and an optional bottom action divider.
// Title auto-scrolls when wider than the available band; pass the
// moment the modal opened (or the cursor last moved) as
// `scrollStartMs` so each open starts the scroll fresh.
//
// `actionStripH` reserves a strip at the bottom of the box for action
// chips (use drawFooterActions, which already paints at baseline
// kFooterBaseY=62 — i.e. inside the modal when the box reaches the
// screen bottom). Pass 0 when the modal owns the whole interior and
// the caller renders body content all the way to bodyBotY.
//
// Caller paints body content inside
// [interiorX, bodyTopY, interiorW, bodyBotY-bodyTopY].
struct ModalChrome {
  int interiorX;
  int interiorY;
  int interiorW;
  int titleStripH;
  int bodyTopY;
  int bodyBotY;
};
ModalChrome drawModalChrome(oled& d, int boxX, int boxY, int boxW, int boxH,
                            const char* title, const char* subhead,
                            uint32_t scrollStartMs,
                            int actionStripH = 0,
                            bool frame = true);

// ── Text-body modal ─────────────────────────────────────────────────
// Convenience wrapper around drawModalChrome that renders an array of
// plain-text body lines. The box expands vertically to fit the content
// (centered between the status header and the footer rule), capped at
// kTextModalViewport visible lines. When lineCount exceeds the viewport,
// pass a scrollOffset to page through the content; the caller owns the
// scroll state and should update it on up/down input.
//
// danger=true draws a double outer frame (emphasis) and a small warning
// triangle at the top-left of the body, with body text indented
// accordingly.
//
// titleScrollMs should be the timestamp when the modal was opened (or
// the cursor last moved) so the title scroll starts fresh.
constexpr int kTextModalViewport = 4;
void drawTextModal(oled& d, const char* title,
                   const char* const* lines, int lineCount,
                   int scrollOffset = 0, bool danger = false,
                   uint32_t titleScrollMs = 0);

void drawSelectedRow(oled& d, int y, int h, int x = 0, int w = kScreenW);
void drawBusySpinner(oled& d, int cx, int cy, uint8_t phase);
void drawStatusBox(oled& d, int x, int y, int w, int h, const char* title,
                   const char* detail = nullptr, bool busy = false);

int drawFooterHint(oled& d, const char* text, int x = 0);
int drawFooterHintRight(oled& d, const char* text, int rightX = kScreenW);
int drawUpperFooterHint(oled& d, const char* text, int x = 0);
int drawFooterActions(oled& d, const char* xLabel, const char* yLabel,
                      const char* bLabel, const char* aLabel, int x = 0,
                      bool leftAlign = false);
int drawUpperFooterActions(oled& d, const char* xLabel, const char* yLabel,
                           const char* bLabel, const char* aLabel, int x = 0,
                           bool leftAlign = false);

// ── Grid cell renderer ─────────────────────────────────────────────
// Draws a single 2x2-grid menu cell using the same geometry the native
// home-menu GridMenuScreen uses. Exposed so MicroPython apps can build
// menus that look pixel-identical to the native chrome.
//
// `col` is 0 or 1, `row` is 0 or 1. `label` is right-aligned within the
// cell after the icon. `iconData` may be NULL — the renderer falls back
// to a blank icon slot (so the label gets ~10 extra px). When `selected`
// is true the cell is filled and the icon + label are drawn in the
// inverse colour.
//
// kGridX = 0, kGridY = 11, kCellW = 62, kCellH = 18, kGapX = kGapY = 4,
// kRowStride = 22. Rounded corners with kCellRadius = 3, matching the
// native renderer.
void drawGridCell(oled& d, uint8_t col, uint8_t row,
                   const char* label, bool selected,
                   const uint8_t* iconData, uint8_t iconW, uint8_t iconH);

// Bottom-edge footer with a scrolling description on the left and a
// confirm-glyph chip on the right ("select"). Same chrome the native
// GridMenuScreen draws so the IR Playground sub-apps line up cleanly.
// Pass nullptr or "" to leave the description blank.
void drawGridFooter(oled& d, const char* description);

}  // namespace OLEDLayout
