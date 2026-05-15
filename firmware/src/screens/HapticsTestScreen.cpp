#include "HapticsTestScreen.h"

#include <cstdio>

#include "../ui/GUI.h"
#include "../hardware/Haptics.h"

HapticsTestScreen::HapticsTestScreen(ScreenId sid)
    : ListMenuScreen(sid, "HAPTIC TEST") {}

uint8_t HapticsTestScreen::itemCount() const { return kItemCount; }

void HapticsTestScreen::formatItem(uint8_t index, char* buf,
                                   uint8_t bufSize) const {
  switch (index) {
    case kIntro:
      std::snprintf(buf, bufSize, "Intro fire");
      break;
    case kStrength:
      std::snprintf(buf, bufSize, "Strength   %3u",
                    (unsigned)Haptics::strength());
      break;
    case kPwmFreq:
      std::snprintf(buf, bufSize, "PWM Hz   %5lu",
                    (unsigned long)userPwmFreq_);
      break;
    case kPulseMs:
      std::snprintf(buf, bufSize, "Pulse ms %5u",
                    (unsigned)Haptics::clickPulseDuration());
      break;
    case kPulseShort:
      std::snprintf(buf, bufSize, "Short pulse");
      break;
    case kPulseLong:
      std::snprintf(buf, bufSize, "Long pulse");
      break;
    case kSteadyMid:
      std::snprintf(buf, bufSize, "Steady half");
      break;
    case kFreqLow:
      std::snprintf(buf, bufSize, "80 Hz pulse");
      break;
    case kFreqHigh:
      std::snprintf(buf, bufSize, "220 Hz pulse");
      break;
    case kPulseAtStr:
      std::snprintf(buf, bufSize, "Pulse @ str");
      break;
    default:
      buf[0] = '\0';
      break;
  }
}

void HapticsTestScreen::onEnter(GUIManager& gui) {
  ListMenuScreen::onEnter(gui);
  userPwmFreq_ = Haptics::pwmFrequencyHz();
  steadyEndMs_ = 0;
  Haptics::off();
}

void HapticsTestScreen::onExit(GUIManager& gui) {
  (void)gui;
  Haptics::off();
  Haptics::setPwmFrequency(userPwmFreq_);
}

void HapticsTestScreen::fireEffect(uint8_t index) {
  Haptics::off();
  Haptics::setPwmFrequency(userPwmFreq_);
  uint32_t now = millis();
  steadyEndMs_ = 0;

  switch (index) {
    case kStrength:
    case kPwmFreq:
      Haptics::setDuty(Haptics::strength());
      break;
    case kPulseShort:
      Haptics::pulse(Haptics::clickPulseDuration(), now);
      break;
    case kPulseLong:
      Haptics::pulse(Haptics::clickPulseDuration() * 4, now);
      break;
    case kSteadyMid:
      Haptics::setDuty(Haptics::strength() / 2);
      steadyEndMs_ = now + 900;
      break;
    case kFreqLow:
      Haptics::setPwmFrequency(80);
      Haptics::pulse(120, now);
      break;
    case kFreqHigh:
      Haptics::setPwmFrequency(220);
      Haptics::pulse(120, now);
      break;
    case kPulseAtStr:
      Haptics::pulse(100, now);
      break;
    default:
      break;
  }
}

void HapticsTestScreen::onItemFocus(uint8_t index, GUIManager& /*gui*/) {
  fireEffect(index);
}

void HapticsTestScreen::onItemSelect(uint8_t index, GUIManager& /*gui*/) {
  fireEffect(index);
}

void HapticsTestScreen::onItemAdjust(uint8_t index, int8_t dir,
                                     GUIManager& /*gui*/) {
  switch (index) {
    case kStrength:
      Haptics::adjustStrength(dir * Haptics::kStrengthStep);
      Haptics::setDuty(Haptics::strength());
      break;
    case kPwmFreq: {
      int32_t step = userPwmFreq_ >= 10000  ? 1000
                     : userPwmFreq_ >= 1000 ? 500
                     : userPwmFreq_ >= 100  ? 50
                                            : 10;
      int32_t f = static_cast<int32_t>(userPwmFreq_) + dir * step;
      if (f < 1) f = 1;
      if (f > 250000) f = 250000;
      userPwmFreq_ = static_cast<uint32_t>(f);
      Haptics::setPwmFrequency(userPwmFreq_);
      Haptics::setDuty(Haptics::strength());
      break;
    }
    case kPulseMs: {
      int32_t ms = Haptics::clickPulseDuration() + dir * 5;
      if (ms < 5) ms = 5;
      if (ms > 500) ms = 500;
      Haptics::setClickPulseDuration(static_cast<uint16_t>(ms));
      Haptics::pulse(static_cast<uint16_t>(ms), millis());
      break;
    }
    default:
      break;
  }
}

void HapticsTestScreen::onUpdate(GUIManager& /*gui*/) {
  if (steadyEndMs_ != 0 &&
      static_cast<int32_t>(millis() - steadyEndMs_) >= 0) {
    Haptics::setDuty(0);
    steadyEndMs_ = 0;
  }
}
