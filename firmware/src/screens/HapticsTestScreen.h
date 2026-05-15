#pragma once
#include "Screen.h"

// ─── Haptics test screen (motor test walkthrough) ───────────────────────────

class HapticsTestScreen : public ListMenuScreen {
 public:
  HapticsTestScreen(ScreenId sid);

  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  void onItemSelect(uint8_t index, GUIManager& gui) override;
  void onItemAdjust(uint8_t index, int8_t dir, GUIManager& gui) override;
  void onItemFocus(uint8_t index, GUIManager& gui) override;
  void onUpdate(GUIManager& gui) override;
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;

 private:
  enum Item : uint8_t {
    kIntro, kStrength, kPwmFreq, kPulseMs,
    kPulseShort, kPulseLong, kSteadyMid,
    kFreqLow, kFreqHigh, kPulseAtStr,
    kItemCount,
  };

  uint32_t userPwmFreq_ = 80;
  uint32_t steadyEndMs_ = 0;

  void fireEffect(uint8_t index);
};
