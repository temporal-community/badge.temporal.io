#include "LEDScreen.h"

#include <cstdio>
#include <cstring>

#include "../apps/AppRegistry.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"

extern "C" {
#include "matrix_app_api.h"
}

namespace {
constexpr uint8_t kCell = 5;
constexpr uint8_t kGridSize = 8 * kCell;
constexpr uint8_t kCarouselGridX = 44;
constexpr uint8_t kCarouselGridY = 13;
constexpr uint8_t kEditorGridX = kCarouselGridX;
constexpr uint8_t kEditorGridY = 13;
constexpr uint16_t kJoyDeadband = 500;

// 8x8 LED-matrix bitmaps, MSB-first (bit 7 = leftmost pixel) so the dot
// pattern reads naturally left-to-right as written.
constexpr uint8_t kHeart[8] = {
    0b01100110,
    0b11111111,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
    0b00000000,
};
constexpr uint8_t kSmiley[8] = {
    0b00111100,
    0b01000010,
    0b10100101,
    0b10000001,
    0b10100101,
    0b10011001,
    0b01000010,
    0b00111100,
};
constexpr uint8_t kGlider[8] = {
    0b00000000,
    0b00000000,
    0b00100000,
    0b00010000,
    0b01110000,
    0b00000000,
    0b00000000,
    0b00000000,
};
constexpr uint8_t kBlinker[8] = {
    0b00000000,
    0b00000000,
    0b00000000,
    0b01110000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
};
constexpr uint8_t kPulsar[8] = {
    0b00000000,
    0b00111000,
    0b00100100,
    0b00111000,
    0b00111000,
    0b00100100,
    0b00111000,
    0b00000000,
};
constexpr uint8_t kRPent[8] = {
    0b00000000,
    0b00011000,
    0b00110000,
    0b00010000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
};

void drawBox(oled& d, int x, int y, int w, int h, uint8_t color = 1) {
  d.setDrawColor(color);
  d.drawBox(x, y, w, h);
  d.setDrawColor(1);
}

void drawOutline(oled& d, int x, int y, int w, int h, uint8_t color = 1) {
  drawBox(d, x, y, w, 1, color);
  drawBox(d, x, y + h - 1, w, 1, color);
  drawBox(d, x, y, 1, h, color);
  drawBox(d, x + w - 1, y, 1, h, color);
}

int16_t delayStep(uint16_t val) {
  if (val >= 1000) return 250;
  if (val >= 200) return 50;
  if (val >= 50) return 10;
  return 5;
}

int16_t clampDelay(int32_t v) {
  if (v < 5) return 5;
  if (v > 10000) return 10000;
  return static_cast<int16_t>(v);
}

int16_t clampBrt(int16_t v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return v;
}

uint8_t matrixAppCountHere() {
  size_t total = AppRegistry::count();
  uint8_t n = 0;
  for (size_t i = 0; i < total; i++) {
    const AppRegistry::DynamicApp* a = AppRegistry::at(i);
    if (a && a->hasMatrixApp) n++;
  }
  return n;
}

const AppRegistry::DynamicApp* matrixAppByOffset(uint8_t offset) {
  size_t total = AppRegistry::count();
  uint8_t cursor = 0;
  for (size_t i = 0; i < total; i++) {
    const AppRegistry::DynamicApp* a = AppRegistry::at(i);
    if (!a || !a->hasMatrixApp) continue;
    if (cursor == offset) return a;
    cursor++;
  }
  return nullptr;
}

void drawSideAction(oled& d, int laneX, int laneW, ButtonGlyphs::Button button,
                    const char* label) {
  const int glyphX = laneX + (laneW - ButtonGlyphs::kGlyphW) / 2;
  ButtonGlyphs::draw(d, button, glyphX, 21);
  const int labelW = d.getStrWidth(label);
  d.drawStr(laneX + (laneW - labelW) / 2, 42, label);
}

}  // namespace

LEDScreen::LEDScreen() = default;

void LEDScreen::onEnter(GUIManager& gui) {
  (void)gui;
  committed_ = false;
  // Refresh matrix-app discovery so newly-added /apps/<slug>/matrix.py files
  // appear without a reboot. AppRegistry::rescan() also rebuilds the
  // foreground main-menu cache via the GUI rebuild path; here we just need
  // the list to be current for the carousel.
  AppRegistry::rescan();
  // Map the persisted runtime mode to a carousel index. PythonApp resolves
  // to the discovered matrix-app entry whose slug matches state.
  if (ledAppRuntime.mode() == LEDAppRuntime::Mode::PythonApp) {
    modeIndex_ = LEDAppRuntime::modeCount();
    const char* persisted = ledAppRuntime.pythonAppSlug();
    uint8_t total = matrixAppCountHere();
    for (uint8_t off = 0; off < total; off++) {
      const AppRegistry::DynamicApp* a = matrixAppByOffset(off);
      if (a && persisted && std::strcmp(a->slug, persisted) == 0) {
        modeIndex_ = LEDAppRuntime::modeCount() + off;
        break;
      }
    }
    selectedMode_ = LEDAppRuntime::Mode::PythonApp;
  } else {
    modeIndex_ = LEDAppRuntime::modeIndex(ledAppRuntime.mode());
    selectedMode_ = LEDAppRuntime::modeAt(modeIndex_);
  }
  delay_ = ledAppRuntime.delay();
  brightness_ = ledAppRuntime.brightness();
  adjDelay_ = true;
  joyRamp_.reset();
  // Pause any active Python matrix callback (e.g. tardigotchi) while
  // we browse — otherwise its tick keeps drawing on top of the
  // built-in carousel previews. onExit re-arms it if nothing was
  // committed.
  matrix_app_stop_from_c();
  enterCarousel();
}

uint8_t LEDScreen::carouselCount() const {
  return static_cast<uint8_t>(LEDAppRuntime::modeCount() + matrixAppCountHere());
}

bool LEDScreen::isPythonAppIndex(uint8_t index) const {
  return index >= LEDAppRuntime::modeCount();
}

uint8_t LEDScreen::matrixAppOffset(uint8_t index) const {
  return static_cast<uint8_t>(index - LEDAppRuntime::modeCount());
}

void LEDScreen::onExit(GUIManager& gui) {
  (void)gui;
  if (!committed_) {
    ledAppRuntime.endPreview();
    // We stopped the Python matrix cb on entry; re-source the persisted
    // slug so backing out without committing leaves the previous Python
    // ambient running again.
    if (ledAppRuntime.mode() == LEDAppRuntime::Mode::PythonApp) {
      ledAppRuntime.restoreAmbient();
    }
  }
}

void LEDScreen::enterCarousel() {
  view_ = View::Carousel;
  if (isPythonAppIndex(modeIndex_)) {
    selectedMode_ = LEDAppRuntime::Mode::PythonApp;
    // No native preview for Python matrix apps — keep whatever is on the
    // matrix until commit. The OLED still shows the picker tile.
    ledAppRuntime.endPreview();
  } else {
    selectedMode_ = LEDAppRuntime::modeAt(modeIndex_);
    ledAppRuntime.beginPreview(selectedMode_);
  }
}

void LEDScreen::enterEditor(LEDAppRuntime::Mode mode) {
  view_ = mode == LEDAppRuntime::Mode::Life ? View::LifeEditor : View::CustomEditor;
  editingMode_ = mode;
  cursorX_ = 0;
  cursorY_ = 0;
  const uint8_t* src = mode == LEDAppRuntime::Mode::Life
      ? ledAppRuntime.lifeSeed()
      : ledAppRuntime.customPattern();
  memcpy(draft_, src, sizeof(draft_));
  ledAppRuntime.updatePreview(mode, draft_);
}

void LEDScreen::enterPresets() {
  view_ = View::Presets;
  presetIndex_ = 0;
}

void LEDScreen::cancelEditor() {
  view_ = View::Carousel;
  ledAppRuntime.updatePreview(selectedMode_);
}

void LEDScreen::saveEditor(GUIManager& gui) {
  committed_ = true;
  if (editingMode_ == LEDAppRuntime::Mode::Life) {
    ledAppRuntime.commitLife(draft_);
  } else {
    ledAppRuntime.commitCustom(draft_);
  }
  Haptics::shortPulse();
  gui.popScreen();
}

void LEDScreen::moveMode(int8_t dir) {
  const uint8_t count = carouselCount();
  modeIndex_ = static_cast<uint8_t>((modeIndex_ + count + dir) % count);
  if (isPythonAppIndex(modeIndex_)) {
    selectedMode_ = LEDAppRuntime::Mode::PythonApp;
    ledAppRuntime.endPreview();
  } else {
    selectedMode_ = LEDAppRuntime::modeAt(modeIndex_);
    ledAppRuntime.updatePreview(selectedMode_);
  }
  Haptics::shortPulse();
}

void LEDScreen::moveCursor(int8_t dx, int8_t dy) {
  cursorX_ = static_cast<uint8_t>((cursorX_ + dx) & 7);
  cursorY_ = static_cast<uint8_t>((cursorY_ + dy) & 7);
}

void LEDScreen::render(oled& d, GUIManager& gui) {
  (void)gui;
  d.setTextWrap(false);
  d.setFontPreset(FONT_TINY);
  d.setDrawColor(1);
  switch (view_) {
    case View::Carousel:
      drawCarousel(d);
      break;
    case View::LifeEditor:
    case View::CustomEditor:
      drawEditor(d);
      break;
    case View::Presets:
      drawPresets(d);
      break;
  }
}

void LEDScreen::drawGrid(oled& d, const uint8_t frame[8], int8_t cx, int8_t cy,
                         int gridX, int gridY, bool showOffDots) {
  d.setDrawColor(1);
  d.drawRFrame(gridX - 2, gridY - 2, kGridSize + 4, kGridSize + 3, 2);
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      const int px = gridX + x * kCell;
      const int py = gridY + y * kCell;
      if (frame[y] & (0x80 >> x)) {
        d.drawRBox(px, py, kCell - 1, kCell - 1, 1);
      } else if (showOffDots) {
        d.drawPixel(px + 2, py + 2);
      }
    }
  }
  if (cx >= 0 && cy >= 0) {
    drawOutline(d, gridX + cx * kCell - 1, gridY + cy * kCell - 1,
                kCell + 1, kCell + 1, 2);
  }
}

void LEDScreen::drawCarousel(oled& d) {
  const uint8_t total = carouselCount();
  const char* label = LEDAppRuntime::modeName(selectedMode_);
  const AppRegistry::DynamicApp* pyApp = nullptr;
  if (isPythonAppIndex(modeIndex_)) {
    pyApp = matrixAppByOffset(matrixAppOffset(modeIndex_));
    if (pyApp) {
      label = pyApp->matrixTitle[0] ? pyApp->matrixTitle : pyApp->title;
    }
  }
  char title[32];
  std::snprintf(title, sizeof(title), "%s %u/%u", label,
                static_cast<unsigned>(modeIndex_ + 1),
                static_cast<unsigned>(total));
  OLEDLayout::drawStatusHeader(d, title);

  uint8_t poster[8];
  if (isPythonAppIndex(modeIndex_)) {
    // Show a glider-shaped placeholder for Python apps so the carousel tile
    // isn't blank. The actual frame at runtime is whatever the app draws.
    static constexpr uint8_t kPyPoster[8] = {
        0x00, 0x20, 0x10, 0x70, 0x00, 0x18, 0x18, 0x00,
    };
    std::memcpy(poster, kPyPoster, 8);
  } else {
    LEDAppRuntime::posterFrame(selectedMode_, ledAppRuntime.lifeSeed(),
                               ledAppRuntime.customPattern(), poster);
  }
  drawGrid(d, poster, -1, -1, kCarouselGridX, kCarouselGridY, false);

  char line[16];
  std::snprintf(line, sizeof(line), "%c%ums", adjDelay_ ? '>' : ' ',
                static_cast<unsigned>(delay_));
  d.drawStr(0, 26, line);
  std::snprintf(line, sizeof(line), "%cbrt %u", adjDelay_ ? ' ' : '>',
                static_cast<unsigned>(brightness_));
  d.drawStr(0, 36, line);

  OLEDLayout::drawGameFooter(d);
  if (isPythonAppIndex(modeIndex_)) {
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "set");
  } else if (selectedMode_ == LEDAppRuntime::Mode::Life ||
             selectedMode_ == LEDAppRuntime::Mode::Custom) {
    OLEDLayout::drawFooterActions(d, "spd/brt", "lab", "back", "save");
  } else {
    OLEDLayout::drawFooterActions(d, "spd/brt", nullptr, "back", "save");
  }
}

void LEDScreen::drawEditor(oled& d) {
  OLEDLayout::drawStatusHeader(
      d, editingMode_ == LEDAppRuntime::Mode::Life ? "Life Lab" : "Custom Lab");
  drawSideAction(d, 0, 38, ButtonGlyphs::Button::X, "toggle");
  drawSideAction(d, 90, 38, ButtonGlyphs::Button::Y, "preset");
  drawGrid(d, draft_, cursorX_, cursorY_, kEditorGridX, kEditorGridY, true);
  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "save");
}

void LEDScreen::drawPresets(oled& d) {
  OLEDLayout::drawStatusHeader(d, editingMode_ == LEDAppRuntime::Mode::Life
                                      ? "Life Preset"
                                      : "Custom Preset");
  const uint8_t count = presetCount();
  uint8_t first = presetIndex_ > 2 ? presetIndex_ - 2 : 0;
  if (first + 5 > count && count > 5) first = count - 5;
  for (uint8_t row = 0; row < 5 && first + row < count; row++) {
    const uint8_t i = first + row;
    char line[24];
    std::snprintf(line, sizeof(line), "%c%s", i == presetIndex_ ? '>' : ' ',
                  presetName(i));
    d.drawStr(0, 18 + row * 8, line);
  }
  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "load");
}

void LEDScreen::handleInput(const Inputs& inputs, int16_t, int16_t,
                            GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  const int16_t dx = static_cast<int16_t>(inputs.joyX()) - 2047;
  const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
  int8_t dir = 0;
  if (abs(dx) > abs(dy)) {
    if (dx > kJoyDeadband) dir = 1;
    else if (dx < -kJoyDeadband) dir = -1;
  } else {
    if (dy > kJoyDeadband) dir = 2;
    else if (dy < -kJoyDeadband) dir = -2;
  }

  if (view_ == View::Carousel) {
    if (dir == 1 || dir == -1) {
      if (joyRamp_.tick(dir, millis())) {
        moveMode(dir);
      }
    } else if (dir == 2 || dir == -2) {
      if (joyRamp_.tick(dir, millis())) {
        const int8_t sign = dir > 0 ? -1 : 1;
        if (adjDelay_) {
          delay_ = static_cast<uint16_t>(
              clampDelay(static_cast<int32_t>(delay_) + sign * delayStep(delay_)));
          ledAppRuntime.setDelay(delay_);
        } else {
          brightness_ = static_cast<uint8_t>(
              clampBrt(static_cast<int16_t>(brightness_) + sign));
          ledAppRuntime.setBrightness(brightness_);
        }
      }
    } else {
      joyRamp_.tick(0, millis());
    }
    if (e.xPressed) {
      adjDelay_ = !adjDelay_;
    }
    if (e.cancelPressed) {
      gui.popScreen();
      return;
    }
    if (e.yPressed && !isPythonAppIndex(modeIndex_) &&
        (selectedMode_ == LEDAppRuntime::Mode::Life ||
         selectedMode_ == LEDAppRuntime::Mode::Custom)) {
      enterEditor(selectedMode_);
      return;
    }
    if (e.confirmPressed) {
      committed_ = true;
      if (isPythonAppIndex(modeIndex_)) {
        const AppRegistry::DynamicApp* a =
            matrixAppByOffset(matrixAppOffset(modeIndex_));
        if (a) {
          ledAppRuntime.commitMatrixApp(a->slug);
        }
      } else {
        ledAppRuntime.commitMode(selectedMode_, delay_, brightness_);
      }
      Haptics::shortPulse();
      gui.popScreen();
      return;
    }
    return;
  }

  if (view_ == View::Presets) {
    if (joyRamp_.tick((dir == 2 || dir == -2) ? dir : 0, millis())) {
      const uint8_t count = presetCount();
      if (dir == 2) presetIndex_ = (presetIndex_ + 1) % count;
      if (dir == -2) presetIndex_ = (presetIndex_ + count - 1) % count;
    }
    if (e.cancelPressed) {
      view_ = editingMode_ == LEDAppRuntime::Mode::Life
          ? View::LifeEditor
          : View::CustomEditor;
    }
    if (e.confirmPressed) {
      loadPreset(presetIndex_);
      view_ = editingMode_ == LEDAppRuntime::Mode::Life
          ? View::LifeEditor
          : View::CustomEditor;
      ledAppRuntime.updatePreview(editingMode_, draft_);
    }
    return;
  }

  if (joyRamp_.tick(dir, millis())) {
    if (dir == 1) moveCursor(1, 0);
    if (dir == -1) moveCursor(-1, 0);
    if (dir == 2) moveCursor(0, 1);
    if (dir == -2) moveCursor(0, -1);
  }
  if (e.xPressed) {
    draft_[cursorY_] ^= (0x80 >> cursorX_);
    ledAppRuntime.updatePreview(editingMode_, draft_);
  }
  if (e.yPressed) {
    enterPresets();
  }
  if (e.confirmPressed) {
    saveEditor(gui);
  }
  if (e.cancelPressed) {
    cancelEditor();
  }
}

const char* LEDScreen::presetName(uint8_t index) const {
  if (editingMode_ == LEDAppRuntime::Mode::Life) {
    static constexpr const char* kLife[] = {
        "Glider", "Blinker", "Pulsar", "R-Pent", "Randomize", "Clear",
    };
    return index < sizeof(kLife) / sizeof(kLife[0]) ? kLife[index] : "";
  }
  static constexpr const char* kCustom[] = {
      "Heart", "Smiley", "Randomize", "Clear",
  };
  return index < sizeof(kCustom) / sizeof(kCustom[0]) ? kCustom[index] : "";
}

uint8_t LEDScreen::presetCount() const {
  return editingMode_ == LEDAppRuntime::Mode::Life ? 6 : 4;
}

void LEDScreen::presetFrame(uint8_t index, uint8_t out[8]) {
  memset(out, 0, 8);
  if (editingMode_ == LEDAppRuntime::Mode::Life) {
    const uint8_t* src = nullptr;
    if (index == 0) src = kGlider;
    else if (index == 1) src = kBlinker;
    else if (index == 2) src = kPulsar;
    else if (index == 3) src = kRPent;
    if (src) {
      memcpy(out, src, 8);
      return;
    }
  } else {
    const uint8_t* src = nullptr;
    if (index == 0) src = kHeart;
    else if (index == 1) src = kSmiley;
    if (src) {
      memcpy(out, src, 8);
      return;
    }
  }

  const bool randomize = editingMode_ == LEDAppRuntime::Mode::Life
      ? index == 4
      : index == 2;
  if (randomize) {
    for (uint8_t i = 0; i < 8; i++) out[i] = randomByte();
  }
}

void LEDScreen::loadPreset(uint8_t index) {
  presetFrame(index, draft_);
}

uint8_t LEDScreen::randomByte() {
  rng_ = (rng_ * 1103515245UL) + 12345UL;
  return static_cast<uint8_t>((rng_ >> 16) & 0xFF);
}
