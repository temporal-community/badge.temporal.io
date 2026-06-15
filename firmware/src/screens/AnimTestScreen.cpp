#include "AnimTestScreen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <Arduino.h>
#include <ArduinoJson.h>

#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/Images.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"

void AnimTestScreen::onEnter(GUIManager& /*gui*/) {
  frameIdx_ = 0;
  scaleIdx_ = 0;
  frameDelayMs_ = kDefaultDelay;
  lastFrameMs_ = millis();
  lastJoyNavMs_ = 0;
  if (isExternal()) {
    // External mode renders 1:1 — no scale ladder.
    scaleCount_ = 0;
    scaleIdx_ = 0;
  } else {
    updateScaleOptions();
  }
}

void AnimTestScreen::onExit(GUIManager& /*gui*/) {
  // Always release external buffers when leaving so PSRAM isn't pinned
  // while the screen sits offstack. Catalog state stays untouched.
  freeExternal();
}

void AnimTestScreen::updateScaleOptions() {
  const ImageInfo& img = kImageCatalog[imageIdx_];
  scaleCount_ = ImageScaler::availableScales(img, scaleDims_, 8);
  if (scaleIdx_ >= scaleCount_) scaleIdx_ = 0;
  frameIdx_ = 0;
}

// ─── External-mode helpers ──────────────────────────────────────────────────

void AnimTestScreen::freeExternal() {
  if (extBits_) free(extBits_);
  extBits_ = nullptr;
  extW_ = extH_ = 0;
  extFrameCount_ = 1;
  extFrameBytes_ = 0;
  extName_[0] = '\0';
  extLoadError_ = false;
}

namespace {

// Read sibling info.json and resolve (w, h) for a `.fb` file. Mirrors
// the prior ImageViewScreen helper. Returns true iff dims were found.
bool fbDimsFromInfoJson(const char* path, uint16_t* outW, uint16_t* outH) {
  if (!path || !path[0]) return false;
  const char* slash = std::strrchr(path, '/');
  if (!slash) return false;
  size_t dirLen = static_cast<size_t>(slash - path);
  char infoPath[160];
  if (dirLen + std::strlen("/info.json") + 1 > sizeof(infoPath)) return false;
  std::memcpy(infoPath, path, dirLen);
  std::strcpy(infoPath + dirLen, "/info.json");
  char* buf = nullptr;
  size_t len = 0;
  if (!Filesystem::readFileAlloc(infoPath, &buf, &len, 32 * 1024)) return false;
  StaticJsonDocument<8192> doc;
  DeserializationError err = deserializeJson(doc, buf, len);
  free(buf);
  if (err) return false;
  uint16_t w = doc["w"].as<uint16_t>();
  uint16_t h = doc["h"].as<uint16_t>();
  const char* fname = slash + 1;
  if (fname[0] == 'o') {
    char objId[16] = {};
    size_t fnLen = std::strlen(fname);
    if (fnLen > 4 && fname[fnLen - 3] == '.' && fname[fnLen - 2] == 'f' &&
        fname[fnLen - 1] == 'b') {
      size_t copy = fnLen - 4;
      if (copy >= sizeof(objId)) copy = sizeof(objId) - 1;
      std::memcpy(objId, fname + 1, copy);
      objId[copy] = '\0';
      JsonArray objs = doc["objects"].as<JsonArray>();
      for (JsonObject jo : objs) {
        const char* id = jo["id"] | "";
        if (std::strcmp(id, objId) == 0) {
          uint16_t ow = jo["w"].as<uint16_t>();
          uint16_t oh = jo["h"].as<uint16_t>();
          if (ow > 0 && oh > 0) {
            *outW = ow;
            *outH = oh;
            return true;
          }
          break;
        }
      }
    }
  }
  if (w > 0 && h > 0) {
    *outW = w;
    *outH = h;
    return true;
  }
  return false;
}

void inferFbDims(size_t bytes, uint16_t* outW, uint16_t* outH) {
  switch (bytes) {
    case 1024: *outW = 128; *outH = 64; return;
    case  512: *outW =  64; *outH = 64; return;
    case  256: *outW =  64; *outH = 32; return;
    case   64: *outW =  32; *outH = 16; return;
    case   32: *outW =  16; *outH = 16; return;
    default: break;
  }
  size_t bits = bytes * 8;
  uint16_t side = static_cast<uint16_t>(std::sqrt((float)bits));
  if (side < 8) side = 8;
  side = (side / 8) * 8;
  *outW = side;
  *outH = side;
}

void copyTailName(const char* path, char* out, size_t cap) {
  if (!out || cap == 0) return;
  const char* name = path ? path : "";
  const char* slash = std::strrchr(name, '/');
  if (slash && slash[1]) name = slash + 1;
  std::strncpy(out, name, cap - 1);
  out[cap - 1] = '\0';
}

// Credit `.bin` body bytes are MSB-first (bit 7 = left); `oled::drawXBM`
// expects LSB-first — same fix as `scripts/gen_credit_xbms.py`.
static inline uint8_t creditBinRev8(uint8_t b) {
  b = static_cast<uint8_t>(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
  b = static_cast<uint8_t>(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
  b = static_cast<uint8_t>(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
  return b;
}

}  // namespace

void AnimTestScreen::loadFB(const char* path) {
  freeExternal();
  if (!path || !path[0]) {
    extLoadError_ = true;
    return;
  }
  copyTailName(path, extName_, sizeof(extName_));
  char* buf = nullptr;
  size_t len = 0;
  if (!Filesystem::readFileAlloc(path, &buf, &len, 32 * 1024) || len == 0) {
    extLoadError_ = true;
    return;
  }
  uint16_t w = 0, h = 0;
  if (!fbDimsFromInfoJson(path, &w, &h)) inferFbDims(len, &w, &h);
  // `.fb` files use SSD1306 page format (the same layout as
  // `oled_get_framebuffer()`): width * ceil(height/8) bytes per
  // frame, with each byte packing 8 *vertical* pixels (LSB = top).
  uint32_t frameBytes =
      static_cast<uint32_t>(w) * ((h + 7) / 8);
  if (frameBytes == 0) {
    free(buf);
    extLoadError_ = true;
    return;
  }
  uint8_t frames = static_cast<uint8_t>(len / frameBytes);
  if (frames < 1) frames = 1;
  if (frames > 32) frames = 32;
  uint32_t need = frameBytes * frames;
  if (need > len) need = len;
  extBits_ = static_cast<uint8_t*>(BadgeMemory::allocPreferPsram(need));
  if (!extBits_) {
    free(buf);
    extLoadError_ = true;
    return;
  }
  std::memcpy(extBits_, buf, need);
  free(buf);
  extW_ = w;
  extH_ = h;
  extFrameBytes_ = frameBytes;
  extFrameCount_ = frames;
  extFormat_ = ExtFormat::kPage;
  Serial.printf("[AnimTest] loadFB %s: %ux%u %u frame(s) @ %u B (page)\n",
                path, (unsigned)w, (unsigned)h,
                (unsigned)frames, (unsigned)frameBytes);
}

void AnimTestScreen::loadXBM(const char* path) {
  freeExternal();
  if (!path || !path[0]) {
    extLoadError_ = true;
    return;
  }
  copyTailName(path, extName_, sizeof(extName_));
  char* src = nullptr;
  size_t len = 0;
  if (!Filesystem::readFileAlloc(path, &src, &len, 64 * 1024)) {
    extLoadError_ = true;
    return;
  }
  // Width / height come from `..._width` / `..._height` macros.
  const char* w = std::strstr(src, "_width ");
  const char* h = std::strstr(src, "_height ");
  if (!w || !h) {
    free(src);
    extLoadError_ = true;
    return;
  }
  w += std::strlen("_width");
  h += std::strlen("_height");
  while (*w == ' ' || *w == '\t') w++;
  while (*h == ' ' || *h == '\t') h++;
  uint16_t imgW = (uint16_t)strtol(w, nullptr, 10);
  uint16_t imgH = (uint16_t)strtol(h, nullptr, 10);
  if (imgW == 0 || imgH == 0 || imgW > 1024 || imgH > 1024) {
    free(src);
    extLoadError_ = true;
    return;
  }
  uint32_t frameBytes = static_cast<uint32_t>((imgW + 7) / 8) * imgH;
  uint8_t* bits = static_cast<uint8_t*>(BadgeMemory::allocPreferPsram(frameBytes));
  if (!bits) {
    free(src);
    extLoadError_ = true;
    return;
  }
  // Find `_bits` array and parse hex literals.
  const char* p = std::strstr(src, "_bits");
  if (p) p = std::strchr(p, '{');
  if (p) p++;
  uint32_t idx = 0;
  while (p && *p && idx < frameBytes) {
    while (*p && *p != '0' && *p != '}') p++;
    if (!*p || *p == '}') break;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
      char* endp = nullptr;
      unsigned long v = strtoul(p, &endp, 16);
      bits[idx++] = static_cast<uint8_t>(v & 0xFF);
      p = endp ? endp : p + 1;
    } else {
      p++;
    }
  }
  while (idx < frameBytes) bits[idx++] = 0;
  free(src);
  extBits_ = bits;
  extW_ = imgW;
  extH_ = imgH;
  extFrameBytes_ = frameBytes;
  extFrameCount_ = 1;
  extFormat_ = ExtFormat::kXBM;
  Serial.printf("[AnimTest] loadXBM %s: %ux%u (xbm)\n", path,
                (unsigned)imgW, (unsigned)imgH);
}

bool AnimTestScreen::loadCreditBin(const char* path) {
  freeExternal();
  if (!path || !path[0]) return false;
  copyTailName(path, extName_, sizeof(extName_));
  char* buf = nullptr;
  size_t len = 0;
  if (!Filesystem::readFileAlloc(path, &buf, &len, 64 * 1024) || len < 6) {
    extLoadError_ = true;
    return false;
  }
  uint16_t imgW =
      static_cast<uint16_t>(static_cast<uint8_t>(buf[0])) |
      (static_cast<uint16_t>(static_cast<uint8_t>(buf[1])) << 8);
  uint16_t imgH =
      static_cast<uint16_t>(static_cast<uint8_t>(buf[2])) |
      (static_cast<uint16_t>(static_cast<uint8_t>(buf[3])) << 8);
  if (imgW == 0 || imgH == 0 || imgW > 1024 || imgH > 1024) {
    free(buf);
    extLoadError_ = true;
    return false;
  }
  uint32_t frameBytes =
      static_cast<uint32_t>((imgW + 7) / 8) * static_cast<uint32_t>(imgH);
  if (frameBytes == 0) {
    free(buf);
    extLoadError_ = true;
    return false;
  }
  size_t bodyLen = len - 4;
  if (bodyLen < frameBytes || bodyLen % frameBytes != 0) {
    free(buf);
    extLoadError_ = true;
    return false;
  }
  uint32_t frames = static_cast<uint32_t>(bodyLen / frameBytes);
  if (frames > 32) frames = 32;
  uint32_t need = frameBytes * frames;
  extBits_ = static_cast<uint8_t*>(BadgeMemory::allocPreferPsram(need));
  if (!extBits_) {
    free(buf);
    extLoadError_ = true;
    return false;
  }
  const uint8_t* src = reinterpret_cast<const uint8_t*>(buf + 4);
  for (uint32_t i = 0; i < need; i++) {
    extBits_[i] = creditBinRev8(src[i]);
  }
  free(buf);
  extW_ = imgW;
  extH_ = imgH;
  extFrameBytes_ = frameBytes;
  extFrameCount_ = static_cast<uint8_t>(frames);
  extFormat_ = ExtFormat::kXBM;
  Serial.printf("[AnimTest] loadCreditBin %s: %ux%u %u frame(s) (bin)\n", path,
                (unsigned)imgW, (unsigned)imgH, (unsigned)frames);
  return true;
}

// ─── Render ─────────────────────────────────────────────────────────────────

void AnimTestScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);

  if (isExternal()) {
    // Native 1:1 render — no scaling. Bottom-right justified to mirror
    // the catalog-mode layout. Anything larger than the panel just
    // clips at the screen edges.
    const int dstW = extW_;
    const int dstH = extH_;
    int imgX = 128 - dstW;
    int imgY = 64 - dstH;
    if (imgX < 0) imgX = 0;
    if (imgY < 0) imgY = 0;

    const uint8_t* frameData =
        extBits_ + static_cast<uint32_t>(frameIdx_) * extFrameBytes_;

    if (extFormat_ == ExtFormat::kPage) {
      // SSD1306 page format: each byte = 8 vertical pixels in a column.
      //
      // The badge's display is physically mounted upside-down relative
      // to the U8G2 framebuffer, and `oled_set_framebuffer()`
      // rotates 180° before push (see mp_api_display.cpp ::
      // rotateFramebuffer180). Source `.fb` files are dumps of the
      // pre-rotation buffer, so they need the same 180° flip on the
      // way back out — otherwise the image renders upside-down.
      d.setDrawColor(1);
      const int pageRows = (extH_ + 7) / 8;
      const int xMax = extW_ - 1;
      const int yMax = extH_ - 1;
      for (int page = 0; page < pageRows; page++) {
        for (int px = 0; px < extW_; px++) {
          const uint8_t b = frameData[page * extW_ + px];
          if (!b) continue;
          for (uint8_t bit = 0; bit < 8; bit++) {
            const int py = page * 8 + bit;
            if (py >= extH_) break;
            if (b & (1 << bit)) {
              d.drawPixel(imgX + (xMax - px), imgY + (yMax - py));
            }
          }
        }
      }
    } else {
      // XBM: row-major, LSB-leftmost — drawXBM eats this directly.
      d.drawXBM(imgX, imgY, extW_, extH_, frameData);
    }

    // Info panel — left 64 px column, mirrors catalog mode layout.
    d.setFontPreset(FONT_TINY);
    char buf[24];
    d.drawStr(0, 6, extName_);
    d.drawHLine(0, 8, 64);
    std::snprintf(buf, sizeof(buf), "%dx%d", extW_, extH_);
    d.drawStr(0, 16, buf);
    if (extFrameCount_ > 1) {
      std::snprintf(buf, sizeof(buf), "F%d/%d %dms", frameIdx_ + 1,
                    extFrameCount_, frameDelayMs_);
      d.drawStr(0, 24, buf);
    }

    OLEDLayout::drawGameFooter(d);
    if (extFrameCount_ > 1) {
      OLEDLayout::drawUpperFooterActions(d, "fast", "slow", nullptr, nullptr,
                                         0, /*leftAlign=*/true);
    }
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "esc", nullptr,
                                  0, /*leftAlign=*/true);

    uint32_t now = millis();
    if (extFrameCount_ > 1 && now - lastFrameMs_ >= frameDelayMs_) {
      frameIdx_ = (frameIdx_ + 1) % extFrameCount_;
      lastFrameMs_ = now;
    }
    return;
  }

  // ── Catalog mode (original behaviour). ──────────────────────────
  const ImageInfo& img = kImageCatalog[imageIdx_];
  const uint8_t dispW = scaleDims_[scaleIdx_];
  const uint8_t dispH = dispW;  // all sources are square
  const bool use48 = img.data48 && (dispW == 48 || dispW == 24 || dispW == 12);
  const uint8_t srcDim = use48 ? 48 : 64;

  const uint8_t* frameData = ImageScaler::getFrame(img, frameIdx_, dispW);

  int imgX = 128 - dispW;
  int imgY = 64 - dispH;
  d.drawXBM(imgX, imgY, srcDim, srcDim, frameData, dispW, dispH);

  d.setFontPreset(FONT_TINY);
  char buf[16];

  d.drawStr(0, 6, img.name);
  d.drawHLine(0, 8, 64);

  std::snprintf(buf, sizeof(buf), "%dx%d", dispW, dispH);
  d.drawStr(0, 16, buf);

  // When authored per-frame times exist, frameDelayMs_ is a proportional
  // speed multiplier: at kDefaultDelay it plays at 1×; lower/higher values
  // scale every frame time proportionally.
  uint16_t effectiveDelay;
  if (img.frameTimes && frameIdx_ < img.frameCount) {
    uint32_t scaled =
        static_cast<uint32_t>(img.frameTimes[frameIdx_]) * frameDelayMs_ /
        kDefaultDelay;
    effectiveDelay = static_cast<uint16_t>(
        scaled < kMinDelay ? kMinDelay
                           : (scaled > kMaxDelay ? kMaxDelay : scaled));
  } else {
    effectiveDelay = frameDelayMs_;
  }
  std::snprintf(buf, sizeof(buf), "F%d/%d %dms",
                frameIdx_ + 1, img.frameCount, effectiveDelay);
  d.drawStr(0, 24, buf);

  std::snprintf(buf, sizeof(buf), "%d/%d", imageIdx_ + 1, kImageCatalogCount);
  d.drawStr(0, 32, buf);

  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawUpperFooterActions(d, "fast", "slow", nullptr, nullptr,
                                     0, /*leftAlign=*/true);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "next",
                                0, /*leftAlign=*/true);

  uint32_t now = millis();
  if (img.frameCount > 1 && now - lastFrameMs_ >= effectiveDelay) {
    frameIdx_ = (frameIdx_ + 1) % img.frameCount;
    lastFrameMs_ = now;
  }
}

// ─── Input ──────────────────────────────────────────────────────────────────

void AnimTestScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                 int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }

  if (e.confirmPressed) {
    if (isExternal()) {
      // No catalog cycling in external mode — A also exits so the
      // viewer reads as a regular file opener.
      gui.popScreen();
      return;
    }
    imageIdx_ = (imageIdx_ + 1) % kImageCatalogCount;
    updateScaleOptions();
  }

  if (e.xPressed) {
    if (frameDelayMs_ > kMinDelay + kDelayStep)
      frameDelayMs_ -= kDelayStep;
    else
      frameDelayMs_ = kMinDelay;
  }

  if (e.yPressed) {
    if (frameDelayMs_ < kMaxDelay - kDelayStep)
      frameDelayMs_ += kDelayStep;
    else
      frameDelayMs_ = kMaxDelay;
  }

  uint32_t nowMs = millis();
  int16_t joyDeltaY = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(joyDeltaY) > static_cast<int16_t>(kJoyDeadband)) {
    uint32_t repeatMs = 300;
    if (lastJoyNavMs_ == 0 || nowMs - lastJoyNavMs_ >= repeatMs) {
      lastJoyNavMs_ = nowMs;
      // Only the catalog mode has a scale ladder — external mode
      // renders 1:1, so the joystick is a no-op there.
      if (!isExternal() && scaleCount_ > 0) {
        if (joyDeltaY > 0) {
          scaleIdx_ = (scaleIdx_ + 1) % scaleCount_;
        } else {
          scaleIdx_ = (scaleIdx_ == 0) ? scaleCount_ - 1 : scaleIdx_ - 1;
        }
      }
    }
  } else {
    lastJoyNavMs_ = 0;
  }
}
