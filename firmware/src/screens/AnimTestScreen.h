#pragma once
#include "Screen.h"

// ─── Animation test screen (image catalog viewer with scaling) ──────────────
//
// Two modes:
//   1. Catalog mode (default) — cycles through the built-in `kImageCatalog`
//      ziggy frames. Same look-and-feel as the original animation test.
//   2. External mode — owns a PSRAM-backed bit buffer + frame metadata
//      loaded from a file (`.xbm`, `.fb`, or credit `.bin`). Catalog cycling
//      is suppressed while external mode is active; B = pop back to caller (Files
//      browser), Confirm pops as well so external viewing reads as a
//      regular image opener.
//
// External mode is entered via `loadXBM` / `loadFB` / `loadCreditBin` and
// cleared on `onExit`. Memory is released eagerly so we don't pin PSRAM
// while the screen is offstack.

class AnimTestScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenAnimTest; }
  bool showCursor() const override { return false; }

  // Open an .xbm file in external mode. Width/height parsed from the
  // `_width` / `_height` macros; bit array from `_bits[]`.
  void loadXBM(const char* path);
  // Open a raw 1bpp framebuffer (`.fb`) in external mode. Dimensions
  // come from the sibling `info.json` when present, else inferred from
  // file size. Multi-frame .fb files become animation frames.
  void loadFB(const char* path);
  // Credit prebinned `.bin`: `width(LE16) height(LE16)` then row-major
  // packed bits with bit 7 = leftmost per byte (same as
  // assets/credits/prebinned/ + scripts/gen_credit_xbms.py). Bytes are
  // reversed for `drawXBM` (LSB-left). Returns false if the file does
  // not match that layout (caller may open hex view instead).
  bool loadCreditBin(const char* path);

 private:
  uint8_t imageIdx_ = 0;
  uint8_t frameIdx_ = 0;
  uint8_t scaleIdx_ = 0;
  uint16_t frameDelayMs_ = 200;
  uint32_t lastFrameMs_ = 0;
  uint32_t lastJoyNavMs_ = 0;

  static constexpr uint16_t kJoyDeadband = 400;
  static constexpr uint16_t kMinDelay = 5;
  static constexpr uint16_t kMaxDelay = 2000;
  static constexpr uint16_t kDelayStep = 10;
  static constexpr uint16_t kDefaultDelay = 200;

  uint8_t scaleDims_[8];
  uint8_t scaleCount_ = 0;

  // ── External-file state (loadXBM / loadFB / loadCreditBin). When extBits_ != null
  //    the screen renders this buffer instead of the catalog. ─────
  enum class ExtFormat : uint8_t {
    kXBM,   // row-major, LSB-leftmost (per-row stride = ceil(W/8))
    kPage,  // SSD1306 page format (8 vertical px per byte) used by
            // `.fb` files written through `oled_set_framebuffer()`.
  };

  uint8_t* extBits_ = nullptr;
  uint16_t extW_ = 0;
  uint16_t extH_ = 0;
  uint8_t extFrameCount_ = 1;
  uint32_t extFrameBytes_ = 0;
  ExtFormat extFormat_ = ExtFormat::kXBM;
  char extName_[40] = {};
  bool extLoadError_ = false;

  bool isExternal() const { return extBits_ != nullptr; }
  void freeExternal();

  void updateScaleOptions();
};
