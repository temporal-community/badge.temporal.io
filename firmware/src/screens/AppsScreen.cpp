#include "AppsScreen.h"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>

#include "../infra/Filesystem.h"
#include "../ui/GUI.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/LEDmatrix.h"
#include "../hardware/oled.h"

extern "C" {
#include "lib/oofatfs/ff.h"
#include "matrix_app_api.h"
FATFS* replay_get_fatfs(void);
void mpy_gui_exec_file(const char* path);
}

extern Inputs inputs;
extern LEDmatrix badgeMatrix;
extern volatile bool mpy_oled_hold;
extern volatile bool mpy_led_hold;

AppsScreen::AppsScreen(ScreenId sid, const char* dir, const char* title,
                       ScreenId subfolderScreen, const char* subfolderLabel,
                       const char* const* allowedNames, uint8_t allowedCount)
    : ListMenuScreen(sid, title),
      dir_(dir),
      subfolderScreen_(subfolderScreen),
      subfolderLabel_(subfolderLabel),
      allowedNames_(allowedNames),
      allowedCount_(allowedCount) {}

void AppsScreen::onEnter(GUIManager& gui) {
  ListMenuScreen::onEnter(gui);
  scanApps();
}

void AppsScreen::onExit(GUIManager& gui) {
  (void)gui;
  releaseStorage();
}

void AppsScreen::onResume(GUIManager& gui) {
  (void)gui;
  scanApps();
}

uint8_t AppsScreen::itemCount() const {
  return appCount_ + (subfolderLabel_ ? 1 : 0);
}

void AppsScreen::formatItem(uint8_t index, char* buf,
                            uint8_t bufSize) const {
  if (subfolderLabel_ && index == appCount_) {
    std::snprintf(buf, bufSize, "[%s]", subfolderLabel_);
    return;
  }
  if (index >= appCount_) {
    buf[0] = '\0';
    return;
  }
  if (!appNames_ || !appIsDir_) {
    buf[0] = '\0';
    return;
  }
  const char* name = appNames_[index];
  if (appIsDir_[index]) {
    std::snprintf(buf, bufSize, "%s", name);
    return;
  }
  size_t len = strlen(name);
  if (len > 3) {
    const char* ext = name + len - 3;
    if (ext[0] == '.' && (ext[1] | 0x20) == 'p' && (ext[2] | 0x20) == 'y') {
      size_t copyLen = len - 3;
      if (copyLen >= bufSize) copyLen = bufSize - 1;
      memcpy(buf, name, copyLen);
      buf[copyLen] = '\0';
      return;
    }
  }
  std::snprintf(buf, bufSize, "%s", name);
}

void AppsScreen::onItemSelect(uint8_t index, GUIManager& gui) {
  if (subfolderLabel_ && index == appCount_) {
    Haptics::shortPulse();
    gui.pushScreen(subfolderScreen_);
    return;
  }
  if (index >= appCount_) return;

  Haptics::shortPulse();

  char path[64];
  if (appIsDir_[index]) {
    std::snprintf(path, sizeof(path), "%s/%s/main.py", dir_,
                  appNames_[index]);
  } else {
    std::snprintf(path, sizeof(path), "%s/%s", dir_, appNames_[index]);
  }

  // Show launch screen
  oled& d = gui.oledDisplay();
  d.clearBuffer();
  d.setFontPreset(FONT_SMALL);
  d.setDrawColor(1);

  char displayName[kMaxNameLen];
  formatItem(index, displayName, sizeof(displayName));

  int tw = d.getStrWidth("Running:");
  d.drawStr((128 - tw) / 2, 24, "Running:");
  tw = d.getStrWidth(displayName);
  d.drawStr((128 - tw) / 2, 40, displayName);
  d.setFontPreset(FONT_TINY);
  const char* hint = "All btns 1s = exit";
  tw = d.getStrWidth(hint);
  d.drawStr((128 - tw) / 2, 58, hint);
  d.sendBuffer();

  Serial.printf("GUI: Running app %s\n", path);

  mpy_gui_exec_file(path);

  // Reclaim display and LED matrix from MicroPython
  mpy_oled_hold = false;
  if (mpy_led_hold) {
    if (!matrix_app_is_active()) {
      mpy_led_hold = false;
      badgeMatrix.setMicropythonMode(false);
    }
  }

  // Wait for user to release the face buttons so the GUI all-buttons-held
  // deactivation check doesn't fire immediately
  for (int i = 0; i < 200; i++) {
    inputs.service();
    const Inputs::ButtonStates& btns = inputs.buttons();
    if (!btns.up || !btns.down || !btns.left || !btns.right) break;
    delay(10);
  }
  inputs.clearEdges();

  Serial.println("GUI: App finished, returning to menu");
}

bool AppsScreen::isAllowed(const char* name) const {
  if (!allowedNames_ || allowedCount_ == 0) return true;
  for (uint8_t i = 0; i < allowedCount_; i++) {
    if (strcmp(name, allowedNames_[i]) == 0) return true;
  }
  return false;
}

bool AppsScreen::ensureStorage() {
  if (appNames_ && appIsDir_) return true;

  if (!appNames_) {
    appNames_ = static_cast<char (*)[kMaxNameLen]>(
        heap_caps_calloc(kMaxApps, kMaxNameLen,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!appIsDir_) {
    appIsDir_ = static_cast<bool*>(
        heap_caps_calloc(kMaxApps, sizeof(bool),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!appNames_ || !appIsDir_) {
    Serial.println("[Apps] listing buffer alloc failed");
    releaseStorage();
    return false;
  }
  return true;
}

void AppsScreen::releaseStorage() {
  if (appNames_) {
    heap_caps_free(appNames_);
    appNames_ = nullptr;
  }
  if (appIsDir_) {
    heap_caps_free(appIsDir_);
    appIsDir_ = nullptr;
  }
  appCount_ = 0;
}

static bool hasMainPy(FATFS* fs, const char* dir, const char* name) {
  char path[64];
  std::snprintf(path, sizeof(path), "%s/%s/main.py", dir, name);
  FILINFO info;
  return f_stat(fs, path, &info) == FR_OK && !(info.fattrib & AM_DIR);
}

void AppsScreen::scanApps() {
  appCount_ = 0;
  if (!ensureStorage()) return;
  Filesystem::IOLock fsLock;  // serialise vs other journal writers
  FATFS* fs = replay_get_fatfs();
  if (fs == nullptr) return;

  FF_DIR dir;
  FILINFO fno;
  if (f_opendir(fs, &dir, dir_) != FR_OK) return;

  while (appCount_ < kMaxApps) {
    if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
    if (fno.fname[0] == '_') continue;

    if (fno.fattrib & AM_DIR) {
      if (!hasMainPy(fs, dir_, fno.fname)) continue;
      if (!isAllowed(fno.fname)) continue;
      strncpy(appNames_[appCount_], fno.fname, kMaxNameLen - 1);
      appNames_[appCount_][kMaxNameLen - 1] = '\0';
      appIsDir_[appCount_] = true;
      appCount_++;
      continue;
    }

    size_t nameLen = strlen(fno.fname);
    if (nameLen <= 3) continue;
    const char* ext = fno.fname + nameLen - 3;
    if (ext[0] != '.' || (ext[1] | 0x20) != 'p' || (ext[2] | 0x20) != 'y')
      continue;
    char folderName[kMaxNameLen];
    size_t folderLen = nameLen - 3;
    if (folderLen >= sizeof(folderName)) folderLen = sizeof(folderName) - 1;
    memcpy(folderName, fno.fname, folderLen);
    folderName[folderLen] = '\0';
    if (hasMainPy(fs, dir_, folderName)) continue;
    if (!isAllowed(fno.fname)) continue;
    strncpy(appNames_[appCount_], fno.fname, kMaxNameLen - 1);
    appNames_[appCount_][kMaxNameLen - 1] = '\0';
    appIsDir_[appCount_] = false;
    appCount_++;
  }

  for (uint8_t i = 0; i < appCount_; i++) {
    for (uint8_t j = i + 1; j < appCount_; j++) {
      if (strcmp(appNames_[j], appNames_[i]) < 0) {
        char tmp[kMaxNameLen];
        bool tmpIsDir = appIsDir_[i];
        strncpy(tmp, appNames_[i], kMaxNameLen);
        strncpy(appNames_[i], appNames_[j], kMaxNameLen);
        strncpy(appNames_[j], tmp, kMaxNameLen);
        appIsDir_[i] = appIsDir_[j];
        appIsDir_[j] = tmpIsDir;
      }
    }
  }
  f_closedir(&dir);
}
