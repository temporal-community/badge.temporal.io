#pragma once

#include "Screen.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "../hardware/qrcode.h"
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
    kLayoutMigrateInfo,    // legacy: USB-only path, kept as final fallback
    kLayoutMigratePrecheck,// "here is what we'll do" + go/no-go
    kLayoutMigrateRecovery,// "if it bricks: scan this QR" + continue/cancel
    kLayoutMigrateConfirm, // last warning: wipes ffat, rewrites table
    kLayoutMigrating,      // in-flight; transient — function ESP.restarts
    kLayoutMigrateError,   // rolled back or refused; show reason
    kLayoutWelcome,        // one-shot panel after partition layout changed
    kReinstallConfirm, // "you're already on (or ahead of) latest — reinstall?"
  };

  Phase phase_ = Phase::kIdle;
  uint32_t spinnerStartMs_ = 0;
  uint32_t installStartMs_ = 0;
  size_t installBytes_ = 0;
  size_t installTotal_ = 0;
  bool installDone_ = false;
  ota::InstallResult installResult_ = ota::InstallResult::kOk;
  ota::CheckResult lastCheckResult_ = ota::CheckResult::kOkUpToDate;
  ota::MigrationResult migrationResult_ = ota::MigrationResult::kOk;
  bool firstEnter_ = true;

  // Latched in onEnter() from ota::justRebootedFromLayoutMigration().
  // Drives the kLayoutWelcome panel copy (success confirmation vs.
  // generic "layout changed" notice) and is held until the user
  // dismisses the panel. Acknowledged separately from the OTA-side
  // flag so the screen can re-render the same copy across multiple
  // frames before the user presses a button.
  bool migrationJustHappened_ = false;

  // Set by onEnter() to defer the automatic GitHub check until after
  // the screen-entry transition (contrast fade + haptic pulse) has
  // finished. While non-zero, needsRender() returns true so the loop
  // keeps ticking. render() fires runCheck() once the deadline passes
  // and clears this back to zero.
  uint32_t autoCheckAfterMs_ = 0;

  // Recovery-screen QR is generated lazily on first paint and cached
  // for the life of the screen. The URL is short enough that a single
  // QR version covers it; we don't need to retry with bigger versions.
  static constexpr uint16_t kRecoveryQrBufBytes = 256;
  uint8_t recoveryQrWork_[kRecoveryQrBufBytes] = {};
  uint8_t recoveryQrBits_[64 * 8] = {};  // up to 62×62 px @ 1 bpp
  uint8_t recoveryQrPixels_ = 0;         // 0 == not yet generated / failed
  bool recoveryQrTried_ = false;

  void runCheck(bool ignoreCooldown);
  void runInstall();
  void runLayoutMigration();
  void renderExpandConfirm(oled& d, bool secondConfirm);
  void renderLayoutMigrateInfo(oled& d);
  void renderLayoutMigratePrecheck(oled& d);
  void renderLayoutMigrateRecovery(oled& d);
  void renderLayoutMigrateConfirm(oled& d);
  void renderLayoutMigrating(oled& d);
  void renderLayoutMigrateError(oled& d);
  void renderLayoutWelcome(oled& d);
  void renderReinstallConfirm(oled& d);
  bool ensureRecoveryQr();
  static void installProgressCb(const ota::InstallProgress& prog,
                                void* user);
};
