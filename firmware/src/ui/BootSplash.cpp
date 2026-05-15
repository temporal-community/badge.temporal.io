#include "BootSplash.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>

#include "../hardware/LEDmatrix.h"
#include "../hardware/oled.h"
#include "../identity/BadgeVersion.h"   // FIRMWARE_VERSION
#include "temporal_logo.xbm.h"   // defines temporal_bits / temporal_width / temporal_height

extern oled badgeDisplay;
extern LEDmatrix badgeMatrix;

namespace BootSplash {

namespace {

constexpr int kWidth  = 128;
constexpr int kHeight = 64;
constexpr int kFrameMs = 33;        // ~30 FPS
constexpr int kMaxStars = 90;
constexpr int kBrightCount = 6;
constexpr int kSparkleCount = 5;    // stars rendered as a 3×3 corner-and-centre glyph

// Extra dead-zone around the temporal logo's tight bounding box so the
// starfield doesn't crowd the stroke edges. Applied symmetrically to
// every side; bumping this widens the visual breathing room without
// requiring asset changes.
constexpr int kLogoMargin = 4;

struct Star {
  uint8_t x;
  uint8_t y;
  uint16_t phaseQ12;   // twinkle phase, 0..2pi mapped into 0..4096
  uint8_t  speedQ4;    // twinkle speed (rad/s), 0..16 mapped into 0..255
  uint8_t  baseQ8;     // base brightness multiplier in 0..1, 0..255
  bool     bright;
  // Glyph variant: when true and alpha is high enough, the star renders
  // as a 3×3 sparkle pattern { 1 0 1 / 0 1 0 / 1 0 1 } — centre plus
  // four diagonal corners — instead of the single-pixel + cardinal
  // expansion used for `bright`.
  bool     sparkle;
  // When true, this small dot stays SOLID (modulated only by the global
  // dotEnv) instead of doing the per-bucket pseudo-random blink. Mixing
  // a fraction of always-on dots with the blinking ones gives the field
  // a sense of structure — anchor stars + scintillating distant ones —
  // instead of every pixel flashing in isolation.
  bool     steady;
};

inline float urand01() {
  return (esp_random() & 0xFFFFFF) / static_cast<float>(0x1000000);
}

// ── LED matrix sparkle state ───────────────────────────────────────────────
//
// Per-pixel state for an in-process port of LED_ANIM_STAR (the same
// algorithm used by `BootScreen::tickSparkles`). Lives in BootSplash's
// anonymous namespace so its lifetime is bounded by the splash call —
// no global memory cost outside this animation.
#ifdef BADGE_HAS_LED_MATRIX
constexpr int   kLedCells       = LED_MATRIX_WIDTH * LED_MATRIX_HEIGHT;
constexpr uint8_t  kLedSparkleMaxBrt = 60;
constexpr uint8_t  kLedSparkleMaxLive = 9;   // bounded — keep matrix airy
constexpr uint8_t  kLedSparkleSpawnPct = 35; // % per tick; 8x8 is small
constexpr int8_t   kLedSparkleSpeedMin = 5;
constexpr int8_t   kLedSparkleSpeedMax = 16;
constexpr uint32_t kLedSparkleTickMs   = 23;

struct LedSparkle {
  uint8_t bright;
  int8_t  speed;   // 0 = idle; +ve = brightening; -ve = fading
};
static LedSparkle gLedSparkles[kLedCells];
static uint32_t   gLedSparkleLastMs = 0;

static bool ledSparkleIsolated(int idx) {
  const int r = idx / LED_MATRIX_WIDTH;
  const int c = idx % LED_MATRIX_WIDTH;
  for (int dr = -1; dr <= 1; ++dr) {
    for (int dc = -1; dc <= 1; ++dc) {
      if (dr == 0 && dc == 0) continue;
      const int nr = r + dr;
      const int nc = c + dc;
      if (nr < 0 || nr >= LED_MATRIX_HEIGHT) continue;
      if (nc < 0 || nc >= LED_MATRIX_WIDTH)  continue;
      const int ni = nr * LED_MATRIX_WIDTH + nc;
      if (gLedSparkles[ni].speed != 0 || gLedSparkles[ni].bright != 0) {
        return false;
      }
    }
  }
  return true;
}

// Advance LED-matrix sparkles by one tick. Called once per OLED frame; the
// internal cadence gate enforces ~30 fps regardless of caller rate.
static void tickLedSparkles(uint32_t nowMs) {
  if (gLedSparkleLastMs == 0) gLedSparkleLastMs = nowMs;
  if ((nowMs - gLedSparkleLastMs) < kLedSparkleTickMs) return;
  gLedSparkleLastMs = nowMs;

  int activeCount = 0;
  for (int i = 0; i < kLedCells; ++i) {
    if (gLedSparkles[i].speed == 0) continue;
    activeCount++;

    int b = (int)gLedSparkles[i].bright + (int)gLedSparkles[i].speed;
    const uint8_t x = (uint8_t)(i % LED_MATRIX_WIDTH);
    const uint8_t y = (uint8_t)(i / LED_MATRIX_WIDTH);

    if (gLedSparkles[i].speed > 0 && b >= kLedSparkleMaxBrt) {
      b = kLedSparkleMaxBrt;
      gLedSparkles[i].speed = (int8_t)-gLedSparkles[i].speed;
    } else if (gLedSparkles[i].speed < 0 && b <= 0) {
      b = 0;
      gLedSparkles[i].speed = 0;
      gLedSparkles[i].bright = 0;
      badgeMatrix.setPixel(x, y, 0);
      continue;
    }
    gLedSparkles[i].bright = (uint8_t)b;
    badgeMatrix.setPixel(x, y, gLedSparkles[i].bright);
  }

  if (activeCount < kLedSparkleMaxLive &&
      (int)(esp_random() % 100) < kLedSparkleSpawnPct) {
    for (int attempt = 0; attempt < 12; ++attempt) {
      const int idx = (int)(esp_random() % kLedCells);
      if (gLedSparkles[idx].speed == 0 && gLedSparkles[idx].bright == 0 &&
          ledSparkleIsolated(idx)) {
        const int range = kLedSparkleSpeedMax - kLedSparkleSpeedMin + 1;
        gLedSparkles[idx].speed =
            (int8_t)(kLedSparkleSpeedMin + (int)(esp_random() % range));
        break;
      }
    }
  }
}

static void resetLedSparkles() {
  memset(gLedSparkles, 0, sizeof(gLedSparkles));
  gLedSparkleLastMs = 0;
  badgeMatrix.stopAnimation();
  badgeMatrix.clear(0);
}
#else
static void tickLedSparkles(uint32_t)  {}
static void resetLedSparkles()        {}
#endif

}  // namespace

// XBM byte test — bit 0 of each byte is the leftmost pixel.
static inline bool logoLit(int x, int y) {
  if (x < 0 || x >= temporal_width || y < 0 || y >= temporal_height) return false;
  const int bytesPerRow = (temporal_width + 7) >> 3;
  const uint8_t b = temporal_bits[y * bytesPerRow + (x >> 3)];
  return (b >> (x & 7)) & 1;
}

// Tight bounding box of every lit pixel in the temporal logo XBM. Used to
// veto starfield placement inside the logo's footprint — including the
// dark interior between strokes — so the negative space reads cleanly.
//
// Computed once and cached: the XBM is compiled in, so the bounds can't
// change at runtime. Returns false when the logo is entirely empty (would
// only happen if the asset is broken).
struct LogoBounds {
  int minX, minY, maxX, maxY;
  bool valid;
};

static const LogoBounds& logoBounds() {
  static LogoBounds b = []() {
    LogoBounds out{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, false};
    const int bytesPerRow = (temporal_width + 7) >> 3;
    for (int y = 0; y < temporal_height; ++y) {
      for (int xb = 0; xb < bytesPerRow; ++xb) {
        const uint8_t row = temporal_bits[y * bytesPerRow + xb];
        if (row == 0) continue;
        const int xBase = xb << 3;
        for (int bit = 0; bit < 8; ++bit) {
          if (!((row >> bit) & 1)) continue;
          const int x = xBase + bit;
          if (x >= temporal_width) continue;
          if (x < out.minX) out.minX = x;
          if (x > out.maxX) out.maxX = x;
          if (y < out.minY) out.minY = y;
          if (y > out.maxY) out.maxY = y;
          out.valid = true;
        }
      }
    }
    return out;
  }();
  return b;
}

// True if (x, y) lies anywhere within the logo's content bounding box
// EXPANDED by kLogoMargin on every side. Used as the starfield
// rejection test so dots don't crowd the stroke edges.
static inline bool insideLogoBounds(int x, int y) {
  const LogoBounds& b = logoBounds();
  if (!b.valid) return false;
  return x >= (b.minX - kLogoMargin) && x <= (b.maxX + kLogoMargin) &&
         y >= (b.minY - kLogoMargin) && y <= (b.maxY + kLogoMargin);
}

// Render the temporal logo XBM solid into the active OLED framebuffer.
// Caller is responsible for clearBuffer / sendBuffer / draw colour. Used
// every frame instead of the old temporal-dithering ramp; visibility is
// now controlled by hardware contrast fades on either side.
static void drawLogoSolid() {
  const int bytesPerRow = (temporal_width + 7) >> 3;
  for (int y = 0; y < temporal_height; ++y) {
    for (int xb = 0; xb < bytesPerRow; ++xb) {
      const uint8_t row = temporal_bits[y * bytesPerRow + xb];
      if (row == 0) continue;
      const int xBase = xb << 3;
      for (int bit = 0; bit < 8; ++bit) {
        if ((row >> bit) & 1) badgeDisplay.drawPixel(xBase + bit, y);
      }
    }
  }
}

// Tag the firmware version in the bottom-right corner. Drawn alongside
// the logo on every splash frame so the version is visible regardless
// of which animation phase we're in. Tiny font (~4×6) keeps it out of
// the starfield's way; right-aligned at x=127, baseline y=63.
static void drawVersionBadge() {
  badgeDisplay.setDrawColor(1);
  badgeDisplay.setFontPreset(FONT_TINY);
  const char* v = FIRMWARE_VERSION;
  const int w = badgeDisplay.getStrWidth(v);
  badgeDisplay.drawStr(kWidth - w - 1, kHeight - 1, v);
}

void playStarfield(uint32_t durationMs, int numStars) {
  if (numStars > kMaxStars) numStars = kMaxStars;
  if (numStars < 1) numStars = 1;

  Star stars[kMaxStars];
  for (int i = 0; i < numStars; ++i) {
    // Re-roll until the star is OUTSIDE the logo's content bounding box.
    // The logo fills a tight rectangle; rejecting on bbox (not just lit
    // pixels) keeps the negative space inside the logo clean instead of
    // sprinkling dots between strokes.
    uint8_t sx = 0, sy = 0;
    for (int tries = 0; tries < 16; ++tries) {
      sx = static_cast<uint8_t>(esp_random() % kWidth);
      sy = static_cast<uint8_t>(esp_random() % kHeight);
      if (!insideLogoBounds(sx, sy)) break;
    }
    stars[i].x = sx;
    stars[i].y = sy;
    stars[i].phaseQ12 = static_cast<uint16_t>(esp_random() % 4096);
    // Twinkle speed (Q4 fixed point, scale 1/16). Lowered from
    // 40..104 (2.5..6.5 rad/s, period ~1..2.5 s) to 8..24
    // (0.5..1.5 rad/s, period ~4..12 s) so individual stars no longer
    // visibly flicker — they pulse over several seconds instead.
    stars[i].speedQ4 = static_cast<uint8_t>(8 + (esp_random() % 17));
    stars[i].baseQ8  = static_cast<uint8_t>(140 + (esp_random() % 116));
    stars[i].bright = false;
    stars[i].sparkle = false;
    // ~30% of dots are "steady": always on once the dot envelope is up.
    // Independent of the bright/sparkle big-star tagging below — those
    // run on their own envelope/pulse path and don't consult `steady`.
    stars[i].steady = (esp_random() % 100u) < 30u;
  }
  // Tag the leading slots as the "big" set — sparkles first, bright
  // crosses after. Disjoint assignment (no random collisions) means we
  // get a predictable count of each glyph and never accidentally turn
  // a sparkle into a cross. Star positions are already random, so
  // tagging by index keeps the spatial scatter intact.
  for (int i = 0; i < kSparkleCount && i < numStars; ++i) {
    stars[i].sparkle = true;
  }
  for (int i = kSparkleCount;
       i < kSparkleCount + kBrightCount && i < numStars; ++i) {
    stars[i].bright = true;
  }

  // Mirror the OLED starfield onto the LED matrix: random per-cell
  // fade-in/glow/fade-out. State is owned by this call only — reset on
  // entry, cleared on exit, no other animator runs concurrently.
  resetLedSparkles();

  // ── Phase 1: hardware logo fade-in ─────────────────────────────────────
  //
  // Render the temporal logo solid into the framebuffer, then ramp the
  // OLED contrast from 0 up to fullContrast_ via the dedicated hardware
  // helper. Replaces the temporal-dithering animation that ran during
  // the same window — the dithered pixels were visibly noisy, while
  // contrast ramping gives a smooth analog-feeling fade.
  //
  // transitionOut(1) drops contrast to 0 immediately without clobbering
  // fullContrast_, so the matching transitionIn() lands at the user's
  // configured brightness. Both calls block; the LED matrix tick is
  // paused for the ~kLogoFadeInMs window, which is fine because the
  // matrix is uninitialized this early in boot anyway.
  constexpr uint32_t kLogoFadeInMs = 800;
  badgeDisplay.transitionOut(1);
  badgeDisplay.clearBuffer();
  drawLogoSolid();
  drawVersionBadge();
  badgeDisplay.sendBuffer();
  badgeDisplay.transitionIn(kLogoFadeInMs);

  // ── Phase 2: starfield around the now-solid logo ───────────────────────
  //
  // Order of appearance is intentional:
  //   1. Big stars (sparkle 3×3 + bright cardinal cross) lead in first,
  //      ramp to full brightness over kBigFadeInMs, and persist with a
  //      slow per-star pulse so they read as deliberate fixtures.
  //   2. Small dots start later and flicker INDEPENDENTLY — each one
  //      gets its own pseudo-random schedule (per-bucket hash) so they
  //      look like distant scintillating stars rather than co-fading
  //      with the global envelope.
  //   3. Both fade out together over the closing kFadeOutMs.
  constexpr uint32_t kBigStartMs    = 120;   // big stars enter ~one frame in
  constexpr uint32_t kBigFadeInMs   = 350;
  constexpr uint32_t kDotsStartMs   = 650;
  constexpr uint32_t kFadeOutMs     = 400;
  constexpr uint32_t kBlinkBucketMs = 120;   // each dot's on/off resolves at this rate

  const uint32_t start = millis();
  while (true) {
    const uint32_t now = millis();
    const uint32_t elapsed = now - start;
    if (elapsed >= durationMs) break;
    tickLedSparkles(now);

    // Envelopes (0..1).
    float bigEnv;
    if (elapsed < kBigStartMs) {
      bigEnv = 0.f;
    } else if (elapsed < kBigStartMs + kBigFadeInMs) {
      bigEnv = static_cast<float>(elapsed - kBigStartMs) / kBigFadeInMs;
    } else if (durationMs > kFadeOutMs && elapsed > durationMs - kFadeOutMs) {
      bigEnv = static_cast<float>(durationMs - elapsed) / kFadeOutMs;
    } else {
      bigEnv = 1.f;
    }

    float dotEnv;
    if (elapsed < kDotsStartMs) {
      dotEnv = 0.f;
    } else if (durationMs > kFadeOutMs && elapsed > durationMs - kFadeOutMs) {
      dotEnv = static_cast<float>(durationMs - elapsed) / kFadeOutMs;
    } else {
      dotEnv = 1.f;
    }

    badgeDisplay.clearBuffer();
    drawLogoSolid();
    drawVersionBadge();

    const float tSec = elapsed / 1000.0f;
    const uint32_t bucket = elapsed / kBlinkBucketMs;

    for (int i = 0; i < numStars; ++i) {
      const Star& s = stars[i];
      if (insideLogoBounds(s.x, s.y)) continue;

      const bool isBig = s.sparkle || s.bright;

      if (isBig) {
        if (bigEnv <= 0.f) continue;
        // Slow per-star pulse on top of the envelope so each big star
        // breathes a little instead of being statically lit.
        const float starPhase = (s.phaseQ12 / 4096.0f) * 6.2831853f;
        const float starSpeed = s.speedQ4 / 16.0f;
        const float pulse = 0.65f
                          + 0.35f * (0.5f + 0.5f * sinf(tSec * starSpeed + starPhase));
        const float alpha = bigEnv * pulse;

        // Centre lights up first as the envelope ramps; the corner /
        // cardinal expansion only kicks in once the centre is firmly on
        // so each big star "fires up" outward from the middle.
        if (alpha > 0.45f) badgeDisplay.drawPixel(s.x, s.y);
        if (alpha > 0.65f) {
          if (s.sparkle) {
            if (s.x > 0          && s.y > 0           && !insideLogoBounds(s.x - 1, s.y - 1)) badgeDisplay.drawPixel(s.x - 1, s.y - 1);
            if (s.x < kWidth - 1 && s.y > 0           && !insideLogoBounds(s.x + 1, s.y - 1)) badgeDisplay.drawPixel(s.x + 1, s.y - 1);
            if (s.x > 0          && s.y < kHeight - 1 && !insideLogoBounds(s.x - 1, s.y + 1)) badgeDisplay.drawPixel(s.x - 1, s.y + 1);
            if (s.x < kWidth - 1 && s.y < kHeight - 1 && !insideLogoBounds(s.x + 1, s.y + 1)) badgeDisplay.drawPixel(s.x + 1, s.y + 1);
          } else { // bright cross
            if (s.x > 0          && !insideLogoBounds(s.x - 1, s.y)) badgeDisplay.drawPixel(s.x - 1, s.y);
            if (s.x < kWidth - 1 && !insideLogoBounds(s.x + 1, s.y)) badgeDisplay.drawPixel(s.x + 1, s.y);
            if (s.y > 0          && !insideLogoBounds(s.x, s.y - 1)) badgeDisplay.drawPixel(s.x, s.y - 1);
            if (s.y < kHeight - 1&& !insideLogoBounds(s.x, s.y + 1)) badgeDisplay.drawPixel(s.x, s.y + 1);
          }
        }
      } else if (s.steady) {
        // Steady anchor dot — always on once the dot envelope is up.
        // Doesn't blink; just rides the global fade so it appears with
        // the rest of the field and fades out at the end.
        if (dotEnv > 0.4f) badgeDisplay.drawPixel(s.x, s.y);
      } else {
        // Small dot — independent intermittent blink. Each (star, bucket)
        // pair derives a deterministic 32-bit hash; dot is lit when the
        // hash falls under a small threshold scaled by dotEnv. Streams
        // are independent across stars and across time buckets, so the
        // dots scintillate asynchronously.
        if (dotEnv <= 0.f) continue;
        uint32_t h = static_cast<uint32_t>(i) * 2654435761u
                   + bucket * 374761393u;
        h = (h ^ (h >> 16)) * 2246822519u;
        h = (h ^ (h >> 13)) * 3266489917u;
        h = h ^ (h >> 16);
        const uint32_t threshold = static_cast<uint32_t>(dotEnv * 9.f);
        if (threshold > 0 && (h % 100u) < threshold) {
          badgeDisplay.drawPixel(s.x, s.y);
        }
      }
    }

    badgeDisplay.sendBuffer();

    const uint32_t frameElapsed = millis() - now;
    if (frameElapsed < kFrameMs) delay(kFrameMs - frameElapsed);
  }

  // End frame: fully solid logo, no stars, so the splash hands off
  // cleanly to whatever the menu draws next.
  badgeDisplay.clearBuffer();
  drawLogoSolid();
  drawVersionBadge();
  badgeDisplay.sendBuffer();
  delay(150);
  // Hardware fade-down hands off cleanly to the next scene
  // (BootScreen) rather than blanking abruptly. fullContrast_ is
  // preserved so a later transitionIn() ramps back to the user's
  // configured brightness.
  badgeDisplay.transitionOut(300);
  badgeDisplay.clearBuffer();
  badgeDisplay.sendBuffer();
  // Wipe the LED matrix sparkles before handing back to whatever the
  // post-splash flow expects to drive the matrix next.
  resetLedSparkles();
}

}  // namespace BootSplash
