#pragma once

#include "Screen.h"

#include "../ota/AssetRegistry.h"

// Browse + install user-installable assets (DOOM WAD, sound packs,
// etc.) from the registry URL configured in settings.txt.
//
// Two screens:
//   AssetLibraryScreen  — scrollable list of registry entries with a
//                         per-row status badge.
//   AssetDetailScreen   — selected entry's metadata + Install/Remove
//                         action. Owns the install progress UI.

class AssetLibraryScreen : public ListMenuScreen {
 public:
  AssetLibraryScreen();
  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  bool navigableItems() const override { return true; }
  void onItemSelect(uint8_t index, GUIManager& gui) override;
  void onEnter(GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  const char* hintText() const override { return "Confirm:Open  X:Refresh"; }

  // Public so the main menu's launcher can pre-set a target id when
  // routing from DOOM's no-WAD screen.
  static void selectAssetById(const char* id);

 private:
  uint8_t cachedCount_ = 0;
  // Triggers a synchronous fetch from onEnter / X-press. When refreshing
  // we toggle this so render() can surface "Refreshing..." in the empty
  // state instead of staring at a blank list.
  bool refreshing_ = false;
  // Per-row status cache. statusOf() hits NVS + the FATFS lock on every
  // call; without this cache, formatItem() (called every frame per row)
  // pushes the GUI service over its 33ms frame budget. Rebuilt by
  // refreshStatusCache() on enter and after each install/remove.
  static constexpr uint8_t kStatusCacheCap = 64;
  mutable ota::AssetStatus statusCache_[kStatusCacheCap] = {};
  // Number of registry assets whose status is not kInstalled. Drives
  // the synthetic "Install all updates" row at index 0.
  uint8_t pendingCount_ = 0;
  void doRefresh(bool ignoreCooldown);
  void refreshStatusCache();
};

class AssetDetailScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenAssetDetail; }
  bool showCursor() const override { return false; }
  bool needsRender() override;

  // Set the entry the screen will render when next pushed. Caller
  // owns the AssetEntry storage (typically AssetRegistry static).
  static void setActiveAsset(const ota::AssetEntry* entry);

 private:
  enum class Phase : uint8_t {
    kIdle,
    kInstalling,
    kError,
    kDone,
  };

  Phase phase_ = Phase::kIdle;
  size_t bytesWritten_ = 0;
  size_t totalBytes_ = 0;
  ota::AssetInstallResult lastResult_ = ota::AssetInstallResult::kOk;

  void runInstall(GUIManager& gui);
  void runRemove(GUIManager& gui);
  static void progressCb(const ota::AssetProgress& prog, void* user);
};
