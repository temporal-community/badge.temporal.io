#include "AboutCreditsScreen.h"

#include <Arduino.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>

#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"

namespace {

// ── Card geometry ─────────────────────────────────────────────────────────
// Each credit occupies a fixed-height block. The 64×64 headshot pins
// to the left edge; name (large font) and bio (small font with extra
// breathing room) stack on the right. The gap between cards has been
// bumped a touch from the original 6 px so adjacent cards don't read
// as one continuous strip when scrolling fast.
constexpr int kCardImageDim = AboutCredits::kIconSize;    // 64
constexpr int kCardGap      = 10;
constexpr int kCardH        = kCardImageDim + kCardGap;
constexpr int kImageX       = 0;
// Text column gets a small breathing room from the rounded image.
// 2 px is enough margin given the rounded corners, and reclaiming the
// extra pixel buys the per-line font picker a shot at landing on the
// next size up for tight 3-line names like "Kevin / Santo / Cappuccio".
constexpr int kImageGap     = 2;
constexpr int kTextX        = kCardImageDim + kImageGap;          // 66
constexpr int kTextW        = OLEDLayout::kScreenW - kTextX;       // 62

// Text baselines inside one card. The right column is laid out
// top-down with a flexible cursor — name first (large font, may wrap),
// then position/company/website at small font, then bio fills whatever
// vertical room is left. Constants below document the per-line
// heights that the cursor advances by.
//
// Name font ladder — bold Helvetica from biggest to smallest.
// pickNameStyle() walks the ladder top-down (skipping the helvB18 slot
// for any name with more than one line, so multi-line names never get
// the huge headline treatment that overpowers the card) and chooses
// the largest entry where every line fits the right column AND the
// stacked block fits the per-card vertical name budget.
constexpr int kColTopPad      = 6;      // first-baseline drop from card top — keeps the name from sitting flush with the headshot's top edge
constexpr int kNameToMetaGap  = 1;      // pixels between the last name line and position/company/website
constexpr int kMetaLineH      = 7;      // 4×6 font + 1 px leading
constexpr int kBioLineH       = 7;      // matches the meta lines for visual rhythm
constexpr int kBioMaxLines    = 4;      // hard cap so a runaway bio can't bleed into the next card
// Vertical budget the name block is allowed to consume inside one
// card. 64-px image - kColTopPad (6) - ~16 px reserved for meta + 1
// bio line + the trailing gap leaves a ~42-px window for the name
// stack. The picker honors this strictly — overflowing the budget
// drops the picker to the next-smaller font.
constexpr int kNameMaxBlockH  = 42;

struct NameStyle {
  const uint8_t* font;
  int            ascent;   // top-of-glyph to baseline (for first-baseline math)
  int            lineH;    // baseline-to-baseline advance for stacked lines
};

// Each lineH carries +2 px of leading over the font's natural advance
// so stacked names breathe instead of feeling crammed.
constexpr NameStyle kNameLadder[] = {
  { u8g2_font_helvB18_tr, 14, 21 },   // huge headline — single-line names ONLY
  { u8g2_font_helvB14_tr, 11, 17 },
  { u8g2_font_helvB12_tr,  9, 15 },
  { u8g2_font_helvB10_tr,  7, 13 },
  { u8g2_font_helvB08_tr,  6, 11 },   // floor — fitText truncates if even this overflows
};
constexpr uint8_t kNameLadderCount =
    sizeof(kNameLadder) / sizeof(kNameLadder[0]);

// Walk the ladder largest-first and return the biggest entry where
// every supplied line fits `textW` pixels wide AND the total stacked
// height (lineCount * lineH) fits `kNameMaxBlockH` pixels tall. The
// helvB18 slot is reserved for single-line names so a 2-line "Alex /
// Lynd" doesn't inherit the same headline weight as the long
// single-liners.
//
// Mutates the active font on `d`; the caller can keep using whatever
// font got picked without re-setting it.
NameStyle pickNameStyle(oled& d, char* lines[], uint8_t lineCount,
                        int textW) {
  const uint8_t startIdx = (lineCount >= 2) ? 1 : 0;
  for (uint8_t i = startIdx; i < kNameLadderCount; ++i) {
    const NameStyle& s = kNameLadder[i];
    d.setFont(s.font);
    bool fitsW = true;
    for (uint8_t j = 0; j < lineCount; ++j) {
      if (lines[j] == nullptr || *lines[j] == '\0') continue;
      if (d.getStrWidth(lines[j]) > textW) {
        fitsW = false;
        break;
      }
    }
    const int totalH = lineCount * s.lineH;
    if (fitsW && totalH <= kNameMaxBlockH) return s;
  }
  // Fallback: smallest font, even if individual lines still overflow
  // (fitText will truncate per line).
  d.setFont(kNameLadder[kNameLadderCount - 1].font);
  return kNameLadder[kNameLadderCount - 1];
}

// ── Joystick scroll ───────────────────────────────────────────────────────
// Reuses HelpScreen's free-scroll vocabulary. Deadband matches so the
// two screens feel like cousins to a user who has just come from HELP.
constexpr int16_t  kJoyDeadband          = 600;
constexpr float    kScrollPxPerSecAtFull = 160.0f;
constexpr uint16_t kFrameTickMinMs       = 8;

// ── Auto-cycle for static variants ────────────────────────────────────────
// When a card stays in view past this window without any input, the
// screen advances to the next bag entry — gives the user a slow rotation
// through every photo of a person without them having to scroll. Only
// fires on static variants; animations are exempt and play indefinitely
// until the user scrolls them away.
constexpr uint32_t kStaticHoldMs   = 6000;
// Sentinel for `bagPos_[i]` meaning "bag empty / never filled". Any
// value >= kMaxBagSize triggers a refill on the next pick.
constexpr uint8_t  kBagNeedsRefill = 0xFF;

// ── Rounded image corners ────────────────────────────────────────────────
// Radius (in pixels) of the quarter-circle knocked out of each corner
// of every credit headshot. Bumped up for a noticeable but not chunky
// "polaroid" look; r=4 lops ~5 pixels per corner. Keep small relative
// to kCardImageDim so the actual face never gets clipped.
constexpr int kImageCornerR = 5;

// Knock out the four corners of an `dim`×`dim` rectangle anchored at
// (x, y) so a freshly-drawn XBM reads as a rounded card. We blacken
// each pixel inside the corner box that lies outside the inscribed
// quarter-circle of radius `r` — the standard u8g2 drawRBox curve.
void roundImageCorners(oled& d, int x, int y, int dim, int r) {
  if (r <= 0 || dim <= 2 * r) return;
  d.setDrawColor(0);
  const int rsq = (r - 1) * (r - 1);
  for (int j = 0; j < r; ++j) {
    for (int i = 0; i < r; ++i) {
      const int dx = (r - 1) - i;
      const int dy = (r - 1) - j;
      if (dx * dx + dy * dy <= rsq) continue;
      d.drawPixel(x + i,                 y + j);
      d.drawPixel(x + dim - 1 - i,       y + j);
      d.drawPixel(x + i,                 y + dim - 1 - j);
      d.drawPixel(x + dim - 1 - i,       y + dim - 1 - j);
    }
  }
  d.setDrawColor(1);
}

int16_t totalContentHeight() {
  // Last card has no trailing gap — otherwise the bottom of the page
  // would end with empty space below the final headshot. We measure
  // groups (= card slots) here, not raw credit variants; pooled
  // entries collapse into one card.
  if (AboutCredits::kGroupCount == 0) return 0;
  return static_cast<int16_t>(AboutCredits::kGroupCount * kCardH - kCardGap);
}

int16_t maxScroll() {
  const int16_t total = totalContentHeight();
  return total > OLEDLayout::kScreenH
             ? static_cast<int16_t>(total - OLEDLayout::kScreenH)
             : 0;
}

// Greedy word-wrap into `maxLines` chunks of `maxW` pixels using the
// font currently selected on `d`. Writes line slices into `lines` as
// pointers into a writable scratch buffer (NUL-terminated in place by
// overwriting separator spaces). Anything past the last line that fits
// is dropped, with the final visible line ellipsised when it had to be
// truncated. Returns the number of lines written.
uint8_t wrapToLines(oled& d, char* scratch, int maxW, char** lines,
                    uint8_t maxLines) {
  uint8_t count = 0;
  char* cursor = scratch;
  while (count < maxLines && *cursor != '\0') {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0') break;

    char* lineStart = cursor;
    char* lastBreak = nullptr;
    char* probe = cursor;

    while (*probe != '\0') {
      const char saved = *probe;
      *probe = '\0';
      const int w = d.getStrWidth(lineStart);
      *probe = saved;
      if (w > maxW) break;
      if (*probe == ' ') lastBreak = probe;
      ++probe;
    }

    char* lineEnd;
    if (*probe == '\0') {
      lineEnd = probe;
    } else if (lastBreak != nullptr) {
      lineEnd = lastBreak;
    } else {
      // Single word longer than the column; hard-cut it character by
      // character so we still emit something rather than hanging on a
      // monster token.
      lineEnd = lineStart + 1;
      while (lineEnd < probe) {
        const char saved = *lineEnd;
        *lineEnd = '\0';
        const int w = d.getStrWidth(lineStart);
        *lineEnd = saved;
        if (w > maxW) break;
        ++lineEnd;
      }
      if (lineEnd == lineStart) lineEnd = lineStart + 1;
    }

    const bool isLastSlot = (count + 1 == maxLines);
    const bool more = (*lineEnd != '\0' || (lineEnd == probe && *probe != '\0'));
    if (isLastSlot && more) {
      // Truncate the final visible line and tack on an ellipsis so the
      // user knows the bio continues past what fits on the card.
      while (lineEnd > lineStart) {
        const char saved = *lineEnd;
        *lineEnd = '\0';
        char attempt[80];
        std::snprintf(attempt, sizeof(attempt), "%s...", lineStart);
        if (d.getStrWidth(attempt) <= maxW) {
          std::strncpy(lineStart, attempt, sizeof(attempt));
          break;
        }
        *lineEnd = saved;
        --lineEnd;
      }
      if (lineEnd == lineStart) {
        // Even the ellipsis alone doesn't fit; just keep what we have.
        lineEnd = probe;
      } else {
        lineEnd = lineStart + std::strlen(lineStart);
      }
      lines[count++] = lineStart;
      break;
    }

    if (*lineEnd != '\0') {
      *lineEnd = '\0';
      cursor = lineEnd + 1;
    } else {
      cursor = lineEnd;
    }
    lines[count++] = lineStart;
  }
  return count;
}

// Sentinel returned by activeFrame() when the credit is in its
// loop-end pause window AND the credit asked for a blank pause via
// the kFlagLoopBlank bit. Callers must test for this before indexing
// into the bitmap blob.
constexpr uint8_t kBlankFrame = 0xFF;

// Sum of one credit's per-frame durations, plus the loop-end pause.
// Pulled out so both `activeFrame` and the random-transform pre-pass
// can ask "what loop iteration are we in?" off the same number.
uint32_t cycleMsFor(const AboutCredits::Credit& c) {
  if (c.frameCount <= 1) return 0;
  const uint16_t* durs = AboutCredits::kFrameDurationsMs + c.durOffset;
  uint32_t animMs = 0;
  for (uint8_t i = 0; i < c.frameCount; ++i) animMs += durs[i];
  return animMs + AboutCredits::kAnimLoopPauseMs;
}

// Pick the active frame for an animated credit, given how long the
// screen has been alive in milliseconds. Static credits (frameCount
// <= 1) trivially return frame 0. Animated credits walk their per-
// frame duration table, then either hold the final frame OR render
// blank for an extra kAnimLoopPauseMs window before the cycle restarts.
uint8_t activeFrame(const AboutCredits::Credit& c, uint32_t elapsedMs) {
  if (c.frameCount <= 1) return 0;
  const uint16_t* durs = AboutCredits::kFrameDurationsMs + c.durOffset;
  uint32_t animMs = 0;
  for (uint8_t i = 0; i < c.frameCount; ++i) animMs += durs[i];
  const uint32_t cycleMs = animMs + AboutCredits::kAnimLoopPauseMs;
  if (cycleMs == 0) return 0;
  uint32_t t = elapsedMs % cycleMs;
  if (t >= animMs) {
    const bool blank = (c.flags & AboutCredits::kFlagLoopBlank) != 0;
    return blank ? kBlankFrame : (c.frameCount - 1);
  }
  uint32_t accum = 0;
  for (uint8_t i = 0; i < c.frameCount; ++i) {
    accum += durs[i];
    if (t < accum) return i;
  }
  return c.frameCount - 1;
}

// ── XBM rotate / flip blitter ────────────────────────────────────────────────
// `transform` packs:
//   bits 0..1  rotation steps (CW): 0=0°, 1=90°, 2=180°, 3=270°
//   bit  2     horizontal flip applied AFTER rotation
// The 8 combinations cover the dihedral group D4 — every rigid
// rotation/reflection a square supports. Used only for animated credits
// whose Credit::flags has kFlagRandomTransform set; cost is one
// 64×64 = 512 B framebuffer copy per affected card per frame, which is
// fine because there's at most one or two such cards on screen.
void blitTransformedXBM(oled& d, int x, int y,
                        const uint8_t* PROGMEM_src, uint8_t dim,
                        uint8_t transform) {
  // Tile-size buffer lives in static memory rather than on the stack
  // so we don't take 512 bytes off the rendering task's budget for
  // every credits redraw.
  static uint8_t scratch[64 * 64 / 8];
  const uint8_t stride = (dim + 7) / 8;
  std::memset(scratch, 0, stride * dim);

  const uint8_t rot   = transform & 0x3;
  const bool    hflip = (transform & 0x4) != 0;
  const uint8_t lim   = dim - 1;

  for (uint8_t sy = 0; sy < dim; ++sy) {
    for (uint8_t sxByte = 0; sxByte < stride; ++sxByte) {
      const uint8_t byte =
          pgm_read_byte(PROGMEM_src + sy * stride + sxByte);
      if (byte == 0) continue;
      for (uint8_t bit = 0; bit < 8; ++bit) {
        if (!(byte & (1 << bit))) continue;
        const uint8_t sx = (sxByte << 3) + bit;
        if (sx >= dim) break;
        // Rotate first, then optionally horizontal-flip.
        uint8_t dx, dy;
        switch (rot) {
          case 0: dx = sx;       dy = sy;       break;
          case 1: dx = lim - sy; dy = sx;       break;  // 90° CW
          case 2: dx = lim - sx; dy = lim - sy; break;  // 180°
          default: dx = sy;      dy = lim - sx; break;  // 270° CW
        }
        if (hflip) dx = lim - dx;
        scratch[dy * stride + (dx >> 3)] |= 1 << (dx & 7);
      }
    }
  }

  d.drawXBM(x, y, dim, dim, scratch);
}

// Hash a non-negative loop counter into a deterministic 0..7 transform.
// Knuth multiplicative hash is overkill statistically but cheap and
// produces a visibly different sequence even for very small loop counts
// (so the first few cycles after the screen opens already feel varied).
uint8_t randomTransformForLoop(uint32_t loopIdx) {
  uint32_t h = loopIdx * 2654435761u;
  return static_cast<uint8_t>(h >> 28) & 0x7;
}

// Render one credit into the framebuffer. `topY` is in viewport coords
// (already adjusted for scroll) and may be negative or off-screen — the
// render code must clip its own draws since u8g2's primitives don't.
// `elapsedMs` is millis()-since-onEnter so animated credits can pick
// the right frame for the current redraw without holding their own
// per-card clock.
void drawCard(oled& d, const AboutCredits::Credit& c, int topY,
              uint32_t elapsedMs) {
  // Headshot — 64×64 packed XBM, drawn straight from PROGMEM at 1:1.
  // u8g2 clips XBM at the panel edges, so a partially off-screen card
  // just renders the visible band of pixels. For animated credits we
  // skip ahead by `frameIdx * kBytesPerFrame` into the per-credit slab.
  // kBlankFrame means "this is the loop-end pause for a credit that
  // asked for a blank cell" — leave the image area untouched (default
  // OLED black) instead of holding the last frame.
  const uint8_t frameIdx = activeFrame(c, elapsedMs);
  if (frameIdx != kBlankFrame) {
    const uint16_t frameOffset =
        c.offset +
        static_cast<uint16_t>(frameIdx) * AboutCredits::kBytesPerFrame;
    const uint8_t* framePtr = AboutCredits::kCreditBits + frameOffset;

    // Random per-loop transform, only when the credit asked for it.
    // The loop index falls out of `elapsedMs / cycleMs`, so every card
    // computes its own loop count off the same screen-wide clock —
    // multiple animated credits stay in lockstep with their own
    // independent timelines.
    if ((c.flags & AboutCredits::kFlagRandomTransform) && c.frameCount > 1) {
      const uint32_t cycleMs = cycleMsFor(c);
      const uint32_t loopIdx = (cycleMs > 0) ? (elapsedMs / cycleMs) : 0;
      const uint8_t  xform   = randomTransformForLoop(loopIdx);
      if (xform == 0) {
        // Identity — skip the scratch-buffer roundtrip.
        d.drawXBM(kImageX, topY,
                  AboutCredits::kIconSize, AboutCredits::kIconSize,
                  framePtr);
      } else {
        blitTransformedXBM(d, kImageX, topY, framePtr,
                           AboutCredits::kIconSize, xform);
      }
    } else {
      d.drawXBM(kImageX, topY,
                AboutCredits::kIconSize, AboutCredits::kIconSize,
                framePtr);
    }
    // "Polaroid" rounded corners. Punch out a quarter-circle worth of
    // pixels at each corner of the just-drawn image so the headshot
    // reads as a rounded tile against the card's black background.
    roundImageCorners(d, kImageX, topY,
                      AboutCredits::kIconSize, kImageCornerR);
  }

  d.setDrawColor(1);

  // ── Name ────────────────────────────────────────────────────────────────
  // Multi-line names are written with embedded '\n' (or '\r') in the
  // JSON `name` field. We split on either, count lines, then pick a
  // font: 1–2 lines stay at the bold 7×13 size; 3+ lines downshift to
  // 6×10 so the stack can still fit above the meta rows. Each line is
  // truncated independently so a long surname doesn't blow out the
  // image's neighbour column.
  char nameBuf[64];
  std::strncpy(nameBuf, c.name ? c.name : "", sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = '\0';

  constexpr uint8_t kMaxNameLines = 4;
  char* nameLines[kMaxNameLines] = {nullptr};
  uint8_t nameLineCount = 0;
  {
    char* segStart = nameBuf;
    char* p = nameBuf;
    while (*p && nameLineCount < kMaxNameLines) {
      if (*p == '\n' || *p == '\r') {
        *p = '\0';
        nameLines[nameLineCount++] = segStart;
        segStart = p + 1;
      }
      ++p;
    }
    if (*segStart != '\0' && nameLineCount < kMaxNameLines) {
      nameLines[nameLineCount++] = segStart;
    }
    if (nameLineCount == 0) {
      nameLines[0] = nameBuf;
      nameLineCount = 1;
    }
  }

  // Per-name font: walk the ladder and pick the biggest entry where
  // every line fits the right column. pickNameStyle also leaves the
  // chosen font active on `d`, so the loop below can drawStr straight
  // away without an extra setFont.
  const NameStyle name =
      pickNameStyle(d, nameLines, nameLineCount, kTextW);
  int cursorY = topY + kColTopPad + name.ascent;  // first baseline
  for (uint8_t i = 0; i < nameLineCount; ++i) {
    char* line = nameLines[i];
    // In-place skip of any leading whitespace surviving the split (the
    // legacy "\r\n" case ends up looking like "" after splitting twice).
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '\0') continue;
    char trimmed[40];
    std::strncpy(trimmed, line, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    OLEDLayout::fitText(d, trimmed, sizeof(trimmed), kTextW);
    d.drawStr(kTextX, cursorY, trimmed);
    cursorY += name.lineH;
  }
  // After the loop `cursorY` is one lineH past the last drawn baseline,
  // i.e. roughly at the descender of the last line. A small extra gap
  // lands the first meta row with breathing room rather than glued to
  // the name's bottom.
  cursorY += kNameToMetaGap;

  // ── Position / Company / Website ────────────────────────────────────────
  // Tiny 4×6 lines beneath the name. Skip blank fields so a credit with
  // only a name doesn't leave gaping vertical space in the card.
  d.setFont(u8g2_font_4x6_tf);

  auto drawMetaLine = [&](const char* text) {
    if (!text || text[0] == '\0') return;
    char buf[40];
    std::strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    OLEDLayout::fitText(d, buf, sizeof(buf), kTextW);
    d.drawStr(kTextX, cursorY, buf);
    cursorY += kMetaLineH;
  };

  drawMetaLine(c.position);
  drawMetaLine(c.company);

  // Website gets the protocol prefix shaved off so the visible chunk is
  // the part the user actually wants to read. We don't store the
  // shortened form because the underlying field may be reused in other
  // contexts (e.g. a future "boop me a vCard" feature) where the full
  // URL matters.
  if (c.website && c.website[0] != '\0') {
    const char* w = c.website;
    if (std::strncmp(w, "https://", 8) == 0) w += 8;
    else if (std::strncmp(w, "http://", 7) == 0) w += 7;
    drawMetaLine(w);
  }

  // ── Bio ─────────────────────────────────────────────────────────────────
  // Wrapped greedily into the remaining vertical room (capped at
  // kBioMaxLines so we never bleed onto the next card's image).
  if (c.bio && c.bio[0] != '\0') {
    char bioBuf[96];
    std::strncpy(bioBuf, c.bio, sizeof(bioBuf) - 1);
    bioBuf[sizeof(bioBuf) - 1] = '\0';
    char* lines[kBioMaxLines] = {nullptr};
    const uint8_t lineCount =
        wrapToLines(d, bioBuf, kTextW, lines, kBioMaxLines);
    for (uint8_t i = 0; i < lineCount; ++i) {
      d.drawStr(kTextX, cursorY, lines[i]);
      cursorY += kBioLineH;
    }
  }
}

// Which group's card has the most pixels in the visible viewport?
// Used by the L/R snap input so a press hops from the card the user
// is mostly looking at, not the one hanging on by its last row.
uint8_t dominantGroupIndex(int16_t scrollPx) {
  if (AboutCredits::kGroupCount == 0) return 0;
  uint8_t bestIdx = 0;
  int     bestCoverage = -1;
  for (uint8_t i = 0; i < AboutCredits::kGroupCount; ++i) {
    const int top = i * kCardH - scrollPx;
    const int bot = top + kCardImageDim;  // ignore the trailing gap
    const int lo  = top > 0 ? top : 0;
    const int hi  = bot < OLEDLayout::kScreenH ? bot : OLEDLayout::kScreenH;
    const int cov = hi - lo;
    if (cov > bestCoverage) {
      bestCoverage = cov;
      bestIdx = i;
    }
  }
  return bestIdx;
}

}  // namespace

void AboutCreditsScreen::onEnter(GUIManager& /*gui*/) {
  scrollPx_     = 0;
  enterMs_      = millis();
  lastScrollMs_ = enterMs_;
  // Reset every group's bag — a fresh visit to the credits screen
  // always starts a brand-new pass through each pool, so no two
  // re-openings share state. The kBagNeedsRefill sentinel makes the
  // first pickNextVariant() call rebuild + shuffle before consuming.
  for (uint8_t i = 0; i < AboutCredits::kGroupCount; ++i) {
    bagPos_[i] = kBagNeedsRefill;
    currentVariant_[i] = 0;
    variantShownAtMs_[i] = 0;
    wasVisible_[i] = false;
  }
}

void AboutCreditsScreen::refillBag(uint8_t groupIdx) {
  const AboutCredits::CreditGroup& g = AboutCredits::kGroups[groupIdx];
  // Fill bag[0..bagSize) with variant indices, each repeated by the
  // variant's `weight`. e.g. variants {A:1, B:1, C:1, D:5} produce
  // [0, 1, 2, 3, 3, 3, 3, 3] before the shuffle.
  uint8_t pos = 0;
  for (uint8_t v = 0; v < g.count && pos < AboutCredits::kMaxBagSize;
       ++v) {
    const uint8_t w =
        AboutCredits::kCredits[g.firstCredit + v].weight;
    for (uint8_t k = 0;
         k < w && pos < AboutCredits::kMaxBagSize;
         ++k) {
      bag_[groupIdx][pos++] = v;
    }
  }
  // Fisher-Yates shuffle. Single-entry bags fall through cleanly.
  for (int j = static_cast<int>(pos) - 1; j > 0; --j) {
    const uint32_t r = esp_random() % static_cast<uint32_t>(j + 1);
    const uint8_t k = static_cast<uint8_t>(r);
    if (k != j) {
      const uint8_t tmp = bag_[groupIdx][j];
      bag_[groupIdx][j] = bag_[groupIdx][k];
      bag_[groupIdx][k] = tmp;
    }
  }
  bagPos_[groupIdx] = 0;
}

void AboutCreditsScreen::pickNextVariant(uint8_t groupIdx,
                                         uint32_t nowMs) {
  const AboutCredits::CreditGroup& g = AboutCredits::kGroups[groupIdx];
  if (g.bagSize == 0 || g.count == 0) {
    currentVariant_[groupIdx] = 0;
    variantShownAtMs_[groupIdx] = nowMs;
    return;
  }
  // Anti-immediate-repeat: when crossing a bag boundary on a multi-
  // variant group, peek at the next bag entry; if it matches the
  // outgoing variant, swap with the next position so the user doesn't
  // see the same photo twice in a row at the boundary. Single-variant
  // groups skip this entirely (they have nothing else to swap to).
  if (bagPos_[groupIdx] == kBagNeedsRefill ||
      bagPos_[groupIdx] >= g.bagSize) {
    refillBag(groupIdx);
    if (g.count > 1 && g.bagSize > 1 &&
        bag_[groupIdx][0] == currentVariant_[groupIdx]) {
      for (uint8_t k = 1; k < g.bagSize; ++k) {
        if (bag_[groupIdx][k] != currentVariant_[groupIdx]) {
          const uint8_t tmp = bag_[groupIdx][0];
          bag_[groupIdx][0] = bag_[groupIdx][k];
          bag_[groupIdx][k] = tmp;
          break;
        }
      }
    }
  }
  currentVariant_[groupIdx] = bag_[groupIdx][bagPos_[groupIdx]++];
  variantShownAtMs_[groupIdx] = nowMs;
}

void AboutCreditsScreen::snapToGroup(uint8_t idx) {
  if (AboutCredits::kGroupCount == 0) return;
  if (idx >= AboutCredits::kGroupCount) idx = AboutCredits::kGroupCount - 1;
  int32_t target = static_cast<int32_t>(idx) * kCardH;
  const int16_t cap = maxScroll();
  if (target > cap) target = cap;
  if (target < 0)   target = 0;
  scrollPx_ = static_cast<int16_t>(target);
}

void AboutCreditsScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);
  d.setTextWrap(false);

  // Walk the group slots. For each:
  //   * Detect the off→on visibility transition and pop the next entry
  //     from that group's shuffled bag, so a user who scrolls past then
  //     back to a pooled credit (Kevin, Shy, …) sees a fresh face.
  //   * For groups whose currently-shown variant is static (frameCount
  //     == 1), auto-cycle every kStaticHoldMs so a user who lingers
  //     gets a slow rotation through every photo of the person.
  //     Animations are exempt — they play indefinitely.
  // Cull anything whose 64-px image band sits entirely off-screen.
  const uint32_t now      = millis();
  const uint32_t elapsed  = now - enterMs_;
  for (uint8_t i = 0; i < AboutCredits::kGroupCount; ++i) {
    const int topY = i * kCardH - scrollPx_;
    const bool visible =
        (topY + kCardImageDim > 0) && (topY < OLEDLayout::kScreenH);

    if (visible && !wasVisible_[i]) {
      pickNextVariant(i, now);
    } else if (visible && wasVisible_[i]) {
      // Auto-cycle while the user lingers. Only for static variants
      // and only for groups with more than one variant — single-image
      // people have nothing to cycle to.
      const AboutCredits::CreditGroup& g = AboutCredits::kGroups[i];
      if (g.count > 1) {
        const uint8_t v = currentVariant_[i] < g.count
            ? currentVariant_[i] : 0;
        const AboutCredits::Credit& cur =
            AboutCredits::kCredits[g.firstCredit + v];
        const bool isAnim = cur.frameCount > 1;
        if (!isAnim &&
            (now - variantShownAtMs_[i] >= kStaticHoldMs)) {
          pickNextVariant(i, now);
        }
      }
    }
    wasVisible_[i] = visible;

    if (!visible) continue;
    const AboutCredits::CreditGroup& g = AboutCredits::kGroups[i];
    const uint8_t variantIdx = currentVariant_[i] < g.count
        ? currentVariant_[i] : 0;
    const AboutCredits::Credit& c =
        AboutCredits::kCredits[g.firstCredit + variantIdx];
    drawCard(d, c, topY, elapsed);
  }
}

void AboutCreditsScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                     int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }

  // Edge-triggered LEFT / RIGHT (and CONFIRM as "go forward") snap the
  // viewport to a card boundary, so a user who just wants to tab
  // through faces never has to scrub the joystick.
  if (e.rightPressed || e.confirmPressed) {
    const uint8_t idx = dominantGroupIndex(scrollPx_);
    snapToGroup(static_cast<uint8_t>((idx + 1) % AboutCredits::kGroupCount));
    gui.requestRender();
    return;
  }
  if (e.leftPressed) {
    const uint8_t idx = dominantGroupIndex(scrollPx_);
    const uint8_t prev = idx == 0 ? AboutCredits::kGroupCount - 1 : idx - 1;
    snapToGroup(prev);
    gui.requestRender();
    return;
  }

  // Joystick free-scroll, time-integrated so motion stays smooth across
  // variable frame intervals (HelpScreen-style integrator).
  const uint32_t now = millis();
  uint32_t dt = now - lastScrollMs_;
  if (dt < kFrameTickMinMs) return;
  lastScrollMs_ = now;
  if (dt > 100) dt = 100;  // clamp focus-return bursts

  const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(dy) < kJoyDeadband) return;

  const float norm    = static_cast<float>(dy) / 2047.0f;
  const float deltaPx = norm * kScrollPxPerSecAtFull * (dt / 1000.0f);

  const int16_t cap = maxScroll();
  int32_t next = static_cast<int32_t>(scrollPx_) +
                 static_cast<int32_t>(deltaPx);
  if (next < 0)   next = 0;
  if (next > cap) next = cap;
  if (static_cast<int16_t>(next) != scrollPx_) {
    scrollPx_ = static_cast<int16_t>(next);
    gui.requestRender();
  }
}
