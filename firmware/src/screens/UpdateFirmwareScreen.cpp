#include "UpdateFirmwareScreen.h"

#include <cstdio>
#include <cstring>
#include <Arduino.h>

#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/Power.h"
#include "../hardware/oled.h"
#include "../hardware/qrcode.h"
#include "../identity/BadgeVersion.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/QRCodePlate.h"
#include "../api/WiFiService.h"
#include "../ota/AssetRegistry.h"
#include "../infra/RepoUrls.h"

extern BatteryGauge batteryGauge;

namespace {

// Recovery target for the partition migration. Always reachable from
// a phone (HTTPS + GitHub) and deep-linked to the § 5d "Recovery"
// section (HTML anchor `#recovery` in the maintainer doc) that
// describes the one-line `esptool write_flash` path using the
// per-release `temporal-badge-full-flash-16mb.bin` asset. Derived from
// REPO_RECOVERY_URL so forks point at their own recovery doc by default.
// Length budget: 112 chars with the default repo slug → fits QR v6
// (cap=134 with ECC_LOW). See `ensureRecoveryQr()` below for the
// version-selection loop.
constexpr const char* kRecoveryUrl = REPO_RECOVERY_URL;

// Smallest QR version that fits the recovery URL with ECC_LOW. The
// table mirrors the one in HelpScreen — indexed by version (1..7).
constexpr uint16_t kQrByteCapLowEcc[] = {
    0, 17, 32, 53, 78, 106, 134, 154,
};
constexpr uint8_t kQrMaxVersion = 7;



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

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void drawCentered(oled& d, int y, const char* text) {
  if (!text) return;
  const int w = d.getStrWidth(text);
  d.drawStr((128 - w) / 2, y, text);
}

void drawSimpleSpinner(oled& d, int cx, int cy, uint8_t phase) {
  static constexpr int8_t kPts[4][2] = {
      {0, -5}, {5, 0}, {0, 5}, {-5, 0},
  };
  d.setDrawColor(1);
  for (uint8_t i = 0; i < 4; ++i) {
    const int px = cx + kPts[i][0];
    const int py = cy + kPts[i][1];
    if (i == (phase & 3)) {
      d.drawBox(px - 1, py - 1, 3, 3);
    } else {
      d.drawPixel(px, py);
    }
  }
}

void drawCleanProgress(oled& d, int x, int y, int w, int h,
                       size_t bytes, size_t total, uint8_t phase) {
  d.setDrawColor(1);
  d.drawRFrame(x, y, w, h, 3);

  const int innerX = x + 2;
  const int innerY = y + 2;
  const int innerW = w - 4;
  const int innerH = h - 4;
  if (innerW <= 0 || innerH <= 0) return;

  if (total > 0) {
    int fill = static_cast<int>((bytes * innerW) / total);
    fill = clampInt(fill, 0, innerW);
    if (fill > 0) {
      const int radius = (fill >= innerH) ? 2 : 1;
      d.setClipWindow(innerX, innerY, innerX + fill, innerY + innerH);
      d.drawRBox(innerX, innerY, fill + 2, innerH, radius);

      // A small inverted shimmer gives motion without cluttering the
      // screen. It stays clipped to the filled portion of the rounded bar.
      d.setDrawColor(0);
      for (int sx = innerX - innerH + (phase % 8); sx < innerX + fill; sx += 8) {
        d.drawLine(sx, innerY + innerH - 1, sx + innerH - 1, innerY);
      }
      d.setDrawColor(1);
      d.setMaxClipWindow();

      const int glint = innerX + fill - 1;
      if (glint >= innerX && glint < innerX + innerW) {
        d.drawVLine(glint, innerY + 1, innerH - 2);
      }
    }
  } else {
    const int blockW = 24;
    const int travel = innerW + blockW;
    const int bx = innerX - blockW + ((phase * 2) % travel);
    d.setClipWindow(innerX, innerY, innerX + innerW, innerY + innerH);
    d.drawRBox(bx, innerY, blockW, innerH, 2);
    d.setMaxClipWindow();
  }
}

void drawTwoLineError(oled& d, const char* message) {
  char first[80];
  char second[80];
  first[0] = '\0';
  second[0] = '\0';
  std::snprintf(first, sizeof(first), "%s", message && message[0] ? message : "Unknown error");

  char* split = nullptr;
  for (char* p = first; *p; ++p) {
    if (*p == ' ') {
      *p = '\0';
      if (d.getStrWidth(first) <= 120 && d.getStrWidth(p + 1) <= 120) {
        split = p;
      }
      *p = ' ';
    }
  }
  if (split) {
    *split = '\0';
    std::snprintf(second, sizeof(second), "%s", split + 1);
  }
  OLEDLayout::fitText(d, first, sizeof(first), 120);
  OLEDLayout::fitText(d, second, sizeof(second), 120);
  d.drawStr(4, 31, first);
  if (second[0]) d.drawStr(4, 41, second);
}


}  // namespace

void UpdateFirmwareScreen::onEnter(GUIManager& /*gui*/) {
  installDone_ = false;
  installBytes_ = 0;
  installTotal_ = 0;
  // QR generation is ~tens of ms, so defer it to the first paint of
  // the recovery screen. Clear the "tried" flag on each entry so a
  // user who fails the precheck and tries again still picks up any
  // settings that changed in between.
  recoveryQrTried_ = false;
  recoveryQrPixels_ = 0;
  // Two paths into the welcome panel, ranked by signal strength:
  //
  //   1. justRebootedFromLayoutMigration() — deterministic: backed
  //      by an RTC magic that survives soft resets (incl. panics)
  //      but is wiped on power-cycle. GUI auto-pushes this screen
  //      at boot when this fires, so the user sees the success
  //      panel without having to navigate here.
  //
  //   2. layoutJustChanged() — heuristic: NVS string comparison
  //      against the last recorded layout. Also fires after a USB
  //      reflash that swapped the partition table out of band, so
  //      we wait for the user to open FW UPDATE on their own.
  //
  // The deterministic flag is acknowledged in the dismiss handler
  // (handleInput), NOT here — the screen-entry callback fires
  // before any render lands on the OLED, and a panic between
  // push and first paint would otherwise lose the announce
  // forever. The weaker NVS-comparison flag has no such concern
  // (NVS persists) so it acks here as before.
  migrationJustHappened_ = ota::justRebootedFromLayoutMigration();
  if (migrationJustHappened_) {
    phase_ = Phase::kLayoutWelcome;
    ota::acknowledgeLayoutChange();  // suppress the weaker path too
  } else if (ota::layoutJustChanged()) {
    phase_ = Phase::kLayoutWelcome;
    ota::acknowledgeLayoutChange();
  } else {
    phase_ = Phase::kIdle;
    // Defer the auto-check by 200 ms so the screen-entry transition
    // (contrast fade + haptic pulse) can complete before we block on
    // the GitHub HTTP call. needsRender() keeps the loop ticking until
    // the deadline; render() fires runCheck() once it passes.
    // Skipped entirely if WiFi is down — the idle render surfaces the
    // "WiFi off" warning in that case.
    if (wifiService.isConnected()) {
      autoCheckAfterMs_ = millis() + 200;
    }
  }
  firstEnter_ = false;
}

bool UpdateFirmwareScreen::needsRender() {
  // The Installing phase animates a progress bar even when the user
  // isn't pressing buttons; Migrating paints a static panel but we
  // want at least one frame on-screen before the synchronous flash
  // ops start. autoCheckAfterMs_ keeps the loop ticking while we wait
  // for the entry-transition to finish before firing the HTTP check.
  // Other phases only repaint on input.
  return phase_ == Phase::kInstalling ||
         phase_ == Phase::kChecking ||
         phase_ == Phase::kLayoutMigrating ||
         autoCheckAfterMs_ != 0;
}

void UpdateFirmwareScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);

  // Fire the deferred auto-check once the entry-transition window has
  // elapsed. runCheck() paints the kChecking modal synchronously and
  // blocks on the HTTP call, then returns with phase_ set to the
  // result. We re-clear the buffer and fall through so this same
  // render() invocation paints the result into the frame the GUI loop
  // is about to flush — no extra round-trip needed.
  //
  // Guard: if the community-registry async worker (Core 0) is holding
  // a TLS session, a concurrent checkNow() would race it for the same
  // mbedTLS internal-heap pool and fail with SSL -32512 (OOM). This
  // is the same serialisation hazard OTAService avoids with its
  // 1.2 s post-OTA gap. Bumping the deadline 250 ms and retrying is
  // cheaper than catching the error after the fact.
  if (autoCheckAfterMs_ != 0 && millis() >= autoCheckAfterMs_) {
    if (ota::registry::isRefreshing()) {
      autoCheckAfterMs_ = millis() + 250;
      return;
    }
    autoCheckAfterMs_ = 0;
    runCheck(true);
    // Do NOT return here. runCheck() painted and flushed the checking
    // spinner synchronously into the OLED, but the GUI loop's
    // sendBuffer() runs after render() returns and would re-send that
    // stale frame. Re-clear the buffer so the fallthrough below renders
    // the result phase (kIdle or kError) into what the loop will flush.
    d.clearBuffer();
    d.setDrawColor(1);
    // intentional fallthrough to the phase dispatch ↓
  }

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
  if (phase_ == Phase::kReinstallConfirm) {
    renderReinstallConfirm(d);
    return;
  }
  if (phase_ == Phase::kLayoutMigrateInfo) {
    renderLayoutMigrateInfo(d);
    return;
  }
  if (phase_ == Phase::kLayoutMigratePrecheck) {
    renderLayoutMigratePrecheck(d);
    return;
  }
  if (phase_ == Phase::kLayoutMigrateRecovery) {
    renderLayoutMigrateRecovery(d);
    return;
  }
  if (phase_ == Phase::kLayoutMigrateConfirm) {
    renderLayoutMigrateConfirm(d);
    return;
  }
  if (phase_ == Phase::kLayoutMigrating) {
    renderLayoutMigrating(d);
    return;
  }
  if (phase_ == Phase::kLayoutMigrateError) {
    renderLayoutMigrateError(d);
    return;
  }
  if (phase_ == Phase::kLayoutWelcome) {
    renderLayoutWelcome(d);
    return;
  }

  OLEDLayout::drawStatusHeader(d, "FW UPDATE");
  d.setFontPreset(FONT_TINY);

  if (phase_ == Phase::kChecking) {
    const uint8_t phase = static_cast<uint8_t>((millis() - spinnerStartMs_) / 80);
    drawSimpleSpinner(d, 64, 28, phase);
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 43, "Checking GitHub...");
    OLEDLayout::drawNavFooter(d, "Please wait");
    return;
  }

  if (phase_ == Phase::kInstalling) {
    const uint8_t phase = static_cast<uint8_t>((millis() - installStartMs_) / 70);
    const uint8_t pct = installTotal_ > 0
                            ? static_cast<uint8_t>(clampInt(
                                  (int)((installBytes_ * 100u) / installTotal_), 0, 100))
                            : 0;
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 18, "Installing firmware");

    char pctBuf[8];
    if (installTotal_ > 0) {
      std::snprintf(pctBuf, sizeof(pctBuf), "%u%%", (unsigned)pct);
    } else {
      std::snprintf(pctBuf, sizeof(pctBuf), "--%%");
    }
    d.setFontPreset(FONT_SMALL);
    drawCentered(d, 31, pctBuf);

    char szBuf[40];
    if (installTotal_ > 0) {
      std::snprintf(szBuf, sizeof(szBuf), "%u / %u KB",
                    (unsigned)(installBytes_ / 1024),
                    (unsigned)(installTotal_ / 1024));
    } else {
      std::snprintf(szBuf, sizeof(szBuf), "%u KB",
                    (unsigned)(installBytes_ / 1024));
    }
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 41, szBuf);
    drawCleanProgress(d, 16, 47, 96, 6, installBytes_, installTotal_, phase);
    OLEDLayout::drawNavFooter(d, "Do not unplug");
    return;
  }

  if (phase_ == Phase::kError) {
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 22, "Update failed");
    drawTwoLineError(d, ota::lastErrorMessage());
    OLEDLayout::drawNavFooter(d, "Any:Back");
    return;
  }

  // Idle. Keep the screen sparse: versions, status, actions.
  char line[48];
  d.setFontPreset(FONT_TINY);
  std::snprintf(line, sizeof(line), "Current: %s", FIRMWARE_VERSION_DISPLAY);
  d.drawStr(4, 20, line);

  const char* tag = ota::latestKnownTag();
  std::snprintf(line, sizeof(line), "Latest:  %s", tag[0] ? tag : "(not checked)");
  OLEDLayout::fitText(d, line, sizeof(line), 120);
  d.drawStr(4, 30, line);

  const bool wifiUp = wifiService.isConnected();
  const char* warning = nullptr;
  if (!wifiUp) {
    warning = "WiFi off - connect first";
  } else if (lastCheckResult_ == ota::CheckResult::kNoMatchingAsset) {
    warning = "No asset for this build";
  } else if (lastCheckResult_ == ota::CheckResult::kOkOlder) {
    warning = "Newer than published";
  }
  if (warning) {
    std::snprintf(line, sizeof(line), "%s", warning);
  } else {
    char age[20];
    formatRelativeTime(ota::lastCheckEpoch(), age, sizeof(age));
    std::snprintf(line, sizeof(line), "Last check: %s", age);
  }
  OLEDLayout::fitText(d, line, sizeof(line), 120);
  d.drawStr(4, 40, line);

  const size_t volBytes = ota::ffatVolumeBytes();
  if (volBytes > 0) {
    char szLine[48];
    const float curMb = volBytes / (1024.0f * 1024.0f);
    if (ota::ffatExpansionAvailable()) {
      // Free space exists inside the current partition — show the
      // delta so the gain is concrete ("+0.5 MB" beats "expand").
      const float partMb =
          ota::ffatPartitionBytes() / (1024.0f * 1024.0f);
      std::snprintf(szLine, sizeof(szLine),
                    "FS: %.1f MB  X +%.1f MB", curMb, partMb - curMb);
    } else if (ota::canOfferLayoutMigration()) {
      // Already filling the current partition; bigger layout means a
      // full chip erase + reflash. We pitch the absolute target size
      // (6.875 MB) rather than the delta so the user knows what they
      // get for the trouble.
      std::snprintf(szLine, sizeof(szLine),
                    "FS: %.1f MB  X 6.9 MB FS", curMb);
    } else {
      std::snprintf(szLine, sizeof(szLine), "FS: %.1f MB (max)", curMb);
    }
    ButtonGlyphs::drawInlineHint(d, 4, 50, szLine);
  }

  // Footer: D-pad Up = Check, D-pad Right = Install. Drawn through
  // ButtonGlyphs::drawInlineHint so the literal `^` and `>` are
  // substituted with the up/right glyphs (per UI conventions —
  // see firmware/src/ui/ButtonGlyphs.h). drawNavFooter handles the
  // chrome; we feed it the glyph-bearing string ourselves.
  // Right-press always offers an install path now: a strictly newer
  // release runs straight through, otherwise the screen prompts the
  // user to confirm a same-or-older reinstall before flashing.
  const char* hint;
  if (ota::updateAvailable()) {
    hint = "^ Check  > Install";
  } else if (tag[0] && ota::latestKnownAssetUrl()[0]) {
    hint = "^ Check  > Reinstall";
  } else {
    hint = "^ Check";
  }
  OLEDLayout::drawNavFooter(d);
  // drawNavFooter() with no text only paints chrome; render the
  // glyph hint directly afterwards on the footer baseline (y=63 is
  // the project's standard hint baseline used elsewhere).
  ButtonGlyphs::drawInlineHint(d, 2, 63, hint);
}

void UpdateFirmwareScreen::renderExpandConfirm(oled& d, bool secondConfirm) {
  OLEDLayout::drawStatusHeader(d, "EXPAND STORAGE");

  const unsigned partMb =
      static_cast<unsigned>(ota::ffatPartitionBytes() / (1024u * 1024u));

  if (!secondConfirm) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Reformat to %u MB?", partMb);
    const char* lines[] = {buf, "This wipes user data."};
    OLEDLayout::drawTextModal(d, "Expand storage", lines, 2, 0, /*danger=*/true);
    OLEDLayout::drawActionFooter(d, "Continue", "Confirm");
    return;
  }

  {
    const char* lines[] = {"This cannot be undone.", "Badge reboots after format."};
    OLEDLayout::drawTextModal(d, "Are you sure?", lines, 2, 0, /*danger=*/true);
  }
  OLEDLayout::drawActionFooter(d, "WIPE & EXPAND", "Wipe");
}

void UpdateFirmwareScreen::renderLayoutMigrateInfo(oled& d) {
  // Fallback path — reached only when this firmware build doesn't
  // have the partition blob embedded (ota::migrationAssetPresent()
  // returned false). The in-place migration is unavailable, so the
  // only way to the bigger layout is the historical USB script.
  OLEDLayout::drawStatusHeader(d, "BIGGER STORAGE");
  {
    const char* lines[] = {
        "Run erase_and_flash_",
        "expanded.sh over USB",
        "once. Wipes all data.",
    };
    OLEDLayout::drawTextModal(d, "Want 6.9 MB FS?", lines, 3);
  }
  OLEDLayout::drawNavFooter(d, "Any: Back");
}

namespace {

void drawConditionRow(oled& d, int y, const char* label, bool ok) {
  // Left-aligned label, right-aligned status. The status is rendered
  // as an inverted pill so it scans at a glance — OK = white-on-black,
  // FAIL = black-on-white. (Both fit in the same 22 px slot at FONT_TINY.)
  d.setDrawColor(1);
  d.drawStr(4, y, label);
  const char* tag = ok ? "OK" : "FAIL";
  const int tagW = d.getStrWidth(tag) + 6;
  const int tagX = 128 - tagW - 2;
  if (ok) {
    d.drawRFrame(tagX, y - 7, tagW, 9, 2);
    d.drawStr(tagX + 3, y, tag);
  } else {
    d.drawRBox(tagX, y - 7, tagW, 9, 2);
    d.setDrawColor(0);
    d.drawStr(tagX + 3, y, tag);
    d.setDrawColor(1);
  }
}

}  // namespace

void UpdateFirmwareScreen::renderLayoutMigratePrecheck(oled& d) {
  OLEDLayout::drawStatusHeader(d, "EXPAND PARTITION");
  d.setFontPreset(FONT_TINY);

  // The whole body has to fit between the status header (y≈10) and the
  // action footer (kFooterTopY = 54). Subtitle on row 1, then three
  // condition rows at 9 px pitch lands the last row's pill at y=44
  // (top y=37) with 8 px clear of the footer rule.
  drawCentered(d, 17, "Migrate ffat -> 6.9 MB");

  bool battOk = true;
  char battLabel[24];
#ifdef BADGE_HAS_BATTERY_GAUGE
  if (batteryGauge.isReady()) {
    const float pct = batteryGauge.stateOfChargePercent();
    const bool charging = batteryGauge.usbPresent();
    battOk = charging || pct >= 50.0f;
    std::snprintf(battLabel, sizeof(battLabel), "Battery %u%%%s",
                  (unsigned)pct, charging ? " USB" : "");
  } else {
    std::snprintf(battLabel, sizeof(battLabel), "Battery: unknown");
  }
#else
  std::snprintf(battLabel, sizeof(battLabel), "Battery: n/a");
#endif

  const bool app0Ok = ota::canOfferLayoutMigration();  // proxy: not on _ver2
  // canOfferLayoutMigration is the layout precondition; running-slot is
  // re-checked by the C++ function itself. Surface it here so the user
  // is warned before going through the full flow.
  // (We don't have a public getter for the running slot offset; the
  // migrate function will refuse and we'll bounce to kLayoutMigrateError
  // with a clear reason.)
  const bool blobOk = ota::migrationAssetPresent();

  drawConditionRow(d, 27, battLabel, battOk);
  drawConditionRow(d, 36, "Layout: _doom",  app0Ok);
  drawConditionRow(d, 45, "Recovery blob", blobOk);

  // Confirm advances to the recovery/QR step (the C++ function does
  // the same checks before touching flash, so this footer is just an
  // early-warning courtesy).
  OLEDLayout::drawActionFooter(d, "Continue", "Confirm");
}

bool UpdateFirmwareScreen::ensureRecoveryQr() {
  if (recoveryQrTried_) return recoveryQrPixels_ > 0;
  recoveryQrTried_ = true;
  std::memset(recoveryQrBits_, 0, sizeof(recoveryQrBits_));
  recoveryQrPixels_ = 0;

  const uint16_t urlLen = static_cast<uint16_t>(strlen(kRecoveryUrl));
  uint8_t version = 0;
  for (uint8_t v = 1; v <= kQrMaxVersion; v++) {
    if (qrcode_getBufferSize(v) > sizeof(recoveryQrWork_)) break;
    if (urlLen <= kQrByteCapLowEcc[v]) {
      version = v;
      break;
    }
  }
  if (version == 0) return false;

  QRCode qr{};
  if (qrcode_initText(&qr, recoveryQrWork_, version, ECC_LOW,
                      const_cast<char*>(kRecoveryUrl)) != 0) {
    return false;
  }

  // Scale = 1 keeps the QR in the left half of the OLED so we can fit
  // the recovery text on the right. v5 == 37 modules, which scans
  // reliably at the OLED's pixel density when the phone is ~15 cm away.
  const uint8_t modules = qr.size;
  const uint8_t scale = 1;
  const uint8_t pixW = modules * scale;
  const uint16_t rowBytes = (pixW + 7) / 8;
  if (static_cast<uint32_t>(rowBytes) * pixW > sizeof(recoveryQrBits_)) {
    return false;
  }
  for (uint8_t my = 0; my < modules; my++) {
    for (uint8_t mx = 0; mx < modules; mx++) {
      if (!qrcode_getModule(&qr, mx, my)) continue;
      // XBM bit order: LSB = leftmost pixel. Matches HelpScreen's QR
      // generator so QRCodePlate::draw renders both correctly.
      recoveryQrBits_[my * rowBytes + (mx / 8)] |= (1 << (mx % 8));
    }
  }
  recoveryQrPixels_ = pixW;
  return true;
}

void UpdateFirmwareScreen::renderLayoutMigrateRecovery(oled& d) {
  OLEDLayout::drawStatusHeader(d, "IF IT BRICKS");
  d.setFontPreset(FONT_TINY);

  // Left: recovery QR (lazy-built on first paint). Right: terse,
  // actionable steps that match the on-page recovery doc — esptool +
  // the per-release full-flash image. Layout budget: header bottom
  // ≈ y=10, footer top = y=54, so the body has y=11..53 to work with.
  // Plate is sized for QR v6 (41-module recovery URL): 41 px QR + 2 px
  // quiet zone per side = 45 px plate. That leaves ~78 px on the right
  // for text (x=51..127).
  const bool qrOk = ensureRecoveryQr();
  constexpr int kPlate = 45;       // 41-px QR + 2 px quiet zone each side
  constexpr int kPlateX = 2;
  constexpr int kPlateY = 11;      // bottom = 11 + 45 - 1 = 55, clipped at footer
  if (qrOk) {
    // Sub-clip the plate to body region so the bottom row doesn't
    // bleed into the footer rule. The plate is mostly background
    // (the QR centres inside it) so a one-row clip is invisible.
    d.setClipWindow(0, 0, OLEDLayout::kScreenW, OLEDLayout::kFooterTopY - 1);
    QRCodePlate::draw(d, recoveryQrBits_, recoveryQrPixels_,
                      recoveryQrPixels_, kPlateX, kPlateY,
                      kPlate, /*divider=*/false);
    d.setMaxClipWindow();
  } else {
    // URL too long for QR v7 (cap = 154 with ECC_LOW) — extremely
    // unlikely for the compile-time URL but degrade gracefully.
    d.drawStr(4, 26, "(no QR)");
  }

  // Right column: 4 lines × 8 px pitch, baselines y=19..43. All
  // baselines sit above y=53 so nothing clashes with the footer rule.
  // The "scan QR" call to action is implicit from the header + the
  // QR plate dominating the left half.
  const int tx = kPlateX + kPlate + 4;  // 51 — flush against the plate
  d.drawStr(tx, 19, "Recovery:");
  d.drawStr(tx, 28, "USB +");
  d.drawStr(tx, 36, "esptool");
  d.drawStr(tx, 44, "write_flash");
  OLEDLayout::drawActionFooter(d, "Continue", "Confirm");
}

void UpdateFirmwareScreen::renderLayoutMigrateConfirm(oled& d) {
  OLEDLayout::drawStatusHeader(d, "EXPAND PARTITION");
  {
    const char* lines[] = {
        "Rewrites partition tbl",
        "Wipes ffat (all data)",
        "Auto-reboots",
    };
    OLEDLayout::drawTextModal(d, "Final confirmation", lines, 3, 0, /*danger=*/true);
  }
  OLEDLayout::drawActionFooter(d, "DO IT", "Confirm");
}

void UpdateFirmwareScreen::renderLayoutMigrating(oled& d) {
  OLEDLayout::drawStatusHeader(d, "EXPAND PARTITION");
  d.setFontPreset(FONT_TINY);
  drawCentered(d, 22, "Swapping partition table");
  d.setFontPreset(FONT_SMALL);
  drawCentered(d, 36, "DO NOT POWER OFF");
  d.setFontPreset(FONT_TINY);
  drawCentered(d, 50, "Reboot in ~1 sec");
  OLEDLayout::drawNavFooter(d, "Please wait");
}

void UpdateFirmwareScreen::renderLayoutMigrateError(oled& d) {
  OLEDLayout::drawStatusHeader(d, "MIGRATION");
  d.setFontPreset(FONT_TINY);
  const char* reason = "Unknown failure";
  switch (migrationResult_) {
    case ota::MigrationResult::kAlreadyExpanded:
      reason = "Already on _ver2"; break;
    case ota::MigrationResult::kBatteryTooLow:
      reason = "Battery <50% — charge"; break;
    case ota::MigrationResult::kNotRunningFromApp0:
      reason = "Reinstall OTA first"; break;
    case ota::MigrationResult::kEmbedMissing:
      reason = "Build missing blob"; break;
    case ota::MigrationResult::kFlashReadFailed:
    case ota::MigrationResult::kFlashEraseFailed:
    case ota::MigrationResult::kFlashWriteFailed:
      reason = "Flash op failed"; break;
    case ota::MigrationResult::kVerifyFailed:
      reason = "Verify failed (safe)"; break;
    default: break;
  }
  drawCentered(d, 22, "Migration cancelled");
  drawTwoLineError(d, reason);
  OLEDLayout::drawNavFooter(d, "Any:Back");
}

void UpdateFirmwareScreen::renderLayoutWelcome(oled& d) {
  // Two trigger paths, two voices:
  //
  //   migrationJustHappened_ = true: we got here because GUI auto-
  //     popped this screen at boot after the in-place migration's
  //     RTC magic survived ESP.restart() and esp_reset_reason()
  //     came back as ESP_RST_SW. The user explicitly confirmed
  //     the destructive action a few seconds ago — read as a
  //     success confirmation, with the new FS size as proof.
  //
  //   migrationJustHappened_ = false: NVS-comparison heuristic
  //     caught a layout swap that we didn't initiate (user ran
  //     erase_and_flash_expanded.sh or its inverse over USB).
  //     Generic "huh, layout changed" copy is still the right
  //     fit because we can't claim the credit.
  const float partMb = ota::ffatPartitionBytes() / (1024.0f * 1024.0f);
  const bool expanded = ota::ffatUsesExpandedPartitionLayout();

  if (migrationJustHappened_) {
    OLEDLayout::drawStatusHeader(d, "MIGRATION OK");
    char szLine[48];
    std::snprintf(szLine, sizeof(szLine),
                  "FS partition: %.1f MB", partMb);
    const char* lines[] = {
        "Partition table swapped",
        "and verified after reboot.",
        szLine,
    };
    OLEDLayout::drawTextModal(d, "Storage expanded", lines, 3);
  } else {
    OLEDLayout::drawStatusHeader(d,
        expanded ? "EXPANDED LAYOUT" : "DEFAULT LAYOUT");
    char szLine[48];
    std::snprintf(szLine, sizeof(szLine),
                  "FS partition: %.1f MB", partMb);
    const char* line2 = expanded ? "OTA updates work as"
                                 : "Back on the default";
    const char* line3 = expanded ? "normal — same firmware."
                                 : "layout. OTA works.";
    const char* lines[] = {szLine, line2, line3};
    OLEDLayout::drawTextModal(d, "Layout changed", lines, 3);
  }
  OLEDLayout::drawNavFooter(d, "Any:Continue");
}

void UpdateFirmwareScreen::renderReinstallConfirm(oled& d) {
  OLEDLayout::drawStatusHeader(d, "REINSTALL?");

  const char* tag = ota::latestKnownTag();
  char line[48];
  std::snprintf(line, sizeof(line), "%s -> %s", FIRMWARE_VERSION_DISPLAY,
                tag[0] ? tag : "(unknown)");

  // The user requested an install even though we're at-or-ahead of
  // the published tag. Make the consequences explicit so a fat-fingered
  // D-pad press doesn't trigger a multi-MB flash + reboot.
  {
    const char* lines[] = {line, "Already current or newer."};
    OLEDLayout::drawTextModal(d, "Reinstall firmware", lines, 2);
  }
  OLEDLayout::drawActionFooter(d, "Reinstall", "Confirm");
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

  if (phase_ == Phase::kLayoutMigrateInfo) {
    if (e.cancelPressed || e.confirmPressed || e.xPressed || e.yPressed) {
      phase_ = Phase::kIdle;
    }
    return;
  }

  if (phase_ == Phase::kLayoutMigratePrecheck) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      // Re-evaluate the gating conditions on every confirm — battery
      // can drop mid-flow; the active slot doesn't change but cheap to
      // recheck. If anything's wrong, bounce to the error phase with a
      // concrete reason instead of continuing to the recovery screen.
      if (!ota::migrationAssetPresent()) {
        migrationResult_ = ota::MigrationResult::kEmbedMissing;
        phase_ = Phase::kLayoutMigrateError;
        return;
      }
      Haptics::shortPulse();
      phase_ = Phase::kLayoutMigrateRecovery;
    }
    return;
  }

  if (phase_ == Phase::kLayoutMigrateRecovery) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      phase_ = Phase::kLayoutMigrateConfirm;
    }
    return;
  }

  if (phase_ == Phase::kLayoutMigrateConfirm) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      Haptics::off();
      delay(80);
      runLayoutMigration();
    }
    return;
  }

  if (phase_ == Phase::kLayoutMigrating) {
    // The migration call is synchronous and either reboots on success
    // or returns to one of the rollback states. The renderer paints
    // the "do not power off" panel before the call; input is ignored.
    return;
  }

  if (phase_ == Phase::kLayoutMigrateError) {
    if (e.cancelPressed || e.confirmPressed || e.xPressed || e.yPressed) {
      phase_ = Phase::kIdle;
    }
    return;
  }

  if (phase_ == Phase::kLayoutWelcome) {
    if (e.cancelPressed || e.confirmPressed || e.xPressed || e.yPressed) {
      // Ack the deterministic flag NOW — the user has actually
      // seen the panel and pressed a button. Acking clears both
      // the RAM latch and the RTC magic, so subsequent boots
      // (including panic-reboots from later code paths) won't
      // re-show the announce. The weaker NVS-comparison flag
      // was already acked in onEnter().
      if (migrationJustHappened_) {
        ota::acknowledgeMigrationBoot();
        migrationJustHappened_ = false;
      }
      phase_ = Phase::kIdle;
      // Kick the deferred check now that the welcome has been read.
      if (wifiService.isConnected()) {
        runCheck(true);
      }
    }
    return;
  }

  if (phase_ == Phase::kReinstallConfirm) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      Haptics::off();
      delay(100);
      runInstall();
    }
    return;
  }

  if (e.cancelPressed) {
    Haptics::shortPulse();
    gui.popScreen();
    return;
  }

  // X (swap-aware yPressed): expand FAT volume within the current
  // partition, or step into the partition-table migration flow.
  // Migration takes the user through precheck → recovery (QR) →
  // confirm before doing anything destructive. If this build doesn't
  // have the partition blob embedded for some reason we fall back to
  // the legacy USB-only info pane so the user still has an
  // actionable next step.
  if (e.xPressed) {
    if (ota::ffatExpansionAvailable()) {
      Haptics::shortPulse();
      phase_ = Phase::kExpandConfirm;
      return;
    }
    if (ota::canOfferLayoutMigration()) {
      Haptics::shortPulse();
      phase_ = ota::migrationAssetPresent()
                   ? Phase::kLayoutMigratePrecheck
                   : Phase::kLayoutMigrateInfo;
      return;
    }
  }

  // D-pad split: Up = Check, Right = Install. Confirm/Y are unused
  // here so two adjacent presses can't accidentally double-fire the
  // multi-megabyte install. Plan calls for explicit Up/Right rather
  // than the previous toggle-on-Confirm behaviour.
  if (e.upPressed) {
    Haptics::shortPulse();
    Haptics::off();
    delay(100);
    runCheck(true);
    return;
  }
  if (e.rightPressed) {
    // Right-press always offers an install when we have an asset URL
    // cached. If the published tag is strictly newer we go straight
    // into runInstall (most common path); if we're already at-or-ahead
    // of the published version we surface a reinstall confirmation
    // first so the user knows they're flashing the same image.
    const char* tag = ota::latestKnownTag();
    if (!tag[0] || ota::latestKnownAssetUrl()[0] == '\0') {
      // No cached asset — nothing to install. Bounce back to a check
      // so the next press has something to flash.
      Haptics::shortPulse();
      Haptics::off();
      delay(100);
      runCheck(true);
      return;
    }
    Haptics::shortPulse();
    if (ota::updateAvailable()) {
      Haptics::off();
      delay(100);
      runInstall();
    } else {
      phase_ = Phase::kReinstallConfirm;
    }
    return;
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
  installStartMs_ = millis();
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
    OLEDLayout::drawStatusHeader(d, "UPDATE COMPLETE");
    d.setFontPreset(FONT_SMALL);
    drawCentered(d, 32, "Rebooting...");
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 44, "Firmware installed");
    d.sendBuffer();
    delay(800);
    ESP.restart();
  } else {
    phase_ = Phase::kError;
  }
}

void UpdateFirmwareScreen::runLayoutMigration() {
  phase_ = Phase::kLayoutMigrating;
  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  // Paint the "do not power off" panel BEFORE calling the migration
  // function — once the erase starts the user has ~200 ms where
  // they shouldn't unplug, and ESP.restart() never gives us another
  // paint slot on success.
  d.clearBuffer();
  render(d, guiManager);
  d.sendBuffer();
  delay(50);  // give the OLED's I2C a beat to finish the frame

  migrationResult_ = ota::migrateToExpandedLayout();
  // On success the function does not return — ESP.restart() fires
  // after the verify passes. Anything below this is the rollback /
  // refusal path; surface a concrete reason and let the user back
  // out to the idle screen.
  phase_ = Phase::kLayoutMigrateError;
}
