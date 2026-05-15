#ifndef GUI_H
#define GUI_H

#include <Arduino.h>
#include "../infra/Scheduler.h"
#include "screens/Screen.h"

class Inputs;
class oled;
class Config;
class GUIManager;

extern bool gGuiActive;

// All Screen subclasses now live under firmware/src/screens/. See the
// individual headers (e.g. screens/BoopScreen.h, screens/ScheduleScreen.h)
// for declarations; this file no longer hosts any of them.

// ─── GUI manager service ────────────────────────────────────────────────────

class GUIManager : public IService {
 public:
  // Bumped from 16 after the screen table hit the silent-drop path
  // on the old limit (registerScreen stops registering past this cap). Give
  // ourselves enough slack for the handful of screens the current roadmap
  // adds without hitting it again.
  static constexpr uint8_t kMaxScreens = 40;
  static constexpr uint8_t kMaxStack = 6;

  void begin(oled* display, Inputs* inputs);

  void service() override;
  const char* name() const override { return "GUI"; }

  void handleInputIfActive();

  void pushScreen(ScreenId sid);
  void popScreen();
  // Pop N screens in a single fade transition. Used when a child
  // screen's cancel should also dismiss its parent. Bounded so the stack
  // always keeps at least one screen.
  void popScreens(uint8_t n);
  void replaceScreen(ScreenId sid);
  void resetStackTo(ScreenId sid);
  Screen* currentScreen();

  void registerScreen(Screen* screen);

  float cursorX() const { return cursorX_; }
  float cursorY() const { return cursorY_; }

  oled& oledDisplay() { return *oled_; }
  const Inputs& inputs() const { return *inputs_; }

  bool isActive() const { return active_; }
  void activate();
  void activateKeepStack();
  void deactivate();

  void bindConfig(Config* cfg) { config_ = cfg; }
  void requestRender();

  // Badge-flipped-upside-down overlay. Main loop calls this from its
  // IMU flip handler: true when the badge is inverted, false when
  // upright. While inverted, a full-screen nametag covers most screens
  // until the user presses any face button; the press dismisses the
  // overlay and kicks a 30 s idle timer. After 30 s with no further
  // button presses, the overlay re-appears. Upright always disables it.
  // Some screens, like Boop, suppress the nametag while preserving input
  // orientation.
  void setNametagMode(bool inverted);

 private:
  void updateCursor();
  void drawXorCursor();
  void checkActivation(uint32_t nowMs);
  Screen* findScreen(ScreenId sid);
  bool nametagSuppressedForCurrentScreen() const;
  void renderNametagOverlay(oled& d);
  ScreenId homeScreenForBadgeState() const;
  ScreenId resolveScreenForBadgeState(ScreenId sid);
  bool screenAllowedForBadgeState(const Screen* screen) const;
  void syncRouteForBadgeState();

  enum class DisplayPolicy : uint8_t {
    kNormal = 0,
    kNametag,
  };
  void applyDisplayPolicy(DisplayPolicy policy);

  // Render the current screen once into the OLED buffer and push it.
  // Used by the screen-transition helpers so the new content lands on
  // the panel before the contrast fade ramps it back up.
  void renderCurrentToPanel();

  // Hardware fade transition gate. True after begin() finishes, so the
  // initial home-screen push doesn't fade in from black on every boot.
  void enterTransitionOut();
  void enterTransitionIn();

  oled* oled_ = nullptr;
  Inputs* inputs_ = nullptr;
  Config* config_ = nullptr;
  bool active_ = false;

  Screen* screens_[kMaxScreens] = {};
  uint8_t screenCount_ = 0;

  ScreenId stack_[kMaxStack] = {};
  uint8_t stackDepth_ = 0;

  float cursorX_ = 64.0f;
  float cursorY_ = 32.0f;

  static constexpr uint32_t kRenderIntervalMs = 33;
  static constexpr uint32_t kActivateHoldMs = 1000;

  uint32_t lastRenderMs_ = 0;
  uint32_t activateHoldStartMs_ = 0;
  uint8_t routedBadgeState_ = 0xFF;
  bool renderRequested_ = true;
  DisplayPolicy displayPolicy_ = DisplayPolicy::kNormal;
  bool displayPolicyApplied_ = false;

  // Nametag-overlay state machine (see setNametagMode). kDisabled means
  // the badge is upright and the overlay never shows. kShown paints a
  // full-screen nametag on top of the current screen. kHidden keeps the
  // GUI visible after the user has dismissed the overlay and runs the
  // idle timer that re-shows it. kPendingEnter is a short guard interval
  // after entering inverted orientation before the overlay appears.
  enum class NametagMode : uint8_t {
    kDisabled     = 0,
    kShown        = 1,
    kHidden       = 2,
    kPendingEnter = 3,
  };
  bool nametagInverted_ = false;
  NametagMode nametagMode_ = NametagMode::kDisabled;
  uint32_t nametagPendingSinceMs_ = 0;
  uint32_t nametagLastInputMs_ = 0;
  static constexpr uint32_t kNametagEnterDelayMs = 500;
  static constexpr uint32_t kNametagIdleMs = 30000;
};

#endif
