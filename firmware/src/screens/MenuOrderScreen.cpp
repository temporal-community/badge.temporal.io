#include "MenuOrderScreen.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "../apps/MenuOrderStore.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/UIFonts.h"
#include "GridMenuScreen.h"

extern "C" const GridMenuItem* mainMenuSnapshot(uint8_t* outCount);
extern "C" void rebuildMainMenuFromRegistry(void);

namespace {
constexpr uint8_t kRowHeight = 9;
constexpr uint8_t kContentY = OLEDLayout::kContentTopY;
constexpr uint16_t kJoyDeadband = 600;
constexpr uint32_t kJoyRepeatMs = 200;
}  // namespace

MenuOrderScreen::MenuOrderScreen() = default;

void MenuOrderScreen::onEnter(GUIManager& gui) {
  (void)gui;
  cursor_ = 0;
  scroll_ = 0;
  picked_ = false;
  lastJoyMs_ = 0;
  snapshotFromLiveMenu();
}

void MenuOrderScreen::snapshotFromLiveMenu() {
  uint8_t live = 0;
  const GridMenuItem* items = mainMenuSnapshot(&live);
  count_ = 0;
  if (!items) return;
  for (uint8_t i = 0; i < live && count_ < kMaxItems; i++) {
    if (!items[i].label) continue;
    std::strncpy(labels_[count_], items[i].label, kLabelCap - 1);
    labels_[count_][kLabelCap - 1] = '\0';
    count_++;
  }
}

void MenuOrderScreen::moveCursor(int8_t dir) {
  if (count_ == 0) return;
  int16_t next = static_cast<int16_t>(cursor_) + dir;
  if (next < 0) next = 0;
  if (next >= count_) next = count_ - 1;
  if (static_cast<uint8_t>(next) == cursor_) return;

  if (picked_) {
    swap(cursor_, static_cast<uint8_t>(next));
  }
  cursor_ = static_cast<uint8_t>(next);

  // Auto-scroll: keep at least one row of context above and below.
  const uint8_t visibleRows =
      (OLEDLayout::kFooterTopY - kContentY) / kRowHeight;
  if (cursor_ < scroll_) {
    scroll_ = cursor_;
  } else if (cursor_ >= scroll_ + visibleRows) {
    scroll_ = static_cast<uint8_t>(cursor_ - visibleRows + 1);
  }
}

void MenuOrderScreen::swap(uint8_t a, uint8_t b) {
  if (a >= count_ || b >= count_ || a == b) return;
  char tmp[kLabelCap];
  std::memcpy(tmp, labels_[a], kLabelCap);
  std::memcpy(labels_[a], labels_[b], kLabelCap);
  std::memcpy(labels_[b], tmp, kLabelCap);
}

void MenuOrderScreen::commitAndExit(GUIManager& gui) {
  // Persist new positions: the row index becomes the override value.
  // The rebuilder reads these alongside curated/dynamic defaults and
  // stable-sorts. Numbers are spaced wide so future inserts (new apps,
  // new curated tiles) can land between user picks without bumping
  // existing overrides.
  for (uint8_t i = 0; i < count_; i++) {
    int16_t order = static_cast<int16_t>(i * 10);
    MenuOrderStore::put(labels_[i], order);
  }
  rebuildMainMenuFromRegistry();
  Haptics::shortPulse();
  gui.popScreen();
}

void MenuOrderScreen::render(oled& d, GUIManager& gui) {
  (void)gui;
  d.setTextWrap(false);
  d.setDrawColor(1);
  OLEDLayout::drawHeader(d, picked_ ? "Reorder (moving)" : "Reorder Menu",
                         nullptr);
  d.setFont(UIFonts::kText);

  if (count_ == 0) {
    d.drawStr(3, kContentY + d.getAscent() + 2, "(empty)");
  } else {
    const uint8_t visibleRows =
        (OLEDLayout::kFooterTopY - kContentY) / kRowHeight;
    for (uint8_t row = 0; row < visibleRows; row++) {
      const uint8_t idx = scroll_ + row;
      if (idx >= count_) break;
      const uint8_t y = kContentY + row * kRowHeight;
      const bool selected = (idx == cursor_);
      const bool highlighted = selected && picked_;

      // Picked-up rows render filled even when off-cursor; the cursor
      // row also fills as the standard list selection.
      if (selected) {
        OLEDLayout::drawSelectedRow(d, y, kRowHeight, /*x=*/0, /*w=*/123);
      }
      d.setDrawColor(selected ? 0 : 1);

      // 1) marker column: ▶ when picked-up & this is cursor row.
      const char* marker = highlighted ? ">>" : "  ";
      d.drawStr(3, y + d.getAscent() + 1, marker);

      // 2) numeric position.
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%2u  %s",
                    static_cast<unsigned>(idx + 1), labels_[idx]);
      d.drawStr(15, y + d.getAscent() + 1, buf);
      d.setDrawColor(1);
    }
  }

  OLEDLayout::drawGameFooter(d);
  if (picked_) {
    OLEDLayout::drawFooterActions(d, "drop",nullptr, "back", "save");
  } else {
    OLEDLayout::drawFooterActions(d, "pick", nullptr, "back", "save");
  }
}

void MenuOrderScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                  int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }
  if (e.confirmPressed) {
    commitAndExit(gui);
    return;
  }
  if (e.xPressed) {
    picked_ = !picked_;
    Haptics::shortPulse();
    return;
  }

  // Joystick Y navigates the cursor (and drags the picked row).
  const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (std::abs(dy) <= static_cast<int16_t>(kJoyDeadband)) {
    lastJoyMs_ = 0;
    return;
  }
  const uint32_t now = millis();
  if (lastJoyMs_ != 0 && now - lastJoyMs_ < kJoyRepeatMs) return;
  lastJoyMs_ = now;
  moveCursor(dy > 0 ? 1 : -1);
}
