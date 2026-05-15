#include "Screen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/UIFonts.h"

ListMenuScreen::ListMenuScreen(ScreenId sid, const char* title)
    : sid_(sid), title_(title) {}

void ListMenuScreen::onEnter(GUIManager& gui) {
  (void)gui;
  cursor_ = 0;
  scroll_ = 0;
  lastJoyNavMs_ = 0;
  lastJoyAdjustMs_ = 0;
}

uint8_t ListMenuScreen::computeRowHeight(oled& d) const {
  (void)d;
  return 9;
}

uint8_t ListMenuScreen::computeVisibleRows(oled& d) const {
  uint8_t rh = computeRowHeight(d);
  uint8_t rows = kContentHeight / rh;
  if (rows < 1) rows = 1;
  return rows;
}

void ListMenuScreen::drawItem(oled& d, uint8_t index, uint8_t y,
                              uint8_t /*rowHeight*/,
                              bool /*selected*/) const {
  char buf[32];
  formatItem(index, buf, sizeof(buf));
  while (buf[0] && d.getStrWidth(buf) > 120) {
    buf[std::strlen(buf) - 1] = '\0';
  }
  d.drawStr(3, y + d.getAscent() + 1, buf);
}

void ListMenuScreen::render(oled& d, GUIManager& gui) {
  onUpdate(gui);

  d.setTextWrap(false);
  d.setDrawColor(1);

  char rightText[24] = {};
  topRightText(rightText, sizeof(rightText));
  OLEDLayout::drawHeader(d, titleText(), rightText[0] ? rightText : nullptr);

  d.setFont(UIFonts::kText);

  const uint8_t rowHeight = computeRowHeight(d);
  const uint8_t visibleRows = computeVisibleRows(d);
  const uint8_t realCount = itemCount();
  const bool backRow = hasBackRow();
  const uint8_t totalCount = realCount + (backRow ? 1 : 0);

  if (totalCount == 0) {
    OLEDLayout::drawGameFooter(d);
    d.setFont(UIFonts::kText);
    if (const char* hint = hintText()) {
      OLEDLayout::drawFooterHint(d, hint);
    } else {
      OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "set");
    }
    return;
  }

  // Scrollbar geometry — 4-px-wide vertical track on the right edge,
  // always visible so the row width stays constant regardless of
  // total item count. Selection highlights stop short of the
  // scrollbar so the thumb stays readable when the cursor is on a
  // selected row. Track ends 2 px above the footer rule; the thumb
  // is fully inside the track frame (1 px inset).
  constexpr uint8_t kScrollbarW          = 4;
  constexpr uint8_t kScrollbarRightX     = 127;
  constexpr uint8_t kScrollbarLeftX      = kScrollbarRightX - kScrollbarW + 1;
  constexpr uint8_t kRowGapBeforeBar     = 1;
  constexpr uint8_t kScrollbarFooterPad  = 2;
  const uint8_t selRowW = kScrollbarLeftX - kRowGapBeforeBar;

  for (uint8_t i = 0; i < visibleRows && (scroll_ + i) < totalCount; i++) {
    uint8_t idx = scroll_ + i;
    uint8_t y = kContentY + i * rowHeight;

    const bool selected = (idx == cursor_);
    if (selected) {
      OLEDLayout::drawSelectedRow(d, y, rowHeight, /*x=*/0, /*w=*/selRowW);
    }
    // Selected rows render text in inverted (black) color so the
    // default drawItem implementation (which doesn't pass `selected`
    // through to its setDrawColor) doesn't paint white text on the
    // white highlight box. Subclass overrides that already manage
    // color themselves still see selected=true and can do whatever
    // they want; setting color here is a no-op for them.
    d.setDrawColor(selected ? 0 : 1);

    if (idx < realCount) {
      drawItem(d, idx, y, rowHeight, selected);
    } else {
      d.drawStr(3, y + d.getAscent() + 1, "Back");
    }
    d.setDrawColor(1);
  }

  // Scrollbar — vertical rounded-rect track + small filled rounded
  // rect thumb sized + positioned proportional to scroll/total.
  // Track height is clamped so the bottom edge sits at least
  // kScrollbarFooterPad px above the footer rule, leaving an empty
  // gap before the chips. Thumb is contained within the 1-px frame
  // inset and never spills below the track.
  {
    const int rawTrackH = visibleRows * rowHeight;
    const int maxTrackH = OLEDLayout::kFooterTopY - kContentY - kScrollbarFooterPad;
    const int trackH    = (rawTrackH > maxTrackH ? maxTrackH : rawTrackH);
    if (trackH > 4 && totalCount > 0) {
      d.setDrawColor(1);
      d.drawRFrame(kScrollbarLeftX, kContentY, kScrollbarW, trackH, 1);

      const int thumbAvailH = trackH - 4;  // 2-px inset top + bottom for a slightly shorter thumb
      int thumbH = (thumbAvailH * visibleRows + totalCount - 1) / totalCount;
      if (thumbH < 3)            thumbH = 3;
      if (thumbH > thumbAvailH)  thumbH = thumbAvailH;
      const int maxScrollTop = thumbAvailH - thumbH;
      const int scrollSpan   = (totalCount > visibleRows)
                                   ? (totalCount - visibleRows)
                                   : 1;
      int thumbY = kContentY + 2 + (maxScrollTop * scroll_) / scrollSpan;
      // Clamp defensively so any future scroll math drift can't
      // bleed the thumb below the track frame into the footer.
      const int thumbYMax = kContentY + 2 + maxScrollTop;
      if (thumbY > thumbYMax) thumbY = thumbYMax;
      d.drawRBox(kScrollbarLeftX + 1, thumbY,
                 kScrollbarW - 2, thumbH, 1);
    }
  }

  OLEDLayout::drawGameFooter(d);
  d.setFont(UIFonts::kText);
  if (const char* hint = hintText()) {
    OLEDLayout::drawFooterHint(d, hint);
  } else {
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "set");
  }
}

void ListMenuScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                 int16_t /*cy*/, GUIManager& gui) {
  onUpdate(gui);

  const Inputs::ButtonEdges& e = inputs.edges();
  const uint8_t realCount = itemCount();
  const bool backRow = hasBackRow();
  const uint8_t totalCount = realCount + (backRow ? 1 : 0);
  if (totalCount == 0) return;
  oled& d = gui.oledDisplay();
  d.setFont(UIFonts::kText);
  const uint8_t visibleRows = computeVisibleRows(d);

  auto moveCursor = [&](int8_t delta) {
    int16_t next = static_cast<int16_t>(cursor_) + delta;
    if (next < 0) next = 0;
    if (next >= totalCount) next = totalCount - 1;
    if (static_cast<uint8_t>(next) == cursor_) return;
    cursor_ = static_cast<uint8_t>(next);

    if (cursor_ == 0)
      scroll_ = 0;
    else if (cursor_ < scroll_ + 1)
      scroll_ = cursor_ - 1;

    if (cursor_ > scroll_ + visibleRows - 2)
      scroll_ = cursor_ - visibleRows + 2;

    uint8_t maxScroll =
        totalCount > visibleRows ? totalCount - visibleRows + 1 : 0;
    if (scroll_ > maxScroll) scroll_ = maxScroll;

    if (cursor_ < realCount) onItemFocus(cursor_, gui);
  };

  if (navigableItems()) {
    // Menu-style: semantic confirm selects, semantic cancel backs out.
    if (e.confirmPressed) {
      if (cursor_ >= realCount) {
        gui.popScreen();
      } else {
        onItemSelect(cursor_, gui);
      }
      return;
    }
    if (e.cancelPressed) {
      gui.popScreen();
      return;
    }
  } else {
    // Settings-style: semantic confirm selects, semantic cancel backs out.
    if (e.confirmPressed) {
      if (cursor_ >= realCount) {
        gui.popScreen();
        return;
      }
      onItemSelect(cursor_, gui);
      return;
    }
    if (e.cancelPressed) {
      gui.popScreen();
      return;
    }
  }

  uint32_t nowMs = millis();
  auto repeatMsFor = [](int16_t delta) -> uint32_t {
    int16_t magnitude = abs(delta);
    return magnitude > 1500 ? 80 : (magnitude > 900 ? 160 : 300);
  };
  int16_t joyDeltaY = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(joyDeltaY) > static_cast<int16_t>(kJoyDeadband)) {
    uint32_t repeatMs = repeatMsFor(joyDeltaY);
    if (lastJoyNavMs_ == 0 || nowMs - lastJoyNavMs_ >= repeatMs) {
      lastJoyNavMs_ = nowMs;
      moveCursor(joyDeltaY > 0 ? 1 : -1);
    }
  } else {
    lastJoyNavMs_ = 0;
  }

  if (!navigableItems() && cursor_ < realCount) {
    int16_t joyDeltaX = static_cast<int16_t>(inputs.joyX()) - 2047;
    if (abs(joyDeltaX) > static_cast<int16_t>(kJoyDeadband)) {
      uint32_t repeatMs = repeatMsFor(joyDeltaX);
      if (lastJoyAdjustMs_ == 0 || nowMs - lastJoyAdjustMs_ >= repeatMs) {
        lastJoyAdjustMs_ = nowMs;
        onItemAdjust(cursor_, joyDeltaX > 0 ? 1 : -1, gui);
      }
    } else {
      lastJoyAdjustMs_ = 0;
    }
  } else {
    lastJoyAdjustMs_ = 0;
  }
}
