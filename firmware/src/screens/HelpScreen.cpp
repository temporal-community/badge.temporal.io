#include "HelpScreen.h"

#include <Arduino.h>
#include <cstring>

#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../hardware/qrcode.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/QRCodePlate.h"
#include "../ui/UIFonts.h"

namespace {

// ── QR target list ────────────────────────────────────────────────────────
// Index here matches HelpScreen::qrs_[]. Update both together.
constexpr uint8_t kQrDocs = 0;
constexpr uint8_t kQrIde = 1;
constexpr uint8_t kQrTemporal = 2;

constexpr const char* kQrUrls[] = {
    "https://docs.jumperless.org/badge-developer-guide/",
    "https://ide.jumperless.org/",
    "https://badge.temporal.io/",
};
static_assert(sizeof(kQrUrls) / sizeof(kQrUrls[0]) == 3,
              "kQrUrls must match HelpScreen::kQrCount");

// ── Help-page content model ───────────────────────────────────────────────
// Each item is laid out top-to-bottom in the scroll body. We render every
// item with a known fixed height so scroll math + total-height stays
// declarative.
//
// Glyph rows are parsed by ButtonGlyphs::drawInlineHint, which now
// supports "+" chaining: `Y+X+A+B` renders as a single 10×10 cluster
// glyph with all four pads filled, not four side-by-side glyphs.
// Pair tokens (`L/R`, `U/D`) still render as two glyphs separated by
// a slash since they mean "either of these", not "both at once".

enum class ItemKind : uint8_t {
  kHeading,
  kText,
  kGlyphs,
  kQr,
  kGap,
};

struct Item {
  ItemKind kind;
  const char* text;  // for kHeading/kText/kGlyphs; QR label for kQr
  uint8_t qrIndex;   // valid for kQr only
};

// Verified against firmware/src as of edit time:
//   - PanicReset.cpp: kPollPeriodUs * {kMpyExitTicks, kRebootTicks,
//     kForceRebootTicks} = 1s/2s/4s (4-button hold from anywhere).
//   - GridMenuScreen.cpp: kShutdownHoldMs = 3000 (DOWN hold on home menu).
//   - doom_input.cpp: kExitComboMs = 1500; UP=KEY_FIRE,
//     DOWN=KEY_USE/ENTER, LEFT/RIGHT=strafe (joystick X turns,
//     joystick Y moves fwd/back).
//   - GUI.cpp: setNametagMode triggered by IMU `kInverted` orientation.
//   - scripts/generate_build_wifi_config.py: WiFi creds come from
//     wifi.local.env or BADGE_WIFI_{SSID,PASS} env vars at build time.
//
// Anything not directly traceable to the source has been left out.
constexpr Item kItems[] = {
    // ── Panic-reset (4-button hold from anywhere) ─────────────────────
    {ItemKind::kHeading, "RESCUE",                        0},
    {ItemKind::kText,    "Hold all 4:",                   0},
    {ItemKind::kGlyphs,  "ALL 1s exit app",               0},
    {ItemKind::kGlyphs,  "ALL 2s reboot",                 0},  
    {ItemKind::kGlyphs,  "ALL 4s force",                  0},
    {ItemKind::kGap,     nullptr,                         0},

    // ── Deep sleep on home menu ──────────────────────────────────────
    {ItemKind::kHeading, "SLEEP",                         0},
    {ItemKind::kGlyphs,  "v hold 3s on home",             0},
    {ItemKind::kGap,     nullptr,                         0},

    // ── 

    // ── Nametag (IMU flip) ───────────────────────────────────────────
    {ItemKind::kHeading, "NAMETAG",                       0},
    {ItemKind::kText,    "Flip the badge",                0},
    {ItemKind::kText,    "upside down to",                0},
    {ItemKind::kText,    "show nametag.",                 0},
    {ItemKind::kGap,     nullptr,                         0},

    // ── DOOM ─────────────────────────────────────────────────────────
    {ItemKind::kHeading, "DOOM",                          0},
    {ItemKind::kGlyphs,  "L/R 1.5s quit",                 0},
    {ItemKind::kGlyphs,  "Y fire   A use",                0},
    {ItemKind::kGlyphs,  "U/D escape",                    0},
    {ItemKind::kGlyphs,  "L/R strafe",                    0},
    {ItemKind::kText,    "Joystick X turns,",             0},
    {ItemKind::kText,    "Y is fwd/back.",                0},
    {ItemKind::kGap,     nullptr,                         0},

    // ── WiFi ─────────────────────────────────────────────────────────
    {ItemKind::kHeading, "WIFI",                          0},
    {ItemKind::kText,    "Open WIFI tile to",             0},
    {ItemKind::kText,    "add networks. Up",              0},
    {ItemKind::kText,    "to four are saved",             0},
    {ItemKind::kText,    "and tried in order",            0},
    {ItemKind::kText,    "on boot until one",             0},
    {ItemKind::kText,    "connects.",                     0},
    // {ItemKind::kText,    "(Optional: bake",               0},
    // {ItemKind::kText,    "creds via wifi",                0},
    // {ItemKind::kText,    ".local.env at",                 0},
    // {ItemKind::kText,    "build time.)",                  0},
    {ItemKind::kGap,     nullptr,                         0},

    // ── Docs URLs ────────────────────────────────────────────────────
    {ItemKind::kHeading, "DEV GUIDE",                     0},
    {ItemKind::kText,    "docs.jumperless.org/",               0},
    {ItemKind::kText,    "badge-developer-guide/",         0},
    {ItemKind::kQr,      "docs",                          kQrDocs},
    {ItemKind::kGap,     nullptr,                         0},

    {ItemKind::kHeading, "PYTHON IDE",                    0},
    {ItemKind::kText,    "ide.jumperless.org",            0},
    {ItemKind::kQr,      "ide",                           kQrIde},
    {ItemKind::kGap,     nullptr,                         0},

    {ItemKind::kHeading, "MIRROR",                        0},
    {ItemKind::kText,    "badge.temporal.io",             0},
    {ItemKind::kQr,      "temporal",                      kQrTemporal},
};
constexpr size_t kItemCount = sizeof(kItems) / sizeof(kItems[0]);

// Per-item heights. Keep in sync with the renderer.
constexpr uint8_t kHeadingH = 10;  // small inverted bar
constexpr uint8_t kTextH = 8;
constexpr uint8_t kGlyphsH = 12;   // 10-px glyph + small lead
// Plate sized to hold a 29-module v3 QR at 2 px/module (58 px) plus a
// 1-module quiet zone on each side. Smaller QRs (v2 = 25 modules) get
// the same plate with extra surrounding white, which only helps
// scanners lock on faster.
constexpr uint8_t kQrPlateSize = 62;
constexpr uint8_t kQrH = kQrPlateSize + 2;
constexpr uint8_t kGapH = 4;

uint8_t itemHeight(const Item& it) {
  switch (it.kind) {
    case ItemKind::kHeading: return kHeadingH;
    case ItemKind::kText:    return kTextH;
    case ItemKind::kGlyphs:  return kGlyphsH;
    case ItemKind::kQr:      return kQrH;
    case ItemKind::kGap:     return kGapH;
  }
  return 0;
}

int16_t totalContentHeight() {
  int16_t h = 0;
  for (size_t i = 0; i < kItemCount; i++) h += itemHeight(kItems[i]);
  return h;
}

// ── Layout constants ──────────────────────────────────────────────────────
// Sticky header is a slim 7 px strip — just enough to fit the
// Smallsimple cap-height title without the heavy full-width inverted
// bar from the v1 layout. Frees up an extra row of body for content.
constexpr uint8_t kHeaderH = 7;
constexpr uint8_t kHeaderRuleY = kHeaderH;     // 1 px hairline beneath
constexpr uint8_t kBodyTopY = kHeaderRuleY + 1;
constexpr uint8_t kBodyBotY = OLEDLayout::kScreenH;
constexpr uint8_t kBodyH = kBodyBotY - kBodyTopY;
constexpr uint8_t kBodyPadX = 2;

// ── QR sizing ─────────────────────────────────────────────────────────────
// Capacity table: smallest version that holds the URL with ECC_LOW in
// BYTE mode. Indexed by version (1..7); index 0 unused.
constexpr uint16_t kByteCapLowEcc[] = {
    0,
    17,   // v1
    32,   // v2
    53,   // v3
    78,   // v4
    106,  // v5
    134,  // v6
    154,  // v7
};
constexpr uint8_t kMaxQrVersion = 7;

// ── Joystick scroll ───────────────────────────────────────────────────────
constexpr int16_t kJoyDeadband = 600;
// Free-scroll speed at full joystick deflection. Tuned so a deliberate
// flick traverses the page without overshooting any one section, and a
// gentle lean nudges line-by-line.
constexpr float   kScrollPxPerSecAtFull = 120.0f;
// Minimum speed multiplier when a QR completely fills the viewport.
// Speed is interpolated from 1.0 (no QR visible) down to this value
// (QR fully covers body) so users get a "sticky" pause that gives a
// phone time to lock on. 0.25 ≈ 4× slower at the worst-case QR.
constexpr float   kStickyMinMultiplier = 0.35f;
constexpr uint16_t kFrameTickMinMs = 8;

}  // namespace

void HelpScreen::onEnter(GUIManager& gui) {
  (void)gui;
  if (!qrsReady_) generateAll();
  scrollPx_ = 0;
  lastScrollMs_ = millis();
}

void HelpScreen::generateAll() {
  for (uint8_t i = 0; i < kQrCount; i++) {
    if (!generateOne(kQrUrls[i], qrs_[i])) {
      Serial.printf("[help] QR %u FAILED to generate\n", i);
      qrs_[i].pixels = 0;
    }
  }
  qrsReady_ = true;
}

bool HelpScreen::generateOne(const char* url, Qr& out) {
  std::memset(out.bits, 0, sizeof(out.bits));
  out.pixels = 0;

  const uint16_t urlLen = static_cast<uint16_t>(strlen(url));
  uint8_t version = 0;
  for (uint8_t v = 1; v <= kMaxQrVersion; v++) {
    if (qrcode_getBufferSize(v) > sizeof(qrWork_)) break;
    if (urlLen <= kByteCapLowEcc[v]) {
      version = v;
      break;
    }
  }
  if (version == 0) return false;

  QRCode qr{};
  if (qrcode_initText(&qr, qrWork_, version, ECC_LOW,
                      const_cast<char*>(url)) != 0) {
    return false;
  }

  const uint8_t modules = qr.size;
  // Force 2 px/module so phone cameras can resolve modules at the
  // OLED's typical viewing distance — empirically a single-pixel
  // module won't scan against this panel. The plate is sized to fit
  // the largest mirror at scale 2; if a future longer URL pushes us
  // past v3 we fall back to scale 1 rather than overflowing the plate.
  uint8_t scale = 2;
  if (modules * scale > kQrPlateSize) scale = 1;
  const uint8_t pixW = modules * scale;
  const uint16_t rowBytes = (pixW + 7) / 8;
  if (static_cast<uint32_t>(rowBytes) * pixW > sizeof(out.bits)) return false;

  for (uint8_t my = 0; my < modules; my++) {
    for (uint8_t mx = 0; mx < modules; mx++) {
      if (!qrcode_getModule(&qr, mx, my)) continue;
      const uint16_t basePx = mx * scale;
      const uint16_t basePy = my * scale;
      for (uint8_t sy = 0; sy < scale; sy++) {
        for (uint8_t sx = 0; sx < scale; sx++) {
          const uint16_t bx = basePx + sx;
          const uint16_t by = basePy + sy;
          out.bits[by * rowBytes + (bx / 8)] |= (1 << (bx % 8));
        }
      }
    }
  }
  out.pixels = pixW;
  return true;
}

namespace {

void drawHeading(oled& d, int y, const char* text) {
  // Slim heading: inverted box just around the title text — not the
  // full screen width — followed by a hairline that fills the rest of
  // the row. Reads as a section divider without dominating the OLED
  // the way the old solid bar did.
  d.setFont(UIFonts::kText);
  const int textW = d.getStrWidth(text);
  const int boxW = textW + 4;
  d.setDrawColor(1);
  d.drawBox(0, y, boxW, 9);
  d.setDrawColor(0);
  d.drawStr(2, y + 7, text);
  d.setDrawColor(1);
  d.drawHLine(boxW, y + 8, OLEDLayout::kScreenW - boxW);
}

void drawText(oled& d, int y, const char* text) {
  d.setDrawColor(1);
  d.setFont(UIFonts::kText);
  d.drawStr(kBodyPadX, y + 7, text);
}

void drawGlyphs(oled& d, int y, const char* text) {
  d.setDrawColor(1);
  d.setFont(UIFonts::kText);
  // ButtonGlyphs::drawInlineHint expects a baseline; glyphs sit above
  // the baseline by 9 px and text sits on the baseline.
  ButtonGlyphs::drawInlineHint(d, kBodyPadX, y + 10, text);
}

}  // namespace

void HelpScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setTextWrap(false);
  d.setDrawColor(1);

  // Slim sticky header — title at top-left, scroll % at top-right,
  // single hairline separating header from scrollable body.
  d.setDrawColor(1);
  d.setFont(UIFonts::kText);
  d.drawStr(kBodyPadX, kHeaderH - 1, "HELP");

  const int16_t totalH = totalContentHeight();
  const int16_t maxScroll =
      totalH > kBodyH ? static_cast<int16_t>(totalH - kBodyH) : 0;
  if (maxScroll > 0) {
    char pct[8];
    const int p = (scrollPx_ * 100 + maxScroll / 2) / maxScroll;
    snprintf(pct, sizeof(pct), "%d%%", p);
    const int w = d.getStrWidth(pct);
    d.drawStr(OLEDLayout::kScreenW - w - kBodyPadX, kHeaderH - 1, pct);
  }
  d.drawHLine(0, kHeaderRuleY, OLEDLayout::kScreenW);

  // Body — text/glyph/heading items stay clipped to the body so they
  // don't bleed onto the sticky header. QR items deliberately escape
  // that clip and overdraw the header strip while they're scrolling
  // past, so the full QR plate (including its quiet zone) is always
  // scannable instead of getting visually chopped under the "HELP"
  // chrome on its way through the viewport.
  d.setClipWindow(0, kBodyTopY, OLEDLayout::kScreenW, kBodyBotY);

  int16_t y = kBodyTopY - scrollPx_;
  for (size_t i = 0; i < kItemCount; i++) {
    const Item& it = kItems[i];
    const uint8_t h = itemHeight(it);

    // Cull items entirely off-screen. QR culling uses the full screen
    // (not just the body) since QRs are allowed to overdraw the header.
    const int16_t topBound =
        it.kind == ItemKind::kQr ? 0 : static_cast<int16_t>(kBodyTopY);
    const int16_t botBound = kBodyBotY;
    if (y + h > topBound && y < botBound) {
      switch (it.kind) {
        case ItemKind::kHeading:
          drawHeading(d, y, it.text);
          break;
        case ItemKind::kText:
          drawText(d, y, it.text);
          break;
        case ItemKind::kGlyphs:
          drawGlyphs(d, y, it.text);
          break;
        case ItemKind::kQr: {
          const Qr& q = qrs_[it.qrIndex];
          const int plateX =
              (OLEDLayout::kScreenW - kQrPlateSize) / 2;
          // Widen the clip just for this draw so the plate's white
          // backing wipes out the header chrome underneath, then
          // restore the body clip for the next item.
          d.setClipWindow(0, 0, OLEDLayout::kScreenW, kBodyBotY);
          QRCodePlate::draw(d, q.bits, q.pixels, q.pixels, plateX, y + 1,
                            kQrPlateSize, /*divider=*/false);
          d.setClipWindow(0, kBodyTopY, OLEDLayout::kScreenW, kBodyBotY);
          break;
        }
        case ItemKind::kGap:
          break;
      }
    }
    y += h;
  }
  d.setMaxClipWindow();
}

void HelpScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                             int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }

  // Joystick scrolling. Center is 2047; positive Y deflection scrolls
  // down (advances the page). Velocity is proportional to deflection
  // past the deadband. Update every frame so motion stays smooth even
  // when the user is just leaning the stick.
  const uint32_t now = millis();
  uint32_t dt = now - lastScrollMs_;
  if (dt < kFrameTickMinMs) return;
  lastScrollMs_ = now;
  if (dt > 100) dt = 100;  // clamp burst after focus changes

  const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(dy) < kJoyDeadband) return;

  // Sticky slow-down over QR codes: walk every QR item, accumulate
  // how many pixels of the current viewport are covered by a QR
  // bitmap, normalise to 0..1, and lerp the scroll multiplier toward
  // kStickyMinMultiplier. A QR that fully covers the body slows
  // scrolling to ~25 % of normal so a phone has time to lock on; once
  // the page has scrolled past the QR the multiplier ramps back to 1.
  float qrCoverage = 0.0f;
  {
    int16_t cursorY = 0;
    const int16_t viewTop = scrollPx_;
    const int16_t viewBot = scrollPx_ + kBodyH;
    int16_t covered = 0;
    for (size_t i = 0; i < kItemCount; i++) {
      const Item& it = kItems[i];
      const uint8_t h = itemHeight(it);
      if (it.kind == ItemKind::kQr) {
        const int16_t top = cursorY;
        const int16_t bot = cursorY + h;
        const int16_t lo = top > viewTop ? top : viewTop;
        const int16_t hi = bot < viewBot ? bot : viewBot;
        if (hi > lo) covered += (hi - lo);
      }
      cursorY += h;
    }
    if (kBodyH > 0) {
      qrCoverage = static_cast<float>(covered) / static_cast<float>(kBodyH);
      if (qrCoverage > 1.0f) qrCoverage = 1.0f;
    }
  }
  const float stickyMul =
      1.0f - (1.0f - kStickyMinMultiplier) * qrCoverage;

  const float norm = static_cast<float>(dy) / 2047.0f;
  const float deltaPx =
      norm * kScrollPxPerSecAtFull * stickyMul * (dt / 1000.0f);

  const int16_t totalH = totalContentHeight();
  const int16_t maxScroll =
      totalH > kBodyH ? static_cast<int16_t>(totalH - kBodyH) : 0;

  int32_t next = static_cast<int32_t>(scrollPx_) +
                 static_cast<int32_t>(deltaPx);
  if (next < 0) next = 0;
  if (next > maxScroll) next = maxScroll;
  if (static_cast<int16_t>(next) != scrollPx_) {
    scrollPx_ = static_cast<int16_t>(next);
    gui.requestRender();
  }
}
