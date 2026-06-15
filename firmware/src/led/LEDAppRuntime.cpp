#include "LEDAppRuntime.h"

#include <ArduinoJson.h>
#include <cstring>

#include "../hardware/LEDmatrix.h"
#include "../infra/Filesystem.h"

extern "C" {
#include "matrix_app_api.h"
}

namespace {
constexpr const char* kStatePath = "/led_state.json";

// All 8x8 LED-matrix bitmaps below are MSB-first (bit 7 = leftmost
// pixel) so the binary literal reads as the visible dot pattern.
constexpr uint8_t kBlank[LEDAppRuntime::kFrameRows] = {
    0b00000000, 0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000, 0b00000000,
};
constexpr uint8_t kHeart[LEDAppRuntime::kFrameRows] = {
    0b01100110,
    0b11111111,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
    0b00000000,
};
constexpr uint8_t kGlider[LEDAppRuntime::kFrameRows] = {
    0b00000000,
    0b00000000,
    0b00100000,
    0b00010000,
    0b01110000,
    0b00000000,
    0b00000000,
    0b00000000,
};
// "REPLAY" wordmark unrolled as columns scrolling right-to-left across
// the matrix. Each byte is one column; bit 7 = top row.
constexpr uint8_t kReplayColumns[] = {
    0b00000000, 0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000, 0b00000000,
    0b11011111, 0b11011111, 0b11011000, 0b11011110,
    0b11111111, 0b11111111, 0b01110001, 0b11111111,
    0b11111111, 0b11011011, 0b11011011, 0b11011011,
    0b11000011, 0b11000011, 0b11011111, 0b11011111,
    0b11011000, 0b11011000, 0b11011000, 0b11111000,
    0b11111000, 0b11111111, 0b11111111, 0b00000111,
    0b00000011, 0b00000011, 0b00000011, 0b00000011,
    0b11111111, 0b11111111, 0b11111000, 0b11011000,
    0b11011000, 0b11111111, 0b11111111, 0b11111000,
    0b11111000, 0b11111111, 0b00011111, 0b00011111,
    0b11111000, 0b11111000,
    0b00000000, 0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000, 0b00000000,
};
constexpr uint8_t kWaveFrames[][LEDAppRuntime::kFrameRows] = {
    {0b10000000, 0b01000000, 0b00100000, 0b00010000,
     0b00001000, 0b00000100, 0b00000010, 0b00000001},
    {0b01000000, 0b00100000, 0b00010000, 0b00001000,
     0b00000100, 0b00000010, 0b00000001, 0b10000000},
    {0b00100000, 0b00010000, 0b00001000, 0b00000100,
     0b00000010, 0b00000001, 0b10000000, 0b01000000},
    {0b00010000, 0b00001000, 0b00000100, 0b00000010,
     0b00000001, 0b10000000, 0b01000000, 0b00100000},
    {0b00001000, 0b00000100, 0b00000010, 0b00000001,
     0b10000000, 0b01000000, 0b00100000, 0b00010000},
    {0b00000100, 0b00000010, 0b00000001, 0b10000000,
     0b01000000, 0b00100000, 0b00010000, 0b00001000},
    {0b00000010, 0b00000001, 0b10000000, 0b01000000,
     0b00100000, 0b00010000, 0b00001000, 0b00000100},
    {0b00000001, 0b10000000, 0b01000000, 0b00100000,
     0b00010000, 0b00001000, 0b00000100, 0b00000010},
};
constexpr uint32_t kTemporalLogo32[32] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00010000,
    0x00028000, 0x00028000, 0x000FE000, 0x00121000,
    0x000E6000, 0x00028000, 0x00028000, 0x00010000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

void frameFromColumns(const uint8_t* columns, size_t columnCount,
                      uint32_t start, uint8_t out[8]) {
  memset(out, 0, 8);
  for (uint8_t x = 0; x < 8; x++) {
    const uint8_t col = columns[(start + x) % columnCount];
    const uint8_t bit = 0x80 >> x;
    for (uint8_t y = 0; y < 8; y++) {
      if (col & (0x80 >> y)) out[y] |= bit;
    }
  }
}

void frameFrom32(const uint32_t* rows, uint8_t xoff, uint8_t yoff,
                 uint8_t out[8]) {
  if (xoff > 24) xoff = 24;
  if (yoff > 24) yoff = 24;
  const uint8_t shift = 24 - xoff;
  for (uint8_t row = 0; row < 8; row++) {
    out[row] = static_cast<uint8_t>((rows[yoff + row] >> shift) & 0xFFU);
  }
}

}  // namespace

LEDAppRuntime ledAppRuntime;

void LEDAppRuntime::begin(LEDmatrix* matrix) {
  matrix_ = matrix;
  loadState();
  restoreAmbient();
}

void LEDAppRuntime::loadState() {
  if (loaded_) return;
  loaded_ = true;

  char* buf = nullptr;
  size_t len = 0;
  if (!Filesystem::readFileAlloc(kStatePath, &buf, &len, 512)) {
    return;
  }

  StaticJsonDocument<384> doc;
  DeserializationError err = deserializeJson(doc, buf, len);
  free(buf);
  if (err) return;

  const char* mode = doc["mode"] | modeId(Mode::Temporal);
  for (uint8_t i = 0; i < modeCount(); i++) {
    Mode m = modeAt(i);
    if (strcmp(mode, modeId(m)) == 0) {
      state_.mode = m;
      break;
    }
  }

  int32_t d = doc["delay"] | static_cast<int32_t>(kDefaultDelay);
  if (d < 5) d = 5;
  if (d > 10000) d = 10000;
  state_.delay = static_cast<uint16_t>(d);

  int16_t b = doc["brightness"] | static_cast<int16_t>(kDefaultBrightness);
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  state_.brightness = static_cast<uint8_t>(b);

  state_.lifeRandomize = doc["life_randomize"] | false;

  const char* pyslug = doc["matrix_app"] | "";
  strncpy(state_.pythonAppSlug, pyslug, kPythonSlugCap - 1);
  state_.pythonAppSlug[kPythonSlugCap - 1] = '\0';

  JsonArray life = doc["life_seed"].as<JsonArray>();
  if (!life.isNull() && life.size() == kFrameRows) {
    for (uint8_t i = 0; i < kFrameRows; i++) state_.lifeSeed[i] = life[i] | 0;
  }
  JsonArray custom = doc["custom"].as<JsonArray>();
  if (!custom.isNull() && custom.size() == kFrameRows) {
    for (uint8_t i = 0; i < kFrameRows; i++) state_.custom[i] = custom[i] | 0;
  }
}

void LEDAppRuntime::saveState() {
  StaticJsonDocument<384> doc;
  doc["mode"] = modeId(state_.mode);
  doc["delay"] = state_.delay;
  doc["brightness"] = state_.brightness;
  doc["life_randomize"] = state_.lifeRandomize;
  doc["matrix_app"] = state_.pythonAppSlug;
  JsonArray life = doc.createNestedArray("life_seed");
  JsonArray custom = doc.createNestedArray("custom");
  for (uint8_t i = 0; i < kFrameRows; i++) {
    life.add(state_.lifeSeed[i]);
    custom.add(state_.custom[i]);
  }
  char buf[384];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len > 0 && len < sizeof(buf)) {
    Filesystem::writeFileAtomic(kStatePath, buf, len);
  }
}

void LEDAppRuntime::restoreAmbient() {
  loaded_ = false;
  loadState();
  previewActive_ = false;
  previewHasDraft_ = false;
  runningValid_ = false;
  lastTickMs_ = 0;

  // If the persisted state names a Python matrix app, queue its registration
  // script for execution. MicroPythonMatrixService drains this on its next
  // service tick (which runs in a context where mp_embed_exec_str is safe).
  if (state_.mode == Mode::PythonApp && state_.pythonAppSlug[0]) {
    pendingPyExec_ = true;
    strncpy(pendingPySlug_, state_.pythonAppSlug, kPythonSlugCap - 1);
    pendingPySlug_[kPythonSlugCap - 1] = '\0';
  }
}

void LEDAppRuntime::beginPreview(Mode mode, const uint8_t* draft) {
  previewActive_ = true;
  updatePreview(mode, draft);
}

void LEDAppRuntime::updatePreview(Mode mode, const uint8_t* draft) {
  previewMode_ = mode;
  previewHasDraft_ = draft != nullptr;
  if (draft) memcpy(previewDraft_, draft, kFrameRows);
  runningValid_ = false;
  lastTickMs_ = 0;
}

void LEDAppRuntime::endPreview() {
  previewActive_ = false;
  previewHasDraft_ = false;
  runningValid_ = false;
  lastTickMs_ = 0;
}

void LEDAppRuntime::commitMode(Mode mode) {
  state_.mode = mode;
  // Switching to any built-in mode tears down the persistent Python
  // matrix callback (e.g. tardigotchi's ziggy) and clears the saved slug
  // so the next reboot doesn't resurrect it.
  if (mode != Mode::PythonApp) {
    state_.pythonAppSlug[0] = '\0';
    matrix_app_stop_from_c();
  }
  saveState();
  restoreAmbient();
}

void LEDAppRuntime::commitMode(Mode mode, uint16_t delay, uint8_t brightness) {
  state_.mode = mode;
  state_.delay = delay < 5 ? 5 : (delay > 10000 ? 10000 : delay);
  state_.brightness = brightness;
  if (mode != Mode::PythonApp) {
    state_.pythonAppSlug[0] = '\0';
    matrix_app_stop_from_c();
  }
  saveState();
  restoreAmbient();
}

void LEDAppRuntime::commitLife(const uint8_t* seed) {
  copyFrame(state_.lifeSeed, seed, kGlider);
  commitMode(Mode::Life);
}

void LEDAppRuntime::commitCustom(const uint8_t* pattern) {
  copyFrame(state_.custom, pattern, kHeart);
  commitMode(Mode::Custom);
}

void LEDAppRuntime::commitMatrixApp(const char* slug) {
  if (!slug || !slug[0]) return;
  state_.mode = Mode::PythonApp;
  // Tear down whatever Python callback was previously registered so the
  // outgoing app stops drawing immediately. The new slug's matrix.py will
  // re-register on the next MicroPythonMatrixService service tick.
  matrix_app_stop_from_c();
  strncpy(state_.pythonAppSlug, slug, kPythonSlugCap - 1);
  state_.pythonAppSlug[kPythonSlugCap - 1] = '\0';
  saveState();
  // restoreAmbient queues pendingPyExec_ from the freshly-saved state.
  restoreAmbient();
}

bool LEDAppRuntime::consumePendingExec(char* out, size_t cap) {
  if (!pendingPyExec_ || cap == 0) return false;
  size_t n = strlen(pendingPySlug_);
  if (n >= cap) n = cap - 1;
  memcpy(out, pendingPySlug_, n);
  out[n] = '\0';
  pendingPyExec_ = false;
  pendingPySlug_[0] = '\0';
  return true;
}

void LEDAppRuntime::beginOverride() {
  if (overrideDepth_ < 255) overrideDepth_++;
  if (matrix_) {
    matrix_->stopAnimation();
  }
  runningValid_ = false;
}

void LEDAppRuntime::endOverride() {
  if (overrideDepth_ > 0) overrideDepth_--;
  if (overrideDepth_ == 0) {
    if (matrix_) matrix_->setMicropythonMode(false);
    runningValid_ = false;
    lastTickMs_ = 0;
  }
}

LEDAppRuntime::Mode LEDAppRuntime::activeMode() const {
  return previewActive_ ? previewMode_ : state_.mode;
}

const uint8_t* LEDAppRuntime::activeSeed() const {
  if (previewActive_ && previewHasDraft_) return previewDraft_;
  return activeMode() == Mode::Custom ? state_.custom : state_.lifeSeed;
}

uint16_t LEDAppRuntime::activeInterval() const {
  return state_.delay;
}

void LEDAppRuntime::service() {
  if (!matrix_ || !matrix_->isInitialized()) return;
  if (overrideDepth_ > 0 || matrix_->isMicropythonMode()) return;

  const Mode mode = activeMode();
  // When a Python ambient matrix app is selected, MicroPythonMatrixService
  // drives the frames; the native runtime stays out of the way.
  if (mode == Mode::PythonApp) return;
  const uint32_t now = millis();
  if (!runningValid_ || runningMode_ != mode) {
    runningMode_ = mode;
    runningValid_ = true;
    restartActiveMode();
    return;
  }

  if (mode == Mode::Temporal || mode == Mode::Off) return;
  const uint16_t interval = activeInterval();
  if (lastTickMs_ != 0 && now - lastTickMs_ < interval) return;
  lastTickMs_ = now;

  uint8_t frame[kFrameRows] = {};
  buildFrame(mode, frame);
  drawFrame(frame);
  frameIndex_++;
}

void LEDAppRuntime::restartActiveMode() {
  if (!matrix_) return;
  matrix_->setMicropythonMode(false);
  frameIndex_ = 0;
  lastTickMs_ = millis();

  const Mode mode = activeMode();
  if (mode == Mode::Off) {
    stopNativeAnimation();
    matrix_->clear(0);
    return;
  }

  if (mode == Mode::Temporal) {
    matrix_->animator().startTemporalLogoBitmap32(
        255, 35, 1, 0);
    matrix_->animator().setTrackedBrightness(state_.brightness, 0);
    matrix_->updateAnimation();
    return;
  }

  stopNativeAnimation();
  if (isLifeMode(mode)) {
    if (mode == Mode::LifeRandom) {
      for (uint8_t i = 0; i < kFrameRows; i++) life_[i] = randomByte();
    } else {
      copyFrame(life_, activeSeed(), kGlider);
    }
    memset(lifeNext_, 0, sizeof(lifeNext_));
    memcpy(lifePrev_, life_, kFrameRows);
    memcpy(lifePrev2_, life_, kFrameRows);
    staleCount_ = 0;
    lifeRestart_ = false;
  }
  for (uint8_t i = 0; i < kFrameRows; i++) {
    rain_[i] = randomByte() & 7;
  }

  uint8_t frame[kFrameRows] = {};
  buildFrame(mode, frame);
  drawFrame(frame);
  frameIndex_++;
}

void LEDAppRuntime::stopNativeAnimation() {
  if (matrix_) matrix_->stopAnimation();
}

void LEDAppRuntime::drawFrame(const uint8_t frame[kFrameRows]) {
  if (!matrix_) return;
  (void)matrix_->drawMask(frame, state_.brightness, 0);
}

void LEDAppRuntime::copyFrame(uint8_t dst[kFrameRows], const uint8_t* src,
                              const uint8_t fallback[kFrameRows]) {
  const uint8_t* from = src ? src : fallback;
  for (uint8_t i = 0; i < kFrameRows; i++) dst[i] = from[i];
}

void LEDAppRuntime::buildFrame(Mode mode, uint8_t out[kFrameRows]) {
  switch (mode) {
    case Mode::Replay:
      frameFromColumns(kReplayColumns, sizeof(kReplayColumns), frameIndex_, out);
      return;
    case Mode::Sparkle:
      for (uint8_t i = 0; i < kFrameRows; i++) out[i] = randomByte() & randomByte();
      return;
    case Mode::Rain:
      memset(out, 0, kFrameRows);
      for (uint8_t x = 0; x < 8; x++) {
        const uint8_t y = rain_[x] & 7;
        const uint8_t bit = 0x80 >> x;
        out[y] |= bit;
        out[(y - 1) & 7] |= bit;
        if ((randomByte() & 3) == 0) rain_[x] = (y + 1) & 7;
      }
      return;
    case Mode::Wave:
      memcpy(out, kWaveFrames[frameIndex_ & 7], kFrameRows);
      return;
    case Mode::Life:
    case Mode::LifeRandom:
      buildLifeFrame(out);
      return;
    case Mode::Custom:
      copyFrame(out, activeSeed(), kHeart);
      return;
    case Mode::Temporal:
      frameFrom32(kTemporalLogo32, 12, 12, out);
      return;
    case Mode::PythonApp:
    case Mode::Off:
    default:
      memcpy(out, kBlank, kFrameRows);
      return;
  }
}

void LEDAppRuntime::buildLifeFrame(uint8_t out[kFrameRows]) {
  if (frameIndex_ == 0 || lifeRestart_) {
    lifeRestart_ = false;
    memcpy(lifePrev_, life_, kFrameRows);
    memcpy(lifePrev2_, life_, kFrameRows);
    staleCount_ = 0;
    copyFrame(out, life_, kGlider);
    return;
  }

  memcpy(lifePrev2_, lifePrev_, kFrameRows);
  memcpy(lifePrev_, life_, kFrameRows);

  for (uint8_t y = 0; y < 8; y++) {
    uint8_t row = 0;
    for (uint8_t x = 0; x < 8; x++) {
      uint8_t neighbors = 0;
      for (int8_t dy = -1; dy <= 1; dy++) {
        for (int8_t dx = -1; dx <= 1; dx++) {
          if (dx == 0 && dy == 0) continue;
          const uint8_t yy = (y + dy) & 7;
          const uint8_t xx = (x + dx) & 7;
          if (life_[yy] & (0x80 >> xx)) neighbors++;
        }
      }
      const bool alive = (life_[y] & (0x80 >> x)) != 0;
      if ((alive && (neighbors == 2 || neighbors == 3)) ||
          (!alive && neighbors == 3)) {
        row |= 0x80 >> x;
      }
    }
    lifeNext_[y] = row;
  }
  copyFrame(life_, lifeNext_, kBlank);

  bool empty = true, still = true, period2 = true;
  for (uint8_t i = 0; i < kFrameRows; i++) {
    if (life_[i] != 0) empty = false;
    if (life_[i] != lifePrev_[i]) still = false;
    if (life_[i] != lifePrev2_[i]) period2 = false;
  }
  if (empty || still || period2) {
    if (++staleCount_ >= kStaleGrace) {
      for (uint8_t i = 0; i < kFrameRows; i++) life_[i] = randomByte();
      lifeRestart_ = true;
    }
  } else {
    staleCount_ = 0;
  }

  copyFrame(out, life_, kBlank);
}

uint8_t LEDAppRuntime::randomByte() {
  rng_ = (rng_ * 1103515245UL) + 12345UL;
  return static_cast<uint8_t>((rng_ >> 16) & 0xFF);
}

const char* LEDAppRuntime::modeId(Mode mode) {
  switch (mode) {
    case Mode::Temporal: return "temporal";
    case Mode::Replay: return "replay";
    case Mode::Sparkle: return "sparkle";
    case Mode::Rain: return "rain";
    case Mode::Wave: return "wave";
    case Mode::Life: return "life";
    case Mode::LifeRandom: return "life_random";
    case Mode::Custom: return "custom";
    case Mode::Off: return "off";
    case Mode::PythonApp: return "python_app";
    default: return "temporal";
  }
}

const char* LEDAppRuntime::modeName(Mode mode) {
  switch (mode) {
    case Mode::Temporal: return "Temporal";
    case Mode::Replay: return "Replay";
    case Mode::Sparkle: return "Sparkle";
    case Mode::Rain: return "Rain";
    case Mode::Wave: return "Wave";
    case Mode::Life: return "Game of Life";
    case Mode::LifeRandom: return "Random Life";
    case Mode::Custom: return "Custom";
    case Mode::Off: return "Off";
    case Mode::PythonApp: return "App";
    default: return "LED";
  }
}

LEDAppRuntime::Mode LEDAppRuntime::modeAt(uint8_t index) {
  if (index >= modeCount()) return Mode::Temporal;
  return static_cast<Mode>(index);
}

uint8_t LEDAppRuntime::modeIndex(Mode mode) {
  return static_cast<uint8_t>(mode);
}

uint8_t LEDAppRuntime::modeCount() {
  // Excludes PythonApp; that mode is selectable only by picking a discovered
  // matrix-app entry and is never reached by the built-in carousel cycle.
  return static_cast<uint8_t>(Mode::Off) + 1;
}

void LEDAppRuntime::posterFrame(Mode mode, const uint8_t* lifeSeed,
                                const uint8_t* custom, uint8_t out[kFrameRows]) {
  switch (mode) {
    case Mode::Temporal:
      frameFrom32(kTemporalLogo32, 12, 12, out);
      return;
    case Mode::Replay:
      frameFromColumns(kReplayColumns, sizeof(kReplayColumns), 8, out);
      return;
    case Mode::Sparkle: {
      const uint8_t f[kFrameRows] = {
          0b10000001, 0b00100100, 0b00000000, 0b01011010,
          0b00011000, 0b00000000, 0b01000010, 0b00011000,
      };
      memcpy(out, f, kFrameRows);
      return;
    }
    case Mode::Rain: {
      const uint8_t f[kFrameRows] = {
          0b10000000, 0b00000000, 0b00100100, 0b00000000,
          0b00001000, 0b01000000, 0b00000010, 0b00000000,
      };
      memcpy(out, f, kFrameRows);
      return;
    }
    case Mode::Wave:
      memcpy(out, kWaveFrames[0], kFrameRows);
      return;
    case Mode::Life:
      memcpy(out, lifeSeed ? lifeSeed : kGlider, kFrameRows);
      return;
    case Mode::LifeRandom:
      for (uint8_t i = 0; i < kFrameRows; i++)
        out[i] = static_cast<uint8_t>(random(256));
      return;
    case Mode::Custom:
      memcpy(out, custom ? custom : kHeart, kFrameRows);
      return;
    case Mode::PythonApp:
    case Mode::Off:
    default:
      memcpy(out, kBlank, kFrameRows);
      return;
  }
}

extern "C" void led_app_runtime_restore_ambient(void) {
  ledAppRuntime.restoreAmbient();
}

extern "C" void led_app_runtime_begin_override(void) {
  ledAppRuntime.beginOverride();
}

extern "C" void led_app_runtime_end_override(void) {
  ledAppRuntime.endOverride();
}
