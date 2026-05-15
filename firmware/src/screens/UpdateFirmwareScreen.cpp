#include "UpdateFirmwareScreen.h"

#include <cstdio>
#include <cstring>
#include <Arduino.h>

#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../identity/BadgeVersion.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../api/WiFiService.h"

namespace {

void formatRelativeTime(time_t epoch, char* buf, size_t cap) {
  if (epoch <= 1) {
    std::snprintf(buf, cap, "never");
    return;
  }
  time_t now = time(nullptr);
  if (now <= 0 || now < epoch) {
    std::snprintf(buf, cap, "just now");
    return;
  }
  uint32_t delta = static_cast<uint32_t>(now - epoch);
  if (delta < 60) {
    std::snprintf(buf, cap, "%us ago", (unsigned)delta);
  } else if (delta < 3600) {
    std::snprintf(buf, cap, "%um ago", (unsigned)(delta / 60));
  } else if (delta < 86400) {
    std::snprintf(buf, cap, "%uh ago", (unsigned)(delta / 3600));
  } else {
    std::snprintf(buf, cap, "%ud ago", (unsigned)(delta / 86400));
  }
}

}  // namespace

void UpdateFirmwareScreen::onEnter(GUIManager& /*gui*/) {
  phase_ = Phase::kIdle;
  installDone_ = false;
  installBytes_ = 0;
  installTotal_ = 0;
  // First time the user opens the screen after boot we offer to check
  // automatically — reuses the cooldown so we don't hit the API on
  // every re-entry.
  if (firstEnter_) {
    firstEnter_ = false;
  }
}

bool UpdateFirmwareScreen::needsRender() {
  // The Installing phase animates a progress bar even when the user
  // isn't pressing buttons. Other phases only repaint on input.
  return phase_ == Phase::kInstalling || phase_ == Phase::kChecking;
}

void UpdateFirmwareScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);

  // Expand-storage phases own their own status header — paint it
  // first instead of the FW UPDATE header so the two don't overlap.
  if (phase_ == Phase::kExpandConfirm) {
    renderExpandConfirm(d, /*secondConfirm=*/false);
    return;
  }
  if (phase_ == Phase::kExpandConfirm2) {
    renderExpandConfirm(d, /*secondConfirm=*/true);
    return;
  }

  OLEDLayout::drawStatusHeader(d, "FW UPDATE");
  d.setFontPreset(FONT_TINY);

  if (phase_ == Phase::kChecking) {
    d.drawStr(2, 24, "Checking GitHub for new");
    d.drawStr(2, 33, "firmware release...");
    OLEDLayout::drawBusySpinner(d, 64, 46,
                                static_cast<uint8_t>((millis() / 80) & 7));
    OLEDLayout::drawNavFooter(d, "Please wait");
    return;
  }

  if (phase_ == Phase::kInstalling) {
    d.drawStr(2, 24, "Installing...");
    char szBuf[40];
    if (installTotal_ > 0) {
      std::snprintf(szBuf, sizeof(szBuf), "%u / %u KB",
                    (unsigned)(installBytes_ / 1024),
                    (unsigned)(installTotal_ / 1024));
    } else {
      std::snprintf(szBuf, sizeof(szBuf), "%u KB",
                    (unsigned)(installBytes_ / 1024));
    }
    d.drawStr(2, 33, szBuf);
    // Progress bar
    constexpr int kBarX = 4, kBarY = 40, kBarW = 120, kBarH = 8;
    d.drawRFrame(kBarX, kBarY, kBarW, kBarH, 1);
    if (installTotal_ > 0) {
      int fill = static_cast<int>(((installBytes_ * (kBarW - 2)) /
                                   installTotal_));
      if (fill < 0) fill = 0;
      if (fill > kBarW - 2) fill = kBarW - 2;
      d.drawBox(kBarX + 1, kBarY + 1, fill, kBarH - 2);
    } else {
      // Indeterminate: barber-pole.
      uint32_t off = (millis() / 60) % (kBarW - 8);
      d.drawBox(kBarX + 1 + off, kBarY + 1, 6, kBarH - 2);
    }
    OLEDLayout::drawNavFooter(d, "Do not unplug");
    return;
  }

  if (phase_ == Phase::kError) {
    d.drawStr(2, 22, "Update failed:");
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s", ota::lastErrorMessage());
    OLEDLayout::fitText(d, buf, sizeof(buf), 124);
    d.drawStr(2, 32, buf);
    OLEDLayout::drawNavFooter(d, "Any:Back");
    return;
  }

  // Idle. Three lines of status, plus the primary action footer.
  char line[48];
  std::snprintf(line, sizeof(line), "Current:  %s", FIRMWARE_VERSION);
  d.drawStr(2, 18, line);

  const char* tag = ota::latestKnownTag();
  if (tag[0]) {
  std::snprintf(line, sizeof(line), "Latest:   %s", tag);
  } else {
  std::snprintf(line, sizeof(line), "Latest: (not checked)");
  }
  d.drawStr(2, 27, line);

  // Row 3 (y=36): status warning if any; otherwise the friendlier
  // "Last check: <age>". Warnings displace the staleness line because
  // they're more actionable.
  const bool wifiUp = wifiService.isConnected();
  const char* warning = nullptr;
  if (!wifiUp) {
    warning = "WiFi off — connect first";
  } else if (lastCheckResult_ == ota::CheckResult::kNoMatchingAsset) {
    warning = "No asset for this build";
  } else if (lastCheckResult_ == ota::CheckResult::kOkOlder) {
    warning = "Newer than published";
  }
  if (warning) {
    d.drawStr(2, 36, warning);
  } else {
    char age[20];
    formatRelativeTime(ota::lastCheckEpoch(), age, sizeof(age));
    std::snprintf(line, sizeof(line), "Last check: %s", age);
    d.drawStr(2, 36, line);
  }

  // Row 4 (y=45): filesystem capacity. Always shown when the FS is
  // mounted. When the partition has unclaimed space (post-OTA bump),
  // append an X-glyph expand affordance routed through drawInlineHint
  // so the letter becomes the X button glyph (project convention —
  // see firmware/src/ui/ButtonGlyphs.h).
  const size_t volBytes = ota::ffatVolumeBytes();
  if (volBytes > 0) {
    char szLine[48];
    const float curMb = volBytes / (1024.0f * 1024.0f);
    if (ota::ffatExpansionAvailable()) {
      const float partMb =
          ota::ffatPartitionBytes() / (1024.0f * 1024.0f);
      std::snprintf(szLine, sizeof(szLine),
                    "FS: %.1f MB  X expand %.1f", curMb, partMb);
    } else {
      std::snprintf(szLine, sizeof(szLine), "FS: %.1f MB", curMb);
    }
    ButtonGlyphs::drawInlineHint(d, 2, 45, szLine);
  }

  // Action footer label depends on cached state.
  const char* action = "Check now";
  if (ota::updateAvailable()) {
    action = "INSTALL UPDATE";
  } else if (tag[0] && std::strcmp(tag, FIRMWARE_VERSION) == 0) {
    action = "Recheck";
  }
  OLEDLayout::drawActionFooter(d, action, "Check");
}

void UpdateFirmwareScreen::renderExpandConfirm(oled& d, bool secondConfirm) {
  OLEDLayout::drawStatusHeader(d, "EXPAND STORAGE");
  d.setFontPreset(FONT_TINY);

  const unsigned partMb =
      static_cast<unsigned>(ota::ffatPartitionBytes() / (1024u * 1024u));

  if (!secondConfirm) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Reformat to %u MB?", partMb);
    d.drawStr(2, 18, buf);
    d.drawStr(2, 28, "ALL user data is wiped:");
    d.drawStr(2, 37, "  contacts, nametags,");
    d.drawStr(2, 46, "  WAD, settings.txt");
    OLEDLayout::drawActionFooter(d, "Continue", "Confirm");
    return;
  }

  d.drawStr(2, 18, "ARE YOU SURE?");
  d.drawStr(2, 30, "This cannot be undone.");
  d.drawStr(2, 42, "The badge will reboot");
  d.drawStr(2, 51, "after formatting.");
  OLEDLayout::drawActionFooter(d, "WIPE & EXPAND", "Wipe");
}

void UpdateFirmwareScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                       int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (phase_ == Phase::kChecking || phase_ == Phase::kInstalling) {
    return;  // synchronous; ignore input until done
  }

  if (phase_ == Phase::kError) {
    if (e.cancelPressed || e.confirmPressed || e.xPressed || e.yPressed) {
      phase_ = Phase::kIdle;
    }
    return;
  }

  if (phase_ == Phase::kExpandConfirm) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      phase_ = Phase::kExpandConfirm2;
    }
    return;
  }
  if (phase_ == Phase::kExpandConfirm2) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      // Final paint then wipe + reboot. reformatFfatAndReboot()
      // does not return.
      extern GUIManager guiManager;
      oled& d = guiManager.oledDisplay();
      d.clearBuffer();
      OLEDLayout::drawStatusHeader(d, "EXPAND STORAGE");
      d.setFontPreset(FONT_SMALL);
      d.drawStr(8, 30, "Formatting...");
      d.sendBuffer();
      Haptics::off();
      delay(100);
      ota::reformatFfatAndReboot();
    }
    return;
  }

  if (e.cancelPressed) {
    Haptics::shortPulse();
    gui.popScreen();
    return;
  }

  // X button (yPressed in the codebase's swap-aware naming) opens
  // the expand-storage flow when the partition has unclaimed space.
  if (e.xPressed && ota::ffatExpansionAvailable()) {
    Haptics::shortPulse();
    phase_ = Phase::kExpandConfirm;
    return;
  }

  if (e.confirmPressed) {
    Haptics::shortPulse();
    Haptics::off();
    delay(100);
    if (ota::updateAvailable()) {
      runInstall();
    } else {
      runCheck(true);
    }
  }
}

void UpdateFirmwareScreen::runCheck(bool ignoreCooldown) {
  phase_ = Phase::kChecking;
  spinnerStartMs_ = millis();
  // Force one render so the user sees the spinner before we block.
  extern GUIManager guiManager;
  guiManager.requestRender();
  // We can't yield to the render loop from here, so just paint
  // directly before the synchronous call.
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  render(d, guiManager);
  d.sendBuffer();

  lastCheckResult_ = ota::checkNow(ignoreCooldown);
  Serial.printf("[updscreen] checkNow result=%d tag=%s\n",
                (int)lastCheckResult_, ota::latestKnownTag());

  if (lastCheckResult_ == ota::CheckResult::kNetworkError ||
      lastCheckResult_ == ota::CheckResult::kParseError) {
    phase_ = Phase::kError;
  } else {
    phase_ = Phase::kIdle;
  }
}

void UpdateFirmwareScreen::installProgressCb(
    const ota::InstallProgress& prog, void* user) {
  auto* self = static_cast<UpdateFirmwareScreen*>(user);
  if (!self) return;
  self->installBytes_ = prog.bytesWritten;
  self->installTotal_ = prog.totalBytes;
  self->installDone_ = prog.done;
  // Repaint progress.
  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  self->render(d, guiManager);
  d.sendBuffer();
}

void UpdateFirmwareScreen::runInstall() {
  phase_ = Phase::kInstalling;
  installBytes_ = 0;
  installTotal_ = ota::latestKnownAssetSize();
  installDone_ = false;

  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  render(d, guiManager);
  d.sendBuffer();

  Haptics::off();
  // delay(100);
  installResult_ = ota::installCached(&UpdateFirmwareScreen::installProgressCb,
                                      this);

  if (installResult_ == ota::InstallResult::kOk) {
    // Final paint then reboot.
    d.clearBuffer();
    OLEDLayout::drawStatusHeader(d, "FIRMWARE UPDATE");
    d.setFontPreset(FONT_SMALL);
    d.drawStr(20, 30, "Rebooting...");
    d.sendBuffer();
    delay(800);
    ESP.restart();
  } else {
    phase_ = Phase::kError;
  }
}
