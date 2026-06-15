#ifndef SCREENS_SCREEN_H
#define SCREENS_SCREEN_H

#include <Arduino.h>

#include "../ui/OLEDLayout.h"

class Inputs;
class oled;
class GUIManager;

// ─── Screen identifiers ─────────────────────────────────────────────────────

enum ScreenId : uint8_t {
  kScreenNone = 0,
  kScreenBoot,
  kScreenDiagnostics,
  kScreenMainMenu,
  kScreenSchedule,
  kScreenSettings,
  kScreenHaptics,
  kScreenFiles,
  kScreenEditor,
  kScreenHexView,
  kScreenBoop,
  kScreenInputTest,
  kScreenAnimTest,
  kScreenApps,
  kScreenMatrixApps,
  kScreenTests,
  kScreenTextInput,
  kScreenBadgeInfo,
  kScreenContacts,
  kScreenContactDetail,
  kScreenMap,
  kScreenMapSection,
  kScreenMapFloor,
  kScreenSectionModal,
  kScreenScheduleDetailModal,
  kScreenDrawPicker,
  kScreenDraw,
  kScreenDrawStickerPicker,
  kScreenDrawScalePicker,
  kScreenAboutSponsors,
  kScreenAboutCredits,
  kScreenMenuOrder,
  kScreenHelp,
  kScreenWifi,
  kScreenUpdateFirmware,
  kScreenAssetLibrary,
  kScreenAssetDetail,
#ifdef BADGE_HAS_DOOM
  kScreenDoom,
#endif
};

enum class ScreenAccess : uint8_t {
  kAny = 0,
  kPairedOnly,
  kUnpairedOnly,
};

// ─── Screen base class ──────────────────────────────────────────────────────
// All rendering goes through the oled wrapper for driver portability.

class Screen {
 public:
  virtual ~Screen() = default;
  virtual void onEnter(GUIManager& gui) { (void)gui; }
  virtual void onExit(GUIManager& gui) { (void)gui; }
  // Called when this screen returns to the top of the stack because the
  // screen above it was popped. Distinct from `onEnter`, which fires only
  // when the screen is freshly pushed. Default no-op.
  virtual void onResume(GUIManager& gui) { (void)gui; }
  virtual void render(oled& d, GUIManager& gui) = 0;
  virtual void handleInput(const Inputs& inputs, int16_t cursorX,
                           int16_t cursorY, GUIManager& gui) = 0;
  virtual ScreenId id() const = 0;
  virtual bool showCursor() const { return true; }
  virtual ScreenAccess access() const { return ScreenAccess::kPairedOnly; }

  // Default true: GUIManager re-paints every frame. Override to false
  // when nothing has changed since the previous render — used by the
  // QR pairing screen, where re-encoding the bitmap each frame is
  // expensive and the bits change rarely.
  virtual bool needsRender() { return true; }
};

// ─── Scrollable list menu (base for settings, haptics, nav menus, etc.) ─────

class ListMenuScreen : public Screen {
 public:
  ListMenuScreen(ScreenId sid, const char* title);

  virtual uint8_t itemCount() const = 0;
  virtual void formatItem(uint8_t index, char* buf, uint8_t bufSize) const = 0;
  virtual void onItemSelect(uint8_t index, GUIManager& gui) {
    (void)index; (void)gui;
  }
  virtual void onItemAdjust(uint8_t index, int8_t dir, GUIManager& gui) {
    (void)index; (void)dir; (void)gui;
  }
  virtual void onItemFocus(uint8_t index, GUIManager& gui) {
    (void)index; (void)gui;
  }
  virtual void onUpdate(GUIManager& gui) { (void)gui; }
  virtual const char* hintText() const { return nullptr; }

  // When true: semantic confirm = onItemSelect, semantic cancel = popScreen.
  // When false: joystick X = onItemAdjust (settings behavior, default).
  virtual bool navigableItems() const { return false; }

  // When true, the list renders a synthetic "Back" row past the last
  // item and the cursor can navigate to it (A on that row = pop).
  // When false, the back row is hidden from both the render and the
  // cursor; callers are expected to own semantic cancel = pop via the navigable-
  // items path (which is the default). Screens that draw their own
  // fully-custom list body should flip this off so their cursor doesn't march into
  // empty space after the last real row.
  virtual bool hasBackRow() const { return true; }

  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return sid_; }
  bool showCursor() const override { return false; }

 protected:
  ScreenId sid_;
  const char* title_;
  uint8_t cursor_ = 0;
  uint8_t scroll_ = 0;
  uint32_t lastJoyNavMs_ = 0;
  uint32_t lastJoyAdjustMs_ = 0;

  static constexpr uint8_t kContentY = 10;
  static constexpr uint8_t kHintBarY = OLEDLayout::kFooterTopY;
  static constexpr uint8_t kContentHeight = kHintBarY - kContentY;
  static constexpr uint16_t kJoyDeadband = 400;

  // Virtual so subclasses with custom chrome can shrink the list area. The base handleInput uses
  // these through virtual dispatch to compute scroll bookkeeping, so
  // overriding here is enough — no handleInput override needed.
  virtual uint8_t computeRowHeight(oled& d) const;
  virtual uint8_t computeVisibleRows(oled& d) const;
  virtual const char* titleText() const { return title_; }
  virtual void drawItem(oled& d, uint8_t index, uint8_t y,
                        uint8_t rowHeight, bool selected) const;
  virtual void topRightText(char* buf, uint8_t bufSize) const {
    if (buf && bufSize > 0) buf[0] = '\0';
  }
};

#endif  // SCREENS_SCREEN_H
