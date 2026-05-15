#pragma once

#include "JoyRamp.h"
#include "ModalScreen.h"
#include "Screen.h"

// Three-screen map flow ported from Replay-26-Badge_QAFW/Main-Firmware:
//
//   MapScreen           — floor selector (4 stacked parallelograms)
//   MapSectionScreen    — vector polygon floor plan with section picker
//   MapFloorScreen      — room list for the selected floor
//
// Push order is MapScreen → MapSectionScreen → MapFloorScreen. Cancel
// pops back up the stack. Cursor display is suppressed (joystick + face
// buttons drive the UI).

class MapScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenMap; }
  bool showCursor() const override { return false; }

 private:
  int8_t   selectedVis_ = 0;   // 0 = top label (highest floor), kFloorCount-1 = bottom
  JoyRamp  ramp_;
  uint32_t lastBleHeartbeatMs_ = 0;
  uint32_t lastBleAdvCount_    = 0;
  uint32_t enteredMs_          = 0;  // for "Locating..." → "offline" grace period
};

// Forward selector helper used by MapScreen/MapSectionScreen so they can
// hand the next screen the floor index before push.
void mapSetTargetFloor(int floor_idx);

// Optional: pre-select the section that MapSectionScreen highlights on
// entry. Pass -1 to default to the first section. Used by the schedule
// detail modal's Locate action so the map opens already pointing at the
// event's room.
void mapSetTargetSection(int section_idx);

// MapSectionScreen renders the 16×8 floor overview. Confirm on a
// selected section pushes the SectionModalScreen below — a full-
// screen "modal" rendered as a real Screen so it picks up the
// GUIManager's contrast-fade transition on push/pop.
class MapSectionScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenMapSection; }
  bool showCursor() const override { return false; }

 private:
  int8_t  selected_ = 0;
  JoyRamp ramp_;
};

// Stash the (floor_idx, section_idx, label) the next pushed
// SectionModalScreen should display. Called by MapSectionScreen
// before pushing kScreenSectionModal. Label is copied internally so
// the caller's buffer can be transient.
void prepareSectionModal(int floor_idx, int section_idx, const char* label);

// Full-screen modal that surfaces a single section's description (or
// event-count fallback). Subclass of ModalScreen — chrome / chips /
// input dispatch / push-fade all live in the base. Defaults
// (full-screen, unframed) match the base, so this class only
// declares the bits unique to the section modal.
class SectionModalScreen : public ModalScreen {
 public:
  ScreenId id() const override { return kScreenSectionModal; }

 protected:
  const char* title() const override;
  void drawBody(oled& d, const OLEDLayout::ModalChrome& chrome) override;
  const char* yChip() const override { return "locate"; }
  const char* aChip() const override { return "events"; }
  void onConfirm(GUIManager& gui) override;
};

// MapFloorScreen now lists schedule events whose room matches the
// section selected in MapSectionScreen — a "what's happening here"
// drill-down rather than a static room directory.
class MapFloorScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenMapFloor; }
  bool showCursor() const override { return false; }

 private:
  int8_t  cursor_ = 0;
  int8_t  scroll_ = 0;
  JoyRamp ramp_;
};
