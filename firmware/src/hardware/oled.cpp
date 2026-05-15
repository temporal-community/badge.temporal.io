#include "oled.h"
#include "../infra/Bitops.h"
#include "HardwareConfig.h"
#include "PanicReset.h"
#include "ui/Images.h"
#include "../ui/UIFonts.h"
#include "../ui/BadgeEmojiFont.h"
#include <Wire.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string.h>
#include <strings.h>
#include "../ui/BadgeDisplay.h"
extern volatile bool mpy_oled_hold;
extern uint8_t* qrBits;
extern int qrWidth;
extern int qrHeight;
#include "Haptics.h"
#include "IMU.h"
#include "Inputs.h"
#include "Power.h"
#include <U8g2lib.h>

namespace {

constexpr uint32_t kFontCycleHoldMinMs = 300;

// Font tables (kFontGridFamilyCount / kSizeCount / kSizeLabels / kFontGrid /
// kReplayFonts / kReplayFontCount) and fontDisplayName moved to
// FontCatalog.{h,cpp} — included transitively via oled.h.

using bitops::reverseBits;

}  // namespace

static int lookupFontByName(const char *name) {
  if (name == nullptr) {
    return -1;
  }
  for (int i = 0; i < kReplayFontCount; ++i) {
    if (strcasecmp(name, kReplayFonts[i].name) == 0) {
      return i;
    }
  }
  return -1;
}

oled::oled()
    : u8g2_(U8G2_R0, /* reset= */ U8X8_PIN_NONE),
      initialized_(false),
      inputs_(nullptr),
      batteryGauge_(nullptr),
      imu_(nullptr),
      hasBoundPin_(false),
      boundPin_(0),
      boundPinLabel_{0},
      lastUpdateMs_(0),
      tickCount_(0),
      lastFrameHash_(0),
      lastForcedDrawMs_(0),
      textSize_(3),
      currentFontIndex_(-1),
      familyIdx_(0),
      slotInFamily_(3),
      defaultBuiltinScale_(1) {}

void oled::applyHardwareFontState() {
  if (!initialized_) {
    return;
  }
  if (familyIdx_ >= kFontGridFamilyCount) {
    familyIdx_ = 0;
  }
  if (slotInFamily_ >= kSizeCount) {
    slotInFamily_ = 3;
  }
  const uint8_t *font = kFontGrid[familyIdx_][slotInFamily_];
  if (font != nullptr) {
    u8g2_.setFont(font);
  }
  textSize_ = slotInFamily_;
  currentFontIndex_ = -1;
}

int oled::init() {
  // Wire is initialized once in main.cpp setup(); do not re-init here.
  u8g2_.setI2CAddress(OLED_I2C_ADDRESS << 1);
  if (!u8g2_.begin()) {
    initialized_ = false;
    return -1;
  }

  u8g2_.clearBuffer();
  u8g2_.setDrawColor(1);
  u8g2_.setFontMode(0);
  u8g2_.setFontPosBaseline();

  setClockDivOsc(2, 1);
  familyIdx_ = 0;
  slotInFamily_ = 3;
  defaultBuiltinScale_ = 1;
  initialized_ = true;
  applyHardwareFontState();

  return 0;
}

size_t oled::write(uint8_t c) {
  if (!initialized_) {
    return 0;
  }
  if (c == '\n') {
    u8g2_.tx = 0;
    u8g2_.ty += u8g2_.getAscent() - u8g2_.getDescent() + 2;
    return 1;
  }
  if (c == '\r') {
    u8g2_.tx = 0;
    return 1;
  }
  if (u8g2_.ty > OLED_HEIGHT) {
    int lineH = u8g2_.getAscent() - u8g2_.getDescent() + 2;
    uint8_t *buf = u8g2_.getBufferPtr();
    for (int x = 0; x < OLED_WIDTH; x++) {
      uint64_t col = 0;
      for (int p = 0; p < 8; p++) {
        col |= (uint64_t)buf[x + p * OLED_WIDTH] << (p * 8);
      }
      col <<= lineH;
      for (int p = 0; p < 8; p++) {
        buf[x + p * OLED_WIDTH] = (uint8_t)(col >> (p * 8));
      }
    }
    u8g2_.ty -= lineH;
  }
  int cellW = u8g2_.getMaxCharWidth();
  int cellH = u8g2_.getAscent() - u8g2_.getDescent() + 1;
  u8g2_.setDrawColor(0);
  u8g2_.drawBox(u8g2_.tx, u8g2_.ty - u8g2_.getAscent(), cellW, cellH);
  u8g2_.setDrawColor(1);
  if (c == ' ') {
    u8g2_.tx += cellW;
    return 1;
  }
  return u8g2_.write(c);
}

size_t oled::print(const String &text) {
  if (!initialized_) return 0;
  size_t n = 0;
  for (unsigned int i = 0; i < text.length(); i++) {
    n += write((uint8_t)text[i]);
  }
  return n;
}

size_t oled::print(const char *text) {
  if (!initialized_ || !text) return 0;
  size_t n = 0;
  while (*text) {
    n += write((uint8_t)*text++);
  }
  return n;
}

size_t oled::print(int value) {
  if (!initialized_) return 0;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", value);
  return print(buf);
}

size_t oled::print(char value) {
  if (!initialized_) return 0;
  return write((uint8_t)value);
}

size_t oled::print(float value) {
  if (!initialized_) return 0;
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f", (double)value);
  return print(buf);
}

size_t oled::println(const String &text) {
  size_t n = print(text);
  n += write('\r');
  n += write('\n');
  return n;
}

size_t oled::println(const char *text) {
  size_t n = print(text);
  n += write('\r');
  n += write('\n');
  return n;
}

size_t oled::println(int value) {
  size_t n = print(value);
  n += write('\r');
  n += write('\n');
  return n;
}

size_t oled::println(float value) {
  size_t n = print(value);
  n += write('\r');
  n += write('\n');
  return n;
}

bool oled::clear(int waitToFinish) {
  (void)waitToFinish;
  if (!initialized_) {
    return false;
  }
  u8g2_.clearBuffer();
  return true;
}

bool oled::show(int waitToFinish) {
  (void)waitToFinish;
  if (!initialized_) {
    return false;
  }
  sendBuffer();
  return true;
}

void oled::clearDisplay() {
  (void)clear();
}

void oled::display() {
  (void)show();
}

void oled::setCursor(int x, int y) {
  if (!initialized_) {
    return;
  }
  // Callers pass top-left y (Adafruit convention); U8G2 baseline mode
  // needs y at the baseline, so shift down by the font ascent.
  u8g2_.setCursor(x, y + u8g2_.getAscent());
}

void oled::setTextSize(uint8_t size) {
  if (!initialized_) {
    return;
  }
  if (size >= kSizeCount) {
    size = kSizeCount - 1;
  }
  slotInFamily_ = size;
  textSize_ = size;
  applyHardwareFontState();
}

uint8_t oled::getTextSize() const {
  return textSize_;
}

void oled::setTextColor(uint16_t color) {
  if (!initialized_) {
    return;
  }
  u8g2_.setDrawColor(color ? 1 : 0);
}

void oled::setTextWrap(bool wrap) {
  (void)wrap;
  // U8G2's Print class always wraps at the display edge; no toggle needed.
}

void oled::setRotation(uint8_t rotation) {
  if (!initialized_) {
    return;
  }
  const u8g2_cb_t *cb = (rotation == 2) ? U8G2_R2 : U8G2_R0;
  u8g2_.setDisplayRotation(cb);
  flipped_ = (rotation == 2);
}

void oled::invertDisplay(bool invert) {
  if (!initialized_) {
    return;
  }
  // SSD1306 invert display command: 0xA6 = normal, 0xA7 = inverted
  u8x8_t *u8x8 = u8g2_.getU8x8();
  u8x8_cad_StartTransfer(u8x8);
  u8x8_cad_SendCmd(u8x8, invert ? 0xA7 : 0xA6);
  u8x8_cad_EndTransfer(u8x8);
}

void oled::setPowerSave(bool on) {
  if (!initialized_) {
    return;
  }
  u8g2_.setPowerSave(on ? 1 : 0);
}

void oled::bindInputs(const Inputs *inputs) { inputs_ = inputs; }

void oled::bindBatteryGauge(const BatteryGauge *gauge) { batteryGauge_ = gauge; }

void oled::bindAccel(const IMU *imu) { imu_ = imu; }

void oled::bindPin(const char *label, uint8_t pin) {
  boundPin_ = pin;
  hasBoundPin_ = true;

  if (label == nullptr || label[0] == '\0') {
    std::snprintf(boundPinLabel_, sizeof(boundPinLabel_), "PIN%u", static_cast<unsigned int>(pin));
    return;
  }

  std::strncpy(boundPinLabel_, label, sizeof(boundPinLabel_) - 1);
  boundPinLabel_[sizeof(boundPinLabel_) - 1] = '\0';
}

void oled::service() {
  if (!initialized_) {
    return;
  }

  const uint32_t nowMs = millis();

  // Advance any active contrast fade independently of the OLED render
  // cadence. The fade tick interval is ~16 ms; the render gate below
  // throttles to ~30 fps so the fade would otherwise step too slowly.
  if (fadeTotalTicks_ > 0) {
    tickContrastFade();
  }

  if (mpy_oled_hold) return;
  if (PanicReset::rebootPending()) return;

  if (nowMs - lastUpdateMs_ < Power::Policy::oledRefreshMs) {
    return;
  }
  lastUpdateMs_ = nowMs;
  tickCount_++;

  // When the badge pairing display owns the screen, render it.
  if (badgeDisplayActive) {
    static uint8_t* lastQrBits = nullptr;
    static int lastQrWidth = 0;
    static int lastQrHeight = 0;
    static bool qrLatched = false;

    if (renderMode == MODE_QR && qrBits && qrWidth > 0 && qrHeight > 0) {
      bool qrChanged = !qrLatched || screenDirty || lastQrBits != qrBits ||
                       lastQrWidth != qrWidth || lastQrHeight != qrHeight;
      if (!qrChanged) {
        return;
      }

      renderScreen();
      lastQrBits = qrBits;
      lastQrWidth = qrWidth;
      lastQrHeight = qrHeight;
      qrLatched = true;
      return;
    }

    qrLatched = false;
    screenDirty = true;
    renderScreen();
    return;
  }

  if (inputs_ == nullptr) {
    return;
  }

  const uint32_t buttonBits = (inputs_->buttons().up ? 1U : 0U) |
                              (inputs_->buttons().down ? 2U : 0U) |
                              (inputs_->buttons().left ? 4U : 0U) |
                              (inputs_->buttons().right ? 8U : 0U);
  const uint32_t faceDownBit =
      (imu_ != nullptr && imu_->isFaceDown()) ? 1U : 0U;
  const uint32_t pinStateBit =
      (hasBoundPin_ && digitalRead(boundPin_)) ? 1U : 0U;

  uint32_t batteryBucket = 0;
  if (batteryGauge_ != nullptr && batteryGauge_->isReady()) {
    const float pct = batteryGauge_->stateOfChargePercent();
    if (!std::isnan(pct) && pct > 0.f) {
      batteryBucket = static_cast<uint32_t>(pct / 5.f);  // 5% resolution
    }
  }

  const uint32_t hapticBucket = static_cast<uint32_t>(Haptics::strength());
  const uint32_t frameHash = (buttonBits) ^ (faceDownBit << 8) ^
                             (pinStateBit << 9) ^ (batteryBucket << 10) ^ (hapticBucket << 16) ^
                             (static_cast<uint32_t>(currentFontIndex_) << 18) ^
                             (static_cast<uint32_t>(textSize_) << 23) ^
                             (static_cast<uint32_t>(familyIdx_) << 26) ^
                             (static_cast<uint32_t>(slotInFamily_) << 29);
  if (frameHash == lastFrameHash_ && (nowMs - lastForcedDrawMs_) < 1000UL) {
    return;
  }
  lastFrameHash_ = frameHash;
  lastForcedDrawMs_ = nowMs;

  clearDisplay();
  setCursor(0, 0);
  print("U");
  print(inputs_->buttons().up ? 1 : 0);
  print(" D");
  print(inputs_->buttons().down ? 1 : 0);
  print(" L");
  print(inputs_->buttons().left ? 1 : 0);
  print(" R");
  println(inputs_->buttons().right ? 1 : 0);

  print("FD:");
  if (imu_ != nullptr) {
    print(imu_->isFaceDown() ? 1 : 0);
  } else {
    print("--");
  }
  print(" H:");
  println(static_cast<int>(Haptics::strength()));

  print("Bat: ");
  if (batteryGauge_ != nullptr && batteryGauge_->isReady()) {
    const float pct = batteryGauge_->stateOfChargePercent();
    const float volts = batteryGauge_->cellVoltage();
    char line[22];
    if (!std::isnan(pct) && !std::isnan(volts)) {
      std::snprintf(line, sizeof(line), "%.0f%% %.2fV", static_cast<double>(pct),
                    static_cast<double>(volts));
    } else {
      std::snprintf(line, sizeof(line), "--");
    }
    println(line);
  } else {
    println("--");
  }

  if (hasBoundPin_) {
    print(boundPinLabel_);
    print(": ");
    print(digitalRead(boundPin_) ? 1 : 0);
    println("");
  }

  println(getFontName());
  if (familyIdx_ == 0) {
    print("Sz ");
    println(static_cast<int>(textSize_));
  } else {
    println("Hold L=font U=size");
  }

  display();
}

const char *oled::name() const { return "OLED"; }

bool oled::setFont(const char *name) {
  const int idx = lookupFontByName(name);
  if (idx < 0) {
    return false;
  }
  setFont(kReplayFonts[idx].font);
  return true;
}

void oled::setFont(const uint8_t *font) {
  if (font == nullptr) {
    familyIdx_ = 0;
    slotInFamily_ = 3;
    applyHardwareFontState();
    return;
  }
  for (uint8_t f = 0; f < kFontGridFamilyCount; ++f) {
    for (uint8_t s = 0; s < kSizeCount; ++s) {
      if (kFontGrid[f][s] == font) {
        familyIdx_ = f;
        slotInFamily_ = s;
        applyHardwareFontState();
        return;
      }
    }
  }
  u8g2_.setFont(font);
  currentFontIndex_ = -1;
}

void oled::setFontFamilyAndSlot(uint8_t family, uint8_t slot) {
  if (family >= kFontGridFamilyCount) {
    family = 0;
  }
  if (slot >= kSizeCount) {
    slot = kSizeCount - 1;
  }
  familyIdx_ = family;
  slotInFamily_ = slot;
  applyHardwareFontState();
}

int oled::setFontByPixelHeight(uint8_t family, uint8_t pixelHeight) {
  if (!initialized_) return 0;
  if (family >= kFontGridFamilyCount) family = 0;

  // Probe every slot in the family, measure its max glyph height via
  // u8g2, and remember the largest one that fits under pixelHeight.
  // Slots are ordered 0 (smallest) → 9 (largest) in kFontGrid so we
  // iterate forward and track the tallest that still qualifies; if
  // NONE qualify (pixelHeight smaller than the smallest font) we fall
  // back to slot 0. Each probe is a setFont() call which is cheap —
  // u8g2 just swaps a font pointer, no rasterization.
  uint8_t bestSlot = 0;
  int bestHeight = 0;
  bool found = false;
  for (uint8_t s = 0; s < kSizeCount; s++) {
    const uint8_t* font = kFontGrid[family][s];
    if (!font) continue;
    u8g2_.setFont(font);
    int h = u8g2_.getMaxCharHeight();
    if (h <= (int)pixelHeight) {
      if (!found || h > bestHeight) {
        bestHeight = h;
        bestSlot   = s;
        found      = true;
      }
    }
  }

  familyIdx_    = family;
  slotInFamily_ = bestSlot;
  applyHardwareFontState();
  return bestHeight > 0 ? bestHeight : getMaxCharHeight();
}

const char *oled::getFontName() const {
  if (currentFontIndex_ < 0 && familyIdx_ < kFontGridFamilyCount && slotInFamily_ < kSizeCount) {
    static char nameBuf[24];
    extern const char* const kFontFamilyNames[];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s %s",
                  kFontFamilyNames[familyIdx_], kSizeLabels[slotInFamily_]);
    return nameBuf;
  }
  return "Custom";
}

void oled::displayBitmap(int x, int y, const unsigned char *bitmap, int width, int height) {
  if (!initialized_) {
    return;
  }
  // Adafruit bitmaps are MSB-first; U8G2 drawXBM expects LSB-first (XBM).
  // Convert on the fly for small bitmaps.
  const int rowBytes = (width + 7) / 8;
  const int totalBytes = rowBytes * height;
  uint8_t *xbmBuf = (uint8_t *)alloca(totalBytes);
  for (int i = 0; i < totalBytes; ++i) {
    xbmBuf[i] = reverseBits(pgm_read_byte(&bitmap[i]));
  }
  u8g2_.setDrawColor(1);
  u8g2_.drawXBM(x, y, width, height, xbmBuf);
}

void oled::drawBitmap(int x, int y, const uint8_t *bitmap, int w, int h) {
  if (!initialized_) return;
  // Adafruit bitmaps are MSB-first; convert to LSB-first for U8G2 drawXBM.
  const int rowBytes = (w + 7) / 8;
  const int totalBytes = rowBytes * h;
  uint8_t *xbmBuf = (uint8_t *)alloca(totalBytes);
  for (int i = 0; i < totalBytes; ++i) {
    xbmBuf[i] = reverseBits(pgm_read_byte(&bitmap[i]));
  }
  u8g2_.drawXBM(x, y, w, h, xbmBuf);
}

// ─── Pixel-level access ──────────────────────────────────────────────────────

void oled::drawPixel(int x, int y) {
  if (!initialized_) return;
  u8g2_.drawPixel(x, y);
}

bool oled::getPixel(int x, int y){
  if (!initialized_) return false;
  if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return false;
  // U8G2 full-buffer SSD1306 uses page-based layout identical to the hardware:
  // byte at [x + (y/8)*width], bit (y%8).
  const uint8_t *buf = u8g2_.getBufferPtr();
  if (buf == nullptr) return false;
  return (buf[x + (y / 8) * OLED_WIDTH] >> (y & 7)) & 1;
}

uint8_t *oled::getBufferPtr() {
  if (!initialized_) return nullptr;
  return u8g2_.getBufferPtr();
}

int oled::width() const { return OLED_WIDTH; }
int oled::height() const { return OLED_HEIGHT; }

// ─── Font metrics ────────────────────────────────────────────────────────────

int oled::getAscent(){
  if (!initialized_) return 7;
  return u8g2_.getAscent();
}

int oled::getDescent(){
  if (!initialized_) return -2;
  return u8g2_.getDescent();
}

int oled::getMaxCharHeight(){
  if (!initialized_) return 8;
  return u8g2_.getMaxCharHeight();
}

int oled::getLineHeight(){
  if (!initialized_) return 10;
  auto& u = u8g2_;
  int h = u.getAscent() - u.getDescent();
  // if (slotInFamily_ == 0) h -= 2;
  // else if (slotInFamily_ == 1) h -= 1;
  return h;
}

// ─── High-level layout helpers ─────────────────────────────────────────────

namespace {

// Greedy word-by-word wrap at the currently-active font. Writes up to
// maxLines into lines[] using lineCap bytes each, stored at fixed
// lineStride in the flat buffer. Returns true iff the entire string
// fit; returns false if it overflowed (caller gets the first maxLines
// and can react). Always makes forward progress on overlong single
// words via hard-cut.
bool wrapStringAtFont(oled& d, const char* src, int maxW, size_t maxLines,
                      char* lines, size_t lineCap, size_t lineStride,
                      size_t* outCount) {
    auto lineAt = [&](size_t i) -> char* { return lines + i * lineStride; };
    for (size_t i = 0; i < maxLines; i++) lineAt(i)[0] = '\0';
    *outCount = 0;
    if (!src || !*src) return true;

    const char* p = src;
    size_t idx = 0;
    while (*p && idx < maxLines) {
        while (*p == ' ') p++;
        if (!*p) break;

        char line[64] = {};
        size_t linePos = 0;

        while (*p) {
            // CRITICAL: skip inter-word spaces at the top of every inner
            // iteration. Without this, after we accept a word the cursor
            // stays on the trailing space; the next iteration treats
            // that space as a zero-length "word", builds trial = "line "
            // (with trailing space), accepts it because width still
            // fits, and never advances. The line then fills with spaces
            // until trial width exceeds maxW, at which point the
            // stored line is "word<N spaces>". `getStrWidth` on that
            // returns > maxW, the caller clamps drawX to x=0, and the
            // "centered" line ends up visibly left-aligned (which is
            // the exact symptom we were seeing on the badge).
            while (*p == ' ') p++;
            if (!*p) break;

            const char* wordStart = p;
            while (*p && *p != ' ') p++;
            size_t wordLen = (size_t)(p - wordStart);
            if (wordLen == 0) break;  // defensive — shouldn't happen now

            char trial[72];
            if (linePos == 0) {
                std::snprintf(trial, sizeof(trial), "%.*s",
                              (int)wordLen, wordStart);
            } else {
                std::snprintf(trial, sizeof(trial), "%s %.*s",
                              line, (int)wordLen, wordStart);
            }

            int w = d.getStrWidth(trial);
            if (w <= maxW) {
                std::strncpy(line, trial, sizeof(line) - 1);
                line[sizeof(line) - 1] = '\0';
                linePos = std::strlen(line);
                continue;
            }

            if (linePos == 0) {
                // Word alone overflows — hard-cut to fit, then the next
                // line picks up where we left off.
                size_t k = wordLen;
                while (k > 1) {
                    std::snprintf(trial, sizeof(trial), "%.*s",
                                  (int)k, wordStart);
                    if (d.getStrWidth(trial) <= maxW) break;
                    k--;
                }
                std::strncpy(line, trial, sizeof(line) - 1);
                line[sizeof(line) - 1] = '\0';
                linePos = std::strlen(line);
                p = wordStart + k;
            } else {
                p = wordStart;  // defer this word to next line
            }
            break;
        }

        std::strncpy(lineAt(idx), line, lineCap - 1);
        lineAt(idx)[lineCap - 1] = '\0';
        idx++;
    }

    *outCount = idx;
    while (*p == ' ') p++;
    return !*p;
}

}  // namespace

uint8_t oled::drawNametag(const char* name, const char* subtitle,
                          const char* tertiary,
                          uint8_t x, uint8_t y,
                          uint8_t maxW, uint8_t maxH) {
    if (!name)     name     = "";
    if (!subtitle) subtitle = "";
    if (!tertiary) tertiary = "";

    // Clamp to screen — cheap guard against callers passing out-of-range
    // bounds.
    const int scrW = width();
    const int scrH = height();
    if (x >= scrW) return y;
    if (y >= scrH) return y;
    if (x + maxW > scrW) maxW = (uint8_t)(scrW - x);
    if (y + maxH > scrH) maxH = (uint8_t)(scrH - y);

    // ── Save caller's font state ──────────────────────────────────────
    // drawNametag stomps on familyIdx_ / slotInFamily_ as it cycles
    // through name tiers, subtitle, tertiary — so whatever the caller
    // had selected is clobbered by the time we return. Callers that
    // render text BEFORE and AFTER the nametag in the same frame (e.g.
    // list rows) would silently inherit Spleen
    // 5x8 until they remembered to re-select their font. We snapshot
    // the caller's intent here and restore it on every exit path.
    const uint8_t savedFamily = familyIdx_;
    const uint8_t savedSlot   = slotInFamily_;
    const int     savedCustom = currentFontIndex_;
    auto restoreCallerFont = [&]() {
        if (savedCustom >= 0 && savedCustom < kReplayFontCount) {
            // Caller had a specific font pointer set (not a grid slot).
            // Reapply it; setFont() will itself reconcile familyIdx_ /
            // slotInFamily_ / currentFontIndex_ consistently.
            setFont(kReplayFonts[savedCustom].font);
        } else {
            setFontFamilyAndSlot(savedFamily, savedSlot);
        }
    };

    // ── Font strategy ──────────────────────────────────────────────────
    // Spleen family (index 8 in kFontGrid) — clean, blocky pixel-bitmap
    // monospace with excellent legibility at every size. The Spleen
    // grid slots:
    //   slot 2 → spleen5x8    (~8 px tall, 5 px wide)
    //   slot 3 → spleen5x8    (same, duplicated — no smaller Spleen)
    //   slot 4 → spleen6x12   (~12 px)
    //   slot 5 → spleen6x12   (same)
    //   slot 6 → spleen8x16   (~16 px)
    //   slot 7 → spleen12x24  (~24 px)
    //   slot 8 → spleen16x32  (~32 px, half the screen)
    //   slot 9 → spleen32x64  (~64 px, fills the entire screen height —
    //                          useful only for 1-2 char displays)
    //
    // The nametag ladder deliberately skips slot 9: a 64-px glyph
    // leaves no vertical room for subtitle / tertiary. For "Frosty
    // Wolf" (11 chars × 16 px = 176 px at spleen16x32) the tier
    // search will drop a few steps until it fits within 128 px.
    //
    // Subtitle (company) and tertiary (title) land on Spleen's smaller
    // sizes so the hierarchy reads: name (biggest) > company (12 px)
    // > title (8 px). Spleen doesn't offer a size between 6x12 and
    // 5x8 so there's a noticeable 12 → 8 px drop between them.
    //
    // Swap `kFamily` to re-skin every nametag on the device in one line:
    //   1 = Profont  (mono, profont10 → profont29)
    //   7 = Terminus (mono, t0_11 → t0_30 / t0_30b)
    //   8 = Spleen   (mono, spleen5x8 → spleen32x64)
    constexpr uint8_t kSpleen       = 8;
    constexpr uint8_t kSubtitleSlot = 5;   // spleen6x12 — "company" line (~12 px)
    constexpr uint8_t kTertiarySlot = 3;   // spleen5x8  — "title" line (~8 px)
    // Start from spleen16x32 (slot 8) and cascade down. Slot 9 (32x64)
    // is omitted on purpose — see block comment above.
    constexpr uint8_t kNameTiers[]  = { 8, 7, 6, 5, 4, 3 };
    constexpr uint8_t kFamily       = kSpleen;

    constexpr size_t  kLineCap     = 32;
    constexpr size_t  kNameMaxLns  = 6;
    constexpr size_t  kSubMaxLns   = 2;
    constexpr size_t  kTerMaxLns   = 2;
    constexpr uint8_t kLeading     = 2;    // +2 px leading inside each text block
    constexpr uint8_t kGap         = 3;    // +3 px between blocks (name/subtitle/tertiary)

    static char nameLines[kNameMaxLns][kLineCap];
    static char subLines [kSubMaxLns ][kLineCap];
    static char terLines [kTerMaxLns ][kLineCap];

    const bool hasSub = subtitle[0] != '\0';
    const bool hasTer = tertiary[0] != '\0';

    // ── Subtitle pre-pass ─────────────────────────────────────────────
    // Measure + wrap subtitle/tertiary first so the name's vertical
    // budget reserves the right amount of room underneath. Each runs
    // at its own slot and is allowed to wrap up to 2 lines so a long
    // company or long title won't get cropped.
    int subAscent = 0, subLineH = 0;
    size_t subCount = 0;
    int subBlockH = 0;
    if (hasSub) {
        setFontFamilyAndSlot(kFamily, kSubtitleSlot);
        subAscent = getAscent();
        subLineH  = (getAscent() - getDescent()) + kLeading;
        if (subLineH < 1) subLineH = 9;
        wrapStringAtFont(*this, subtitle, maxW, kSubMaxLns,
                         &subLines[0][0], kLineCap, kLineCap, &subCount);
        subBlockH = (int)subCount * subLineH;
    }

    int terAscent = 0, terLineH = 0;
    size_t terCount = 0;
    int terBlockH = 0;
    if (hasTer) {
        setFontFamilyAndSlot(kFamily, kTertiarySlot);
        terAscent = getAscent();
        terLineH  = (getAscent() - getDescent()) + kLeading;
        if (terLineH < 1) terLineH = 9;
        wrapStringAtFont(*this, tertiary, maxW, kTerMaxLns,
                         &terLines[0][0], kLineCap, kLineCap, &terCount);
        terBlockH = (int)terCount * terLineH;
    }

    int footerH = 0;
    if (hasSub && subCount > 0)  footerH += kGap + subBlockH;
    if (hasTer && terCount > 0)  footerH += kGap + terBlockH;

    int nameBudget = (int)maxH - footerH;
    if (nameBudget < 0) nameBudget = 0;

    // ── Name tier search ──────────────────────────────────────────────
    // Try tiers largest → smallest. For each, allowLines = floor(budget
    // / lineH). Accept the first tier whose wrap fits within that line
    // cap. If nothing fits, the smallest tier with the full kNameMaxLns
    // cap renders at the cost of overflowing maxH — "never crop".
    uint8_t chosenSlot = kNameTiers[sizeof(kNameTiers) / sizeof(kNameTiers[0]) - 1];
    int  nameLineH  = 9;
    int  nameAscent = 7;
    size_t nameCount = 0;
    bool fit = false;

    for (uint8_t slot : kNameTiers) {
        setFontFamilyAndSlot(kFamily, slot);
        const int asc  = getAscent();
        const int desc = getDescent();
        int lineH = (asc - desc) + kLeading;
        if (lineH < 1) lineH = 9;

        size_t allow = (nameBudget > 0)
                           ? (size_t)(nameBudget / lineH) : 1;
        if (allow < 1) allow = 1;
        if (allow > kNameMaxLns) allow = kNameMaxLns;

        size_t count = 0;
        bool ok = wrapStringAtFont(*this, name, maxW, allow,
                                   &nameLines[0][0], kLineCap, kLineCap,
                                   &count);
        if (ok) {
            chosenSlot = slot;
            nameLineH  = lineH;
            nameAscent = asc;
            nameCount  = count;
            fit        = true;
            break;
        }
        chosenSlot = slot;
        nameLineH  = lineH;
        nameAscent = asc;
        nameCount  = count;
    }

    if (!fit) {
        setFontFamilyAndSlot(kFamily, chosenSlot);
        wrapStringAtFont(*this, name, maxW, kNameMaxLns,
                         &nameLines[0][0], kLineCap, kLineCap,
                         &nameCount);
        nameAscent = getAscent();
        nameLineH  = (getAscent() - getDescent()) + kLeading;
        if (nameLineH < 1) nameLineH = 9;
    }

    // ── Vertical-center calculation ──────────────────────────────────
    // Full rendered height = name block + per-footer-block (gap + block).
    // Split leftover space evenly above and below so the whole card is
    // visually centered inside (y, maxH). Content taller than maxH pins
    // to the top (topOffset = 0).
    setFontFamilyAndSlot(kFamily, chosenSlot);
    nameAscent = getAscent();
    nameLineH  = (getAscent() - getDescent()) + kLeading;
    if (nameLineH < 1) nameLineH = 9;

    const int nameBlockH = (int)nameCount * nameLineH;
    const int renderedH  = nameBlockH + footerH;

    int topOffset = ((int)maxH - renderedH) / 2;
    if (topOffset < 0) topOffset = 0;

    int curY = (int)y + topOffset;

    // ── Paint name (centered per line) ────────────────────────────────
    for (size_t i = 0; i < nameCount; i++) {
        int w = getStrWidth(nameLines[i]);
        int drawX = x + ((int)maxW - w) / 2;
        if (drawX < x) drawX = x;
        drawStr(drawX, curY + nameAscent, nameLines[i]);
        curY += nameLineH;
    }

    // ── Paint subtitle (company) ─────────────────────────────────────
    if (hasSub && subCount > 0) {
        curY += kGap;
        setFontFamilyAndSlot(kFamily, kSubtitleSlot);
        subAscent = getAscent();
        subLineH  = (getAscent() - getDescent()) + kLeading;
        if (subLineH < 1) subLineH = 9;
        for (size_t i = 0; i < subCount; i++) {
            int w = getStrWidth(subLines[i]);
            int drawX = x + ((int)maxW - w) / 2;
            if (drawX < x) drawX = x;
            drawStr(drawX, curY + subAscent, subLines[i]);
            curY += subLineH;
        }
    }

    // ── Paint tertiary (title) ───────────────────────────────────────
    if (hasTer && terCount > 0) {
        curY += kGap;
        setFontFamilyAndSlot(kFamily, kTertiarySlot);
        terAscent = getAscent();
        terLineH  = (getAscent() - getDescent()) + kLeading;
        if (terLineH < 1) terLineH = 9;
        for (size_t i = 0; i < terCount; i++) {
            int w = getStrWidth(terLines[i]);
            int drawX = x + ((int)maxW - w) / 2;
            if (drawX < x) drawX = x;
            drawStr(drawX, curY + terAscent, terLines[i]);
            curY += terLineH;
        }
    }

    // Restore the caller's active font so subsequent draw calls in the
    // same frame see the state they set before calling drawNametag.
    restoreCallerFont();

    return (uint8_t)(curY < 0 ? 0 : (curY > 255 ? 255 : curY));
}

// ─── U8G2 drawing API ───────────────────────────────────────────────────────

void oled::clearBuffer() {
  if (!initialized_) return;
  u8g2_.clearBuffer();
}

void oled::sendBuffer() {
  if (!initialized_) return;
  u8g2_.sendBuffer();
}

void oled::drawStr(int x, int y, const char *str) {
  if (!initialized_ || !str) return;
  u8g2_.drawStr(x, y, str);
}

int oled::getStrWidth(const char *str) {
  if (!initialized_ || !str) return 0;
  return u8g2_.getStrWidth(str);
}

void oled::drawUTF8(int x, int y, const char *str) {
  if (!initialized_ || !str) return;
  u8g2_.drawUTF8(x, y, str);
}

int oled::getUTF8Width(const char *str) {
  if (!initialized_ || !str) return 0;
  return u8g2_.getUTF8Width(str);
}

void oled::setClipWindow(int x0, int y0, int x1, int y1) {
  if (!initialized_) return;
  u8g2_.setClipWindow(static_cast<u8g2_uint_t>(x0),
                      static_cast<u8g2_uint_t>(y0),
                      static_cast<u8g2_uint_t>(x1),
                      static_cast<u8g2_uint_t>(y1));
}

void oled::setMaxClipWindow() {
  if (!initialized_) return;
  u8g2_.setMaxClipWindow();
}

void oled::setFontDirection(uint8_t dir) {
  if (!initialized_) return;
  u8g2_.setFontDirection(dir & 0x3);
}

void oled::drawXBM(int x, int y, int w, int h, const uint8_t *bits) {
  if (!initialized_) return;
  u8g2_.drawXBM(x, y, w, h, bits);
}

void oled::setBitmapTransparent(bool transparent) {
  if (!initialized_) return;
  u8g2_.setBitmapMode(transparent ? 1 : 0);
}

void oled::setFontMode(uint8_t mode) {
  if (!initialized_) return;
  u8g2_.setFontMode(mode ? 1 : 0);
}

void oled::drawXBM(int x, int y, int srcW, int srcH, const uint8_t *bits,
                   int dstW, int dstH) {
  if (!initialized_) return;
  if (dstW >= srcW && dstH >= srcH) {
    u8g2_.drawXBM(x, y, srcW, srcH, bits);
    return;
  }
  uint8_t dstRowBytes = (dstW + 7) / 8;
  uint16_t bufSize = dstRowBytes * dstH;
  uint8_t buf[128];
  if (bufSize > sizeof(buf)) {
    u8g2_.drawXBM(x, y, srcW, srcH, bits);
    return;
  }
  ImageScaler::scale(bits, srcW, srcH, buf, dstW, dstH);
  u8g2_.drawXBM(x, y, dstW, dstH, buf);
}

void oled::drawBox(int x, int y, int w, int h) {
  if (!initialized_) return;
  u8g2_.drawBox(x, y, w, h);
}

// u8g2 requires 2*r < min(w,h); violating it silently draws garbage.
// Clamp r to a safe max so callers can pass a design-constant radius
// even when the box is very short (e.g. a 1-line chat bubble that
// ends up only 10 px wide).
static inline int clampCornerRadius(int w, int h, int r) {
  if (r < 0) r = 0;
  const int maxR = (w < h ? w : h) / 2 - 1;
  if (r > maxR) r = (maxR < 0 ? 0 : maxR);
  return r;
}

void oled::drawRBox(int x, int y, int w, int h, int r) {
  if (!initialized_ || w <= 0 || h <= 0) return;
  r = clampCornerRadius(w, h, r);
  if (r <= 0) {
    u8g2_.drawBox(x, y, w, h);
  } else {
    u8g2_.drawRBox(x, y, w, h, r);
  }
}

void oled::drawRFrame(int x, int y, int w, int h, int r) {
  if (!initialized_ || w <= 0 || h <= 0) return;
  r = clampCornerRadius(w, h, r);
  if (r <= 0) {
    u8g2_.drawFrame(x, y, w, h);
  } else {
    u8g2_.drawRFrame(x, y, w, h, r);
  }
}

void oled::drawHLine(int x, int y, int w) {
  if (!initialized_) return;
  u8g2_.drawHLine(x, y, w);
}

void oled::drawVLine(int x, int y, int h) {
  if (!initialized_) return;
  u8g2_.drawVLine(x, y, h);
}

void oled::drawLine(int x0, int y0, int x1, int y1) {
  if (!initialized_) return;
  u8g2_.drawLine(x0, y0, x1, y1);
}

void oled::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2) {
  if (!initialized_) return;
  u8g2_.drawTriangle(x0, y0, x1, y1, x2, y2);
}

void oled::setDrawColor(uint8_t color) {
  if (!initialized_) return;
  u8g2_.setDrawColor(color > 2 ? 1 : color);
}

void oled::setFlipped(bool flipped) {
  flipped_ = flipped;
  if (!initialized_) return;
  // Keep rotation inside U8g2 so its draw coordinates and the controller's
  // segment/COM direction always agree. Sending partial remap commands here
  // can leave the SSD1309 mirrored after a tilt transition.
  u8g2_.setDisplayRotation(flipped ? U8G2_R2 : U8G2_R0);
}

void oled::setClockDivOsc(uint8_t osc, uint8_t div) {
  if (!initialized_) return;
  if (div < 1) div = 1;
  uint8_t val = ((osc & 0x0F) << 4) | ((div - 1) & 0x0F);
  u8g2_.sendF("ca", 0xD5, val);
}

void oled::setMuxRatio(uint8_t mux) {
  if (!initialized_) return;
  if (mux < 16) mux = 16;
  if (mux > 64) mux = 64;
  u8g2_.sendF("ca", 0xA8, mux - 1);
}

void oled::setPrecharge(uint8_t phase1, uint8_t phase2) {
  if (!initialized_) return;
  if (phase1 < 1) phase1 = 1;
  if (phase2 < 1) phase2 = 1;
  uint8_t val = ((phase2 & 0x0F) << 4) | (phase1 & 0x0F);
  u8g2_.sendF("ca", 0xD9, val);
}

void oled::setContrast(uint8_t contrast) {
  if (!initialized_) return;
  writeContrastRegisters(contrast);
  // Cancel any in-flight fade — caller is taking direct control.
  fadeTotalTicks_ = 0;
  fullContrast_ = contrast;
}

// ─── Hardware contrast fade ─────────────────────────────────────────────────
//
// SSD1309 register 0x81 alone barely moves perceived brightness on the
// XIAO/ESP32 panels we ship; the auxiliary 0xDB (Vcomh) and 0xD9 (pre-charge)
// registers must scale together with it. The ratios below match the reference
// firmware's curve (see Replay-26-Badge_QAFW/Main-Firmware/display.cpp).
void oled::writeContrastRegisters(uint8_t level) {
  if (!initialized_) return;
  const float t  = (float)level / 255.0f;
  const uint8_t p1 = (uint8_t)(15.0f - t * 13.0f);          // 15 → 2
  const uint8_t p2 = (uint8_t)(1.0f  + t * 14.0f);          //  1 → 15
  const uint8_t vcomh = (uint8_t)((uint8_t)(t * 2.0f) << 4);
  u8g2_.sendF("ca", 0x81, level);
  u8g2_.sendF("ca", 0xDB, vcomh);
  u8g2_.sendF("ca", 0xD9, (uint8_t)((p2 << 4) | p1));
  contrast_ = level;
}

void oled::startContrastFade(uint8_t target, uint32_t durationMs) {
  fadeFrom_       = contrast_;
  fadeTarget_     = target;
  fadeTickCount_  = 0;
  // Round up to the nearest tick so a zero-duration fade still completes.
  uint32_t ticks  = (durationMs + kFadeTickMs - 1) / kFadeTickMs;
  if (ticks == 0) ticks = 1;
  if (ticks > UINT16_MAX) ticks = UINT16_MAX;
  fadeTotalTicks_ = (uint16_t)ticks;
}

void oled::tickContrastFade() {
  if (fadeTotalTicks_ == 0) return;

  fadeTickCount_++;
  if (fadeTickCount_ >= fadeTotalTicks_) {
    writeContrastRegisters(fadeTarget_);
    fadeTotalTicks_ = 0;
    return;
  }

  // Integer linear interpolation: (from*remaining + target*elapsed) / total
  const uint32_t elapsed   = fadeTickCount_;
  const uint32_t remaining = fadeTotalTicks_ - elapsed;
  const uint32_t total     = fadeTotalTicks_;
  const uint8_t  lvl       =
      (uint8_t)(((uint32_t)fadeFrom_ * remaining
               + (uint32_t)fadeTarget_ * elapsed) / total);
  if (lvl != contrast_) writeContrastRegisters(lvl);
}

bool oled::isContrastIdle() const { return fadeTotalTicks_ == 0; }

void oled::awaitContrastFade() {
  // Inline tick loop — used by transitionOut/In. Keeps WiFi/IR running on
  // their own cores while the cooperative scheduler pauses for ~200 ms.
  while (fadeTotalTicks_ > 0) {
    tickContrastFade();
    delay(kFadeTickMs);
  }
}

void oled::transitionOut(uint32_t durationMs) {
  if (contrast_ == 0 && isContrastIdle()) return;
  startContrastFade(0, durationMs);
  awaitContrastFade();
}

void oled::transitionIn(uint32_t durationMs) {
  startContrastFade(fullContrast_, durationMs);
  awaitContrastFade();
}

void oled::rotateBuffer180() {
  uint8_t *buf = u8g2_.getBufferPtr();
  if (buf == nullptr) return;
  const int total = (OLED_WIDTH * OLED_HEIGHT) / 8;
  int lo = 0, hi = total - 1;
  while (lo < hi) {
    uint8_t a = reverseBits(buf[lo]);
    uint8_t b = reverseBits(buf[hi]);
    buf[lo] = b;
    buf[hi] = a;
    ++lo; --hi;
  }
  if (lo == hi) {
    buf[lo] = reverseBits(buf[lo]);
  }
}

// ─── ImageScaler ─────────────────────────────────────────────────────────────

namespace ImageScaler {

void scale(const uint8_t* src, uint8_t srcW, uint8_t srcH,
           uint8_t* dst, uint8_t dstW, uint8_t dstH) {
    const uint8_t srcRowBytes = (srcW + 7) / 8;
    const uint8_t dstRowBytes = (dstW + 7) / 8;
    std::memset(dst, 0, dstRowBytes * dstH);

    const uint8_t scaleX = srcW / dstW;
    const uint8_t scaleY = srcH / dstH;
    const uint8_t threshold = (scaleX * scaleY) / 2;

    for (uint8_t dy = 0; dy < dstH; dy++) {
        const uint8_t sy0 = dy * scaleY;
        for (uint8_t dx = 0; dx < dstW; dx++) {
            const uint8_t sx0 = dx * scaleX;
            uint8_t count = 0;
            for (uint8_t by = 0; by < scaleY; by++) {
                const uint8_t sy = sy0 + by;
                for (uint8_t bx = 0; bx < scaleX; bx++) {
                    const uint8_t sx = sx0 + bx;
                    uint8_t byte = pgm_read_byte(&src[sy * srcRowBytes + sx / 8]);
                    if ((byte >> (sx & 7)) & 1) count++;
                }
            }
            if (count > threshold) {
                dst[dy * dstRowBytes + dx / 8] |= (1 << (dx & 7));
            }
        }
    }
}

uint8_t availableScales(const ImageInfo& img, uint8_t* outDims, uint8_t maxOut) {
    uint8_t count = 0;
    auto addScales = [&](uint8_t srcDim) {
        for (uint8_t div = 1; div <= 8 && count < maxOut; div *= 2) {
            if (srcDim % div != 0) continue;
            uint8_t dim = srcDim / div;
            if (dim < 4) break;
            bool dup = false;
            for (uint8_t i = 0; i < count; i++) { if (outDims[i] == dim) { dup = true; break; } }
            if (!dup) outDims[count++] = dim;
        }
    };
    if (img.data64) addScales(64);
    if (img.data48) addScales(48);
    // Sort descending
    for (uint8_t i = 1; i < count; i++) {
        for (uint8_t j = i; j > 0 && outDims[j] > outDims[j-1]; j--) {
            uint8_t tmp = outDims[j]; outDims[j] = outDims[j-1]; outDims[j-1] = tmp;
        }
    }
    return count;
}

const ImageInfo* find(const char* name) {
    if (!name) return nullptr;
    for (uint8_t i = 0; i < kImageCatalogCount; i++) {
        if (std::strcmp(kImageCatalog[i].name, name) == 0)
            return &kImageCatalog[i];
    }
    return nullptr;
}

}  // namespace ImageScaler

// ─── Font presets ────────────────────────────────────────────────────────────

void oled::setFontPreset(FontPreset preset) {
  if (!initialized_) return;
  switch (preset) {
    case FONT_TINY:
      u8g2_.setFont(UIFonts::kText);
      break;
    case FONT_SMALL:
      u8g2_.setFont(u8g2_font_6x10_tr);
      break;
    case FONT_LARGE:
      u8g2_.setFont(u8g2_font_profont22_tr);
      break;
    case FONT_XLARGE:
      u8g2_.setFont(u8g2_font_inb24_mr);
      break;
    case FONT_EMOJI:
      // Generated GNU Unifont subset for the badge emoji keyboard.
      // Supported emoji are remapped to printable chars 0x20..0x73;
      // use drawStr() with the remapped char.
      u8g2_.setFont(u8g2_font_badge_emoji);
      break;
  }
}
