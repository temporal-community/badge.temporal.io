#pragma once
#include "Screen.h"
#include "../boops/BadgeBoops.h"  // for BadgeBoops::BoopPhase used in member type

// ─── Boop screen (IR pairing) ───────────────────────────────────────────────

class BoopScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenBoop; }
  bool showCursor() const override { return false; }

 private:
  // Floating-chip band — ephemeral sprites that animate through the
  // left-info area with one of two directions:
  //   rising=true  → TX event: starts at y=46, ascends off the top edge
  //                 where the IR LED physically sits.
  //   rising=false → RX event: starts at y=-6 (above top), falls down
  //                 into the info area.
  // The two directions share the same ring buffer and throttle so they
  // never stack on top of each other. Chips drift through the name/
  // company overlay so it reads as "data streaming in and out".
  struct TxChip {
    uint32_t bornMs;       // 0 = empty slot
    uint8_t  tag;          // FIELD_* enum or 0xA0/0xA1 sentinel for beacons
    uint8_t  xHint;        // pseudo-random x jitter per chip
    bool     rising;       // true = TX (up), false = RX (down)
    char     label[10];    // short display text
  };
  static constexpr uint8_t  kMaxTxChips         = 6;
  static constexpr uint32_t kTxChipTtlMs        = 1200;
  // Minimum gap between successive chip spawns. The state machine pre-marks
  // empty/masked field bits in a single loop iteration, so without throttling
  // we'd spawn a cluster of chips at the same (x,y) and they'd overlap into
  // an unreadable smear.
  static constexpr uint32_t kChipSpawnMinGapMs      = 180;
  static constexpr uint32_t kBeaconChipCooldownMs   = 3000;
  TxChip   txChips_[kMaxTxChips] = {};
  // TX trackers
  uint16_t prevTxMask_            = 0;
  uint16_t pendingTxEdges_        = 0;
  uint8_t  prevBeaconTxCount_     = 0;
  bool     pendingBeaconTxChip_   = false;
  uint32_t lastBeaconTxChipMs_    = 0;
  // RX trackers (mirror of TX)
  uint16_t prevRxMask_            = 0;
  uint16_t pendingRxEdges_        = 0;
  uint8_t  prevBeaconRxCount_     = 0;
  bool     pendingBeaconRxChip_   = false;
  uint32_t lastBeaconRxChipMs_    = 0;
  uint32_t lastChipSpawnMs_       = 0;

  // Bottom log strip (rows 48..63) — vertically scrolling event lines.
  // Newest chip enters from below; older lines slide upward and off the
  // top. Ring of recent entries with an animation offset that decays
  // from kLogLineH to 0 after a push.
  static constexpr uint8_t  kMaxLogLines = 5;
  static constexpr uint8_t  kLogLineH    = 8;
  static constexpr uint32_t kLogAnimMs   = 280;
  char     logLines_[kMaxLogLines][12] = {};
  uint8_t  logCount_ = 0;            // total chips ever pushed (for ordering)
  uint32_t logAnimStartMs_ = 0;      // when most recent chip was pushed

  // Ziggy animation — frame index ticks at a phase-dependent interval.
  uint8_t  ziggyFrame_ = 0;
  uint32_t lastZiggyStepMs_ = 0;
  BadgeBoops::BoopPhase prevPhase_ = BadgeBoops::BOOP_PHASE_IDLE;

  void poll(uint32_t nowMs);
  bool spawnChip(const char* label, uint8_t tagForXHint,
                 bool rising, uint32_t nowMs);
  void pushLogLine(const char* chip, uint32_t nowMs);
  void renderTopTxBand(oled& d, uint32_t nowMs);
  void renderActiveAtmospherics(oled& d, uint32_t nowMs);
  // Sparkle-wipe reveal animation overlaid on the confirmed peer card.
  // Plays once for ~1 second after PAIRED_OK; the underlying contact
  // card is drawn by the per-handler drawContent() (peer_drawContent)
  // which auto-fits font sizes to the available area.
  void renderConfirmedReveal(oled& d, uint32_t nowMs);
  void renderZiggy(oled& d, uint32_t nowMs);
  void renderLog(oled& d, uint32_t nowMs);
};
