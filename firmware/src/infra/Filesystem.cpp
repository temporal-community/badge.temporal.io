#include "Filesystem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BadgeConfig.h"
#include "PsramAllocator.h"
#include "hardware/Inputs.h"
#include "hardware/Power.h"
#include "hardware/oled.h"
#include "ui/OLEDLayout.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

extern SleepService sleepService;
extern Scheduler scheduler;

// ═══════════════════════════════════════════════════════════════════════════
//  Filesystem — central FATFS lock + atomic helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Recursive mutex so helpers can call each other under the lock without
// self-deadlocking (e.g. writeFileAtomic could in principle reuse
// removeFile in the future).  Lazily created on first IOLock() so we
// don't depend on a separate Filesystem::begin() being called early in
// boot.  All eight current journal modules can construct an IOLock
// before mpy_start has even mounted the FS — the lock is a pure
// FreeRTOS object and doesn't touch the FATFS driver.
SemaphoreHandle_t g_io_mutex = nullptr;

void ensureMutex() {
    // Note: this races on its very first call across cores, but
    // Journal begin() calls run synchronously from setup() on Core 1
    // before any other journal task is spawned, so the first
    // construction is always single-threaded.  Subsequent calls find
    // the mutex already created.
    if (g_io_mutex == nullptr) {
        g_io_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

}  // namespace

namespace Filesystem {

IOLock::IOLock() {
    ensureMutex();
    if (g_io_mutex) {
        xSemaphoreTakeRecursive(g_io_mutex, portMAX_DELAY);
    }
}

IOLock::~IOLock() {
    if (g_io_mutex) {
        xSemaphoreGiveRecursive(g_io_mutex);
    }
}

bool writeFileAtomic(const char* path, const void* data, size_t len) {
    if (!path || !path[0]) return false;
    if (len > 0 && data == nullptr) return false;

    IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) {
        Serial.printf("[FS] writeFileAtomic %s: no fatfs\n", path);
        return false;
    }

    // tmp path = "<path>.tmp" — bounded by FATFS max path length (512
    // chars in the LFN config).  64 chars covers every journal we have.
    char tmpPath[80];
    int n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmpPath)) {
        Serial.printf("[FS] writeFileAtomic %s: path too long\n", path);
        return false;
    }

    // Always nuke the tmp path first.  FA_CREATE_ALWAYS would also
    // overwrite, but f_unlink + FA_CREATE_NEW gives us a strict
    // "this file is brand-new" guarantee that's friendlier to the
    // directory-cluster bookkeeping under stress.
    f_unlink(fs, tmpPath);

    FIL fil;
    FRESULT ores = f_open(fs, &fil, tmpPath, FA_WRITE | FA_CREATE_NEW);
    if (ores != FR_OK) {
        Serial.printf("[FS] writeFileAtomic %s: open tmp fr=%d\n",
                      path, (int)ores);
        return false;
    }

    UINT written = 0;
    FRESULT wres = (len > 0)
        ? f_write(&fil, data, (UINT)len, &written)
        : FR_OK;
    FRESULT sres = f_sync(&fil);
    f_close(&fil);

    if (wres != FR_OK || written != (UINT)len) {
        Serial.printf("[FS] writeFileAtomic %s: write fr=%d sync=%d "
                      "wrote=%u/%u\n",
                      path, (int)wres, (int)sres,
                      (unsigned)written, (unsigned)len);
        f_unlink(fs, tmpPath);
        return false;
    }

    f_unlink(fs, path);
    FRESULT rres = f_rename(fs, tmpPath, path);
    if (rres != FR_OK) {
        Serial.printf("[FS] writeFileAtomic %s: rename fr=%d\n",
                      path, (int)rres);
        f_unlink(fs, tmpPath);
        return false;
    }
    return true;
}

bool readFileAlloc(const char* path, char** outBuf, size_t* outLen,
                   size_t maxLen) {
    if (outBuf) *outBuf = nullptr;
    if (outLen) *outLen = 0;
    if (!path || !path[0] || !outBuf || !outLen) return false;

    IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;

    FIL fil;
    if (f_open(fs, &fil, path, FA_READ) != FR_OK) return false;

    UINT fsize = f_size(&fil);
    if (fsize > maxLen) {
        f_close(&fil);
        Serial.printf("[FS] readFileAlloc %s: too large %u > %u\n",
                      path, (unsigned)fsize, (unsigned)maxLen);
        return false;
    }

    char* buf = (char*)BadgeMemory::allocPreferPsram((size_t)fsize + 1);
    if (!buf) {
        f_close(&fil);
        Serial.printf("[FS] readFileAlloc %s: alloc %u failed\n",
                      path, (unsigned)(fsize + 1));
        return false;
    }

    UINT bytesRead = 0;
    FRESULT res = (fsize > 0)
        ? f_read(&fil, buf, fsize, &bytesRead)
        : FR_OK;
    f_close(&fil);

    if (res != FR_OK || bytesRead != fsize) {
        free(buf);
        Serial.printf("[FS] readFileAlloc %s: read fr=%d got=%u/%u\n",
                      path, (int)res, (unsigned)bytesRead, (unsigned)fsize);
        return false;
    }

    buf[fsize] = '\0';
    *outBuf = buf;
    *outLen = (size_t)fsize;
    return true;
}

bool removeFile(const char* path) {
    if (!path || !path[0]) return false;
    IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;
    FRESULT r = f_unlink(fs, path);
    return (r == FR_OK || r == FR_NO_FILE);
}

int32_t fileSize(const char* path) {
    if (!path || !path[0]) return -1;
    IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return -1;
    FILINFO fno;
    if (f_stat(fs, path, &fno) != FR_OK) return -1;
    return (int32_t)fno.fsize;
}

bool fileExists(const char* path) {
    if (!path || !path[0]) return false;
    IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;
    FILINFO fno;
    return f_stat(fs, path, &fno) == FR_OK;
}

}  // namespace Filesystem

// ═══════════════════════════════════════════════════════════════════════════════
//  FileBrowser — OLED config editor + file viewer
// ═══════════════════════════════════════════════════════════════════════════════

void FileBrowser::begin(oled* display, const Inputs* inputs, Config* config) {
  display_ = display;
  inputs_ = inputs;
  config_ = config;
  state_ = State::Inactive;
}

const char* FileBrowser::name() const { return "FileBrowser"; }

void FileBrowser::service() {
  if (display_ == nullptr || inputs_ == nullptr) {
    return;
  }

  if (state_ == State::Inactive) {
    return;
  }

  const uint32_t nowMs = millis();

  // Deactivation: hold A+B.
  const Inputs::ButtonStates& b = inputs_->buttons();
  if (b.a && b.b && !b.y && !b.x) {
    if (activateHoldStartMs_ == 0) {
      activateHoldStartMs_ = nowMs;
    } else if (nowMs - activateHoldStartMs_ >= kActivateHoldMs) {
      deactivate();
      activateHoldStartMs_ = 0;
      return;
    }
  } else if (!(b.a && b.b)) {
    activateHoldStartMs_ = 0;
  }

  switch (state_) {
    case State::ConfigEdit:
      serviceConfigEdit(nowMs);
      break;
    case State::FileList:
      serviceFileList();
      break;
    case State::FileView:
      serviceFileView();
      break;
    default:
      break;
  }
}

void FileBrowser::activate() {
  scheduler.setServiceState("OLED", false);
  scheduler.setExecutionDivisors(1, 1, 1);
  cursor_ = 0;
  scroll_ = 0;
  lastJoyNavMs_ = 0;
  lastJoyAdjustMs_ = 0;
  state_ = State::ConfigEdit;

  display_->setFontPreset(FONT_TINY);
  display_->setTextWrap(false);

  renderConfigEdit();
  sleepService.caffeine = true;
}

void FileBrowser::deactivate() {
  if (config_ != nullptr) {
    config_->save();
  }
  state_ = State::Inactive;
  display_->applyHardwareFontState();
  scheduler.setExecutionDivisors(Power::Policy::schedulerHighDivisor,
                                 Power::Policy::schedulerNormalDivisor,
                                 Power::Policy::schedulerLowDivisor);
  scheduler.setServiceState("OLED", true);
}

// ─── Config Editor ───────────────────────────────────────────────────────────

void FileBrowser::serviceConfigEdit(uint32_t nowMs) {
  if (config_ == nullptr) {
    return;
  }

  const Inputs::ButtonEdges& e = inputs_->edges();
  const Inputs::ButtonStates& b = inputs_->buttons();
  if (b.a && b.b) {
    return;
  }

  const uint8_t count = Config::kCount;
  bool needsRedraw = false;

  auto openFileList = [&]() {
    scanDirectory("/");
    cursor_ = 0;
    scroll_ = 0;
    lastJoyNavMs_ = 0;
    state_ = State::FileList;
    renderFileList();
  };

  if (e.cancelPressed) {
    deactivate();
    return;
  }
  if (e.confirmPressed) {
    openFileList();
    return;
  }

  auto moveCursor = [&](int8_t delta) {
    if (delta < 0 && cursor_ > 0) {
      cursor_--;
      if (cursor_ < scroll_) {
        scroll_ = cursor_;
      }
      needsRedraw = true;
      sleepService.caffeine = true;
    } else if (delta > 0) {
      if (cursor_ + 1 < count) {
        cursor_++;
        if (cursor_ >= scroll_ + kVisibleRows) {
          scroll_ = cursor_ - kVisibleRows + 1;
        }
        needsRedraw = true;
        sleepService.caffeine = true;
      } else {
        openFileList();
      }
    }
  };

  const int16_t joyDeltaY = static_cast<int16_t>(inputs_->joyY()) - 2047;
  if (abs(joyDeltaY) > static_cast<int16_t>(kJoyDeadband)) {
    if (nowMs - lastJoyNavMs_ >= kJoyRepeatMs) {
      lastJoyNavMs_ = nowMs;
      moveCursor(joyDeltaY > 0 ? 1 : -1);
      if (state_ != State::ConfigEdit) return;
    }
  } else {
    lastJoyNavMs_ = 0;
  }

  // Joystick X-axis for fast analog adjustment.
  const int16_t joyDelta = static_cast<int16_t>(inputs_->joyX()) - 2047;
  if (abs(joyDelta) > static_cast<int16_t>(kJoyDeadband)) {
    if (nowMs - lastJoyAdjustMs_ >= kJoyRepeatMs) {
      lastJoyAdjustMs_ = nowMs;
      const int16_t absD = abs(joyDelta);
      const uint8_t magnitude = (absD > 1500) ? 5 : (absD > 900) ? 2 : 1;
      const int8_t dir = (joyDelta < 0) ? -1 : +1;
      config_->set(cursor_, Config::nextValue(cursor_, config_->get(cursor_),
                                              dir, magnitude));
      config_->apply(cursor_);
      needsRedraw = true;
      sleepService.caffeine = true;
    }
  } else {
    lastJoyAdjustMs_ = 0;
  }

  if (needsRedraw) {
    renderConfigEdit();
  }
}

void FileBrowser::renderConfigEdit() {
  display_->setFontPreset(FONT_TINY);
  display_->setTextWrap(false);
  display_->clearDisplay();

  display_->setCursor(0, 0);
  display_->print("SETTINGS");
  display_->drawHLine(0, 8, 128);

  const uint8_t count = Config::kCount;
  for (uint8_t i = 0; i < kVisibleRows && (scroll_ + i) < count; ++i) {
    const uint8_t idx = scroll_ + i;
    const uint8_t y = 10 + i * 8;
    display_->setCursor(0, y);
    display_->print(idx == cursor_ ? '>' : ' ');

    char line[22];
    if (idx == kFontFamily) {
      const uint8_t fam = static_cast<uint8_t>(config_->get(kFontFamily));
      const char* name = fam < kFontFamilyCount ? kFontFamilyNames[fam] : "?";
      std::snprintf(line, sizeof(line), "Font  %s", name);
    } else if (idx == kFontSize) {
      std::snprintf(line, sizeof(line), "Font Size    %ld",
                    (long)config_->get(idx));
    } else {
      std::snprintf(line, sizeof(line), "%-10s %5ld",
                    Config::kDefs[idx].label,
                    (long)config_->get(idx));
    }
    display_->print(line);
  }

  OLEDLayout::drawGameFooter(*display_);
  OLEDLayout::drawFooterActions(*display_, nullptr, nullptr, "exit", "files");
  display_->display();
}

// ─── File List (oofatfs) ─────────────────────────────────────────────────────

void FileBrowser::scanDirectory(const char* path) {
  fileCount_ = 0;

  Filesystem::IOLock fsLock;  // serialise vs other journal writers
  FATFS* fs = replay_get_fatfs();
  if (fs == nullptr) {
    return;
  }

  FF_DIR dir;
  FILINFO fno;
  if (f_opendir(fs, &dir, path) != FR_OK) {
    return;
  }

  while (fileCount_ < kMaxFiles) {
    if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') {
      break;
    }
    if (fno.fattrib & AM_DIR) {
      continue;
    }
    strncpy(fileNames_[fileCount_], fno.fname, kMaxNameLen - 1);
    fileNames_[fileCount_][kMaxNameLen - 1] = '\0';
    fileSizes_[fileCount_] = static_cast<uint32_t>(fno.fsize);
    fileCount_++;
  }
  f_closedir(&dir);
}

void FileBrowser::serviceFileList() {
  const Inputs::ButtonEdges& e = inputs_->edges();
  const Inputs::ButtonStates& b = inputs_->buttons();
  if (b.a && b.b) {
    return;
  }

  bool needsRedraw = false;

  auto returnToConfig = [&]() {
    cursor_ = Config::kCount > 0 ? Config::kCount - 1 : 0;
    scroll_ = cursor_ > kVisibleRows ? cursor_ - kVisibleRows + 1 : 0;
    lastJoyNavMs_ = 0;
    state_ = State::ConfigEdit;
    renderConfigEdit();
  };

  if (e.cancelPressed) {
    returnToConfig();
    return;
  }
  if (e.confirmPressed && fileCount_ > 0) {
    openSelectedFile();
    return;
  }

  const uint32_t nowMs = millis();
  const int16_t joyDeltaY = static_cast<int16_t>(inputs_->joyY()) - 2047;
  if (abs(joyDeltaY) > static_cast<int16_t>(kJoyDeadband)) {
    if (nowMs - lastJoyNavMs_ >= kJoyRepeatMs) {
      lastJoyNavMs_ = nowMs;
      if (joyDeltaY < 0) {
        if (cursor_ > 0) {
          cursor_--;
          if (cursor_ < scroll_) {
            scroll_ = cursor_;
          }
          needsRedraw = true;
        } else {
          returnToConfig();
          return;
        }
      } else if (fileCount_ > 0 && cursor_ + 1 < fileCount_) {
        cursor_++;
        if (cursor_ >= scroll_ + kVisibleRows) {
          scroll_ = cursor_ - kVisibleRows + 1;
        }
        needsRedraw = true;
      }
      sleepService.caffeine = true;
    }
  } else {
    lastJoyNavMs_ = 0;
  }

  if (needsRedraw) {
    renderFileList();
  }
}

void FileBrowser::renderFileList() {
  display_->setFontPreset(FONT_TINY);
  display_->setTextWrap(false);
  display_->clearDisplay();

  display_->setCursor(0, 0);
  display_->print("FILES");
  if (fileCount_ == 0) {
    display_->print(" (none)");
  }
  display_->drawHLine(0, 8, 128);

  for (uint8_t i = 0; i < kVisibleRows && (scroll_ + i) < fileCount_; ++i) {
    const uint8_t idx = scroll_ + i;
    const uint8_t y = 10 + i * 8;
    display_->setCursor(0, y);
    display_->print(idx == cursor_ ? '>' : ' ');
    display_->print(' ');
    display_->print(fileNames_[idx]);

    char sizeBuf[10];
    if (fileSizes_[idx] < 1024) {
      std::snprintf(sizeBuf, sizeof(sizeBuf), "%luB", (unsigned long)fileSizes_[idx]);
    } else {
      std::snprintf(sizeBuf, sizeof(sizeBuf), "%luK", (unsigned long)(fileSizes_[idx] / 1024));
    }
    const int sizePixels = static_cast<int>(strlen(sizeBuf)) * 6;
    display_->setCursor(128 - sizePixels, y);
    display_->print(sizeBuf);
  }

  OLEDLayout::drawGameFooter(*display_);
  OLEDLayout::drawFooterActions(*display_, nullptr, nullptr, "back", "open");
  display_->display();
}
void FileBrowser::openSelectedFile() {
  if (cursor_ >= fileCount_) {
    return;
  }

  Filesystem::IOLock fsLock;  // serialise vs other journal writers
  FATFS* fs = replay_get_fatfs();
  if (fs == nullptr) {
    return;
  }

  strncpy(viewTitle_, fileNames_[cursor_], kMaxNameLen - 1);
  viewTitle_[kMaxNameLen - 1] = '\0';

  viewLen_ = 0;
  viewTotalLines_ = 1;
  memset(viewBuf_, 0, kViewBufSize);

  FIL fil;
  if (f_open(fs, &fil, fileNames_[cursor_], FA_READ) == FR_OK) {
    UINT bytesRead = 0;
    f_read(&fil, viewBuf_, kViewBufSize - 1, &bytesRead);
    viewLen_ = static_cast<uint16_t>(bytesRead);
    viewBuf_[viewLen_] = '\0';
    f_close(&fil);

    viewTotalLines_ = 1;
    for (uint16_t i = 0; i < viewLen_; ++i) {
      if (viewBuf_[i] == '\n') {
        viewTotalLines_++;
      }
    }
  }

  cursor_ = 0;
  scroll_ = 0;
  lastJoyNavMs_ = 0;
  state_ = State::FileView;
  renderFileView();
}

// ─── File Viewer ─────────────────────────────────────────────────────────────

void FileBrowser::serviceFileView() {
  const Inputs::ButtonEdges& e = inputs_->edges();
  bool needsRedraw = false;

  const uint8_t maxScroll =
      viewTotalLines_ > kVisibleRows + 1 ? viewTotalLines_ - kVisibleRows - 1 : 0;

  if (e.cancelPressed) {
    scanDirectory("/");
    cursor_ = 0;
    scroll_ = 0;
    lastJoyNavMs_ = 0;
    state_ = State::FileList;
    renderFileList();
    return;
  }

  const uint32_t nowMs = millis();
  const int16_t joyDeltaY = static_cast<int16_t>(inputs_->joyY()) - 2047;
  if (abs(joyDeltaY) > static_cast<int16_t>(kJoyDeadband)) {
    if (nowMs - lastJoyNavMs_ >= kJoyRepeatMs) {
      lastJoyNavMs_ = nowMs;
      if (joyDeltaY < 0 && cursor_ > 0) {
        cursor_--;
        needsRedraw = true;
        sleepService.caffeine = true;
      } else if (joyDeltaY > 0 && cursor_ < maxScroll) {
        cursor_++;
        needsRedraw = true;
        sleepService.caffeine = true;
      }
    }
  } else {
    lastJoyNavMs_ = 0;
  }

  if (needsRedraw) {
    renderFileView();
  }
}

void FileBrowser::renderFileView() {
  display_->setFontPreset(FONT_TINY);
  display_->setTextWrap(false);
  display_->clearDisplay();
  display_->setCursor(0, 0);
  display_->print(viewTitle_);
  display_->drawHLine(0, 8, 128);

  const char* p = viewBuf_;
  uint8_t lineNum = 0;
  while (lineNum < cursor_ && p < viewBuf_ + viewLen_) {
    if (*p == '\n') {
      lineNum++;
    }
    p++;
  }

  for (uint8_t row = 0; row < kVisibleRows && p < viewBuf_ + viewLen_; ++row) {
    display_->setCursor(0, 10 + row * 8);
    uint8_t col = 0;
    while (p < viewBuf_ + viewLen_ && *p != '\n' && col < 21) {
      display_->print(*p);
      p++;
      col++;
    }
    while (p < viewBuf_ + viewLen_ && *p != '\n') {
      p++;
    }
    if (p < viewBuf_ + viewLen_ && *p == '\n') {
      p++;
    }
  }

  OLEDLayout::drawGameFooter(*display_);
  OLEDLayout::drawFooterActions(*display_, nullptr, nullptr, "back", nullptr);
  char page[12];
  std::snprintf(page, sizeof(page), "%u/%u",
                static_cast<unsigned>(cursor_ + 1),
                static_cast<unsigned>(viewTotalLines_));
  display_->drawStr(128 - display_->getStrWidth(page), OLEDLayout::kFooterBaseY,
                    page);
  display_->display();
}
