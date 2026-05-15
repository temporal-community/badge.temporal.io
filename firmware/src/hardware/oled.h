#ifndef OLED_H
#define OLED_H

#include <Arduino.h>
#include <U8g2lib.h>
#include "../ui/FontCatalog.h"
#include "../infra/Scheduler.h"

// ReplayFont, kReplayFonts, kReplayFontCount, kFontGrid, kFontFamilyCount,
// kSizeCount, kSizeLabels, fontDisplayName all live in FontCatalog.h now.

enum FontPreset : uint8_t {
  FONT_TINY,    // Smallsimple UI text
  FONT_SMALL,   // ~6x10 (Iosevka-equivalent 6pt)
  FONT_LARGE,   // ~16pt (Berkeley-equivalent 12pt)
  FONT_XLARGE,  // ~32px (large built-in)
  FONT_EMOJI,   // badge emoji subset (~16x16)
};

class Inputs;
class BatteryGauge;
class IMU;

class oled : public IService {
 public:
  oled();
  int init();

  // Basic text output helpers
  size_t print(const String &text);
  size_t print(const char *text);
  size_t print(int value);
  size_t print(char value);
  size_t print(float value);
  size_t println(const String &text);
  size_t println(const char *text);
  size_t println(int value);
  size_t println(float value);
  size_t write(uint8_t c);

  // Display controls
  bool clear(int waitToFinish = 0);
  bool show(int waitToFinish = 0);
  void clearDisplay();
  void display();
  void setCursor(int x, int y);
  void setTextSize(uint8_t size);
  uint8_t getTextSize() const;
  void setTextColor(uint16_t color);
  void setTextWrap(bool wrap);
  void setRotation(uint8_t rotation);
  void invertDisplay(bool invert);
  void setPowerSave(bool on);
  void bindInputs(const Inputs *inputs);
  void bindBatteryGauge(const BatteryGauge *gauge);
  void bindAccel(const IMU *imu);
  void bindPin(const char *label, uint8_t pin);
  void service() override;
  const char *name() const override;

  // Font management
  bool setFont(const char *name);
  void setFont(const uint8_t *font);
  const char *getFontName() const;
  uint8_t fontFamilyIndex() const { return familyIdx_; }
  uint8_t fontSlotIndex() const { return slotInFamily_; }
  void setFontFamilyAndSlot(uint8_t family, uint8_t slot);
  void setFontPreset(FontPreset preset);

  // Pixel-size-based font selection. Picks the largest slot within
  // `family` whose max glyph height is <= pixelHeight. If nothing fits,
  // falls back to the smallest slot. Returns the actual max char
  // height of the chosen font.
  //
  // Use this when you want "about 14 px of text" without caring which
  // specific slot in the family that maps to, or when a caller wants
  // to request text by physical size rather than the family-agnostic
  // FontPreset enum.
  int setFontByPixelHeight(uint8_t family, uint8_t pixelHeight);

  // Drawing primitives
  void displayBitmap(int x, int y, const unsigned char *bitmap, int width, int height);
  void drawBitmap(int x, int y, const uint8_t *bitmap, int w, int h);

  // Pixel-level access (for MicroPython bridge)
  void drawPixel(int x, int y);
  bool getPixel(int x, int y);
  uint8_t *getBufferPtr();
  int width() const;
  int height() const;

  // Font metrics
  int getAscent();
  int getDescent();
  int getMaxCharHeight();
  int getLineHeight();

  // High-level layout helpers ─────────────────────────────────────────────
  //
  // drawNametag — render a prominent `name` plus up to two smaller
  // secondary lines (`subtitle` and optional `tertiary`), centered and
  // vertically centered inside the (x, y, maxW, maxH) rect. Picks the
  // largest Profont size that lets the entire name fit without
  // truncation and word-wraps to as many lines as needed. Subtitle and
  // tertiary are always rendered in a smaller Profont size and each
  // may wrap to 2 lines. Line spacing uses the active font's real
  // ascent/descent plus a small leading value so adjacent lines never
  // overlap regardless of which size is chosen.
  //
  // Typical use on the contact card:
  //   name     = "Frosty Wolf"    (big)
  //   subtitle = "WaveTrace"      (company, medium)
  //   tertiary = "Byte Alchemist" (title, smaller)
  //
  // Does NOT draw a divider — callers paint their own if they want one.
  //
  // Returns the pixel row immediately below the last drawn glyph.
  uint8_t drawNametag(const char *name,
                      const char *subtitle,
                      const char *tertiary,
                      uint8_t x, uint8_t y, uint8_t maxW, uint8_t maxH);

  // U8G2 drawing API
  void clearBuffer();
  void sendBuffer();
  void drawStr(int x, int y, const char *str);
  int  getStrWidth(const char *str);
  // UTF-8-aware variants. Route multi-byte sequences (emoji glyphs
  // from unifont presets) through u8g2's UTF-8 decoder instead of
  // treating them as raw bytes. For ASCII-only input the behavior
  // is identical to `drawStr` / `getStrWidth`.
  void drawUTF8(int x, int y, const char *str);
  int  getUTF8Width(const char *str);
  // Clip subsequent drawing to a rectangle (inclusive x0,y0 to
  // exclusive x1,y1 — matches u8g2's convention). Applies to
  // drawStr / drawBox / drawRBox / drawPixel / drawHLine etc.
  // Pair with `setMaxClipWindow()` to reset to the full 128×64.
  void setClipWindow(int x0, int y0, int x1, int y1);
  void setMaxClipWindow();
  // Font rotation for drawStr. Values map to U8g2:
  //   0 = left-to-right (default)
  //   1 = top-to-bottom (90° CW)
  //   2 = right-to-left, upside-down (180°)
  //   3 = bottom-to-top (90° CCW)
  // After dir=2, the reference (x, y) passed to drawStr is the top-right
  // of the rendered text; glyphs extend left and downward from there.
  // Remember to reset to 0 after drawing rotated text.
  void setFontDirection(uint8_t dir);
  void drawXBM(int x, int y, int w, int h, const uint8_t *bits);
  void drawXBM(int x, int y, int srcW, int srcH, const uint8_t *bits,
               int dstW, int dstH);
  // Toggle u8g2's bitmap mode: solid (0 = paint background) is the default
  // and overwrites the area; transparent (1 = skip 0-bits) preserves
  // underlying pixels and gives OR-blend semantics for overlapping bitmaps.
  void setBitmapTransparent(bool transparent);
  // u8g2 font mode: 0 = solid background, 1 = transparent glyph background.
  void setFontMode(uint8_t mode);
  void drawBox(int x, int y, int w, int h);
  // Rounded-rectangle helpers. `r` is the corner radius in pixels; u8g2
  // requires 2*r < min(w,h) so callers should keep r small relative to
  // the box dimensions (we clamp internally to keep it safe).
  void drawRBox(int x, int y, int w, int h, int r);    // filled
  void drawRFrame(int x, int y, int w, int h, int r);  // outline only
  void drawHLine(int x, int y, int w);
  void drawVLine(int x, int y, int h);
  void drawLine(int x0, int y0, int x1, int y1);
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2);
  void setDrawColor(uint8_t color);

  void setFlipped(bool flipped);
  bool isFlipped() const { return flipped_; }

  // SSD1309 display timing registers (for tuning frame rate / tearing)
  void setClockDivOsc(uint8_t osc, uint8_t div);
  void setMuxRatio(uint8_t mux);
  void setPrecharge(uint8_t phase1, uint8_t phase2);
  void setContrast(uint8_t contrast);

  // ─── Hardware contrast fade ───────────────────────────────────────────────
  //
  // Tick-based interpolation of three SSD1309 brightness registers
  // (0x81 contrast, 0xDB Vcomh deselect, 0xD9 pre-charge). 0x81 alone
  // produces nearly no perceptible change on this panel — all three must
  // move together, matching what the reference firmware does.
  //
  // startContrastFade: kicks off an async ramp from current contrast to
  //   target over durationMs. Returns immediately. Ticks advance from
  //   service() at the OLED refresh cadence (~30 fps).
  // tickContrastFade: advance one step. Public so blocking helpers can
  //   spin on it.
  // awaitContrastFade: spin+delay until the current ramp settles.
  // isContrastIdle: true when no fade is in progress.
  // transitionOut/In: convenience blocking wrappers used by GUIManager
  //   on screen push/pop. Default duration matches the reference (275 ms).
  void startContrastFade(uint8_t target, uint32_t durationMs);
  void tickContrastFade();
  void awaitContrastFade();
  bool isContrastIdle() const;
  void transitionOut(uint32_t durationMs = 200);
  void transitionIn(uint32_t durationMs = 200);
  uint8_t currentContrast() const { return contrast_; }
  uint8_t fullContrast() const { return fullContrast_; }
  void setFullContrast(uint8_t level) { fullContrast_ = level; }

  void applyHardwareFontState();

 private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2_;
  bool initialized_;
  const Inputs *inputs_;
  const BatteryGauge *batteryGauge_;
  const IMU *imu_;
  bool hasBoundPin_;
  uint8_t boundPin_;
  char boundPinLabel_[12];
  uint32_t lastUpdateMs_;
  uint32_t tickCount_;
  uint32_t lastFrameHash_;
  uint32_t lastForcedDrawMs_;
  uint8_t textSize_;
  int currentFontIndex_;
  uint8_t familyIdx_;
  uint8_t slotInFamily_;
  uint8_t defaultBuiltinScale_;
  bool flipped_ = false;

  // Contrast ramp state. fadeTotalTicks_ == 0 means idle.
  uint8_t  contrast_         = 255;  // last value pushed to hardware
  uint8_t  fullContrast_     = 255;  // "bright" target for transitionIn()
  uint8_t  fadeFrom_         = 0;
  uint8_t  fadeTarget_       = 0;
  uint16_t fadeTickCount_    = 0;    // ticks elapsed in current fade
  uint16_t fadeTotalTicks_   = 0;    // 0 = idle
  static constexpr uint32_t kFadeTickMs = 100;

  void writeContrastRegisters(uint8_t level);

  void rotateBuffer180();
};

#endif
