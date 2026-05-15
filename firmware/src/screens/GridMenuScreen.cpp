#include "GridMenuScreen.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <esp_sleep.h>

#include "../BadgeGlobals.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/LEDmatrix.h"
#include "../hardware/oled.h"
#include "../ui/AppIcons.h"
#include "../ui/BadgeDisplay.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/UIFonts.h"

extern LEDmatrix badgeMatrix;

extern BadgeState badgeState;
extern void firstNameFromBadgeName(char* out, size_t outCap);

// Brings up the brownout-sensitive peripherals (haptics, IMU, LED
// matrix, IR task) on first entry to the home grid menu. Defined in
// main.cpp; the function self-guards against repeat calls.
extern "C" void initDeferredPeripherals();

namespace {
constexpr uint8_t kCols = 2;
constexpr uint8_t kRows = 2;
constexpr uint8_t kCellW = 62;
constexpr uint8_t kCellH = 18;
constexpr uint8_t kCellPadX = 3;
constexpr uint8_t kCellIconX = 1;
constexpr uint8_t kGapX = 4;
constexpr uint8_t kGapY = 4;
constexpr uint8_t kRowStride = kCellH + kGapY;
constexpr uint8_t kGridX = 0;
constexpr uint8_t kGridY = 11;   // -1 px from the prior 12 — pulls the icon grid up
                                  // the new centered time pill in the
                                  // shared status header.
constexpr uint8_t kCellRadius = 3;
constexpr uint8_t kFooterSepY = OLEDLayout::kFooterTopY;
constexpr uint8_t kFooterBaseY = OLEDLayout::kFooterBaseY;
constexpr uint16_t kJoyDeadband = 500;
constexpr uint16_t kScrollAnimMs = 140;
constexpr uint16_t kLabelScrollMs = 45;
constexpr uint16_t kBadgePulseMs = 180;
constexpr uint16_t kNavPulseMs = 12;
constexpr uint8_t kBadgeH = 8;
constexpr uint8_t kBadgePadX = 1;
constexpr uint8_t kLabelGapX = 3;
constexpr uint8_t kLabelScrollGapPx = 10;

// Hold-to-shutdown — DOWN button on the home menu only.
//   kBtnDownIndex   matches Inputs::kDown (private enum); see Inputs.h.
//   kShutdownArmMs  small floor so a stray DOWN tap doesn't flash the
//                   countdown; user has to be holding deliberately.
//   kShutdownHoldMs total hold required to actually deep-sleep.
constexpr uint8_t  kBtnDownIndex   = 1;
constexpr uint32_t kShutdownArmMs  = 250;
constexpr uint32_t kShutdownHoldMs = 3000;

void drawNotificationBadge(oled& d, int x, int y, uint16_t count,
                           bool selected, uint8_t pulsePx) {
  if (count == 0) return;

  char text[4] = {};
  if (count > 99) std::snprintf(text, sizeof(text), "99+");
  else std::snprintf(text, sizeof(text), "%u", count);

  d.setFont(UIFonts::kText);
  const int textW = d.getStrWidth(text);
  const int badgeW = textW + kBadgePadX * 2;
  const uint8_t fill = selected ? 0 : 1;
  const uint8_t textColor = selected ? 1 : 0;
  const int drawX = x - pulsePx;
  const int drawY = y - pulsePx;
  const int drawW = badgeW + pulsePx * 2;
  const int drawH = kBadgeH + pulsePx * 2;

  d.setDrawColor(fill);
  d.drawRBox(drawX, drawY, drawW, drawH, 2);
  d.setDrawColor(textColor);
  d.drawStr(x + kBadgePadX, y + 6, text);
}

uint8_t smoothstep255(uint32_t elapsedMs, uint32_t durationMs) {
  if (elapsedMs >= durationMs) return 255;
  const uint32_t t = (elapsedMs * 255UL) / durationMs;
  return (t * t * (3UL * 255UL - 2UL * t)) / (255UL * 255UL);
}

}  // namespace

GridMenuScreen::GridMenuScreen(ScreenId sid, const char* title,
                               const GridMenuItem* items, uint8_t count)
    : sid_(sid), title_(title), items_(items), count_(count) {}

void GridMenuScreen::setItems(const GridMenuItem* items, uint8_t count) {
  items_ = items;
  count_ = count;
  visibleCacheValid_ = false;
  cursor_ = 0;
  topRow_ = 0;
  scrollAnimating_ = false;
}

void GridMenuScreen::onEnter(GUIManager& gui) {
  (void)gui;
  if (sid_ == kScreenMainMenu) {
    unloadNametagAnimationDoc();
  }

  // First time the home grid is reached, bring up the
  // brownout-sensitive peripherals deferred from setup() (haptics,
  // IMU, LED matrix, IR task) and restore user-preferred OLED
  // contrast. Subsequent entries are no-ops thanks to the function's
  // internal one-shot guard.
  initDeferredPeripherals();

  visibleCacheValid_ = false;
  cursor_ = 0;
  topRow_ = 0;
  scrollAnimating_ = false;
  scrollAnimStartMs_ = 0;
  scrollFromPx_ = 0;
  scrollToPx_ = 0;
  joyRamp_.reset();
}

void GridMenuScreen::rebuildVisibleCache() const {
  visibleCount_ = 0;
  const uint8_t cacheCap = sizeof(visibleIndices_) / sizeof(visibleIndices_[0]);
  for (uint8_t i = 0; i < count_ && visibleCount_ < cacheCap; i++) {
    if (items_[i].visible && !items_[i].visible()) continue;
    visibleIndices_[visibleCount_++] = i;
  }
  visibleCacheValid_ = true;
}

void GridMenuScreen::ensureVisibleCache() const {
  if (!visibleCacheValid_) rebuildVisibleCache();
}

const GridMenuItem* GridMenuScreen::itemAtVisibleIndex(uint8_t index) const {
  ensureVisibleCache();
  if (index >= visibleCount_) return nullptr;
  return &items_[visibleIndices_[index]];
}

uint8_t GridMenuScreen::visibleCount() const {
  ensureVisibleCache();
  return visibleCount_;
}

uint8_t GridMenuScreen::rowCount() const {
  const uint8_t count = visibleCount();
  return count == 0 ? 1 : (count + kCols - 1) / kCols;
}

uint8_t GridMenuScreen::maxTopRow() const {
  const uint8_t rows = rowCount();
  return rows > kRows ? rows - kRows : 0;
}

uint8_t GridMenuScreen::targetTopRowForSelection() const {
  const uint8_t selectedRow = cursor_ / kCols;
  const uint8_t maxTop = maxTopRow();
  return selectedRow > maxTop ? maxTop : selectedRow;
}

int16_t GridMenuScreen::currentScrollPx() const {
  if (!scrollAnimating_) return scrollToPx_;

  const uint32_t elapsed = millis() - scrollAnimStartMs_;
  const uint8_t eased = smoothstep255(elapsed, kScrollAnimMs);
  const int16_t delta = scrollToPx_ - scrollFromPx_;
  return scrollFromPx_ +
         static_cast<int16_t>((static_cast<int32_t>(delta) * eased) / 255);
}

void GridMenuScreen::syncScrollToSelection(bool animate) {
  const uint8_t targetTop = targetTopRowForSelection();
  const int16_t targetPx = targetTop * kRowStride;
  topRow_ = targetTop;

  if (!animate) {
    scrollAnimating_ = false;
    scrollFromPx_ = targetPx;
    scrollToPx_ = targetPx;
    return;
  }

  const int16_t fromPx = currentScrollPx();
  scrollFromPx_ = fromPx;
  scrollToPx_ = targetPx;
  scrollAnimStartMs_ = millis();
  scrollAnimating_ = fromPx != targetPx;
}

void GridMenuScreen::finishScrollIfNeeded() {
  if (!scrollAnimating_) return;
  if (millis() - scrollAnimStartMs_ < kScrollAnimMs) return;

  scrollAnimating_ = false;
  scrollFromPx_ = scrollToPx_;
}

void GridMenuScreen::clampSelection() {
  const uint8_t count = visibleCount();
  const uint8_t oldCursor = cursor_;
  if (count == 0) {
    cursor_ = 0;
    syncScrollToSelection(false);
    return;
  }
  if (cursor_ >= count) cursor_ = count - 1;
  if (oldCursor != cursor_ || topRow_ != targetTopRowForSelection()) {
    syncScrollToSelection(false);
  }
}

void GridMenuScreen::formatLabel(const GridMenuItem& item, char* buf,
                                 uint8_t bufSize) const {
  if (!buf || bufSize == 0) return;
  if (item.labelFn) {
    item.labelFn(buf, bufSize);
  } else {
    std::snprintf(buf, bufSize, "%s", item.label ? item.label : "");
  }
}

// ── Public OLEDLayout helpers ──────────────────────────────────────────────
// These live here (instead of OLEDLayout.cpp) because they need the
// `kCellW`, `kCellH`, `kGridX`, `kGridY` constants from this TU's
// anonymous namespace. They expose the same cell geometry the home grid
// uses so MicroPython apps can draw native-looking menus without
// pixel-pushing in Python.

namespace OLEDLayout {

void drawGridCell(oled& d, uint8_t col, uint8_t row,
                   const char* label, bool selected,
                   const uint8_t* iconData, uint8_t iconW, uint8_t iconH) {
  if (col >= kCols || row >= kRows) return;
  const int x = kGridX + col * (kCellW + kGapX);
  const int y = kGridY + row * kRowStride;

  if (iconData != nullptr && (iconW == 0 || iconH == 0)) {
    iconW = AppIcons::kW;
    iconH = AppIcons::kH;
  }

  // Cell shape — same draw sequence the native drawCell uses.
  if (selected) {
    d.setDrawColor(1);
    d.drawRBox(x, y, kCellW, kCellH, kCellRadius);
    d.setDrawColor(0);
  } else if (iconData != nullptr) {
    // Inverted backing strip behind the icon, matching the native chrome
    // — gives the icon a slight "knockout" look against the outline.
    const int iconFillH = iconH + 4 <= kCellH ? iconH + 4 : kCellH;
    const int iconFillX = x + kCellIconX - 1;
    const int iconFillY = y + (kCellH - iconFillH) / 2;
    const int iconFillW = iconW + 2;
    d.setDrawColor(1);
    d.drawBox(iconFillX, iconFillY, iconFillW, iconFillH);
    d.setDrawColor(0);
    for (int dr = 0; dr < kCellRadius; dr++) {
      for (int dc = 0; dc < kCellRadius - dr; dc++) {
        d.drawPixel(x + dc, y + dr);
        d.drawPixel(x + dc, y + kCellH - 1 - dr);
      }
    }
    d.setDrawColor(1);
    d.drawRFrame(x, y, kCellW, kCellH, kCellRadius);
    d.setDrawColor(0);
  } else {
    // No icon — plain rounded outline, no inverted backing strip.
    d.setDrawColor(1);
    d.drawRFrame(x, y, kCellW, kCellH, kCellRadius);
    d.setDrawColor(0);
  }

  // Icon (if any).
  int labelLeftPx;
  if (iconData != nullptr) {
    const int iconX = x + kCellIconX;
    const int iconY = y + (kCellH - iconH) / 2;
    d.drawXBM(iconX, iconY, iconW, iconH, iconData);
    labelLeftPx = iconX + iconW + kLabelGapX;
  } else {
    labelLeftPx = x + kCellPadX;
  }

  // Label \u2014 right-aligned in the remaining width, single line, fitted
  // to the cell.  Native renderer also supports marquee scrolling; for
  // the MP-side helper we just clip + ellipsize, since rendering is
  // single-shot per call.
  d.setDrawColor(selected ? 0 : 1);
  d.setFont(UIFonts::kText);
  char buf[32] = {};
  if (label) {
    std::strncpy(buf, label, sizeof(buf) - 1);
  }
  const int labelW = x + kCellW - kCellPadX - labelLeftPx;
  fitText(d, buf, sizeof(buf), labelW);
  const int tw = d.getStrWidth(buf);
  const int labelBaseY = y + (kCellH + d.getAscent()) / 2;
  d.drawStr(x + kCellW - tw - kCellPadX, labelBaseY, buf);
  d.setDrawColor(1);
}

void drawGridFooter(oled& d, const char* description) {
  drawActionFooter(d, description ? description : "", "select");
}

}  // namespace OLEDLayout

void GridMenuScreen::drawHeader(oled& d) const {
  const char* title = title_;
  // Personalised "Hi <first>" greeting: shown whenever the badge is
  // activated (paired OR offline) — being temporarily out of WiFi range
  // shouldn't drop the user back to the generic "MENU" title.
  if (badgeIsActivated(badgeState)) {
    char first[20] = {};
    firstNameFromBadgeName(first, sizeof(first));
    if (first[0]) {
      std::snprintf(dynamicTitle_, sizeof(dynamicTitle_), "Hi %s", first);
      title = dynamicTitle_;
    }
  }
  OLEDLayout::drawStatusHeader(d, title);
}

void GridMenuScreen::drawCell(oled& d, uint8_t col, int y,
                              uint8_t visibleIndex, bool selected) const {
  const GridMenuItem* item = itemAtVisibleIndex(visibleIndex);
  if (!item) return;

  const int x = kGridX + col * (kCellW + kGapX);
  const uint8_t iconW = item->iconW ? item->iconW : AppIcons::kW;
  const uint8_t iconH = item->iconH ? item->iconH : AppIcons::kH;
  const int iconX = x + kCellIconX;
  const int iconY = y + (kCellH - iconH) / 2;
  const int iconFillH = iconH + 4 <= kCellH ? iconH + 4 : kCellH;
  const int iconFillX = iconX - 1;
  const int iconFillY = y + (kCellH - iconFillH) / 2;
  const int iconFillW = iconW + 2;

  if (selected) {
    d.setDrawColor(1);
    d.drawRBox(x, y, kCellW, kCellH, kCellRadius);
    d.setDrawColor(0);
  } else {
    d.setDrawColor(1);
    d.drawBox(iconFillX, iconFillY, iconFillW, iconFillH);
    d.setDrawColor(0);
    for (int dr = 0; dr < kCellRadius; dr++) {
      for (int dc = 0; dc < kCellRadius - dr; dc++) {
        d.drawPixel(x + dc, y + dr);
        d.drawPixel(x + dc, y + kCellH - 1 - dr);
      }
    }
    d.setDrawColor(1);
    d.drawRFrame(x, y, kCellW, kCellH, kCellRadius);
    d.setDrawColor(0);
  }

  const uint8_t* icon = item->icon ? item->icon : AppIcons::apps;
  if (item->iconWhiteOnBlack) {
    d.setDrawColor(0);
    d.drawBox(iconX, iconY, iconW, iconH);
    d.setDrawColor(1);
  }
  d.drawXBM(iconX, iconY, iconW, iconH, icon);

  const uint16_t badgeCount = item->badgeFn ? item->badgeFn() : 0;
  uint8_t badgePulsePx = 0;
  if (visibleIndex < visibleCount_) {
    const uint8_t itemSlot = visibleIndices_[visibleIndex];
    const bool wasVisible = badgeSeenFrame_[itemSlot] + 1 == badgeRenderFrame_;
    if (!badgeCountsValid_[itemSlot] || !wasVisible) {
      badgeCounts_[itemSlot] = badgeCount;
      badgeCountsValid_[itemSlot] = true;
      badgePulseStartMs_[itemSlot] = 0;
    } else if (badgeCounts_[itemSlot] != badgeCount) {
      badgeCounts_[itemSlot] = badgeCount;
      badgePulseStartMs_[itemSlot] = millis();
    }
    badgeSeenFrame_[itemSlot] = badgeRenderFrame_;

    if (badgeCount > 0 && badgePulseStartMs_[itemSlot] != 0) {
      const uint32_t elapsed = millis() - badgePulseStartMs_[itemSlot];
      if (elapsed < kBadgePulseMs) badgePulsePx = 1;
    }
  }
  drawNotificationBadge(d, iconX + iconW - 4, y + 1, badgeCount, selected,
                        badgePulsePx);

  d.setDrawColor(selected ? 0 : 1);
  d.setFont(UIFonts::kText);
  char label[32] = {};
  formatLabel(*item, label, sizeof(label));
  const int labelX = iconX + iconW + kLabelGapX;
  const int labelBaseY = y + (kCellH + d.getAscent()) / 2;
  const int labelTextW = d.getStrWidth(label);
  const int labelW = x + kCellW - kCellPadX - labelX;

  if (labelTextW > labelW) {
    const int period = labelTextW + kLabelScrollGapPx;
    const int labelScrollX =
        -static_cast<int>((millis() / kLabelScrollMs) % period);
    const int clipY0 = y < kGridY ? kGridY : y;
    const int clipY1 =
        y + kCellH > kFooterSepY ? kFooterSepY : y + kCellH;
    if (clipY1 > clipY0) {
      d.setClipWindow(labelX, clipY0, labelX + labelW, clipY1);
      d.drawStr(labelX + labelScrollX, labelBaseY, label);
      const int duplicateX =
          labelX + labelScrollX + labelTextW + kLabelScrollGapPx;
      if (duplicateX < labelX + labelW) {
        d.drawStr(duplicateX, labelBaseY, label);
      }
      d.setClipWindow(0, kGridY, 128, kFooterSepY);
    }
  } else {
    OLEDLayout::fitText(d, label, sizeof(label), labelW);
    const int tw = d.getStrWidth(label);
    d.drawStr(x + kCellW - tw - kCellPadX, labelBaseY, label);
  }
  d.setDrawColor(1);
}

void GridMenuScreen::drawFooter(oled& d) {
  // Hold-DOWN-to-shutdown takes over the footer once the timer is armed.
  // Show whole-second remaining (rounded up) so the user sees 3 → 2 → 1
  // before the chip actually sleeps.
  if (shutdownHeldMs_ >= kShutdownArmMs) {
    const uint32_t remaining = (shutdownHeldMs_ < kShutdownHoldMs)
        ? (kShutdownHoldMs - shutdownHeldMs_)
        : 0;
    const uint32_t secs = (remaining + 999) / 1000;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "Shutdown in %lu...",
                  static_cast<unsigned long>(secs));
    OLEDLayout::drawActionFooter(d, buf, "");
    return;
  }
  const GridMenuItem* item = itemAtVisibleIndex(cursor_);
  const char* text = item ? item->description : "";
  if (!text || !text[0]) text = item && item->label ? item->label : "";
  OLEDLayout::drawActionFooter(d, text, "select");
}

namespace {
// Power-down sequence: blank the user-visible peripherals so the device
// looks "off", then deep-sleep. esp_deep_sleep_start() never returns.
// Wake config is whatever the SleepService set up earlier (typically
// IMU motion on INT_GP_PIN); if no source is enabled the chip stays
// asleep until the user power-cycles, which matches "shutdown" semantics.
[[noreturn]] void performShutdown(oled& d) {
#ifdef BADGE_HAS_LED_MATRIX
  badgeMatrix.stopAnimation();
  badgeMatrix.clear(0);
#endif
  d.clearBuffer();
  d.sendBuffer();
  d.transitionOut(150);
  delay(50);
  Serial.flush();
  esp_deep_sleep_start();
  // Unreachable; loop guards against the off-chance esp_deep_sleep_start
  // returns (it does not in practice) so we don't fall through into
  // post-sleep state with the screen blanked.
  while (true) {}
}
}  // namespace

void GridMenuScreen::render(oled& d, GUIManager& gui) {
  (void)gui;
  ensureVisibleCache();
  clampSelection();
  finishScrollIfNeeded();

  d.setTextWrap(false);
  d.setDrawColor(1);
  drawHeader(d);

  badgeRenderFrame_++;
  if (badgeRenderFrame_ == 0) {
    badgeRenderFrame_ = 1;
    std::memset(badgeSeenFrame_, 0, sizeof(badgeSeenFrame_));
    std::memset(badgeCountsValid_, 0, sizeof(badgeCountsValid_));
  }

  const int16_t scrollPx = currentScrollPx();
  d.setClipWindow(0, kGridY, 128, kFooterSepY);
  const uint8_t rows = rowCount();
  for (uint8_t row = 0; row < rows; row++) {
    const int y = kGridY + row * kRowStride - scrollPx;
    if (y + kCellH < kGridY || y >= kFooterSepY) continue;
    for (uint8_t col = 0; col < kCols; col++) {
      const uint8_t idx = row * kCols + col;
      if (idx >= visibleCount()) break;
      drawCell(d, col, y, idx, idx == cursor_);
    }
  }
  d.setMaxClipWindow();

  drawFooter(d);

  // Hold-DOWN-to-shutdown: trigger AFTER the footer has rendered the
  // final "Shutdown in 0..." frame and that frame has been pushed to
  // the panel. Doing it here (post-drawFooter, post-loop-flush by
  // GUIManager) means the user sees 3 → 2 → 1 → 0 before deep sleep.
  if (sid_ == kScreenMainMenu && shutdownHeldMs_ >= kShutdownHoldMs) {
    performShutdown(d);
  }
}

void GridMenuScreen::moveSelection(int8_t dir) {
  const uint8_t count = visibleCount();
  if (count == 0) return;

  const uint8_t old = cursor_;

  if (dir == 1) {
    if (cursor_ + 1 < count) cursor_++;
  } else if (dir == -1) {
    if (cursor_ > 0) cursor_--;
  } else if (dir == 2) {
    const uint8_t sameColBelow = cursor_ + kCols;
    if (sameColBelow < count) {
      cursor_ = sameColBelow;
    } else {
      const uint8_t leftBelow = (cursor_ / kCols + 1) * kCols;
      if (leftBelow < count) cursor_ = leftBelow;
    }
  } else if (dir == -2) {
    if (cursor_ >= kCols) cursor_ -= kCols;
  }

  if (cursor_ != old) {
    const uint8_t navStrength =
        (static_cast<uint16_t>(Haptics::strength()) *
         Haptics::repeatStrengthPercent()) /
        100;
    Haptics::shortPulse(navStrength, kNavPulseMs);
    syncScrollToSelection(true);
  }
}

void GridMenuScreen::selectCurrent(GUIManager& gui) {
  const GridMenuItem* item = itemAtVisibleIndex(cursor_);
  if (!item) return;
  Haptics::shortPulse();
  if (item->action) {
    item->action(gui);
  } else if (item->target != kScreenNone) {
    gui.pushScreen(item->target);
  }
}

void GridMenuScreen::handleInput(const Inputs& inputs, int16_t /*cursorX*/,
                                 int16_t /*cursorY*/, GUIManager& gui) {
  ensureVisibleCache();
  clampSelection();

  // Hold-DOWN-to-shutdown — only on the actual home menu. While the
  // timer is armed (DOWN held past kShutdownArmMs) we keep requesting
  // re-renders so the seconds-remaining footer counts down even
  // without further input. Submenus get 0 (= disabled).
  shutdownHeldMs_ = (sid_ == kScreenMainMenu)
      ? inputs.heldMs(kBtnDownIndex)
      : 0;
  if (shutdownHeldMs_ >= kShutdownArmMs) {
    gui.requestRender();
  }

  const Inputs::ButtonEdges& e = inputs.edges();
  if (e.confirmPressed) {
    selectCurrent(gui);
    return;
  }
  if (e.cancelPressed) {
    // No-op at the root (popScreen() guards against stackDepth <= 1), so
    // submenus like Games escape while the top-level menu stays put.
    gui.popScreen();
    return;
  }

  const int16_t dx = static_cast<int16_t>(inputs.joyX()) - 2047;
  const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
  int8_t dir = 0;
  if (abs(dx) > abs(dy)) {
    if (dx > kJoyDeadband) dir = 1;
    else if (dx < -kJoyDeadband) dir = -1;
  } else {
    if (dy > kJoyDeadband) dir = 2;
    else if (dy < -kJoyDeadband) dir = -2;
  }

  if (joyRamp_.tick(dir, millis())) {
    moveSelection(dir);
  }
}
