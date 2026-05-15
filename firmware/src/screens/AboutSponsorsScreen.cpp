#include "AboutSponsorsScreen.h"

#include <Arduino.h>

#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"

namespace {

// Two 32px rows fill the 64px screen exactly — no header, no gap.
constexpr int kTopRowY         = 0;
constexpr int kBottomRowY      = 32;
constexpr int kLogoGapPx       = 16;    // gutter between adjacent logos
constexpr int kScrollPxPerSec  = 22;    // logo travel speed
// GUIManager wraps push/pop in a ~200 ms hardware contrast fade. The fade-up
// is blocking, so the panel holds whatever was rendered just before it — i.e.
// the frame at scroll=0. Pin scroll at zero for a touch longer than the fade
// so the reveal lands on that clean static frame and motion starts smoothly
// instead of jumping forward by the time millis() advanced during the fade.
constexpr uint32_t kFadeSettleMs = 240;

uint16_t rowTotalWidth(const uint8_t* indices, uint8_t count) {
  if (count == 0) return 0;
  uint32_t w = 0;
  for (uint8_t i = 0; i < count; ++i) {
    w += AboutSponsors::kSponsors[indices[i]].width;
  }
  w += static_cast<uint32_t>(count) * kLogoGapPx;
  return static_cast<uint16_t>(w);
}

}  // namespace

void AboutSponsorsScreen::onEnter(GUIManager& /*gui*/) {
  randomSeed(millis() ^ micros());
  shufflePartition();
  enterMs_ = millis();
}

void AboutSponsorsScreen::shufflePartition() {
  // Fisher-Yates over a local index buffer.
  uint8_t order[AboutSponsors::kCount];
  for (uint8_t i = 0; i < AboutSponsors::kCount; ++i) order[i] = i;
  for (int i = AboutSponsors::kCount - 1; i > 0; --i) {
    int j = static_cast<int>(random(i + 1));
    uint8_t t = order[i];
    order[i] = order[j];
    order[j] = t;
  }

  // Half-and-half. With an odd sponsor count, the extra goes to whichever
  // row wins the coin flip — keeps the layout from always being top-heavy.
  uint8_t half = AboutSponsors::kCount / 2;
  if ((AboutSponsors::kCount & 1) && (random(2) == 0)) half += 1;

  topRow_.count = half;
  bottomRow_.count = AboutSponsors::kCount - half;

  for (uint8_t i = 0; i < topRow_.count; ++i) topRow_.indices[i] = order[i];
  for (uint8_t i = 0; i < bottomRow_.count; ++i) {
    bottomRow_.indices[i] = order[topRow_.count + i];
  }

  topRow_.totalWidth    = rowTotalWidth(topRow_.indices,    topRow_.count);
  bottomRow_.totalWidth = rowTotalWidth(bottomRow_.indices, bottomRow_.count);
}

void AboutSponsorsScreen::drawRow(oled& d, const Row& row, int y,
                                  int32_t scrollPx, bool reverse) const {
  if (row.count == 0 || row.totalWidth == 0) return;

  // Normalize scroll into [0, totalWidth) so the wrap math stays bounded.
  int32_t mod = static_cast<int32_t>(row.totalWidth);
  int32_t s = scrollPx % mod;
  if (s < 0) s += mod;

  // For "reverse" (scroll left→right) we negate the offset. Both rows then
  // walk through the same partial-then-wrap blit logic, just mirrored.
  int32_t base = reverse ? s : -s;

  // Two passes: at base and at base ± totalWidth so logos wrap seamlessly
  // across the screen edges.
  const int32_t passes[2] = {
    base,
    reverse ? base - mod : base + mod,
  };

  for (int p = 0; p < 2; ++p) {
    int32_t cursorX = passes[p];
    for (uint8_t i = 0; i < row.count; ++i) {
      const AboutSponsors::Sponsor& s = AboutSponsors::kSponsors[row.indices[i]];
      int x = static_cast<int>(cursorX);
      // Clip in software so we don't ask u8g2 to scan thousands of off-screen
      // bytes for sponsors that aren't visible this frame.
      if (x + s.width > 0 && x < OLEDLayout::kScreenW) {
        d.drawXBM(x, y, s.width, AboutSponsors::kHeight,
                  AboutSponsors::kSponsorBits + s.byteOffset);
      }
      cursorX += s.width + kLogoGapPx;
    }
  }
}

void AboutSponsorsScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);

  uint32_t now = millis();
  uint32_t elapsed = now - enterMs_;
  uint32_t motionMs = (elapsed > kFadeSettleMs) ? (elapsed - kFadeSettleMs) : 0;
  int32_t scroll = static_cast<int32_t>(motionMs * kScrollPxPerSec / 1000U);

  drawRow(d, topRow_,    kTopRowY,    scroll, /*reverse=*/false);
  drawRow(d, bottomRow_, kBottomRowY, scroll, /*reverse=*/true);
}

void AboutSponsorsScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                      int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  if (e.cancelPressed || e.confirmPressed) {
    gui.popScreen();
  }
}
