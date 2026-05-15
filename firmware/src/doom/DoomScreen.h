#ifndef DOOM_SCREEN_H
#define DOOM_SCREEN_H

#ifdef BADGE_HAS_DOOM

#include "../screens/Screen.h"

class DoomScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX,
                   int16_t cursorY, GUIManager& gui) override;
  ScreenId id() const override { return kScreenDoom; }
  bool showCursor() const override { return false; }
  bool needsRender() override;

 private:
  enum class Phase : uint8_t { kHelp, kSettings, kRunning, kExiting, kNoWad };
  Phase phase_ = Phase::kHelp;
  uint8_t settingsCursor_ = 0;

  void renderHelpScreen(oled& d);
  void renderSettingsScreen(oled& d);
  void renderNoWadScreen(oled& d);
  void launchDoom(GUIManager& gui);

  static void oledPresentCb(const uint8_t* fb, void* user);
  static void matrixPresentCb(const uint8_t pixels[8], void* user);
  static void vibrateCb(uint32_t duration_ms, uint8_t strength, void* user);
  static void getInputCb(void* user, float* joy_x, float* joy_y, uint8_t* buttons);
};

#endif // BADGE_HAS_DOOM
#endif // DOOM_SCREEN_H
