#include "BoopScreen.h"

#include <cstdio>
#include <cstring>

#include "../ui/GUI.h"
#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../ble/BadgeBeaconAdv.h"
#include "../ble/BleBeaconScanner.h"
#endif
#include "../boops/BadgeBoops.h"
#include "../ir/BadgeIR.h"
#include "../identity/BadgeUID.h"
#include "../identity/BadgeVersion.h"
#include "../ui/graphics.h"
#include "../hardware/Haptics.h"
#include "../ui/Images.h"
#include "../ui/OLEDLayout.h"
#include "../ui/UIFonts.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  BoopScreen — new layout
//  +-------------------+--+----------------+
//  |                   |  | ziggy 48x48    |  ← IR LED sits at top edge
//  |                   |  | (state-driven) |
//  | Peer name (LARGE) |  |                |
//  | Company (SMALL)   |  |                |
//  | Title  (SMALL)    |  |                |
//  | Email  (TINY)     |  |                |
//  +----------------------+----------------+
//  | Marquee strip (FONT_TINY) 16px tall   |
//  +---------------------------------------+
// ═══════════════════════════════════════════════════════════════════════════════

// New layout: standard status header at top (drawStatusHeader) +
// standard game/action footer at bottom. Ziggy sprite sits
// in the lower-left, mirror-flipped so the character faces right
// into the content area, with its bottom row touching the footer
// rule. Text content (peer info / handler-provided body) lives in
// the strip to the right of the sprite.
static constexpr int kZiggySize = 48;
static constexpr int kZiggyX    = 0;
static constexpr int kZiggyY    = OLEDLayout::kFooterTopY - kZiggySize;  // 54 - 48 = 6

// Right info block — sits to the right of the ziggy sprite, between
// header (y=0..7) and footer (y=54..63).
static constexpr int kLeftX = kZiggySize + 4;
static constexpr int kLeftY = OLEDLayout::kContentTopY;
static constexpr int kLeftW = 128 - kLeftX;
static constexpr int kLeftH = OLEDLayout::kFooterTopY - kLeftY;

// Mirror an XBM bitmap horizontally as we draw it. Per-pixel iteration
// because u8g2 has no built-in horizontal flip; the boop sprites are
// 48×48 (~2300 pixels) so cost is negligible at the badge's frame
// rate.
static void drawXBMFlippedX(oled& d, int x, int y, int w, int h,
                            const uint8_t* src) {
  if (!src) return;
  const int bytesPerRow = (w + 7) / 8;
  for (int r = 0; r < h; r++) {
    const uint8_t* row = src + r * bytesPerRow;
    for (int c = 0; c < w; c++) {
      const int byte = c >> 3;
      const int bit  = c & 7;
      if (row[byte] & (1u << bit)) {
        d.drawPixel(x + (w - 1 - c), y + r);
      }
    }
  }
}

// Bottom log strip: 16 px below the divider line.
static constexpr int kLogDividerY = 47;
static constexpr int kLogBaseY    = 48;
static constexpr int kLogH        = 16;

// Short display label for a BoopFieldTag — delegates to the single
// source of truth in BadgeBoops so chip labels never drift from the
// BoopFeedback marquee text.
static const char* fieldShortLabel(uint8_t tag) {
    return BadgeBoops::fieldShortName(tag);
}

void BoopScreen::onEnter(GUIManager& /*gui*/) {
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  BadgeBeaconAdv::setPausedForIr(true);
  BleBeaconScanner::stopScan();
  BleBeaconScanner::clearScanCache();
  for (int i = 0; i < 25 && BadgeBeaconAdv::isBroadcasting(); ++i) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
#endif
  irHardwareEnabled = true;
  BadgeBoops::boopEngaged = false;
  // Full zero-out so a stale label byte from a previous session can never
  // bleed into the first rendered chip.
  memset(txChips_,  0, sizeof(txChips_));
  memset(logLines_, 0, sizeof(logLines_));
  prevTxMask_           = BadgeBoops::boopStatus.fieldTxMask;
  pendingTxEdges_       = 0;
  prevBeaconTxCount_    = BadgeBoops::boopStatus.beaconTxCount;
  pendingBeaconTxChip_  = false;
  lastBeaconTxChipMs_   = 0;
  prevRxMask_           = BadgeBoops::boopStatus.fieldRxMask;
  pendingRxEdges_       = 0;
  prevBeaconRxCount_    = BadgeBoops::boopStatus.beaconRxCount;
  pendingBeaconRxChip_  = false;
  lastBeaconRxChipMs_   = 0;
  lastChipSpawnMs_      = 0;
  logCount_           = 0;
  logAnimStartMs_     = 0;
  ziggyFrame_         = 0;
  lastZiggyStepMs_    = 0;
  prevPhase_          = BadgeBoops::boopStatus.phase;
  // Drain any stale events that BoopFeedback posted before we entered.
  char dump[16];
  while (BoopFeedback::popMarqueeEvent(dump, sizeof(dump))) { /* discard */ }
  Serial.println("GUI: BoopScreen entered, IR enabled");
}

void BoopScreen::onExit(GUIManager& /*gui*/) {
  irHardwareEnabled = false;
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  BadgeBeaconAdv::setPausedForIr(false);
#endif
  BadgeBoops::boopEngaged = false;
  BadgeBoops::pairingCancelRequested = false;
  BadgeBoops::smReset();
  Serial.println("GUI: BoopScreen exited, IR disabled");
}

void BoopScreen::pushLogLine(const char* chip, uint32_t nowMs) {
  // Shift ring up by one — oldest at [0], newest at [kMaxLogLines-1].
  for (uint8_t i = 0; i + 1 < kMaxLogLines; i++) {
    memcpy(logLines_[i], logLines_[i + 1], sizeof(logLines_[i]));
  }
  strncpy(logLines_[kMaxLogLines - 1], chip, sizeof(logLines_[0]) - 1);
  logLines_[kMaxLogLines - 1][sizeof(logLines_[0]) - 1] = '\0';
  logCount_++;
  logAnimStartMs_ = nowMs;
}

// Fill an empty TxChip slot with the given label/tag + direction. Returns
// true on success. `tagForXHint` only seeds the per-chip horizontal jitter
// (0xA0 for beacon TX, 0xA1 for beacon RX, FIELD_* enum for field events).
bool BoopScreen::spawnChip(const char* label, uint8_t tagForXHint,
                           bool rising, uint32_t nowMs) {
  for (auto& c : txChips_) {
    if (c.bornMs != 0 && (nowMs - c.bornMs) <= kTxChipTtlMs) continue;
    c.bornMs = nowMs;
    c.tag    = tagForXHint;
    c.xHint  = static_cast<uint8_t>(nowMs ^ (tagForXHint * 37));
    c.rising = rising;
    strncpy(c.label, label, sizeof(c.label) - 1);
    c.label[sizeof(c.label) - 1] = '\0';
    return true;
  }
  return false;
}

void BoopScreen::poll(uint32_t nowMs) {
  const BadgeBoops::BoopPhase phase = BadgeBoops::boopStatus.phase;
  if (phase != prevPhase_) {
    prevPhase_ = phase;
    if (phase == BadgeBoops::BOOP_PHASE_PAIRED_OK) {
      memset(txChips_, 0, sizeof(txChips_));
      pendingTxEdges_ = 0;
      pendingRxEdges_ = 0;
      pendingBeaconTxChip_ = false;
      pendingBeaconRxChip_ = false;
    }
  }

  // Snap all edge trackers back to zero when the state machine lands in
  // IDLE. Without this, the counters getting zeroed in handleTerminal
  // (e.g. 15 → 0) looks like a negative increment, which wraps under
  // uint8 arithmetic and fires spurious beacon chips on the ready screen
  // after dismissing a completed boop. Do the same for field masks so a
  // newly started boop's fresh bits don't compare against stale prevs.
  if (phase == BadgeBoops::BOOP_PHASE_IDLE) {
    prevTxMask_           = 0;
    prevRxMask_           = 0;
    prevBeaconTxCount_    = 0;
    prevBeaconRxCount_    = 0;
    pendingTxEdges_       = 0;
    pendingRxEdges_       = 0;
    pendingBeaconTxChip_  = false;
    pendingBeaconRxChip_  = false;
  }

  // ── TX edge detection ─────────────────────────────────────────────────
  // Only track edges when actively booping (not while idle/ready).
  if (phase != BadgeBoops::BOOP_PHASE_IDLE) {
    // 1) Per-field TX events during EXCHANGE phase (fieldTxMask bits).
    {
      uint16_t mask  = BadgeBoops::boopStatus.fieldTxMask;
      uint16_t edges = mask & ~prevTxMask_;
      prevTxMask_ = mask;
      pendingTxEdges_ |= edges;
      pendingTxEdges_ &= ~(1 << FIELD_ATTENDEE_TYPE);
    }
    // 2) Beacon TXs during BEACONING.
    {
      uint8_t beaconTx = BadgeBoops::boopStatus.beaconTxCount;
      if (beaconTx > prevBeaconTxCount_) {
        pendingBeaconTxChip_ = true;
      }
      prevBeaconTxCount_ = beaconTx;
    }

    // ── RX edge detection (mirror of TX) ──────────────────────────────────
    // 3) Per-field RX events — peer's fields landing via fieldRxMask.
    {
      uint16_t mask  = BadgeBoops::boopStatus.fieldRxMask;
      uint16_t edges = mask & ~prevRxMask_;
      prevRxMask_ = mask;
      pendingRxEdges_ |= edges;
      pendingRxEdges_ &= ~(1 << FIELD_ATTENDEE_TYPE);
    }
    // 4) Beacon RX — peer's beacons as we hear them via beaconRxCount.
    {
      uint8_t beaconRx = BadgeBoops::boopStatus.beaconRxCount;
      if (beaconRx > prevBeaconRxCount_) {
        pendingBeaconRxChip_ = true;
      }
      prevBeaconRxCount_ = beaconRx;
    }
  } else {
    // While IDLE, just sync counters so we don't fire stale edges on transition.
    prevTxMask_        = BadgeBoops::boopStatus.fieldTxMask;
    prevBeaconTxCount_ = BadgeBoops::boopStatus.beaconTxCount;
    prevRxMask_        = BadgeBoops::boopStatus.fieldRxMask;
    prevBeaconRxCount_ = BadgeBoops::boopStatus.beaconRxCount;
  }

  // ── Release at most one chip per kChipSpawnMinGapMs. Priority:
  //    field TX > field RX > beacon TX > beacon RX. Field events carry
  //    more information so they jump the queue; beacons dominate in
  //    BEACONING where no field events can compete anyway.
  const bool canSpawn =
      (lastChipSpawnMs_ == 0) ||
      ((nowMs - lastChipSpawnMs_) >= kChipSpawnMinGapMs);

  if (canSpawn) {
    if (pendingTxEdges_ != 0) {
      uint8_t tag = __builtin_ctz(pendingTxEdges_);
      pendingTxEdges_ &= ~(1 << tag);
      if (spawnChip(fieldShortLabel(tag), tag, /*rising=*/true, nowMs)) {
        lastChipSpawnMs_ = nowMs;
      }
    } else if (pendingRxEdges_ != 0) {
      uint8_t tag = __builtin_ctz(pendingRxEdges_);
      pendingRxEdges_ &= ~(1 << tag);
      if (spawnChip(fieldShortLabel(tag), tag, /*rising=*/false, nowMs)) {
        lastChipSpawnMs_ = nowMs;
      }
    } else if (pendingBeaconTxChip_) {
      bool cooldownOk = (lastBeaconTxChipMs_ == 0) ||
                        ((nowMs - lastBeaconTxChipMs_) >= kBeaconChipCooldownMs);
      if (cooldownOk) {
        pendingBeaconTxChip_ = false;
        if (spawnChip("ping", 0xA0, /*rising=*/true, nowMs)) {
          lastChipSpawnMs_ = nowMs;
          lastBeaconTxChipMs_ = nowMs;
        }
      } else {
        pendingBeaconTxChip_ = false;
      }
    } else if (pendingBeaconRxChip_) {
      bool cooldownOk = (lastBeaconRxChipMs_ == 0) ||
                        ((nowMs - lastBeaconRxChipMs_) >= kBeaconChipCooldownMs);
      if (cooldownOk) {
        pendingBeaconRxChip_ = false;
        if (spawnChip("seen", 0xA1, /*rising=*/false, nowMs)) {
          lastChipSpawnMs_ = nowMs;
          lastBeaconRxChipMs_ = nowMs;
        }
      } else {
        pendingBeaconRxChip_ = false;
      }
    }
  }

  // ── Drain feedback events into the vertical log ring.
  char chip[16];
  while (BoopFeedback::popMarqueeEvent(chip, sizeof(chip))) {
    pushLogLine(chip, nowMs);
  }
}

void BoopScreen::renderTopTxBand(oled& d, uint32_t nowMs) {
  d.setFontPreset(FONT_TINY);
  // Bounds:
  //   Rising (TX): y = 46 → -6. The chip starts just above the log
  //                divider at row 47 and ascends off the top edge where
  //                the IR LED physically sits.
  //   Falling (RX): y = -6 → 46. Mirror image — chip enters from above
  //                 the top edge and settles into the info overlay.
  // Horizontal sway is a small integer triangle wave. This keeps the
  // "data spark" feel without pulling floating-point trig into the hot
  // render path.
  constexpr int kStartY   = 46;   // rising=true start; rising=false end
  constexpr int kEndY     = -6;   // rising=true end;   rising=false start
  constexpr int kTravel   = kStartY - kEndY;  // 52 px
  constexpr int kSwayPx   = 5;
  for (auto& c : txChips_) {
    if (c.bornMs == 0) continue;
    if (c.label[0] == '\0') { c.bornMs = 0; continue; }
    uint32_t age = nowMs - c.bornMs;
    if (age > kTxChipTtlMs) { c.bornMs = 0; continue; }

    const int travelled = static_cast<int>((age * kTravel) / kTxChipTtlMs);
    const int y = c.rising ? (kStartY - travelled)
                           : (kEndY   + travelled);

    uint8_t wave = static_cast<uint8_t>(((age * 96UL) / kTxChipTtlMs) + c.xHint);
    wave &= 0x3F;
    int tri = (wave < 32) ? wave : (63 - wave);  // 0..31..0
    int sway = ((tri - 16) * kSwayPx) / 16;

    // Both directions use a trailing "^"; RX chips render upside-down
    // so the caret points down and the glyphs visually drip into the
    // info overlay. TX chips render normally.
    char line[12];
    snprintf(line, sizeof(line), "%s^", c.label);

    // Center each chip around the middle of the left info panel with a
    // random scatter seeded off xHint. kScatterPx controls the half-
    // width of the scatter band; sway adds the subtle soap-bubble drift.
    const int labelW = d.getStrWidth(line);
    constexpr int kScatterPx = 18;
    const int scatter = static_cast<int>(c.xHint % (2 * kScatterPx + 1)) - kScatterPx;
    const int labelCenter = (kLeftW / 2) + scatter + sway;
    int x = labelCenter - (labelW / 2);
    if (x < 0) x = 0;
    if (x + labelW > kLeftW) x = kLeftW - labelW;

    if (c.rising) {
      d.drawStr(x, y, line);
    } else {
      // After setFontDirection(2) the reference (x, y) becomes the
      // top-right corner of the rotated text. To make the rotated chip
      // occupy the same bounding box a normal chip at (x, y) would, we
      // shift the reference by +width horizontally and (roughly) by the
      // font's cap height vertically.
      d.setFontDirection(2);
      d.drawStr(x + labelW, y - 5, line);
      d.setFontDirection(0);
    }
  }
}

void BoopScreen::renderActiveAtmospherics(oled& d, uint32_t nowMs) {
  using namespace BadgeBoops;
  const BoopPhase phase = boopStatus.phase;
  if (phase == BOOP_PHASE_IDLE || phase == BOOP_PHASE_PAIRED_OK) return;

  const uint8_t beat = static_cast<uint8_t>((nowMs / 140) & 0x07);
  const bool hasPeer = boopStatus.beaconRxCount > 0 || boopStatus.peerUID[0];

  if (phase == BOOP_PHASE_BEACONING) {
    // Searching: three little radio pips chase upward beside the sprite.
    for (uint8_t i = 0; i < 3; i++) {
      const int y = 9 + i * 11;
      const int x = 73 + ((beat + i) & 0x03);
      d.drawPixel(x, y);
      if (((beat + i) & 0x03) == 0) d.drawHLine(x - 2, y + 2, 5);
    }
  }

  if (hasPeer) {
    // Peer detected: bracket the active avatar area like a quick lock-on.
    const int inset = (beat & 0x01) ? 1 : 0;
    const int x0 = kZiggyX + inset;
    const int y0 = kZiggyY + inset;
    const int x1 = kZiggyX + kZiggySize - 1 - inset;
    const int y1 = kZiggyY + kZiggySize - 1 - inset;
    d.drawHLine(x0, y0, 8);
    d.drawVLine(x0, y0, 8);
    d.drawHLine(x1 - 7, y0, 8);
    d.drawVLine(x1, y0, 8);
    d.drawHLine(x0, y1, 8);
    d.drawVLine(x0, y1 - 7, 8);
    d.drawHLine(x1 - 7, y1, 8);
    d.drawVLine(x1, y1 - 7, 8);
  }

  if (phase == BOOP_PHASE_EXCHANGE) {
    d.setFontPreset(FONT_TINY);
    const int x = 95 + (beat & 0x01);
    d.drawStr(x, 45, "sync");
  }
}

void BoopScreen::renderConfirmedReveal(oled& d, uint32_t nowMs) {
  using namespace BadgeBoops;
  if (boopStatus.phase != BOOP_PHASE_PAIRED_OK || boopStatus.boopType != BOOP_PEER) {
    return;
  }

  // Animation timing: a left-to-right wipe uncovers the contact card,
  // then a brief sparkle dance plays around the edges.  Card itself is
  // drawn underneath by peer_drawContent — this overlay is purely
  // decorative.
  constexpr uint32_t kRevealMs = 700;
  constexpr uint32_t kSparkMs  = 950;
  const uint32_t age = nowMs - boopStatus.phaseEnteredMs;

  // Mask the right side of the card while the wipe is sweeping.
  if (age < kRevealMs) {
    const int revealed = static_cast<int>((age * 128UL) / kRevealMs);
    const int wipeX = revealed < 0 ? 0 : (revealed > 128 ? 128 : revealed);
    d.setDrawColor(0);
    d.drawBox(wipeX, 0, 128 - wipeX, kLogDividerY);
    d.setDrawColor(1);
    if (wipeX > 0 && wipeX < 128) d.drawVLine(wipeX, 2, kLogDividerY - 4);
  }

  // Sparkle: a few transient pixels around the card border.
  if (age < kSparkMs) {
    const uint8_t step = static_cast<uint8_t>((age / 95) & 0x07);
    const int cx = 116 - step * 5;
    const int cy = 9 + (step % 3) * 9;
    d.drawPixel(cx, cy);
    d.drawHLine(cx - 2, cy, 5);
    d.drawVLine(cx, cy - 2, 5);
    if (step < 5) {
      d.drawPixel(8 + step * 9, 4 + step);
      d.drawPixel(122 - step * 7, 38 - step);
    }
  }
}

#if 0  // Scrollable-card path removed; kept here as reference until v3 lands.
void BoopScreen::renderConfirmedCardDeprecated(oled& d, uint32_t nowMs) {
  using namespace BadgeBoops;
  if (boopStatus.phase != BOOP_PHASE_PAIRED_OK || boopStatus.boopType != BOOP_PEER) {
    return;
  }
  (void)nowMs;

  // Use the full content band (under the status header, above the
  // footer). The reveal is now a stand-alone screen-spanning card,
  // not a strip beside the ziggy sprite, so company / email / web /
  // bio aren't truncated to the right-hand 76 px column anymore.
  constexpr int kCardTop    = OLEDLayout::kContentTopY;       // ~10
  constexpr int kCardBottom = OLEDLayout::kFooterTopY;        // ~54
  const int viewportH = kCardBottom - kCardTop;
  d.setDrawColor(0);
  d.drawBox(0, kCardTop, 128, viewportH);
  d.setDrawColor(1);

  // Field assembly. Each entry is (label, value); label is "" for the
  // big name header. Empty fields are skipped so the layout collapses
  // when peer hasn't shared something. Bio is laid out separately so
  // we can soft-wrap it across multiple lines.
  struct Row { const char* label; const char* value; bool wrap; };
  const BoopStatus& s = boopStatus;
  const char* name = s.peerName[0] ? s.peerName
                   : s.peerUID[0]  ? s.peerUID
                   : "...";
  const Row rows[] = {
    { "",       name,           false },  // big header
    { "Title",  s.peerTitle,    true  },
    { "Co",     s.peerCompany,  true  },
    { "Type",   s.peerAttendeeType, false },
    { "Email",  s.peerEmail,    true  },
    { "Web",    s.peerWebsite,  true  },
    { "Phone",  s.peerPhone,    false },
    { "Bio",    s.peerBio,      true  },
    { "UID",    s.peerUID,      false },
  };

  // First pass: measure total content height with the current wrap. We
  // need this both for clamping the scroll offset and for drawing the
  // scrollbar thumb. Lay out into a virtual canvas; the second pass
  // re-runs the same math but draws and clips against the viewport.
  auto measureLines = [&](oled& dd, const char* val, int maxW) -> int {
    if (!val || !val[0]) return 0;
    if (dd.getStrWidth(val) <= maxW) return 1;
    // Naive word wrap by spaces. If a word doesn't fit, hard-cut.
    int lines = 1;
    int w = 0;
    char tmp[8];
    const char* p = val;
    while (*p) {
      const char* word = p;
      while (*p && *p != ' ') p++;
      const int wordLen = (int)(p - word);
      if (wordLen <= 0) { p++; continue; }
      strncpy(tmp, word, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = '\0';
      // Approximate per-char width by the source string slice — close
      // enough to detect "this word + space won't fit on the current line".
      char slice[80];
      const int sliceLen = wordLen < (int)sizeof(slice) - 1 ? wordLen : (int)sizeof(slice) - 1;
      memcpy(slice, word, sliceLen);
      slice[sliceLen] = '\0';
      const int wordW = dd.getStrWidth(slice);
      const int spaceW = (w > 0) ? dd.getStrWidth(" ") : 0;
      if (w + spaceW + wordW > maxW && w > 0) {
        lines++;
        w = wordW;
      } else {
        w += spaceW + wordW;
      }
      while (*p == ' ') p++;
    }
    return lines;
  };

  // Layout constants. Header is FONT_LARGE on its own ascender; rows
  // are FONT_SMALL with a label-width gutter on the left.
  constexpr int kRowH       = 10;   // FONT_SMALL line pitch
  constexpr int kHeaderH    = 14;   // FONT_LARGE for the name
  constexpr int kHeaderGapY = 3;
  constexpr int kRowGapY    = 1;
  constexpr int kLabelW     = 30;   // "Email" etc fit comfortably
  const int contentX = 2;
  const int contentMaxW = 128 - contentX - 4;  // leave room for scrollbar

  // Pass 1: measure
  int totalH = 0;
  d.setFontPreset(FONT_LARGE);
  totalH += kHeaderH + kHeaderGapY;
  d.setFontPreset(FONT_SMALL);
  for (size_t i = 1; i < sizeof(rows) / sizeof(rows[0]); i++) {
    if (!rows[i].value || !rows[i].value[0]) continue;
    if (rows[i].wrap) {
      const int valW = contentMaxW - kLabelW;
      const int lines = measureLines(d, rows[i].value, valW);
      totalH += lines * kRowH + kRowGapY;
    } else {
      totalH += kRowH + kRowGapY;
    }
  }
  confirmedContentH_ = (int16_t)totalH;

  // Clamp scroll: don't scroll past where the last row would still be
  // partly visible. If everything fits, force scroll=0.
  const int maxScroll = totalH > viewportH ? (totalH - viewportH) : 0;
  if (confirmedScrollPx_ < 0) confirmedScrollPx_ = 0;
  if (confirmedScrollPx_ > maxScroll) confirmedScrollPx_ = (int16_t)maxScroll;

  // Pass 2: draw with scroll offset, clipping to viewport.
  int yCursor = kCardTop - confirmedScrollPx_;
  d.setFontPreset(FONT_LARGE);
  if (yCursor + kHeaderH > kCardTop && yCursor < kCardBottom) {
    d.drawStr(contentX, yCursor + kHeaderH - 2, name);
  }
  yCursor += kHeaderH + kHeaderGapY;

  d.setFontPreset(FONT_SMALL);
  for (size_t i = 1; i < sizeof(rows) / sizeof(rows[0]); i++) {
    const Row& r = rows[i];
    if (!r.value || !r.value[0]) continue;

    if (!r.wrap) {
      if (yCursor + kRowH > kCardTop && yCursor < kCardBottom) {
        d.drawStr(contentX, yCursor + kRowH - 2, r.label);
        d.drawStr(contentX + kLabelW, yCursor + kRowH - 2, r.value);
      }
      yCursor += kRowH + kRowGapY;
      continue;
    }

    // Wrapped row: print label once on the first line, then wrap value.
    const int valX = contentX + kLabelW;
    const int valW = contentMaxW - kLabelW;
    bool firstLine = true;
    const char* p = r.value;
    while (*p) {
      // Greedy word-pack into one display line.
      char line[64] = {};
      int lineW = 0;
      int linePos = 0;
      while (*p) {
        const char* word = p;
        while (*p && *p != ' ') p++;
        const int wordLen = (int)(p - word);
        if (wordLen <= 0) { p++; continue; }
        char slice[80];
        const int sliceLen = wordLen < (int)sizeof(slice) - 1 ? wordLen : (int)sizeof(slice) - 1;
        memcpy(slice, word, sliceLen);
        slice[sliceLen] = '\0';
        const int wordPx  = d.getStrWidth(slice);
        const int spacePx = (lineW > 0) ? d.getStrWidth(" ") : 0;
        if (lineW + spacePx + wordPx > valW && lineW > 0) {
          // word doesn't fit; defer to next line
          break;
        }
        if (linePos > 0 && linePos < (int)sizeof(line) - 1) {
          line[linePos++] = ' ';
        }
        const int copy = sliceLen < (int)sizeof(line) - 1 - linePos
                         ? sliceLen : (int)sizeof(line) - 1 - linePos;
        memcpy(line + linePos, slice, copy);
        linePos += copy;
        line[linePos] = '\0';
        lineW += spacePx + wordPx;
        while (*p == ' ') p++;
      }
      if (linePos == 0 && *p) {
        // Single word longer than valW — hard-cut a chunk so we make
        // forward progress instead of looping forever.
        const int hardCut = 12;
        const int copy = hardCut < (int)sizeof(line) - 1 ? hardCut : (int)sizeof(line) - 1;
        memcpy(line, p, copy);
        line[copy] = '\0';
        p += copy;
      }
      if (yCursor + kRowH > kCardTop && yCursor < kCardBottom) {
        if (firstLine) d.drawStr(contentX, yCursor + kRowH - 2, r.label);
        d.drawStr(valX, yCursor + kRowH - 2, line);
      }
      yCursor += kRowH;
      firstLine = false;
    }
    yCursor += kRowGapY;
  }

  // Scrollbar (only when content overflows).
  if (totalH > viewportH) {
    constexpr int kBarX = 124;
    constexpr int kBarW = 3;
    d.drawVLine(kBarX + 1, kCardTop, viewportH);
    const int thumbH = (viewportH * viewportH) / totalH;
    const int thumbY = kCardTop + (confirmedScrollPx_ * (viewportH - thumbH))
                                  / (maxScroll > 0 ? maxScroll : 1);
    d.drawBox(kBarX, thumbY, kBarW, thumbH < 4 ? 4 : thumbH);
  }
}
#endif  // Scrollable-card path

void BoopScreen::renderZiggy(oled& d, uint32_t nowMs) {
  // Look up the sprite by catalog name so we pick up any per-frame timing
  // arrays (kBlinkTimes, kSleepTimes) the catalog defines for it. That's
  // the same per-image animator AnimTestScreen uses.
  BadgeBoops::BoopPhase phase = BadgeBoops::boopStatus.phase;
  const char* name = "Ziggy Blink";
  uint32_t fallbackMs = 500;
  switch (phase) {
    case BadgeBoops::BOOP_PHASE_BEACONING:  name = "Ziggy Fly";    fallbackMs = 220; break;
    case BadgeBoops::BOOP_PHASE_EXCHANGE:   name = "Ziggy Wow";    fallbackMs = 300; break;
    case BadgeBoops::BOOP_PHASE_PAIRED_OK:  name = "Ziggy Heart";  fallbackMs = 400; break;
    case BadgeBoops::BOOP_PHASE_FAILED:
    case BadgeBoops::BOOP_PHASE_CANCELLED:  name = "Ziggy Sleep";  fallbackMs = 550; break;
    case BadgeBoops::BOOP_PHASE_IDLE:
    default:                                name = "Ziggy Blink";  fallbackMs = 500; break;
  }

  const ImageInfo* img = ImageScaler::find(name);
  if (!img || !img->data48 || img->frameCount == 0) return;

  if (ziggyFrame_ >= img->frameCount) ziggyFrame_ = 0;

  // Per-frame time, identical rule to AnimTestScreen: use the catalog
  // array if present, fall back to the phase default if not.
  uint32_t frameMs = (img->frameTimes != nullptr)
                     ? img->frameTimes[ziggyFrame_]
                     : fallbackMs;

  if (nowMs - lastZiggyStepMs_ >= frameMs) {
    lastZiggyStepMs_ = nowMs;
    ziggyFrame_ = (ziggyFrame_ + 1) % img->frameCount;
  }

  drawXBMFlippedX(d, kZiggyX, kZiggyY, kZiggySize, kZiggySize,
                  img->data48 + (size_t)ziggyFrame_ * img->stride48);
}

void BoopScreen::renderLog(oled& d, uint32_t nowMs) {
  d.drawHLine(0, kLogDividerY, 128);
  d.setFontPreset(FONT_TINY);
  BadgeBoops::BoopPhase phase = BadgeBoops::boopStatus.phase;
  const bool terminalPhase =
      (phase == BadgeBoops::BOOP_PHASE_PAIRED_OK  ||
       phase == BadgeBoops::BOOP_PHASE_FAILED     ||
       phase == BadgeBoops::BOOP_PHASE_CANCELLED);

  // Bottom line baseline sits on the screen edge; the button glyph
  // footer reserves y=54..63 when it takes over the log strip.
  constexpr int kBottomBaseY = OLEDLayout::kFooterBaseY;

  // Compute scroll offset: starts at kLogLineH when a new line arrives,
  // decays linearly to 0 over kLogAnimMs. So the newest line appears to
  // slide up from below, pushing older lines upward.
  int offset = 0;
  if (logCount_ > 0) {
    uint32_t age = nowMs - logAnimStartMs_;
    if (age < kLogAnimMs) {
      offset = kLogLineH
             - static_cast<int>((age * kLogLineH) / kLogAnimMs);
    }
  }

  // Nothing has happened yet — fall back to a single-line status readout
  // with a right-aligned nav hint. Same visual weight as a normal log line.
  if (logCount_ == 0) {
    const char* msg = BadgeBoops::boopStatus.statusMsg;
    if (!msg || !msg[0]) msg = BadgeBoops::statusShort(phase);
    d.drawStr(2, kBottomBaseY - kLogLineH, msg);
    return;
  }

  // Draw the 3 newest lines (fewer if we haven't accumulated that many).
  // line index kMaxLogLines-1 = newest, anchored at kBottomBaseY.
  for (uint8_t slot = 0; slot < kMaxLogLines; slot++) {
    const char* s = logLines_[kMaxLogLines - 1 - slot];
    if (!s[0]) continue;
    int y = kBottomBaseY - slot * kLogLineH + offset;
    if (y < kLogDividerY + 5) continue;  // above the divider — clipped
    if (y > 64) continue;
    d.drawStr(2, y, s);
  }

  (void)terminalPhase;
}

void BoopScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);

  uint32_t nowMs = millis();
  poll(nowMs);

  const bool confirmedPeer =
      BadgeBoops::boopStatus.phase == BadgeBoops::BOOP_PHASE_PAIRED_OK &&
      BadgeBoops::boopStatus.boopType == BOOP_PEER;

  // ── Standard chrome: status header + nav footer matches the rest
  //    of the badge UI.
  OLEDLayout::drawStatusHeader(d, "Boop");

  // ── Lower-left: mirror-flipped ziggy avatar, bottom row touching
  //    the footer. Confirmed-peer reveal uses the full width as a
  //    contact-card and skips the sprite.
  if (!confirmedPeer) {
    renderZiggy(d, nowMs);
    renderActiveAtmospherics(d, nowMs);
  }

  // ── Right: handler-provided content or idle prompt, in the strip
  //    to the right of the sprite.
  const BadgeBoops::BoopHandlerOps* h =
      BadgeBoops::handlerFor(BadgeBoops::boopStatus.boopType);
  if (BadgeBoops::boopStatus.phase == BadgeBoops::BOOP_PHASE_IDLE) {
    d.setFontPreset(FONT_SMALL);
    const char* prompt = BadgeBoops::boopEngaged ? "starting..." : "ready";
    d.drawStr(kLeftX, kLeftY + 8, prompt);
    d.setFontPreset(FONT_TINY);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d contacts",
             BadgeBoops::countUniqueActivePairings());
    d.drawStr(kLeftX, kLeftY + 20, buf);
    // Action hints live in the shared footer so the top-button boop
    // prompt and back prompt stay consistent with other screens.
  } else {
    // Confirmed-peer reveal uses the full screen width as a contact
    // card (skips the sprite via the if(!confirmedPeer) above), with
    // the sparkly wipe overlay layered on top.
    h->drawContent(d, BadgeBoops::boopStatus, kLeftX, kLeftY,
                   confirmedPeer ? 128 : kLeftW, kLeftH);
    if (confirmedPeer) {
      renderConfirmedReveal(d, nowMs);
    }
  }

  if (!confirmedPeer) {
    renderTopTxBand(d, nowMs);
  }

  const bool inProgress =
      (BadgeBoops::boopStatus.phase == BadgeBoops::BOOP_PHASE_BEACONING ||
       BadgeBoops::boopStatus.phase == BadgeBoops::BOOP_PHASE_EXCHANGE);

  // Footer actions use the shared swap-aware chrome for back/cancel, while
  // boop itself is intentionally pinned to the physical top (Y) button.
  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawFooterActions(d, nullptr,
                                inProgress ? nullptr : "boop",
                                "back", nullptr);
}

void BoopScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                             int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  BadgeBoops::BoopPhase phase = BadgeBoops::boopStatus.phase;

  const bool terminalPhase =
      (phase == BadgeBoops::BOOP_PHASE_PAIRED_OK  ||
       phase == BadgeBoops::BOOP_PHASE_FAILED     ||
       phase == BadgeBoops::BOOP_PHASE_CANCELLED);

  // Physical top (Y) starts/restarts boops. Back/cancel follows the shared
  // semantic mapping so it stays consistent with the rest of the UI.
  if (terminalPhase) {
    if (e.rightPressed) {
      BadgeBoops::smReset();
      memset(txChips_,  0, sizeof(txChips_));
      memset(logLines_, 0, sizeof(logLines_));
      prevTxMask_          = 0;
      pendingTxEdges_      = 0;
      prevBeaconTxCount_   = 0;
      pendingBeaconTxChip_ = false;
      prevRxMask_          = 0;
      pendingRxEdges_      = 0;
      prevBeaconRxCount_   = 0;
      pendingBeaconRxChip_ = false;
      logCount_            = 0;
      logAnimStartMs_      = 0;
      BadgeBoops::boopEngaged = true;
      return;
    }
    if (e.downPressed) {
      BadgeBoops::smReset();
      gui.popScreen();
    }
    return;
  }

  if (e.yPressed && phase == BadgeBoops::BOOP_PHASE_IDLE) {
    BadgeBoops::boopEngaged = true;
    return;
  }
  if (e.cancelPressed) {
    if (phase == BadgeBoops::BOOP_PHASE_BEACONING ||
        phase == BadgeBoops::BOOP_PHASE_EXCHANGE) {
      BadgeBoops::pairingCancelRequested = true;
      return;
    }
    gui.popScreen();
    return;
  }
}
