// BadgeDisplay.cpp — Display rendering, adapted to use oled class (Adafruit SSD1306)

#include "BadgeDisplay.h"
#include "../infra/BadgeConfig.h"
#include "ir/BadgeIR.h"
#include "../boops/BadgeBoops.h"
#include "ReplayBootAnimation.h"
#include "hardware/Inputs.h"
#include "hardware/IMU.h"
#include "graphics.h"
#include "hardware/oled.h"
#include "ButtonGlyphs.h"
#include <WiFi.h>
#include <cmath>

extern oled badgeDisplay;

// ─── Globals (declared extern in BadgeDisplay.h) ──────────────────────────────
SemaphoreHandle_t displayMutex = nullptr;

String screenLine1 = "";
String screenLine2 = "";
bool screenDirty = false;
RenderMode renderMode = MODE_BOOT;
unsigned long inputTestLastActivity = 0;
unsigned long boopResultShownAt = 0;
bool badgeDisplayActive = false;

// ─── Display control ─────────────────────────────────────────────────────────

void displayInit()
{
  displayMutex = xSemaphoreCreateMutex();
}

static bool displayFlipped = false;

void setDisplayFlip(bool flip)
{
  if (flip == displayFlipped)
    return;
  displayFlipped = flip;
  badgeDisplay.setRotation(flip ? 0 : 2);
}

void setScreenText(const char* line1, const char* line2)
{
  screenLine1 = line1;
  screenLine2 = line2;
  screenDirty = true;
}

// ─── Drawing helpers ─────────────────────────────────────────────────────────

void drawXBM(int x, int y, int w, int h, const uint8_t *bits)
{
  badgeDisplay.drawXBM(x, y, w, h, bits);
}

void drawStringCharWrap(int x, int y, int maxWidth, int lineHeight, const char* str)
{
  badgeDisplay.setFontPreset(FONT_SMALL);
  char line[128];
  int lineLen = 0;
  line[0] = '\0';
  for (int i = 0; str[i] != '\0'; i++)
  {
    char test[129];
    memcpy(test, line, lineLen);
    test[lineLen]     = str[i];
    test[lineLen + 1] = '\0';
    if (badgeDisplay.getStrWidth(test) > maxWidth)
    {
      badgeDisplay.drawStr(x, y + lineHeight, line);
      y += lineHeight;
      line[0] = str[i];
      line[1] = '\0';
      lineLen  = 1;
    }
    else
    {
      line[lineLen]     = str[i];
      line[++lineLen]   = '\0';
    }
  }
  if (lineLen > 0)
    badgeDisplay.drawStr(x, y + lineHeight, line);
}

// ─── Boot render ──────────────────────────────────────────────────────────────

namespace {

static unsigned long bootAnimationStartedAt = 0;
static bool bootAnimationStarted = false;
static bool bootAnimationFinished = false;

static constexpr uint8_t kBootStatusFrameCount =
    ReplayBootAnimation::kFrameCount - 1;
static constexpr uint16_t kBootRenderMs = 33;
static constexpr uint16_t kBootFinalHoldMs = 1000;

static void startBootAnimation()
{
  bootAnimationStartedAt = millis();
  bootAnimationStarted = true;
  bootAnimationFinished = false;
}

static void ensureBootAnimationStarted()
{
  if (!bootAnimationStarted)
    startBootAnimation();
}

static uint8_t currentBootAnimationFrame()
{
  if (bootAnimationFinished)
    return ReplayBootAnimation::kFrameCount - 1;
  ensureBootAnimationStarted();
  const unsigned long elapsed = millis() - bootAnimationStartedAt;
  const uint32_t frame = (elapsed / ReplayBootAnimation::kFrameMs) %
                         kBootStatusFrameCount;
  return static_cast<uint8_t>(frame);
}

static void drawBootStatusText(const char* msg)
{
  badgeDisplay.setFontPreset(FONT_TINY);
  badgeDisplay.setDrawColor(0);
  badgeDisplay.drawBox(0, 56, 128, 8);
  badgeDisplay.setDrawColor(1);
  badgeDisplay.drawStr(0, 64, msg ? msg : "");
}

static void drawBootScreen(const char* msg)
{
  const uint8_t frame = currentBootAnimationFrame();

  badgeDisplay.clearBuffer();
  drawXBM(0,
          0,
          ReplayBootAnimation::kFrameWidth,
          ReplayBootAnimation::kFrameHeight,
          ReplayBootAnimation::kFrames[frame]);
  drawBootStatusText(msg);
  badgeDisplay.sendBuffer();
}

}  // namespace

void drawReplayBootFinal(oled& d)
{
  drawReplayBootFrame(d, ReplayBootAnimation::kFrameCount - 1);
}

void drawReplayBootFrame(oled& d, uint8_t frame)
{
  if (frame >= ReplayBootAnimation::kFrameCount) {
    frame = ReplayBootAnimation::kFrameCount - 1;
  }
  d.drawXBM(0,
            0,
            ReplayBootAnimation::kFrameWidth,
            ReplayBootAnimation::kFrameHeight,
            ReplayBootAnimation::kFrames[frame]);
}

uint8_t replayBootFrameCount()
{
  return ReplayBootAnimation::kFrameCount;
}

uint16_t replayBootFrameMs()
{
  return ReplayBootAnimation::kFrameMs;
}

void tickBootAnimation()
{
  if (renderMode != MODE_BOOT || bootAnimationFinished)
    return;
  drawBootScreen(screenLine1.c_str());
}

void bootAnimationDelay(uint32_t durationMs)
{
  const uint32_t startMs = millis();
  while ((millis() - startMs) < durationMs) {
    tickBootAnimation();
    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= durationMs)
      break;
    const uint32_t remaining = durationMs - elapsed;
    delay(remaining < kBootRenderMs ? remaining : kBootRenderMs);
  }
}

void showBootFinalFrame()
{
  ensureBootAnimationStarted();
  bootAnimationFinished = true;
  screenLine1 = "";

  badgeDisplay.clearBuffer();
  drawReplayBootFinal(badgeDisplay);
  badgeDisplay.sendBuffer();
}

void completeBootAnimation()
{
  const bool needsHold = !bootAnimationFinished;
  showBootFinalFrame();
  if (needsHold)
    delay(kBootFinalHoldMs);
}

void bootPrint(const char* msg) {
  if (bootAnimationFinished)
    startBootAnimation();
  Serial.println(msg);
  screenLine1 = msg;
  drawBootScreen(msg);
}

static void renderBoot() {
  drawBootScreen(screenLine1.c_str());
}

// ─── QR render ───────────────────────────────────────────────────────────────

extern uint8_t *qrBits;
extern int      qrByteCount;
extern int      qrWidth;
extern int      qrHeight;
extern char uid_hex[];

static void renderQR()
{
  badgeDisplay.clearBuffer();
  if (qrBits && qrWidth > 0 && qrHeight > 0) {
    int x = ((128 - qrWidth) / 2) & ~7;
    int y = (64 - qrHeight) / 2;
    if (y < 0) y = 0;
    badgeDisplay.setDrawColor(1);
    badgeDisplay.drawBox(0, 0, 128, 64);
    badgeDisplay.setDrawColor(0);
    badgeDisplay.drawXBM(x, y, qrWidth, qrHeight, qrBits);
    badgeDisplay.setDrawColor(1);
  } else {
    const char* status = "offline";
    badgeDisplay.setFontPreset(FONT_TINY);
    int statusX = 128 - badgeDisplay.getStrWidth(status);
    badgeDisplay.drawStr(statusX, 6, status);
  }
  badgeDisplay.sendBuffer();
}

// ─── Main render ─────────────────────────────────────────────────────────────

extern BadgeState badgeState;
extern char badgeName[];
extern char badgeTitle[];
extern char badgeCompany[];
extern char badgeAtType[];

// ─── Nametag render ───────────────────────────────────────────────────────────
static void renderNametag() {
  setDisplayFlip(true);
  badgeDisplay.clearBuffer();

  if (badgeAtType[0]) {
    badgeDisplay.setFontPreset(FONT_TINY);
    int w = badgeDisplay.getStrWidth(badgeAtType);
    badgeDisplay.drawStr((128 - w) / 2, 7, badgeAtType);
  }

  if (badgeName[0]) {
    badgeDisplay.setFontPreset(FONT_LARGE);
    int w = badgeDisplay.getStrWidth(badgeName);
    if (w > 124) {
      badgeDisplay.setFontPreset(FONT_SMALL);
      w = badgeDisplay.getStrWidth(badgeName);
    }
    badgeDisplay.drawStr((128 - w) / 2, 32, badgeName);
  }

  if (badgeTitle[0]) {
    badgeDisplay.setFontPreset(FONT_SMALL);
    int w = badgeDisplay.getStrWidth(badgeTitle);
    badgeDisplay.drawStr((128 - w) / 2, 46, badgeTitle);
  }

  if (badgeCompany[0]) {
    badgeDisplay.setFontPreset(FONT_TINY);
    int w = badgeDisplay.getStrWidth(badgeCompany);
    badgeDisplay.drawStr((128 - w) / 2, 58, badgeCompany);
  }

  badgeDisplay.sendBuffer();
}

// ─── IR arrow helper ──────────────────────────────────────────────────────────
static void drawIrArrow(int cx, int cy, bool up, bool filled)
{
  const int SW = 6;
  const int SH = 14;
  const int HW = 18;
  const int HH = 10;
  const int H  = SH + HH;
  int top = cy - H / 2;

  if (up) {
    if (filled) {
      for (int i = 0; i < HH; i++) {
        int w = 2 + (HW - 2) * i / (HH - 1);
        badgeDisplay.drawHLine(cx - w / 2, top + i, w);
      }
      badgeDisplay.drawBox(cx - SW / 2, top + HH, SW, SH);
    } else {
      badgeDisplay.drawLine(cx, top, cx - HW / 2, top + HH - 1);
      badgeDisplay.drawLine(cx, top, cx + HW / 2, top + HH - 1);
      badgeDisplay.drawVLine(cx - SW / 2, top + HH, SH);
      badgeDisplay.drawVLine(cx + SW / 2, top + HH, SH);
      badgeDisplay.drawHLine(cx - SW / 2, top + H - 1, SW + 1);
    }
  } else {
    if (filled) {
      badgeDisplay.drawBox(cx - SW / 2, top, SW, SH);
      for (int i = 0; i < HH; i++) {
        int w = HW - (HW - 2) * i / (HH - 1);
        badgeDisplay.drawHLine(cx - w / 2, top + SH + i, w);
      }
    } else {
      badgeDisplay.drawVLine(cx - SW / 2, top, SH);
      badgeDisplay.drawVLine(cx + SW / 2, top, SH);
      badgeDisplay.drawHLine(cx - SW / 2, top, SW + 1);
      badgeDisplay.drawLine(cx - HW / 2, top + SH, cx, top + H - 1);
      badgeDisplay.drawLine(cx + HW / 2, top + SH, cx, top + H - 1);
    }
  }
}

static void renderBoop()
{
  setDisplayFlip(false);
  badgeDisplay.clearBuffer();

  BadgeBoops::BoopPhase phase = BadgeBoops::boopStatus.phase;
  bool arrowRX  = false;
  bool arrowTX  = false;
  const char* statusA = nullptr;
  const char* statusB = nullptr;
  bool statusAInlineHint = false;

  switch (phase)
  {
  case BadgeBoops::BOOP_PHASE_IDLE:
    statusA = "Y:boop";
    statusAInlineHint = true;
    break;
  case BadgeBoops::BOOP_PHASE_BEACONING:
    arrowRX = true;
    arrowTX = true;
    statusA = "Beaconing...";
    statusB = BadgeBoops::boopStatus.statusMsg[0]
                ? BadgeBoops::boopStatus.statusMsg : nullptr;
    break;
  case BadgeBoops::BOOP_PHASE_EXCHANGE:
  case BadgeBoops::BOOP_PHASE_PAIRED_OK:
  case BadgeBoops::BOOP_PHASE_FAILED:
  case BadgeBoops::BOOP_PHASE_CANCELLED:
    break;
  }

  drawIrArrow(46, 24, false, arrowRX);
  drawIrArrow(82, 24, true,  arrowTX);

  if (statusA)
  {
    badgeDisplay.setFontPreset(FONT_SMALL);
    int w = statusAInlineHint
              ? ButtonGlyphs::measureInlineHint(badgeDisplay, statusA)
              : badgeDisplay.getStrWidth(statusA);
    if (statusAInlineHint)
      ButtonGlyphs::drawInlineHint(badgeDisplay, (128 - w) / 2, 46, statusA);
    else
      badgeDisplay.drawStr((128 - w) / 2, 46, statusA);
    if (statusB)
    {
      w = badgeDisplay.getStrWidth(statusB);
      badgeDisplay.drawStr((128 - w) / 2, 58, statusB);
    }
  }

  badgeDisplay.sendBuffer();
}

// ─── Input test render ───────────────────────────────────────────────────────

extern Inputs inputs;
extern IMU imu;

static void renderInputTest()
{
  if (inputTestLastActivity == 0)
    inputTestLastActivity = millis();

  static uint8_t prevBtns = 0;
  static bool prevTilt = false;

  const Inputs::ButtonStates& btns = inputs.buttons();
  uint8_t curBtns = 0;
  if (btns.up)    curBtns |= 1;
  if (btns.down)  curBtns |= 2;
  if (btns.left)  curBtns |= 4;
  if (btns.right) curBtns |= 8;

  bool curTilt = imu.isReady() && imu.isFaceDown();
  if (curBtns != prevBtns || curTilt != prevTilt)
  {
    prevBtns = curBtns;
    prevTilt = curTilt;
    inputTestLastActivity = millis();
  }

  unsigned long elapsed = millis() - inputTestLastActivity;
  if (elapsed >= 5000)
  {
    inputTestLastActivity = 0;
    renderMode = MODE_MENU;
    screenDirty = true;
    return;
  }
  int secsLeft = (int)((5000 - elapsed + 999) / 1000);

  setDisplayFlip(false);
  badgeDisplay.clearBuffer();
  drawXBM(0, 0, Graphics_Base_width, Graphics_Base_height, Graphics_Base_bits);

  constexpr int kJoyCircleCx = 100;
  constexpr int kJoyCircleCy = 53;
  constexpr int kJoyCircleR = 6;
  constexpr int kJoyDeadband = 170;
  float nx = (static_cast<int>(inputs.joyX()) - 2047) / 2047.0f;
  float ny = (static_cast<int>(inputs.joyY()) - 2047) / 2047.0f;
  if (abs(static_cast<int>(inputs.joyX()) - 2047) < kJoyDeadband) nx = 0.0f;
  if (abs(static_cast<int>(inputs.joyY()) - 2047) < kJoyDeadband) ny = 0.0f;
  float px = nx * kJoyCircleR;
  float py = ny * kJoyCircleR;
  float dist = sqrtf(px * px + py * py);
  if (dist > kJoyCircleR) {
    px = px / dist * kJoyCircleR;
    py = py / dist * kJoyCircleR;
  }
  int jx = kJoyCircleCx + static_cast<int>(roundf(px));
  int jy = kJoyCircleCy + static_cast<int>(roundf(py));
  badgeDisplay.drawBox(jx - 1, jy - 1, 3, 3);

  if (!curTilt)
    badgeDisplay.drawBox(84, 48, 4, 5);
  else
    badgeDisplay.drawBox(84, 54, 4, 5);

  // Button indicator positions on the Graphics_Base hardware diagram
  struct { int x; int y; } btnPos[] = {
    {118, 48},  // UP
    {118, 58},  // DOWN
    {113, 53},  // LEFT
    {123, 53},  // RIGHT
  };
  if (btns.up)    badgeDisplay.drawBox(btnPos[0].x - 1, btnPos[0].y - 1, 3, 3);
  if (btns.down)  badgeDisplay.drawBox(btnPos[1].x - 1, btnPos[1].y - 1, 3, 3);
  if (btns.left)  badgeDisplay.drawBox(btnPos[2].x - 1, btnPos[2].y - 1, 3, 3);
  if (btns.right) badgeDisplay.drawBox(btnPos[3].x - 1, btnPos[3].y - 1, 3, 3);

  char buf[4];
  snprintf(buf, sizeof(buf), "%d", secsLeft);
  badgeDisplay.setFontPreset(FONT_XLARGE);
  int tw = badgeDisplay.getStrWidth(buf);
  badgeDisplay.setDrawColor(0);
  badgeDisplay.drawBox(64 - tw / 2 - 2, 16, tw + 4, 34);
  badgeDisplay.setDrawColor(1);
  badgeDisplay.drawStr(64 - tw / 2, 48, buf);

  badgeDisplay.sendBuffer();
}

// ─── Boop result render ───────────────────────────────────────────────────────

static void renderBoopResult()
{
  if (boopResultShownAt == 0) boopResultShownAt = millis();

  if (millis() - boopResultShownAt >= 10000) {
    boopResultShownAt = 0;
    renderMode = MODE_MENU;
    screenDirty = true;
    return;
  }

  setDisplayFlip(false);
  badgeDisplay.clearBuffer();

  const BadgeBoops::BoopStatus& s = BadgeBoops::boopStatus;
  BadgeBoops::BoopPhase phase = s.phase;

  if (phase == BadgeBoops::BOOP_PHASE_PAIRED_OK) {
    badgeDisplay.setFontPreset(FONT_SMALL);
    badgeDisplay.drawStr(0, 10, "Booped with");

    const char* name = s.peerName[0] ? s.peerName : s.peerUID;
    badgeDisplay.setFontPreset(FONT_LARGE);
    int w = badgeDisplay.getStrWidth(name);
    if (w > 124) {
      badgeDisplay.setFontPreset(FONT_SMALL);
      w = badgeDisplay.getStrWidth(name);
    }
    badgeDisplay.drawStr((128 - w) / 2, 32, name);
  } else {
    const char* header =
      (phase == BadgeBoops::BOOP_PHASE_FAILED)    ? "Boop failed"    :
      (phase == BadgeBoops::BOOP_PHASE_CANCELLED) ? "Boop cancelled" :
                                                    "Boop error";
    badgeDisplay.setFontPreset(FONT_SMALL);
    int hw = badgeDisplay.getStrWidth(header);
    badgeDisplay.drawStr((128 - hw) / 2, 28, header);
  }

  badgeDisplay.drawHLine(0, 38, 128);

  badgeDisplay.setFontPreset(FONT_SMALL);
  ButtonGlyphs::drawInlineHint(badgeDisplay, 4, 52, "Cancel:menu");
  const char* menuLabel = "Y:New Boop";
  ButtonGlyphs::drawInlineHintRight(badgeDisplay, 124, 52, menuLabel);

  badgeDisplay.sendBuffer();
}

// ─── renderScreen ────────────────────────────────────────────────────────────

void renderScreen()
{
  DISPLAY_TAKE();
  screenDirty = false;
  static RenderMode lastRenderMode = renderMode;
  if (renderMode != lastRenderMode) {
    if (renderMode == MODE_BOOT)
      startBootAnimation();
    else
      bootAnimationStarted = false;
    lastRenderMode = renderMode;
  }
  bool tiltActive = imu.isReady() && imu.isFaceDown();
  if (TILT_SHOWS_BADGE && tiltActive && badgeName[0] != '\0') {
    renderNametag();
    DISPLAY_GIVE();
    return;
  }
  switch (renderMode)
  {
  case MODE_BOOT:
    renderBoot();
    break;
  case MODE_QR:
    renderQR();
    break;
  default:
    break;
  }
  DISPLAY_GIVE();
}
