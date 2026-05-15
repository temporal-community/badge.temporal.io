#include "OLEDLayout.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "../api/WiFiService.h"
#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../ble/BleBeaconScanner.h"
#endif
#include "../infra/BadgeConfig.h"
#include "../hardware/Power.h"
#include "../hardware/IMU.h"
#include "../hardware/oled.h"
#include "BatteryIcons.h"
#include "ButtonGlyphs.h"
#include "UIFonts.h"
#include "StarIcon.h"
#include "UpdateIcon.h"
#include "WifiIcon.h"
#include "../ota/BadgeOTA.h"

extern BatteryGauge batteryGauge;
extern IMU imu;

namespace OLEDLayout {
namespace {
constexpr uint8_t kWifiIconW = WifiIcon::kWidth;   // 11 px (ported from QA-Firmware)
constexpr uint8_t kBatteryIconW = 12;
constexpr uint8_t kStatusGap = 3;
constexpr uint16_t kFooterScrollDelayMs = 1200;
constexpr uint16_t kFooterScrollMs = 35;
constexpr uint8_t kFooterTextX = 1;
constexpr uint8_t kFooterScrollGapPx = 12;

// Painted at both screen edges by every footer rule; the rule itself
// is shortened 3 px on each side to leave a clean gap between line and
// bookend stars. The stars pin to the bottom of the screen (y=57..63)
// regardless of which footer-rule variant called this — keeps both
// footer styles visually consistent.
void drawFooterStars(oled& d, uint8_t /*lineY*/) {
  const uint8_t topY = kScreenH - StarIcon::kHeight;
  d.setDrawColor(1);
  d.drawXBM(0, topY, StarIcon::kWidth, StarIcon::kHeight, StarIcon::kBits);
  d.drawXBM(kScreenW - StarIcon::kWidth, topY,
            StarIcon::kWidth, StarIcon::kHeight, StarIcon::kBits);
}

ButtonGlyphs::Button confirmButton() {
  return badgeConfig.get(kSwapConfirmCancel) != 0
             ? ButtonGlyphs::Button::B
             : ButtonGlyphs::Button::A;
}

void formatSfTime(char* out, size_t outCap) {
  if (!out || outCap == 0) return;
  std::snprintf(out, outCap, "--:--");

  time_t now = 0;
  if (!wifiService.currentTime(&now)) return;

  struct tm local = {};
  localtime_r(&now, &local);
  int hour12 = local.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  // 12-hour clock without an AM/PM suffix; zero-padded to "00:00" so
  // the pill's width doesn't jiggle as the hour ticks past midnight or
  // dips below 10.
  std::snprintf(out, outCap, "%02d:%02d", hour12, local.tm_min);
}

void drawWifiIcon(oled& d, int x, int y, bool connected, bool busy) {
  if (!connected && !busy) return;

  d.setDrawColor(1);
  const unsigned char* bits;
  if (!connected) {
    bits = WifiIcon::kBitsNoWifi;
  } else {
    // Map signal level to the matching arc count. Level 0 here means
    // "associated but no RSSI yet" — render as 1 bar so the icon is
    // never blank while we believe the radio is up.
    const uint8_t level = wifiService.signalLevel();
    if (level >= 3)      bits = WifiIcon::kBits;
    else if (level == 2) bits = WifiIcon::kBitsLevel2;
    else                 bits = WifiIcon::kBitsLevel1;
  }
  d.drawXBM(x, y, WifiIcon::kWidth, WifiIcon::kHeight, bits);

  if (connected && busy && ((millis() / 250) & 1)) {
    // Blink a small dot under the icon while a sync is in flight.
    d.drawBox(x + WifiIcon::kWidth - 2, y + WifiIcon::kHeight, 2, 2);
  }
}

// BLE-scan glyph that takes the place of the WiFi icon while the
// MAP app is scanning for venue beacons. 7×7 hollow circle with a
// 1 px centre dot — visually distinct from the arched WiFi glyph
// but fits the same right-aligned slot in the status header.
//
// `hasFix` flips the cue between "listening but nothing heard"
// (icon + diagonal slash, mirroring the WiFi-disconnect cue) and
// "beacon in range" (clean glyph). Gives the user an at-a-glance
// "BLE OFFLINE / ONLINE" without claiming a separate slot.
void drawBleScanIcon(oled& d, int x, int y, bool hasFix) {
  d.setDrawColor(1);
  static const uint8_t kBleW = 7;
  static const uint8_t kBleH = 7;
  static const uint8_t kBleBits[] = {
      0x1c,  //  ░░###░░
      0x22,  //  ░#░░░#░
      0x41,  //  #░░░░░#
      0x49,  //  #░░#░░#  (centre dot)
      0x41,  //  #░░░░░#
      0x22,  //  ░#░░░#░
      0x1c,  //  ░░###░░
  };
  // Centre the 7×7 glyph horizontally inside the WifiIcon's 11×6
  // footprint so the right-edge alignment matches the WiFi icon's
  // visual bounds.
  const int gx = x + (WifiIcon::kWidth - kBleW) / 2;
  const int gy = y - 1;  // shift up 1 px to fit 7 px tall in 6 px slot
  d.drawXBM(gx, gy, kBleW, kBleH, kBleBits);
  if (!hasFix) {
    // XOR-cut a diagonal slash so the no-beacons cue matches the
    // WiFi icon's disconnected treatment.
    d.setDrawColor(0);
    d.drawLine(gx, gy + kBleH - 1, gx + kBleW - 1, gy);
    d.setDrawColor(1);
  }
}

// State-aware battery icon, ported from the reference firmware's header
// element. The 10×6 outline is fixed; the 7×4 inner slot at (x+2, y+1)
// shows one of:
//
//   • warning chevrons      — battery missing, gauge unready, or < 5 %
//     (unplugged only; never while USB/charger status is active)
//   • charging bolt         — USB power present + active charge
//   • solid full            — USB power present, charge complete
//   • two-digit pixel font  — 5..99 % on battery (tens / divider / units)
void drawBatteryIcon(oled& d, int x, int y, bool ready, float pct) {
  // Always-on; the low-battery flash was disabled because it was
  // jarring without adding information beyond what the percent
  // numerals (and the warning glyph at <5 %) already convey.
  d.setDrawColor(1);
  d.drawXBM(x, y, BatteryIcons::kBatW, BatteryIcons::kBatH,
            BatteryIcons::kBatBits);

  const int fillX = x + 2;
  const int fillY = y + 1;

  // Helper: paint the two-digit percent into the 7px-wide fill slot.
  auto drawPercentDigits = [&]() {
    const uint8_t tens  = batteryGauge.displayTens();
    const uint8_t units = batteryGauge.displayUnits();
    d.drawXBM(fillX, fillY,
              BatteryIcons::kNumW, BatteryIcons::kNumH,
              BatteryIcons::kNumBits[tens]);
    d.drawVLine(fillX + 3, fillY, BatteryIcons::kNumH);
    d.drawXBM(fillX + 4, fillY,
              BatteryIcons::kNumW, BatteryIcons::kNumH,
              BatteryIcons::kNumBits[units]);
  };

  // Helper: paint the inverted exclamation warning glyph.
  auto drawExclamation = [&]() {
    d.setDrawColor(1);
    d.drawBox(fillX, fillY, 7, 4);
    d.setDrawColor(0);
    d.drawXBM(fillX + 3, fillY,
              BatteryIcons::kExclamationW, BatteryIcons::kExclamationH,
              BatteryIcons::kExclamationBits);
    d.setDrawColor(1);
  };

  // Shared alternation cadence: ~1.6 s cycle, primary glyph holds for
  // 900 ms then the digits show for 700 ms. Phase derives from millis()
  // so every screen rendering this icon stays in sync.
  constexpr uint32_t kCyclePeriodMs = 1600;
  constexpr uint32_t kGlyphHoldMs   = 900;
  const bool showGlyph = (millis() % kCyclePeriodMs) < kGlyphHoldMs;

  const bool charging = batteryGauge.isCharging();
  const bool usb      = batteryGauge.usbPresent();

  // External power / charge FSM from the gauge — show bolt or full-charge
  // glyph on the clock phase even before the first open-circuit probe lands
  // (ready/pct may still be indeterminate).
  if (charging || usb) {
    const bool pctKnown = pct > 0.f;
    if (pctKnown && !showGlyph) {
      drawPercentDigits();
      return;
    }
    // Unknown or still-reading 0%: never alternate to "00" — keep bolt/full.
    if (charging) {
      d.drawXBM(fillX, fillY,
                BatteryIcons::kChargingW, BatteryIcons::kChargingH,
                BatteryIcons::kChargingBits);
    } else {
      d.drawXBM(fillX, fillY,
                BatteryIcons::kFullW, BatteryIcons::kFullH,
                BatteryIcons::kFullBits);
    }
    return;
  }

  if (!ready || pct <= 0.f) {
    // Gauge unready, OR ready but no trustworthy sample yet (unplugged).
    drawExclamation();
    return;
  }

  if (pct < 5.f) {
    // Critical battery: alternate the exclamation warning with the
    // numeric percent so the user can read just how low it actually is
    // ("3 %" vs "1 %" matters), without losing the at-a-glance alert.
    if (showGlyph) drawExclamation();
    else           drawPercentDigits();
    return;
  }

  // 5..99 % on battery (no external power): always show digits.
  drawPercentDigits();
}

// Renders a short string as an "artificial horizon" — each character's
// baseline is offset by IMU pitch (vertical translation) and roll
// (slope across the string). Used by the centered time pill so the
// "--:--" placeholder (or live "HH:MM") sloshes with the badge tilt.
//
// Smoothing is intentionally minimal — the IMU service already runs an
// EMA, so we only add a featherweight follow-up filter to suppress
// 200 Hz sample-to-sample noise without dampening the responsiveness
// the artificial-horizon effect needs.
//
// When a glyph overlaps the pill bottom ornament row, paint a 1 px black
// halo (8-neighbour offsets) in draw-color 0, then redraw all glyphs in
// color 1. The halo pass runs for the *entire* string before any
// foreground pass — otherwise a later glyph's outline would carve into
// an earlier character's white pixels (smallsimple_tr is transparent
// and shows up as streaks or missing bars inside the numerals).
void drawHorizonText(oled& d, int baseX, int baseY, const char* text,
                     int notchBottomY = -1, int pillFrameX = 0,
                     int pillFrameW = 0) {
  if (!text || !text[0]) return;
  // User-toggleable in Settings → Clock → Horizon Clock. When off,
  // fall through to a flat draw (no IMU sampling, no per-glyph offsets,
  // no inter-glyph gap) so the time pill renders the way every other
  // text element does.
  const bool enabled = badgeConfig.get(kHorizonClock) != 0;
  if (!enabled || !imu.isReady()) {
    d.drawStr(baseX, baseY, text);
    return;
  }

  static float emaXmg = 0.f;
  static float emaYmg = 0.f;
  static bool  emaInit = false;
  const float xmgRaw = imu.tiltXMg();  // roll axis (left/right)
  const float ymgRaw = imu.tiltYMg();  // pitch axis (forward/back)
  if (!emaInit) {
    emaXmg = xmgRaw;
    emaYmg = ymgRaw;
    emaInit = true;
  }
  // Very light follow-up filter — far more responsive than the
  // IMU's own ~0.88 internal smoothing. Snaps within a few frames.
  constexpr float kAlpha = 0.55f;
  emaXmg += kAlpha * (xmgRaw - emaXmg);
  emaYmg += kAlpha * (ymgRaw - emaYmg);

  // Map mg → pixels. ±1000 mg ≈ ±90° tilt; cap so glyphs don't drift
  // far enough to collide with the menu grid below the header.
  constexpr float kPxPerG       = 6.0f;   // pitch translation
  constexpr float kRollEdgePxG  = 6.0f;   // roll slope at left/right edge
  constexpr int   kMaxOffsetUp   = 3;
  constexpr int   kMaxOffsetDown = 2;

  // Pitching the badge "back" (top toward the user) drops the horizon —
  // negate so the digits fall when the user looks down at the badge.
  const float pitchPx = -emaYmg * (kPxPerG / 1000.f);
  const float rollPxPerG = kRollEdgePxG / 1000.f;

  const int totalW = d.getStrWidth(text);
  if (totalW <= 0) {
    d.drawStr(baseX, baseY, text);
    return;
  }
  const float halfW  = totalW * 0.5f;
  const float midX   = baseX + halfW;

  constexpr int kMaxHorizonStr = 16;
  {
    size_t len = 0;
    while (text[len]) {
      ++len;
      if (len > kMaxHorizonStr) {
        d.drawStr(baseX, baseY, text);
        return;
      }
    }
  }

  // Pull each glyph apart by 1 px so adjacent dashes in the unset
  // "--:--" placeholder read as four discrete horizon segments instead
  // of one continuous slanted bar. Same gap applies to the digit
  // glyphs when a real time is showing — keeps the artificial-horizon
  // segmentation visually consistent across both states.
  constexpr int kGlyphGapPx = 1;

  char glyph[2] = {0, 0};
  const int fontAsc = d.getAscent();
  const int fontDesc = d.getDescent();

  int  xs[kMaxHorizonStr];
  int  baselines[kMaxHorizonStr];
  bool stroked[kMaxHorizonStr];
  size_t nGlyph = 0;

  int cursorX = baseX;
  for (size_t i = 0; text[i]; ++i) {
    glyph[0] = text[i];
    const int gw = d.getStrWidth(glyph);
    const float cx = cursorX + gw * 0.5f;
    // Roll: characters left of center rise, right of center fall when
    // the badge rolls right (positive emaXmg) — flip sign to taste.
    const float rollOffset = ((cx - midX) / halfW) * (emaXmg * rollPxPerG);
    int yOff = static_cast<int>(lroundf(pitchPx + rollOffset));
    if (yOff < -kMaxOffsetUp)   yOff = -kMaxOffsetUp;
    if (yOff >  kMaxOffsetDown) yOff =  kMaxOffsetDown;
    const int baselineAdj = baseY + yOff;

    const int glyphTop = baselineAdj - fontAsc;
    const int glyphBot = baselineAdj - fontDesc;
    const bool spansChromeRow =
        (notchBottomY >= 0 && pillFrameW > 6 && glyphTop <= notchBottomY &&
         notchBottomY <= glyphBot);

    xs[nGlyph]       = cursorX;
    baselines[nGlyph] = baselineAdj;
    stroked[nGlyph]   = spansChromeRow;
    ++nGlyph;

    cursorX += gw + kGlyphGapPx;
  }

  // If any glyph needs the bottom-row halo, stroke the whole string.
  // Otherwise a stroked neighbor paints color-0 in another glyph's
  // transparent margin; that glyph's WHITE pass does not touch those
  // pixels (_tr keeps holes), which reads as random bright specks.
  bool anyChrome = false;
  for (size_t g = 0; g < nGlyph; ++g) {
    if (stroked[g]) {
      anyChrome = true;
      break;
    }
  }
  if (anyChrome) {
    for (size_t g = 0; g < nGlyph; ++g) stroked[g] = true;
  }

  // Crop the sloshing glyphs (and halo offsets) to the time-pill interior
  // so nothing paints past the frame or into the rounded corners / header
  // rule region. Matches drawStatusHeaderImpl: top at y=0, bottom at the
  // pill's bottom row, horizontal inset for the 1 px RFrame stroke.
  constexpr int kTimePillClipTopY = 0;
  bool clipPill = (pillFrameW > 6 && notchBottomY >= 0);
  int clipL = 0;
  int clipR = 0;
  if (clipPill) {
    clipL = pillFrameX + 1;
    clipR = pillFrameX + pillFrameW - 2;
    if (clipR < clipL) {
      clipPill = false;
    }
  }
  if (clipPill) {
    d.setClipWindow(clipL, kTimePillClipTopY, clipR, notchBottomY);
  }

  // _tr fonts are defined for transparent mode; solid mode (0) can
  // leave garbage pixels when compositing halo + glyphs.
  d.setFontMode(1);

  d.setDrawColor(0);
  for (size_t g = 0; g < nGlyph; ++g) {
    if (!stroked[g]) continue;
    glyph[0] = text[g];
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        d.drawStr(xs[g] + dx, baselines[g] + dy, glyph);
      }
    }
  }

  d.setDrawColor(1);
  for (size_t g = 0; g < nGlyph; ++g) {
    glyph[0] = text[g];
    d.drawStr(xs[g], baselines[g], glyph);
  }

  if (clipPill) {
    d.setMaxClipWindow();
  }
  d.setFontMode(0);
}

void copyHeaderText(char* out, size_t cap, const char* src) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!src) return;

  size_t j = 0;
  for (size_t i = 0; src[i] && j < cap - 1; i++) {
    const uint8_t c = static_cast<uint8_t>(src[i]);
    if (c >= 32 && c <= 126) {
      out[j++] = static_cast<char>(c);
    } else if (c == '\n' || c == '\r' || c == '\t') {
      out[j++] = ' ';
    }
  }
  out[j] = '\0';
}

void copyFirstHeaderToken(char* out, size_t cap, const char* src) {
  copyHeaderText(out, cap, src);
  if (!out || cap == 0) return;
  for (size_t i = 0; out[i]; i++) {
    if (out[i] == ' ') {
      out[i] = '\0';
      return;
    }
  }
}
}  // namespace

void fitText(oled& d, char* text, size_t cap, int maxW) {
  if (!text || cap == 0) return;
  text[cap - 1] = '\0';
  if (maxW <= 0) {
    text[0] = '\0';
    return;
  }
  size_t len = std::strlen(text);
  while (len > 0 && d.getStrWidth(text) > maxW) {
    text[--len] = '\0';
  }
}

void drawStatusHeaderImpl(oled& d, const char* title, bool firstNameFallback);

void drawHeader(oled& d, const char* title, const char* /*right_legacy*/) {
  // Every app's header now uses the same status-bar layout — title
  // on the left, then the wall-clock time and battery gauge stacked
  // on the right. The legacy `right` slot is
  // ignored so existing call sites pick up the time+icons treatment
  // automatically without each screen having to opt in.
  drawStatusHeader(d, title);
}

void drawStatusHeader(oled& d, const char* title) {
  drawStatusHeaderImpl(d, title, false);
}

void drawNameStatusHeader(oled& d, const char* name) {
  drawStatusHeaderImpl(d, name, true);
}

void drawStatusHeaderImpl(oled& d, const char* title, bool firstNameFallback) {
  d.setFont(UIFonts::kText);
  d.setDrawColor(1);

  char timeBuf[8] = {};
  const bool syncing = wifiService.isAutoSyncInProgress();
  formatSfTime(timeBuf, sizeof(timeBuf));
  // drawHorizonText() inserts a 1 px gap between glyphs when the
  // artificial-horizon effect is enabled; account for it in the pill
  // width so the rounded frame still wraps the full string. Toggle
  // off → no gap, no inflation.
  int timeW = d.getStrWidth(timeBuf);
  if (badgeConfig.get(kHorizonClock) != 0 && imu.isReady()) {
    for (size_t i = 0; timeBuf[i]; ++i) {
      if (timeBuf[i + 1]) timeW += 1;
    }
  }

  // Battery on the right edge, optional network activity icon to its left,
  // optional firmware-update glyph immediately to the left of WiFi.
  const int batX  = kScreenW - kBatteryIconW;
  const int wifiX = batX - kStatusGap - kWifiIconW;
  const int updX  = wifiX - kStatusGap - UpdateIcon::kWidth;
  if (ota::updateAvailable()) {
    d.drawXBM(updX, 1, UpdateIcon::kWidth, UpdateIcon::kHeight,
              UpdateIcon::kBits);
  }

  // Time pill is centered horizontally on the screen, bordered by a
  // bottom-only rounded rectangle (no top edge so it reads as a tab
  // hanging off the top of the display).
  constexpr int kPillPadX  = 3;
  constexpr int kPillTopY  = 0;
  constexpr int kPillBotY  = 7;     // 1 px tighter Y padding around the time
  const int     pillW      = timeW + kPillPadX * 2;
  const int     pillX      = (kScreenW - pillW) / 2;
  const int     pillH      = kPillBotY - kPillTopY + 1;  // 8 px

  // Title gets whatever room is left of the time pill. If it still
  // overflows after the optional first-name fallback, marquee-scroll
  // it horizontally so the full name is visible over time instead of
  // being silently truncated into the time-pill region.
  char titleBuf[32] = {};
  copyHeaderText(titleBuf, sizeof(titleBuf), title);
  const int titleMaxW = pillX - 2;
  if (firstNameFallback && d.getStrWidth(titleBuf) > titleMaxW) {
    char firstName[32] = {};
    copyFirstHeaderToken(firstName, sizeof(firstName), title);
    if (firstName[0]) {
      std::snprintf(titleBuf, sizeof(titleBuf), "%s", firstName);
    }
  }
  const int titleW = d.getStrWidth(titleBuf);
  if (titleW <= titleMaxW) {
    d.drawStr(0, 6, titleBuf);
  } else {
    constexpr uint32_t kPxStepMs = 50;
    constexpr int      kWrapGap  = 12;
    const int loop   = titleW + kWrapGap;
    const int offset = (loop > 0) ? (millis() / kPxStepMs) % loop : 0;
    d.setClipWindow(0, 0, titleMaxW - 1, 7);
    d.drawStr(0 - offset, 6, titleBuf);
    d.drawStr(0 - offset + loop, 6, titleBuf);
    d.setMaxClipWindow();
  }

  // Bottom-only rounded rectangle: drawRFrame paints the full outline,
  // then we erase the top edge to give the pill a "tab" silhouette.
  d.drawRFrame(pillX, kPillTopY, pillW, pillH, 2);
  d.setDrawColor(0);
  d.drawHLine(pillX + 1, kPillTopY, pillW - 2);
  d.setDrawColor(1);

  drawHorizonText(d, pillX + kPillPadX, 6, timeBuf, kPillBotY, pillX,
                  pillW);

  // While a BLE scan is active, the venue-proximity glyph can stand in
  // for the network slot. Otherwise the offline firmware leaves this
  // slot empty until explicit MicroPython networking succeeds.
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  if (BleBeaconScanner::isScanning()) {
    drawBleScanIcon(d, wifiX, 1, BleBeaconScanner::hasFix());
  } else {
    drawWifiIcon(d, wifiX, 1, wifiService.networkIndicatorActive(), syncing);
  }
#else
  drawWifiIcon(d, wifiX, 1, wifiService.networkIndicatorActive(), syncing);
#endif
  const bool batteryReady = batteryGauge.isReady();
  drawBatteryIcon(d, batX, 1, batteryReady,
                  batteryGauge.stateOfChargePercent());
}

void drawNetworkIndicator(oled& d, int x, int y, bool busy) {
  drawWifiIcon(d, x, y, wifiService.networkIndicatorActive(), busy);
}

void drawHeaderRule(oled& d, uint8_t y) {
  // Header rule retired — kept as a no-op so screens that still call
  // drawHeaderRule() compile and run without painting a divider.
  (void)d;
  (void)y;
}

void drawNavFooter(oled& d, const char* text, const char* actionLabel) {
  d.setDrawColor(1);
  // Divider sits 2 px below kFooterTopY (nav-only — star and game
  // footers stay where they are).
  const uint8_t y = kFooterTopY + 2;
  d.drawHLine(0, y, kScreenW);

  // Optional right-aligned action chip — same chrome as
  // drawActionFooter so the two helpers visually agree.
  int textClipR = kScreenW;
  if (actionLabel && actionLabel[0]) {
    d.setFont(UIFonts::kText);
    const ButtonGlyphs::Button button = confirmButton();
    const int actionW = ButtonGlyphs::measureHint(d, button, actionLabel);
    const int actionX = 121 - actionW;
    const int actionDividerX = actionX - 4;
    d.drawVLine(actionDividerX, kFooterTopY + 1, kScreenH - kFooterTopY - 1);
    ButtonGlyphs::drawHint(d, actionX, kFooterBaseY, button, actionLabel);
    textClipR = actionDividerX - 2;
  }

  if (text && text[0]) {
    d.setFontPreset(FONT_TINY);
    if (textClipR < kScreenW) {
      d.setClipWindow(0, kFooterTopY + 1, textClipR, kScreenH);
      d.drawStr(0, kFooterTextBaseY, text);
      d.setMaxClipWindow();
    } else {
      d.drawStr(0, kFooterTextBaseY, text);
    }
  }
}

void drawActionFooter(oled& d, const char* text, const char* actionLabel) {
  if (!text) text = "";
  if (!actionLabel) actionLabel = "";

  static const char* lastText = nullptr;
  static const char* lastAction = nullptr;
  static uint32_t footerScrollStartMs = 0;
  static uint32_t footerLastTickMs = 0;
  static int16_t footerScrollX = 0;

  if (text != lastText || actionLabel != lastAction) {
    lastText = text;
    lastAction = actionLabel;
    footerScrollStartMs = millis() + kFooterScrollDelayMs;
    footerLastTickMs = 0;
    footerScrollX = 0;
  }

  d.setDrawColor(1);
  d.drawHLine(0, kFooterTopY, kScreenW);
  d.setFont(UIFonts::kText);

  const bool hasAction = actionLabel[0] != '\0';
  int textClipW = kScreenW - kFooterTextX;
  if (hasAction) {
    const ButtonGlyphs::Button button = confirmButton();
    const int actionW = ButtonGlyphs::measureHint(d, button, actionLabel);
    const int actionX = 121 - actionW;
    const int actionDividerX = actionX - 4;
    textClipW = actionDividerX - kFooterTextX - 2;
    d.drawVLine(actionDividerX, kFooterTopY + 1, kScreenH - kFooterTopY - 1);
    ButtonGlyphs::drawHint(d, actionX, kFooterBaseY, button, actionLabel);
  }

  if (textClipW <= 0 || !text[0]) {
    return;
  }

  d.setClipWindow(kFooterTextX, kFooterTopY + 1,
                  kFooterTextX + textClipW, kScreenH);

  const int textW = d.getStrWidth(text);
  if (textW <= textClipW) {
    d.drawStr(kFooterTextX, kFooterTextBaseY, text);
  } else {
    const uint32_t now = millis();
    if (now < footerScrollStartMs) {
      d.drawStr(kFooterTextX, kFooterTextBaseY, text);
    } else {
      if (footerLastTickMs == 0 ||
          (now - footerLastTickMs) >= kFooterScrollMs) {
        footerLastTickMs = now;
        footerScrollX--;
        const int period = textW + kFooterScrollGapPx;
        if (footerScrollX <= -period) footerScrollX = 0;
      }
      const int textX = kFooterTextX + footerScrollX;
      d.drawStr(textX, kFooterTextBaseY, text);
      const int duplicateX = textX + textW + kFooterScrollGapPx;
      if (duplicateX < kFooterTextX + textClipW) {
        d.drawStr(duplicateX, kFooterTextBaseY, text);
      }
    }
  }

  d.setMaxClipWindow();
}

// Internal: the body of drawStarFooter / drawStarFooterNoLine. `withRule`
// controls whether the horizontal divider between the bookend stars is
// drawn — kept identical for both variants otherwise so the text
// centring / marquee behaviour stays in lock-step.
static void drawStarFooterImpl(oled& d, const char* text, bool withRule) {
  d.setDrawColor(1);
  // Divider sits 1 px higher than the other footer styles so the
  // bookend stars (which centre on the divider y) read cleanly above
  // the descender row of the text below.
  const uint8_t y = kFooterTopY + 1;
  if (withRule) {
    // Rule sits between the bookend stars; insetting 8 px each side
    // keeps the line clear of the star glyphs and matches the text
    // band's padding.
    d.drawHLine(StarIcon::kWidth + 1, y,
                kScreenW - 2 * (StarIcon::kWidth + 1));
  }
  drawFooterStars(d, y);
  if (!text || !text[0]) return;

  d.setFontPreset(FONT_TINY);
  constexpr int kPadX = 8;
  constexpr int availW = kScreenW - 2 * kPadX;     // 112 px
  const int textW = d.getStrWidth(text);

  if (textW <= availW) {
    // Centred, static.
    const int tx = kPadX + (availW - textW) / 2;
    d.drawStr(tx, kFooterTextBaseY, text);
    return;
  }

  // Marquee scroll. Per-text static state — resets when the caller
  // passes a different pointer, which matches how the home grid's
  // description strip swaps strings on cursor moves. The 16 px
  // wrap-gap plus a duplicate draw produces a continuous loop.
  static const char* lastText = nullptr;
  static uint32_t    startMs  = 0;
  if (text != lastText) {
    lastText = text;
    startMs  = millis();
  }
  constexpr uint32_t kStartDelayMs = 700;
  constexpr uint32_t kPxPerStepMs  = 35;
  constexpr int      kWrapGap      = 16;
  const uint32_t now = millis();
  int offset = 0;
  if (now - startMs > kStartDelayMs) {
    const uint32_t elapsed = now - startMs - kStartDelayMs;
    offset = (elapsed / kPxPerStepMs) % (textW + kWrapGap);
  }
  d.setClipWindow(kPadX, kFooterTopY, kPadX + availW - 1, kScreenH - 1);
  d.drawStr(kPadX - offset,                       kFooterTextBaseY, text);
  d.drawStr(kPadX - offset + textW + kWrapGap,    kFooterTextBaseY, text);
  d.setMaxClipWindow();
}

void drawStarFooter(oled& d, const char* text) {
  drawStarFooterImpl(d, text, /*withRule=*/true);
}

void drawStarFooterNoLine(oled& d, const char* text) {
  drawStarFooterImpl(d, text, /*withRule=*/false);
}

void drawGameFooter(oled& d, int x, int w) {
  // Action-row rule above the 10-px button-glyph row that starts at
  // y=53 (baseline kFooterBaseY=62 minus kGlyphH=10 plus 1). Avoids
  // the rule slicing through the middle of drawFooterActions /
  // drawFooterHint glyphs. No bookend stars on this style.
  if (w <= 0) return;
  d.setDrawColor(1);
  d.drawHLine(x, kFooterRuleY, w);
}

ModalChrome drawModalChrome(oled& d, int boxX, int boxY, int boxW, int boxH,
                            const char* title, const char* subhead,
                            uint32_t scrollStartMs,
                            int actionStripH, bool frame) {
  if (!title) title = "";
  if (subhead && !subhead[0]) subhead = nullptr;

  constexpr int kRadius   = 4;
  // Title strip height — sized for UIFonts::kText (smallsimple,
  // ~7 px tall + 1 px clearance for descenders) so the modal title
  // matches the rest of the UI's text font instead of the legacy
  // 4×6 chrome font. Bumped from 7 → 9 to give the larger glyphs
  // room without clipping. The title divider sits 1 px below the
  // strip so the strip-bottom and divider don't collide.
  constexpr int kTitleH   = 9;
  // When the modal owns the entire screen, drop the rounded box +
  // outer frame — the title strip + body + bottom divider are the
  // chrome. Otherwise the modal sits inset and we paint a rounded
  // frame around it. Interior bookkeeping shifts so full-screen
  // modals reach all the way to the screen edges.
  const int interiorX     = frame ? (boxX + 1) : boxX;
  const int interiorY     = frame ? (boxY + 1) : boxY;
  const int interiorW     = frame ? (boxW - 2) : boxW;
  const int titleBaseY    = interiorY + kTitleH - 1;
  const int titleDivY     = interiorY + kTitleH;

  // Wipe everything the box covers (incl. a 2 px overscan above) so
  // whatever is drawn underneath doesn't bleed through the corners or
  // the inverted title strip. Clamp to the screen so a y=0 modal
  // doesn't write into negative rows.
  d.setDrawColor(0);
  const int wipeY = boxY > 2 ? (boxY - 2) : 0;
  const int wipeH = boxY > 2 ? (boxH + 3) : (boxH + 1);
  d.drawBox(boxX, wipeY, boxW, wipeH);

  if (frame) {
    // Black interior + white frame
    d.drawRBox(boxX, boxY, boxW, boxH, kRadius);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, kRadius);
  } else {
    d.setDrawColor(1);
  }

  // Inverted title strip. Only paint the corner-pixel knockout when
  // we're inside a rounded frame — for full-screen modals the strip
  // can run flush to the screen edges.
  d.drawBox(interiorX, interiorY, interiorW, kTitleH);
  if (frame) {
    d.setDrawColor(0);
    d.drawPixel(interiorX,                  interiorY);
    d.drawPixel(interiorX + interiorW - 1,  interiorY);
  } else {
    d.setDrawColor(0);
  }

  // Same font as the rest of the UI (Schedule list, Settings, etc.)
  // so modal title + body have a consistent voice.
  d.setFont(UIFonts::kText);

  // Optional subhead — sticky, right-aligned in the title strip with
  // a 1-px vertical divider on its left.
  int titleClipR = interiorX + interiorW - 3;
  if (subhead) {
    const int sw = d.getStrWidth(subhead);
    const int sx = interiorX + interiorW - 2 - sw;
    d.drawStr(sx, titleBaseY, subhead);
    const int divX = sx - 2;
    d.drawVLine(divX, interiorY, kTitleH);
    titleClipR = divX - 2;
  }

  // Scrolling title — bounce out to the left, then back in from the
  // right, repeating. Same pattern as the schedule detail modal.
  const int titleW    = d.getStrWidth(title);
  const int titleAvail = titleClipR - (interiorX + 2);
  int tx;
  if (titleW <= titleAvail) {
    tx = interiorX + 2;
  } else {
    constexpr uint32_t kPxStepMs = 35;
    const uint32_t now = millis();
    const uint32_t elapsed =
        (now >= scrollStartMs) ? (now - scrollStartMs) : 0;
    const int scrollOut = titleW + 2;
    const int scrollIn  = titleAvail;
    const int range     = scrollOut + scrollIn;
    if (range <= 0) {
      tx = interiorX + 2;
    } else {
      const uint32_t period = static_cast<uint32_t>(range) * kPxStepMs;
      const int tick =
          static_cast<int>((elapsed % period) / kPxStepMs);
      tx = (tick < scrollOut) ? (interiorX + 2 - tick)
                              : (titleClipR - (tick - scrollOut));
    }
  }
  d.setClipWindow(interiorX, interiorY, titleClipR, interiorY + kTitleH - 1);
  d.drawStr(tx, titleBaseY, title);
  d.setMaxClipWindow();

  d.setDrawColor(1);
  d.drawHLine(interiorX, titleDivY, interiorW);

  // Optional bottom action strip — paint a divider above where the
  // chips will land. Body region returned to the caller is shrunk
  // accordingly so body content can't bleed into the chip row.
  int bodyBotY = boxY + boxH - 2;
  if (actionStripH > 0) {
    const int actionDivY = boxY + boxH - 1 - actionStripH;
    if (actionDivY > titleDivY + 2) {
      d.drawHLine(interiorX, actionDivY, interiorW);
      bodyBotY = actionDivY - 1;
    }
  }

  return ModalChrome{
      interiorX,
      interiorY,
      interiorW,
      kTitleH,
      titleDivY + 1,
      bodyBotY,
  };
}

void clearFooter(oled& d) {
  // All current callers follow the wipe with drawFooterActions /
  // button-glyph hints. Repaint the game rule so the band is
  // consistent without each caller having to opt back in.
  d.setDrawColor(0);
  d.drawBox(0, kFooterRuleY, kScreenW, kScreenH - kFooterRuleY);
  drawGameFooter(d);
}

void setFooterClip(oled& d) {
  d.setClipWindow(0, kFooterTextTopY, kScreenW, kScreenH);
}

void drawSelectedRow(oled& d, int y, int h, int x, int w) {
  d.setDrawColor(1);
  d.drawBox(x, y, w, h);
  // The legacy 1-px black knockout vline at x+1 was visible alongside
  // SettingsScreen's rounded-outline selection (which erases + repaints
  // the row) and read as a "scrolling line" tracking the cursor.
  // Removed — selection is communicated by the filled box / outline only.
}

void drawBusySpinner(oled& d, int cx, int cy, uint8_t phase) {
  static constexpr int8_t kPts[8][2] = {
      {0, -3}, {2, -2}, {3, 0}, {2, 2},
      {0, 3}, {-2, 2}, {-3, 0}, {-2, -2},
  };
  phase &= 7;
  d.setDrawColor(1);
  for (uint8_t i = 0; i < 8; i++) {
    const int px = cx + kPts[i][0];
    const int py = cy + kPts[i][1];
    if (i == phase) {
      d.drawBox(px - 1, py - 1, 3, 3);
    } else if (((i + 1) & 7) == phase || ((i + 7) & 7) == phase) {
      d.drawBox(px, py, 2, 2);
    } else {
      d.drawPixel(px, py);
    }
  }
}

void drawStatusBox(oled& d, int x, int y, int w, int h, const char* title,
                   const char* detail, bool busy) {
  if (w <= 4 || h <= 4) return;
  d.setFont(UIFonts::kText);
  d.setDrawColor(1);
  d.drawRFrame(x, y, w, h, 2);

  const int pad = 4;
  const int textX = x + pad + (busy ? 11 : 0);
  const int textW = w - pad * 2 - (busy ? 11 : 0);
  if (busy) drawBusySpinner(d, x + pad + 3, y + h / 2, millis() / 125);

  char line[32] = {};
  std::snprintf(line, sizeof(line), "%s", title ? title : "");
  fitText(d, line, sizeof(line), textW);
  if (line[0]) d.drawStr(textX, y + 8, line);

  if (detail && detail[0] && h >= 20) {
    std::snprintf(line, sizeof(line), "%s", detail);
    // Upstream change: prefer ButtonGlyphs inline-hint rendering when the
    // detail string fits as a chip, fall back to plain text + fitText.
    if (ButtonGlyphs::measureInlineHint(d, line) <= textW) {
      ButtonGlyphs::drawInlineHint(d, textX, y + h - 5, line);
    } else {
      fitText(d, line, sizeof(line), textW);
      d.drawStr(textX, y + h - 5, line);
    }
  }
}

int drawFooterHint(oled& d, const char* text, int x) {
  const int w = ButtonGlyphs::measureInlineHint(d, text);
  const int drawX = w > 0 ? max(x, kScreenW - w) : x;
  return ButtonGlyphs::drawInlineHint(d, drawX, kFooterBaseY, text);
}

int drawFooterHintRight(oled& d, const char* text, int rightX) {
  return ButtonGlyphs::drawInlineHintRight(d, rightX, kFooterBaseY, text);
}

int drawUpperFooterHint(oled& d, const char* text, int x) {
  const int w = ButtonGlyphs::measureInlineHintCompact(d, text);
  const int drawX = w > 0 ? max(x, kScreenW - w) : x;
  return ButtonGlyphs::drawInlineHintCompact(d, drawX, kFooterUpperBaseY, text);
}

int measureActions(oled& d, const char* xLabel, const char* yLabel,
                   const char* bLabel, const char* aLabel) {
  int width = 0;
  auto addOne = [&](ButtonGlyphs::Button button, const char* label) {
    if (!label || !label[0]) return;
    if (width != 0) width += 4;
    width += ButtonGlyphs::measureHint(d, button, label);
  };
  const bool swapped = badgeConfig.get(kSwapConfirmCancel) != 0;
  const char* cancelSlotLabel = swapped ? aLabel : bLabel;
  const char* confirmSlotLabel = swapped ? bLabel : aLabel;
  addOne(ButtonGlyphs::Button::X, xLabel);
  addOne(ButtonGlyphs::Button::Y, yLabel);
  addOne(ButtonGlyphs::Button::B, cancelSlotLabel);
  addOne(ButtonGlyphs::Button::A, confirmSlotLabel);
  return width;
}

int drawActions(oled& d, int x, int baseline, const char* xLabel,
                const char* yLabel, const char* bLabel, const char* aLabel) {
  int cursor = x;
  auto drawOne = [&](ButtonGlyphs::Button button, const char* label) {
    if (!label || !label[0]) return;
    if (cursor != x) cursor += 4;
    cursor += ButtonGlyphs::drawHint(d, cursor, baseline, button, label);
  };
  const bool swapped = badgeConfig.get(kSwapConfirmCancel) != 0;
  const char* cancelSlotLabel = swapped ? aLabel : bLabel;
  const char* confirmSlotLabel = swapped ? bLabel : aLabel;
  drawOne(ButtonGlyphs::Button::X, xLabel);
  drawOne(ButtonGlyphs::Button::Y, yLabel);
  drawOne(ButtonGlyphs::Button::B, cancelSlotLabel);
  drawOne(ButtonGlyphs::Button::A, confirmSlotLabel);
  return cursor - x;
}

struct ActionScrollState {
  char key[96] = {};
  uint32_t startMs = 0;
};

void actionKey(char* out, size_t cap, int x, int baseline,
               const char* xLabel, const char* yLabel,
               const char* bLabel, const char* aLabel) {
  if (!out || cap == 0) return;
  std::snprintf(out, cap, "%d|%d|%d|%s|%s|%s|%s",
                x, baseline, badgeConfig.get(kSwapConfirmCancel),
                xLabel ? xLabel : "", yLabel ? yLabel : "",
                bLabel ? bLabel : "", aLabel ? aLabel : "");
}

int drawActionsFitting(oled& d, ActionScrollState& state, int x, int baseline,
                       const char* xLabel, const char* yLabel,
                       const char* bLabel, const char* aLabel,
                       bool leftAlign = false) {
  const int w = measureActions(d, xLabel, yLabel, bLabel, aLabel);
  const int availW = kScreenW - x + 1;
  if (w <= 0 || availW <= 0) return w;

  if (w <= availW) {
    const int drawX = leftAlign ? x : max(x, kScreenW - w);
    drawActions(d, drawX, baseline, xLabel, yLabel, bLabel, aLabel);
    return w;
  }

  char key[sizeof(state.key)] = {};
  actionKey(key, sizeof(key), x, baseline, xLabel, yLabel, bLabel, aLabel);
  if (std::strcmp(state.key, key) != 0) {
    std::snprintf(state.key, sizeof(state.key), "%s", key);
    state.startMs = millis();
  }

  const uint32_t now = millis();
  int offset = 0;
  if (now - state.startMs > kFooterScrollDelayMs) {
    const uint32_t elapsed = now - state.startMs - kFooterScrollDelayMs;
    const int period = w + kFooterScrollGapPx;
    if (period > 0) offset = (elapsed / kFooterScrollMs) % period;
  }

  const int clipTop = baseline - ButtonGlyphs::kGlyphH + 1;
  const int clipBottom = min(static_cast<int>(kScreenH) - 1, baseline);
  d.setClipWindow(x, clipTop, kScreenW - 1, clipBottom);
  const int drawX = x - offset;
  drawActions(d, drawX, baseline, xLabel, yLabel, bLabel, aLabel);
  const int duplicateX = drawX + w + kFooterScrollGapPx;
  if (duplicateX < kScreenW) {
    drawActions(d, duplicateX, baseline, xLabel, yLabel, bLabel, aLabel);
  }
  d.setMaxClipWindow();
  return w;
}

int drawFooterActions(oled& d, const char* xLabel, const char* yLabel,
                      const char* bLabel, const char* aLabel, int x,
                      bool leftAlign) {
  // Always paint chip labels in the standard UI font so callers
  // (modals, etc.) that left a different font active for body
  // content don't bleed that font into the footer chips.
  d.setFont(UIFonts::kText);
  static ActionScrollState scrollState;
  return drawActionsFitting(d, scrollState, x, kFooterBaseY,
                            xLabel, yLabel, bLabel, aLabel, leftAlign);
}

int drawUpperFooterActions(oled& d, const char* xLabel, const char* yLabel,
                           const char* bLabel, const char* aLabel, int x,
                           bool leftAlign) {
  d.setFont(UIFonts::kText);
  static ActionScrollState scrollState;
  return drawActionsFitting(d, scrollState, x, kFooterUpperBaseY,
                            xLabel, yLabel, bLabel, aLabel, leftAlign);
}

}  // namespace OLEDLayout
