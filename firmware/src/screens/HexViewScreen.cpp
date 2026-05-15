#include "HexViewScreen.h"

#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

HexViewScreen::HexViewScreen() = default;

HexViewScreen::~HexViewScreen() {
  if (window_) free(window_);
}

bool HexViewScreen::ensureWindow() {
  if (window_) return true;
  window_ = static_cast<uint8_t*>(BadgeMemory::allocPreferPsram(kWindowBytes));
  return window_ != nullptr;
}

void HexViewScreen::loadFile(const char* path) {
  scrollOffset_ = 0;
  windowStart_ = 0;
  windowLen_ = 0;
  fileSize_ = 0;
  std::strncpy(path_, path ? path : "", sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';

  Filesystem::IOLock lock;
  FATFS* fs = replay_get_fatfs();
  if (!fs || !path_[0]) return;
  FILINFO fno;
  if (f_stat(fs, path_, &fno) == FR_OK) {
    fileSize_ = static_cast<uint32_t>(fno.fsize);
  }
  refillWindow(0);
}

void HexViewScreen::refillWindow(uint32_t start) {
  if (!ensureWindow()) {
    windowLen_ = 0;
    return;
  }
  if (start >= fileSize_) {
    windowStart_ = start;
    windowLen_ = 0;
    return;
  }
  Filesystem::IOLock lock;
  FATFS* fs = replay_get_fatfs();
  if (!fs) {
    windowLen_ = 0;
    return;
  }
  FIL fil;
  if (f_open(fs, &fil, path_, FA_READ) != FR_OK) {
    windowLen_ = 0;
    return;
  }
  if (f_lseek(&fil, start) != FR_OK) {
    f_close(&fil);
    windowLen_ = 0;
    return;
  }
  uint32_t want = fileSize_ - start;
  if (want > kWindowBytes) want = kWindowBytes;
  UINT got = 0;
  f_read(&fil, window_, want, &got);
  f_close(&fil);
  windowStart_ = start;
  windowLen_ = got;
}

void HexViewScreen::onEnter(GUIManager& /*gui*/) { lastJoyMs_ = 0; }

void HexViewScreen::onExit(GUIManager& /*gui*/) {
  if (window_) {
    free(window_);
    window_ = nullptr;
  }
  windowLen_ = 0;
  windowStart_ = 0;
  fileSize_ = 0;
  path_[0] = '\0';
}

void HexViewScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);
  d.setFontPreset(FONT_TINY);
  d.setTextWrap(false);

  // Header.
  const char* name = path_;
  const char* slash = std::strrchr(name, '/');
  if (slash && slash[1]) name = slash + 1;
  d.drawStr(0, 7, name);

  char right[20];
  std::snprintf(right, sizeof(right), "%lu/%lu",
                (unsigned long)scrollOffset_, (unsigned long)fileSize_);
  int rw = d.getStrWidth(right);
  d.drawStr(128 - rw, 7, right);
  d.drawHLine(0, 8, 128);

  // Make sure the visible window is loaded.
  uint32_t needBytes = kBytesPerRow * kVisibleRows;
  if (scrollOffset_ < windowStart_ ||
      scrollOffset_ + needBytes > windowStart_ + windowLen_ ||
      windowLen_ == 0) {
    // Center the next window slightly behind the cursor so back-scroll
    // doesn't trigger immediate refill.
    uint32_t newStart = scrollOffset_;
    if (newStart > kWindowBytes / 4) newStart -= kWindowBytes / 4;
    refillWindow(newStart);
  }

  // Body rows.
  const int rowH = 7;
  for (uint8_t r = 0; r < kVisibleRows; r++) {
    uint32_t rowOffset = scrollOffset_ + r * kBytesPerRow;
    if (rowOffset >= fileSize_) break;
    int y = 10 + r * rowH + 6;
    char line[40];
    int pos = std::snprintf(line, sizeof(line), "%05lX",
                            (unsigned long)rowOffset);

    char ascii[kBytesPerRow + 1];
    uint8_t actual = 0;
    for (uint8_t b = 0; b < kBytesPerRow; b++) {
      uint32_t off = rowOffset + b;
      if (off >= fileSize_) {
        pos += std::snprintf(line + pos, sizeof(line) - pos, "   ");
        continue;
      }
      uint32_t winIdx = off - windowStart_;
      uint8_t v = (winIdx < windowLen_) ? window_[winIdx] : 0;
      pos += std::snprintf(line + pos, sizeof(line) - pos, "%02X", v);
      ascii[actual++] = (v >= 0x20 && v < 0x7F) ? (char)v : '.';
    }
    ascii[actual] = '\0';

    // Draw hex column at x=0, ASCII column at right edge so they stay
    // aligned.
    d.drawStr(0, y, line);
    int aw = d.getStrWidth(ascii);
    d.drawStr(128 - aw, y, ascii);
  }

  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawFooterActions(d, "pgup", "pgdn", "esc", nullptr);
}

void HexViewScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }

  uint32_t step = kBytesPerRow;
  uint32_t page = kBytesPerRow * kVisibleRows;

  if (e.upPressed) {
    if (scrollOffset_ >= step) scrollOffset_ -= step;
    else scrollOffset_ = 0;
  }
  if (e.downPressed) {
    uint32_t maxScroll = (fileSize_ > page) ? fileSize_ - page : 0;
    scrollOffset_ += step;
    if (scrollOffset_ > maxScroll) scrollOffset_ = maxScroll;
  }
  if (e.xPressed) {  // page-up
    if (scrollOffset_ >= page) scrollOffset_ -= page;
    else scrollOffset_ = 0;
  }
  if (e.yPressed) {  // page-down
    uint32_t maxScroll = (fileSize_ > page) ? fileSize_ - page : 0;
    scrollOffset_ += page;
    if (scrollOffset_ > maxScroll) scrollOffset_ = maxScroll;
  }

  uint32_t now = millis();
  int16_t jy = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(jy) > 600) {
    uint32_t repeat = abs(jy) > 1500 ? 60 : 160;
    if (lastJoyMs_ == 0 || now - lastJoyMs_ >= repeat) {
      lastJoyMs_ = now;
      uint32_t maxScroll = (fileSize_ > page) ? fileSize_ - page : 0;
      if (jy > 0) {
        scrollOffset_ += step;
        if (scrollOffset_ > maxScroll) scrollOffset_ = maxScroll;
      } else {
        if (scrollOffset_ >= step) scrollOffset_ -= step;
        else scrollOffset_ = 0;
      }
    }
  } else {
    lastJoyMs_ = 0;
  }
}
