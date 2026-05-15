#pragma once
#include "Screen.h"

// ─── Apps screen (Python app launcher from /apps) ───────────────────────────

class AppsScreen : public ListMenuScreen {
 public:
  AppsScreen(ScreenId sid, const char* dir, const char* title,
             ScreenId subfolderScreen = kScreenNone,
             const char* subfolderLabel = nullptr,
             const char* const* allowedNames = nullptr,
             uint8_t allowedCount = 0);

  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  void onItemSelect(uint8_t index, GUIManager& gui) override;
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void onResume(GUIManager& gui) override;
  bool navigableItems() const override { return true; }
  const char* hintText() const override { return "Cancel:back Confirm:run"; }

 private:
  static constexpr uint8_t kMaxApps = 64;
  static constexpr uint8_t kMaxNameLen = 28;

  const char* dir_;
  ScreenId subfolderScreen_;
  const char* subfolderLabel_;
  const char* const* allowedNames_;
  uint8_t allowedCount_;

  char (*appNames_)[kMaxNameLen] = nullptr;
  bool* appIsDir_ = nullptr;
  uint8_t appCount_ = 0;

  bool isAllowed(const char* name) const;
  bool ensureStorage();
  void releaseStorage();
  void scanApps();
};
