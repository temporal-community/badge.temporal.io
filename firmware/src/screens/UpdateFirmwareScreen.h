#pragma once

#include "Screen.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "../ota/BadgeOTA.h"

// Settings → Firmware Update screen.
//
// Drives the BadgeOTA façade. State machine:
//
//   Idle          — show current + cached versions, primary action
//                   ("Check now" / "INSTALL UPDATE" / "Reinstall").
//   Checking      — synchronous spinner while api.github.com is hit.
//                   Re-enters Idle with updated cache.
//   Installing    — progress bar driven by Update.write callback.
//                   On success, immediately ESP.restart().
//   Error         — display BadgeOTA::lastErrorMessage(); any button
//                   returns to Idle.

class UpdateFirmwareScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenUpdateFirmware; }
  bool showCursor() const override { return false; }
  ScreenAccess access() const override { return ScreenAccess::kAny; }
  bool needsRender() override;

 private:
  enum class Phase : uint8_t {
    kIdle,
    kChecking,
    kInstalling,
    kError,
    kExpandConfirm,    // first prompt: "really wipe and expand?"
    kExpandConfirm2,   // second prompt: "are you really sure?"
  };

  Phase phase_ = Phase::kIdle;
  uint32_t spinnerStartMs_ = 0;
  size_t installBytes_ = 0;
  size_t installTotal_ = 0;
  bool installDone_ = false;
  ota::InstallResult installResult_ = ota::InstallResult::kOk;
  ota::CheckResult lastCheckResult_ = ota::CheckResult::kOkUpToDate;
  bool firstEnter_ = true;

  void runCheck(bool ignoreCooldown);
  void runInstall();
  void renderExpandConfirm(oled& d, bool secondConfirm);
  static void installProgressCb(const ota::InstallProgress& prog,
                                void* user);
};
