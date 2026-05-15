#pragma once
#include "Screen.h"

// ─── Sponsors credits screen ────────────────────────────────────────────────
//
// Two rows of 32px-tall sponsor logos scroll past in opposite directions.
// Sponsors are randomly partitioned onto the two rows on every entry so
// the layout doesn't repeat.

#include "AboutSponsors.h"

class AboutSponsorsScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenAboutSponsors; }
  bool showCursor() const override { return false; }
  ScreenAccess access() const override { return ScreenAccess::kAny; }

 private:
  // Per-row metadata: the indices into AboutSponsors::kSponsors picked for
  // this row, and the total pixel width of the row including inter-logo
  // gutters (used to wrap the scroll).
  struct Row {
    uint8_t  indices[AboutSponsors::kCount];
    uint8_t  count;
    uint16_t totalWidth;
  };

  void shufflePartition();
  void drawRow(oled& d, const Row& row, int y, int32_t scrollPx,
               bool reverse) const;

  Row topRow_{};
  Row bottomRow_{};
  uint32_t enterMs_ = 0;
};
