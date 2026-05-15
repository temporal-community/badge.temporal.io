#pragma once

#include "JoyRamp.h"
#include "ModalScreen.h"
#include "ScheduleData.h"
#include "Screen.h"

// Stash the (day, event index, column) of the schedule cell whose
// detail modal should open next. Called from ScheduleScreen on
// confirm; consumed by ScheduleDetailModalScreen on push.
void prepareScheduleDetailModal(int8_t day, int8_t eventIdx, int8_t column);

// Detail modal for a schedule event — kept inset (rounded-frame
// modal) and equipped with a scrolling title + right-aligned time
// subhead. Shares the ModalScreen base with the section modal so
// chrome / chips / input dispatch / push-fade are uniform.
class ScheduleDetailModalScreen : public ModalScreen {
 public:
  ScreenId id() const override { return kScreenScheduleDetailModal; }

 protected:
  // Inherits the base ModalScreen layout: full-screen with rounded
  // rectangle frame. The chrome helper handles the rounded corners
  // and the title-strip knockouts so this looks identical to the
  // section modal except for the time-range subhead.
  const char* title()   const override;
  const char* subhead() const override;
  void drawBody(oled& d, const OLEDLayout::ModalChrome& chrome) override;

  // BACK + (LOCATE when the room resolves on the venue map and the
  // user isn't already in a filtered room view).
  const char* aChip() const override;
  void onConfirm(GUIManager& gui) override;
};

class ScheduleScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenSchedule; }
  bool showCursor() const override { return false; }
  bool needsRender() override;

 private:
  enum class Mode : uint8_t {
    kDateTabs,
    kEvents,
  };

  void chooseInitialDay();
  void drawHeader(oled& d);
  void drawDateTabs(oled& d);
  void drawEventList(oled& d);
  void drawFilteredView(oled& d);
  int drawEventRow(oled& d, int8_t index, int y, bool showTime);
  void drawDetailModal(oled& d);
  void openDetail();
  void closeDetail();
  void locateSelection(GUIManager& gui);
  void moveDate(int8_t dir);
  void moveEvent(int8_t dir);
  int visibleRows() const;
  bool sameSlot(uint8_t a, uint8_t b) const;
  int8_t slotStart(int8_t index) const;
  // First event index of the next/previous time slot, clamped to bounds.
  int8_t nextSlotStart(int8_t idx) const;
  int8_t prevSlotStart(int8_t idx) const;
  // Pagination: events within a slot are paired 2-by-2 into rows. Each
  // row has a col-0 event and (when the slot has more events) a col-1
  // event. Solo rows have col1Idx == -1.
  int8_t rowOfEvent(int8_t eventIdx) const;
  int8_t pairedCol1(int8_t col0Idx) const;
  int8_t nextRow(int8_t col0Idx) const;
  int8_t prevRow(int8_t col0Idx) const;
  uint8_t countConcurrent(int8_t base_idx) const;
  int8_t  findConcurrent(int8_t base_idx, uint8_t k) const;
  // For column 1 (concurrent column): which event index is currently
  // displayed/selected (or base_idx itself if there are no concurrents).
  int8_t  concurrentEventIndex(int8_t base_idx, uint8_t k) const;
  // Resolve the room name to use for "Locate" — concurrent column shows
  // the concurrent event's room, base column shows the row's own room.
  const char* selectedRoomName(const ScheduleData::SchedEvent& base) const;
  void clampSelection();
  void resetSelectionTicker();
  Mode mode_ = Mode::kDateTabs;
  uint8_t day_ = 0;
  int8_t event_ = 0;
  uint8_t column_ = 0;
  uint8_t talk_ = 0;
  int8_t scroll_ = 0;
  // Filter mode (entered via map's section confirm) — independent
  // cursor / scroll into the filtered events list, so opening the
  // detail modal can stomp event_/day_ with the actual values
  // without disturbing the list-view position.
  int8_t filterCursor_ = 0;
  int8_t filterScroll_ = 0;
  JoyRamp ramp_;

  int8_t prevEvent_ = -1;
  uint8_t prevColumn_ = 0;
  uint32_t selectionMs_ = 0;

};
