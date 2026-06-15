#pragma once
#include "Screen.h"

// ─── Credits screen ─────────────────────────────────────────────────────────
//
// HelpScreen-style smooth vertical scroll through the badge crew. Each
// "card" is a 64-px-tall slot with a 64×64 dithered headshot on the
// left and the contributor's name + bio on the right. Joystick Y
// scrolls continuously; LEFT/RIGHT (or CONFIRM) snap the next/previous
// card to the top of the viewport.
//
// Cards correspond to AboutCredits::kGroups[] entries — credits whose
// `group` key matches in credits.json (default: their `name`) are
// pooled into a single card slot, and a fresh variant from that pool
// is randomly chosen every time the slot scrolls into view. So if you
// scroll past "Kevin," scroll back, you'll most likely see a different
// Kevin headshot.
//
// Bitmap data lives in AboutCredits.h (kCreditBits, kCredits[],
// kGroups[]), produced by scripts/gen_credit_xbms.py from the source
// images in assets/credits/. The MicroPython companion app
// (data/apps/credits.py) renders the same headshots without grouping —
// kCreditsUsePython picks which backend launches.

#include "AboutCredits.h"

class AboutCreditsScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenAboutCredits; }
  bool showCursor() const override { return false; }
  ScreenAccess access() const override { return ScreenAccess::kAny; }

 private:
  // Snap the viewport so group `idx`'s card is flush with the top
  // (or the last legal scroll position when near the end of the list).
  void snapToGroup(uint8_t idx);

  int16_t  scrollPx_     = 0;
  uint32_t lastScrollMs_ = 0;
  // Animation epoch — every animated credit's frame timeline is
  // computed relative to this so the loops stay in phase across redraws.
  uint32_t enterMs_      = 0;

  // Per-group state for the bag-shuffle variant picker.
  //
  // `bag_[i][...]` holds variant indices for group i, each variant
  // repeated by its `weight` so a `weight: 2` photo appears twice per
  // pass. The bag is Fisher-Yates shuffled at the start of every
  // pass, so within a single pass no variant repeats unless its
  // weight calls for it.
  //
  // `bagPos_[i]` is the next index to consume from `bag_[i]`. When it
  // hits `kGroups[i].bagSize` the bag is reshuffled and `bagPos_`
  // resets to 0. A sentinel (kBagNeedsRefill = 0xFF) is used after
  // onEnter() to force a refill on the first render.
  //
  // `currentVariant_[i]` is the variant currently being painted on
  // the card. `variantShownAtMs_[i]` is the millis() timestamp the
  // current variant was first shown — used to auto-cycle every ~6 s
  // for static variants while the user lingers (animations are
  // exempt and stay until the user scrolls them away).
  uint8_t  bag_[AboutCredits::kGroupCount]
                [AboutCredits::kMaxBagSize] = {};
  uint8_t  bagPos_[AboutCredits::kGroupCount] = {};
  uint8_t  currentVariant_[AboutCredits::kGroupCount] = {};
  uint32_t variantShownAtMs_[AboutCredits::kGroupCount] = {};
  bool     wasVisible_[AboutCredits::kGroupCount] = {};

  // Helpers for the bag picker. `refillBag` rebuilds + reshuffles a
  // group's bag (no-op for single-variant groups). `pickNextVariant`
  // advances the bag position and stamps `variantShownAtMs_`.
  void refillBag(uint8_t groupIdx);
  void pickNextVariant(uint8_t groupIdx, uint32_t nowMs);
};
