#include "ScheduleScreen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ScheduleData.h"
#include "../api/DataCache.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/UIFonts.h"
#include "MapData.h"
#include "MapScreens.h"

namespace {
constexpr uint8_t kDateY = 7;
constexpr uint8_t kDateH = 12;
constexpr uint8_t kSepY = kDateY + kDateH;
constexpr uint8_t kListY = kSepY + 2;
constexpr uint8_t kListBottom = OLEDLayout::kFooterTopY;
constexpr uint8_t kRowH = 8;

constexpr uint8_t kColTimeX = 0;
constexpr uint8_t kColTimeW = 22;
constexpr uint8_t kColEventX = 24;
constexpr uint8_t kColEventW = 50;
constexpr uint8_t kColTalkX = 77;
constexpr uint8_t kColTalkW = 51;
constexpr uint8_t kColGutter = 3;

constexpr uint16_t kJoyDeadband = 500;
constexpr uint16_t kEvtScrollSpeedMs = 35;
constexpr uint16_t kEvtScrollDelayMs = 0;   // start scrolling immediately

constexpr uint8_t kArrowRightW = 3;
constexpr uint8_t kArrowRightH = 5;
static const uint8_t kArrowRightBits[] PROGMEM = {
    0x01, 0x03, 0x07, 0x03, 0x01
};
constexpr uint8_t kArrowLeftW = 3;
constexpr uint8_t kArrowLeftH = 5;
static const uint8_t kArrowLeftBits[] PROGMEM = {
    0x04, 0x06, 0x07, 0x06, 0x04
};
constexpr uint8_t kArrowBoxW = kArrowRightW + 4;

// 50% checkerboard fill — pixels where (x + y) is even. Matches the
// dither pattern used by MapScreens::floorDither so the visual texture
// is consistent across the firmware.
void ditherFill(oled& d, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  d.setDrawColor(1);
  for (int dy = 0; dy < h; dy++) {
    const int absY = y + dy;
    const int startOff = ((x + absY) & 1);
    for (int dx = startOff; dx < w; dx += 2) {
      d.drawPixel(x + dx, absY);
    }
  }
}

// Holds at base_x for kEvtScrollDelayMs, then scrolls left until the text
// fully exits the left clip, then re-enters from clip_r and rolls back to
// base_x, then holds. Matches reference firmware scroll_draw_x — so a
// freshly-selected row's title starts visible from its natural left
// position and scrolls left from there, rather than popping in from the
// right edge.
int scrollDrawX(int colX, int clipR, int textW, uint32_t selectionMs) {
  const int baseX     = colX + 2;
  const int scrollOut = textW + 2;
  const int scrollIn  = clipR - baseX;
  const int range     = scrollOut + scrollIn;
  if (range <= 0) return baseX;

  const uint32_t elapsed = millis() - selectionMs;
  const uint32_t period =
      static_cast<uint32_t>(kEvtScrollDelayMs) +
      static_cast<uint32_t>(range) * kEvtScrollSpeedMs;
  const uint32_t phase = elapsed % period;
  if (phase < kEvtScrollDelayMs) return baseX;

  const int tick = static_cast<int>(
      (phase - kEvtScrollDelayMs) / kEvtScrollSpeedMs);
  return (tick < scrollOut) ? baseX - tick : clipR - (tick - scrollOut);
}

const char* monthLabel(uint8_t month) {
  static const char* kMonths[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (month >= 12) return nullptr;
  return kMonths[month];
}
}  // namespace

using namespace ScheduleData;

void ScheduleScreen::onEnter(GUIManager& gui) {
  (void)gui;
  mode_ = Mode::kDateTabs;
  day_ = 0;
  event_ = 0;
  column_ = 0;
  talk_ = 0;
  scroll_ = 0;
  filterCursor_ = 0;
  filterScroll_ = 0;
  ramp_.reset();
  prevEvent_ = -1;
  prevColumn_ = 0;
  selectionMs_ = millis();
  sched_use_full_mode();
  chooseInitialDay();
  // Cache-first: surface whatever we already have on disk before kicking
  // off the network fetch task. If task creation later fails (heap
  // fragmentation / brownout aftermath), the screen still has data and
  // the header just flags it as stale.
  sched_ensure_cache_loaded();
  sched_start_refresh(/*force=*/true);
}

void ScheduleScreen::onExit(GUIManager& gui) {
  (void)gui;
  // Drop any room filter so re-entering from the home menu lands on
  // the standard schedule view instead of a filtered one.
  sched_set_room_filter(nullptr);
}

bool ScheduleScreen::needsRender() {
  return true;
}

void ScheduleScreen::chooseInitialDay() {
  time_t now = time(nullptr);
  if (now <= 1700000000) return;

  struct tm local = {};
  localtime_r(&now, &local);
  const char* month = monthLabel(static_cast<uint8_t>(local.tm_mon));
  if (!month) return;

  char today[SCHED_LABEL_MAX] = {};
  std::snprintf(today, sizeof(today), "%s %d", month, local.tm_mday);
  const SchedDay* days = sched_days();
  for (uint8_t i = 0; i < SCHED_MAX_DAYS; i++) {
    if (days[i].label && std::strcmp(days[i].label, today) == 0) {
      day_ = i;
      return;
    }
  }
}

int ScheduleScreen::visibleRows() const {
  return (kListBottom - kListY) / kRowH;
}

bool ScheduleScreen::sameSlot(uint8_t a, uint8_t b) const {
  const SchedDay& day = sched_days()[day_];
  return day.events[a].start_min == day.events[b].start_min;
}

int8_t ScheduleScreen::slotStart(int8_t index) const {
  int8_t start = index;
  while (start > 0 && sameSlot(start - 1, index)) start--;
  return start;
}

int8_t ScheduleScreen::nextSlotStart(int8_t idx) const {
  if (idx < 0) return 0;
  const SchedDay& day = sched_days()[day_];
  int8_t i = idx + 1;
  while (i < (int8_t)day.count && sameSlot(i, idx)) i++;
  return i;
}

int8_t ScheduleScreen::prevSlotStart(int8_t idx) const {
  const int8_t s = slotStart(idx);
  if (s == 0) return s;
  return slotStart(s - 1);
}

// ── Row pagination helpers ──
//
// Concurrent events at the same start_min are grouped 2-by-2 into rows
// (col 0 + col 1). When a slot has an odd count, the last event of the
// slot becomes a solo row (col-1 omitted, event spans cols 2+3).
//
// rowOfEvent: for any event index, return the col-0 of the row that
//   contains it. (events at slot offset 0,2,4,... are col-0; offset
//   1,3,5,... are col-1 of the same row.)
int8_t ScheduleScreen::rowOfEvent(int8_t eventIdx) const {
  if (eventIdx < 0) return 0;
  const int8_t s = slotStart(eventIdx);
  const int8_t off = eventIdx - s;
  return s + (off & ~1);
}

// Returns the col-1 event index for the row whose col-0 is `col0Idx`,
// or -1 if the row is solo (no concurrent in its pair).
int8_t ScheduleScreen::pairedCol1(int8_t col0Idx) const {
  const SchedDay& day = sched_days()[day_];
  if (col0Idx < 0 || col0Idx >= (int8_t)day.count) return -1;
  if (col0Idx + 1 >= (int8_t)day.count) return -1;
  if (!sameSlot(col0Idx, col0Idx + 1)) return -1;
  return col0Idx + 1;
}

// Next row's col-0 index (or day.count if there is no next row).
int8_t ScheduleScreen::nextRow(int8_t col0Idx) const {
  const SchedDay& day = sched_days()[day_];
  if (col0Idx < 0) return 0;
  const int8_t step = (pairedCol1(col0Idx) >= 0) ? 2 : 1;
  const int8_t next = col0Idx + step;
  return (next < (int8_t)day.count) ? next : (int8_t)day.count;
}

// Previous row's col-0 index (or -1 if `col0Idx` is already the first row).
int8_t ScheduleScreen::prevRow(int8_t col0Idx) const {
  if (col0Idx <= 0) return -1;
  return rowOfEvent(static_cast<int8_t>(col0Idx - 1));
}

// Number of OTHER events in `event_`'s time slot. Powers the column-1
// concurrent-event display and bounds talk_ navigation.
uint8_t ScheduleScreen::countConcurrent(int8_t base_idx) const {
  if (base_idx < 0) return 0;
  const SchedDay& day = sched_days()[day_];
  if (base_idx >= (int8_t)day.count) return 0;
  uint8_t c = 0;
  for (int8_t i = 0; i < (int8_t)day.count; ++i) {
    if (i == base_idx) continue;
    if (day.events[i].start_min == day.events[base_idx].start_min) c++;
  }
  return c;
}

// Returns the 0-based event index of the k-th OTHER event sharing the
// same start_min as base_idx, or -1 if k is out of range.
int8_t ScheduleScreen::findConcurrent(int8_t base_idx, uint8_t k) const {
  if (base_idx < 0) return -1;
  const SchedDay& day = sched_days()[day_];
  if (base_idx >= (int8_t)day.count) return -1;
  uint8_t found = 0;
  for (int8_t i = 0; i < (int8_t)day.count; ++i) {
    if (i == base_idx) continue;
    if (day.events[i].start_min != day.events[base_idx].start_min) continue;
    if (found == k) return i;
    found++;
  }
  return -1;
}

// What does the column-1 cell show for slot k?  When at least one
// concurrent event exists, return that event's index. Otherwise we
// fall back to the base row itself (so the room column stays useful
// even on slots with no parallel sessions).
int8_t ScheduleScreen::concurrentEventIndex(int8_t base_idx, uint8_t k) const {
  const int8_t i = findConcurrent(base_idx, k);
  return (i >= 0) ? i : base_idx;
}

const char* ScheduleScreen::selectedRoomName(const SchedEvent& base) const {
  // Col 0 = the row's col-0 event's room. Col 1 = the row's paired
  // col-1 event's room when present, else falls back to col-0's room.
  if (column_ == 0) {
    return (base.talk_count > 0) ? base.talks[0].title : nullptr;
  }
  const int8_t c1 = pairedCol1(event_);
  if (c1 < 0) {
    return (base.talk_count > 0) ? base.talks[0].title : nullptr;
  }
  const SchedEvent& ce = sched_days()[day_].events[c1];
  return (ce.talk_count > 0) ? ce.talks[0].title : nullptr;
}

void ScheduleScreen::resetSelectionTicker() {
  prevEvent_ = event_;
  prevColumn_ = column_;
  selectionMs_ = millis();
}

void ScheduleScreen::clampSelection() {
  if (day_ >= SCHED_MAX_DAYS) day_ = 0;
  const SchedDay& day = sched_days()[day_];
  if (day.count == 0) {
    event_ = 0;
    talk_ = 0;
    scroll_ = 0;
    column_ = 0;
    return;
  }
  if (event_ < 0) event_ = 0;
  if (event_ >= static_cast<int8_t>(day.count)) {
    event_ = static_cast<int8_t>(day.count) - 1;
  }
  // event_ must always be a paginated row's col-0 index.
  event_ = rowOfEvent(event_);
  if (scroll_ < 0) scroll_ = 0;
  scroll_ = rowOfEvent(scroll_);
  if (scroll_ > event_) scroll_ = event_;
  // Slide scroll_ forward by rows until event_ falls within visible window.
  const int rows = visibleRows();
  int rowsAhead = 0;
  for (int8_t r = scroll_;
       r < event_ && r < static_cast<int8_t>(day.count);
       r = nextRow(r), ++rowsAhead) {}
  if (rowsAhead >= rows) {
    int shift = rowsAhead - (rows - 1);
    while (shift-- > 0 && scroll_ < event_) {
      scroll_ = nextRow(scroll_);
    }
  }
  // column_ snaps to col 0 when the row doesn't have a col-1 cell.
  if (column_ == 1 && pairedCol1(event_) < 0) column_ = 0;
  talk_ = 0;  // no longer used; keep zeroed for any legacy reads
}

void ScheduleScreen::drawHeader(oled& d) {
  // Title shows mode (ALL/MINE) plus a short refresh-status token —
  // SYNC while a fetch is running, OLD when we're showing cache,
  // OFFLINE when we have nothing live to lean on, empty when fresh.
  // The DAY/EVENT mode tag is gone — the user navigates between them
  // freely and the chrome shouldn't grow with the position.
  char title[32];
  const char* status = sched_status_short();
  if (status && status[0]) {
    std::snprintf(title, sizeof(title), "SCHED %s %s",
                  sched_mode_label(), status);
  } else {
    std::snprintf(title, sizeof(title), "SCHED %s",
                  sched_mode_label());
  }
  OLEDLayout::drawStatusHeader(d, title);
}

void ScheduleScreen::drawDateTabs(oled& d) {
  const SchedDay* days = sched_days();
  const int tabW = 128 / SCHED_MAX_DAYS;
  d.setFont(UIFonts::kText);
  const int ascent = d.getAscent();

  for (uint8_t i = 0; i < SCHED_MAX_DAYS; i++) {
    const int tx = i * tabW;
    const int tw = (i == SCHED_MAX_DAYS - 1) ? (128 - tx) : tabW;
    const int rx = tx + 1;
    const int ry = kDateY + 2;   // shifted +1 px down so the rounded
                                  // tabs sit clear of the new header
                                  // chrome (time pill, icons).
    const int rw = tw - 2;
    const int rh = kDateH - 2;

    const bool selected = (i == day_);
    const bool fill = selected &&
        (mode_ == Mode::kDateTabs || ((millis() / 500) % 2 == 0));
    if (fill) {
      d.setDrawColor(1);
      d.drawRBox(rx, ry, rw, rh, 2);
      d.setDrawColor(0);
    } else {
      d.setDrawColor(1);
      d.drawRFrame(rx, ry, rw, rh, 2);
    }

    const char* label = days[i].label;
    const int labelW = d.getStrWidth(label);
    const int ty = ry + (rh + ascent) / 2;
    d.drawStr(tx + (tw - labelW) / 2, ty, label);
    d.setDrawColor(1);
  }
}

// Renders one paginated row — time column + col-0 event + (when not
// solo) col-1 event. The selection frame is drawn afterward by
// drawEventList so it can span vertically across slots.
//
// `index` is the col-0 event index for this row.
int ScheduleScreen::drawEventRow(oled& d, int8_t index, int y,
                                 bool /*showTime*/) {
  if (y + kRowH > kListBottom) return y + kRowH;

  const SchedDay&   dayRef = sched_days()[day_];
  const SchedEvent& col0Event = dayRef.events[index];
  const int8_t      col1Idx   = pairedCol1(index);
  const bool solo      = (col1Idx < 0);
  const int  eventColX = kColEventX;
  const int  eventColW = solo
                            ? (kColEventW + kColGutter + kColTalkW)
                            : kColEventW;

  const bool rowIsSelected =
      (mode_ == Mode::kEvents && index == event_);
  const bool col0Selected = rowIsSelected && (column_ == 0);
  const bool col1Selected = rowIsSelected && (column_ == 1) && !solo;

  d.setFont(UIFonts::kText);
  d.setDrawColor(1);

  // Time column — shown on every paginated row, even when redundant
  // (the user wants the time visible on every line so they can see
  // which slot the row belongs to).
  char timeBuf[6] = {};
  sched_fmt_time(col0Event.start_min, timeBuf);
  d.drawStr(kColTimeX, y + kRowH - 2, timeBuf);

  // Col-0 event cell.
  // Selected rows give the title an extra px of left-side breathing
  // room inside the selector frame so scrolling text doesn't crowd the
  // frame line.
  const int eventBaseX = eventColX + (col0Selected ? 3 : 2);
  const int eventBoxX  = col0Selected
                             ? eventColX + eventColW - 1 - kArrowBoxW
                             : eventColX + eventColW - kArrowBoxW;
  const int eventClipL = col0Selected ? eventColX + 3 : eventColX;
  const int eventClipR = col0Selected ? eventBoxX - 2
                                      : eventColX + eventColW - 1;
  const int eventAvail = eventClipR - eventBaseX;
  const int eventTextW = d.getStrWidth(col0Event.title);
  const bool eventTextFits = (eventTextW <= eventAvail);

  int eventDrawX;
  if (eventTextFits) {
    eventDrawX = solo ? (eventBaseX + (eventAvail - eventTextW) / 2)
                      : eventBaseX;
  } else if (col0Selected) {
    eventDrawX = scrollDrawX(eventColX, eventClipR,
                             eventTextW, selectionMs_);
  } else {
    eventDrawX = eventBaseX;
  }
  d.setClipWindow(eventClipL, y, eventClipR, y + kRowH);
  d.drawStr(eventDrawX, y + kRowH - 2, col0Event.title);
  d.setMaxClipWindow();

  // Dither only for solo rows (centered text in the merged column).
  if (eventTextFits && solo) {
    const int textL = eventDrawX;
    const int textR = eventDrawX + eventTextW;
    ditherFill(d, eventBaseX, y + 1, (textL - 2) - eventBaseX, kRowH - 2);
    ditherFill(d, textR + 2,  y + 1, eventClipR - (textR + 2), kRowH - 2);
  }

  // Col-1 event cell.
  if (!solo) {
    const SchedEvent& col1Event = dayRef.events[col1Idx];
    const int talkBaseX = kColTalkX + (col1Selected ? 3 : 2);
    const int talkBoxX  = col1Selected
                             ? kColTalkX + kColTalkW - 1 - kArrowBoxW
                             : kColTalkX + kColTalkW - kArrowBoxW;
    const int talkClipL = col1Selected ? kColTalkX + 3 : kColTalkX;
    const int talkClipR = col1Selected ? talkBoxX - 2
                                       : kColTalkX + kColTalkW - 1;
    const int talkAvail = talkClipR - talkBaseX;
    const int talkTextW = d.getStrWidth(col1Event.title);
    const bool talkTextFits = (talkTextW <= talkAvail);

    int talkDrawX;
    if (talkTextFits) {
      talkDrawX = talkBaseX;
    } else if (col1Selected) {
      talkDrawX = scrollDrawX(kColTalkX, talkClipR,
                              talkTextW, selectionMs_);
    } else {
      talkDrawX = talkBaseX;
    }
    d.setClipWindow(talkClipL, y, talkClipR, y + kRowH);
    d.drawStr(talkDrawX, y + kRowH - 2, col1Event.title);
    d.setMaxClipWindow();
  }

  return y + kRowH;
}

void ScheduleScreen::drawEventList(oled& d) {
  const SchedDay& day = sched_days()[day_];
  if (day.count == 0) {
    d.setFont(UIFonts::kText);
    d.setDrawColor(1);
    const char* msg = sched_empty_message();
    OLEDLayout::drawStatusBox(d, 9, 25, 110, 22, msg,
                              sched_is_loading() ? "Loading" : nullptr,
                              sched_is_loading());
    OLEDLayout::drawGameFooter(d);
    d.setFont(UIFonts::kText);
    OLEDLayout::drawFooterActions(d, nullptr, nullptr,
                                  mode_ == Mode::kEvents ? "days" : "back",
                                  nullptr);
    return;
  }

  if (event_ != prevEvent_ || column_ != prevColumn_) {
    resetSelectionTicker();
  }

  // event_ and scroll_ are paginated row col-0 indices; defensive
  // normalize so any stale values land on a valid row col-0.
  event_  = rowOfEvent(event_);
  scroll_ = rowOfEvent(scroll_);

  // Render visible rows (paginated). For each rendered row, remember
  // its col-0 index and y so gutter bars and the selection frame can
  // line up with visible content.
  static constexpr int kMaxRows =
      ((kListBottom - kListY) / kRowH) + 1;
  struct VisibleRow { int8_t col0; int y; };
  VisibleRow rows[kMaxRows];
  int rowCount = 0;
  {
    int    yLoop = kListY;
    int8_t ri    = scroll_;
    while (ri < static_cast<int8_t>(day.count) &&
           yLoop + kRowH <= kListBottom) {
      drawEventRow(d, ri, yLoop, /*showTime=*/true);
      if (rowCount < kMaxRows) rows[rowCount++] = {ri, yLoop};
      yLoop += kRowH;
      ri = nextRow(ri);
    }
  }

  auto visibleRowY = [&](int8_t col0) -> int {
    for (int r = 0; r < rowCount; r++) {
      if (rows[r].col0 == col0) return rows[r].y;
    }
    return -1;
  };

  // y where the first row of the slot whose start_min >= endMin
  // begins. If that slot is off-bottom (or doesn't exist), snap to
  // the y just past the last rendered row so the frame bottom stays
  // on the row grid — keeps the end-time label aligned with every
  // other row's time baseline (otherwise the off-grid kListBottom
  // pushes it 1px lower than its neighbours).
  const int offBottomY = (rowCount > 0)
                             ? rows[rowCount - 1].y + kRowH
                             : kListBottom;
  auto endY = [&](uint16_t endMin) -> int {
    for (int8_t s = 0; s < (int8_t)day.count; s = nextSlotStart(s)) {
      if (day.events[s].start_min >= endMin) {
        const int sy = visibleRowY(s);
        return (sy >= 0) ? sy : offBottomY;
      }
    }
    return offBottomY;
  };

  // ── Column dividers ──
  //   gx1 (x=22): between time and event columns. Always drawn full
  //               height for visual consistency; doesn't depend on
  //               which rows have content.
  //   gx2 (x=75): between event and talk columns. Drawn per row, only
  //               when the row has BOTH a col-0 and a col-1 event
  //               (non-solo). Acts as a divider, NOT a duration
  //               indicator.
  d.setDrawColor(1);
  constexpr int gx1 = kColEventX - 2;                                 // 22
  constexpr int gx2 = kColEventX + kColEventW + (kColGutter / 2);     // 75
  d.drawVLine(gx1, kListY, kListBottom - kListY);
  for (int r = 0; r < rowCount; r++) {
    if (pairedCol1(rows[r].col0) >= 0) {
      d.drawVLine(gx2, rows[r].y, kRowH);
    }
  }

  // ── Selection frame: extends vertically for the selected event's duration ──
  if (mode_ == Mode::kEvents) {
    const int8_t selRow  = event_;          // col-0 of the selected row
    const int8_t selCol1 = pairedCol1(selRow);
    int8_t selIdx;
    if (column_ == 1 && selCol1 >= 0) {
      selIdx = selCol1;
    } else {
      selIdx = selRow;
    }
    const int yTop = visibleRowY(selRow);
    if (yTop >= 0) {
      const SchedEvent& selEvent = day.events[selIdx];
      const int yEnd  = endY(selEvent.end_min);
      const int frameH =
          ((yEnd < kListBottom) ? yEnd : kListBottom) - yTop;
      if (frameH > 0) {
        const bool rowIsSolo = (selCol1 < 0);
        const bool useMerged = rowIsSolo;
        const int  baseColX = (column_ == 0 || useMerged) ? kColEventX
                                                          : kColTalkX;
        const int  baseColW = useMerged
                                  ? (kColEventW + kColGutter + kColTalkW)
                                  : (column_ == 0 ? kColEventW : kColTalkW);
        // Selector rectangle is inset by 1 px on each X edge from the
        // raw column, so it doesn't crowd the column dividers.
        const int  colX = baseColX + 1;
        const int  colW = baseColW - 2;
        const int  boxX   = colX + colW - kArrowBoxW;
        const int  arrowX = boxX + (kArrowBoxW - kArrowRightW) / 2;
        // Arrow centred on the entire selector frame, not just the
        // title row — when the frame spans multiple rows the icon
        // sits at the visual middle of the highlighted region.
        const int  arrowY = yTop + (frameH - kArrowRightH) / 2;

        // When the frame spans more than one row, scrub everything
        // inside the selector's column (other events' titles, the
        // gx2 divider, the row's own title at the top, etc.) and
        // re-paint the title vertically centered with dither above
        // AND below. Applies for solo, column-0, and column-1
        // selectors uniformly — colX/colW already scope the wipe to
        // just this selector's column so adjacent columns aren't
        // touched.
        if (frameH > kRowH) {
          d.setDrawColor(0);
          d.drawBox(colX + 1, yTop + 1, colW - 2, frameH - 2);

          // Title band — sized for UIFonts::kText (smallsimple, ~7-8 px
          // tall including descenders) with kTitlePad of clear space
          // above and below so the dither pattern doesn't crowd or
          // clip the glyph box. Was 6 px (sized for a 4x6 font)
          // which clipped the now-larger title text.
          constexpr int kTitleBandH = 8;
          constexpr int kTitlePad   = 2;
          const int titleBandTop = yTop + (frameH - kTitleBandH) / 2;
          const int titleBandBot = titleBandTop + kTitleBandH;
          const int ditherX      = colX + 2;
          const int ditherW      = colW - kArrowBoxW - 3;

          // Dither above the title band — 1 px inset from frame top
          // so a thin black gutter sits between the rounded outline
          // and the dither pattern, plus kTitlePad clearance below.
          const int ditherTopBot = titleBandTop - kTitlePad;
          if (ditherTopBot > yTop + 2) {
            ditherFill(d, ditherX, yTop + 2, ditherW,
                       ditherTopBot - (yTop + 2));
          }
          // Dither below the title band, with the same 1 px inset
          // and matching kTitlePad clearance above.
          const int ditherBotTop = titleBandBot + kTitlePad;
          if (ditherBotTop < yTop + frameH - 2) {
            ditherFill(d, ditherX, ditherBotTop, ditherW,
                       (yTop + frameH - 2) - ditherBotTop);
          }

          // Title text — centered when it fits, otherwise scrolled
          // horizontally inside the content clip using the same
          // selection-relative cadence as the row scroll. Font matches
          // the rest of the schedule (UIFonts::kText) so spans don't
          // visually shrink the title.
          d.setDrawColor(1);
          d.setFont(UIFonts::kText);
          const int contentL = colX + 1;
          const int contentR = colX + colW - kArrowBoxW - 2;
          const int avail = contentR - contentL;
          const int titleW = d.getStrWidth(selEvent.title);
          int textX;
          if (titleW <= avail) {
            textX = contentL + (avail - titleW) / 2;
          } else {
            textX = scrollDrawX(contentL - 2, contentR, titleW,
                                selectionMs_);
          }
          const int textY = titleBandBot - 1;  // small-text baseline
          d.setClipWindow(contentL, titleBandTop, contentR,
                          titleBandBot - 1);
          d.drawStr(textX, textY, selEvent.title);
          d.setMaxClipWindow();
        }

        // Filled tab + corner-pixel knockouts + rounded frame.
        d.setDrawColor(1);
        d.drawBox(boxX, yTop, kArrowBoxW, frameH);

        const int rx = colX + colW - 1;
        d.setDrawColor(0);
        d.drawPixel(rx,     yTop);
        d.drawPixel(rx - 1, yTop);
        d.drawPixel(rx,     yTop + 1);
        d.drawPixel(rx,     yTop + frameH - 1);
        d.drawPixel(rx - 1, yTop + frameH - 1);
        d.drawPixel(rx,     yTop + frameH - 2);

        d.setDrawColor(1);
        d.drawRFrame(colX, yTop, colW, frameH, 2);
        d.setDrawColor(0);
        d.drawXBM(arrowX, arrowY,
                  kArrowRightW, kArrowRightH, kArrowRightBits);
        d.setDrawColor(1);

        // ── End time on bottom row ──
        // When the selector spans more than one row, the bottom row's
        // time column shows the event's end time (stable, no blink).
        // The x,y match the standard time draw so it slots into the
        // last row exactly where a regular start time would appear.
        if (frameH > kRowH) {
          const int lastCellY = yTop + frameH - kRowH;
          char endTimeStr[6] = {};
          sched_fmt_time(selEvent.end_min, endTimeStr);
          d.setDrawColor(0);
          d.drawBox(kColTimeX, lastCellY, kColTimeW, kRowH);
          d.setDrawColor(1);
          d.setFont(UIFonts::kText);
          d.drawStr(kColTimeX, lastCellY + kRowH - 2, endTimeStr);
        }
      }
    }
  }

  d.setDrawColor(1);
  // Per-row gx2 dividers were drawn earlier (line 651-656) and only
  // for paired rows — solo / merged-column events skip the divider
  // so it doesn't slice through the event title. The previous full-
  // height redraw here re-painted the divider over those exact
  // spans, undoing the per-row gating; removed.
  OLEDLayout::drawGameFooter(d);
  d.setFont(UIFonts::kText);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr,
                                mode_ == Mode::kEvents ? "DAYS" : "BACK",
                                mode_ == Mode::kDateTabs ? "EVENTS" : "INFO");
}

// Detail modal lives in ScheduleDetailModalScreen — a pushed Screen
// (full chrome / chips / push-fade come from ModalScreen). The
// kept-here drawDetailModal stub is just so the existing render
// callsite continues to compile while the rest of this file is
// migrated; the body resolves to a no-op now.
void ScheduleScreen::drawDetailModal(oled& /*d*/) {}

void ScheduleScreen::render(oled& d, GUIManager& gui) {
  (void)gui;
  d.setTextWrap(false);
  d.setDrawColor(1);

  // Hidden room-filter mode (set by the map's section-confirm action).
  // Renders a stripped-down "Events in <room>" list — header is the
  // section name, time column shows "May X" instead of clock time,
  // and the date-tabs / mode-toggle chrome is suppressed. The detail
  // modal still pops on confirm — drawDetailModal sees through
  // sched_filter_active() and replaces "← BACK | LOCATE →" with a
  // single ← BACK cell.
  if (sched_filter_active()) {
    drawFilteredView(d);
    return;
  }

  clampSelection();
  drawHeader(d);
  drawDateTabs(d);
  drawEventList(d);
}

namespace {
// Strip an optional "The " prefix and uppercase. Mirrors the helper
// in MapScreens.cpp; copied locally so ScheduleScreen doesn't need
// to depend on MapScreens internals.
void uppercaseSectionTitle(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!in) return;
  if (std::strncmp(in, "The ", 4) == 0) in += 4;
  size_t i = 0;
  while (*in && i + 1 < cap) {
    char c = *in++;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    out[i++] = c;
  }
  out[i] = '\0';
}
}  // namespace

// Collect (day, event_idx) of every event matching the room filter.
// Used by both the filtered list render and the confirm/locate path.
namespace {
constexpr int kFilterMaxRefs = SCHED_MAX_DAYS * SCHED_MAX_EVENTS;
struct FilterRef { uint8_t day; uint8_t idx; };
int collectFilteredRefs(const char* room, FilterRef* out, int cap) {
  if (!room || !room[0]) return 0;
  const SchedDay* days = sched_days();
  if (!days) return 0;
  int n = 0;
  for (uint8_t d_idx = 0; d_idx < SCHED_MAX_DAYS && n < cap; d_idx++) {
    const SchedDay& day = days[d_idx];
    for (uint8_t i = 0; i < day.count && n < cap; i++) {
      const SchedEvent& ev = day.events[i];
      if (ev.talk_count > 0 && ev.talks[0].title &&
          std::strcmp(ev.talks[0].title, room) == 0) {
        out[n++] = {d_idx, i};
      }
    }
  }
  return n;
}
}  // namespace

void ScheduleScreen::drawFilteredView(oled& d) {
  const char* room = sched_room_filter();

  // Header — uppercased section name.
  char headerTitle[24];
  uppercaseSectionTitle(room, headerTitle, sizeof(headerTitle));
  OLEDLayout::drawStatusHeader(d, headerTitle[0] ? headerTitle : "ROOM");

  // "Event-selector area" replacement: where the date tabs would be,
  // a centered "Events in <room>" pill — 5×7 font, inverted text on
  // a filled rounded rect spanning the date-row height.
  d.setFont(u8g2_font_5x7_tf);
  char caption[48];
  std::snprintf(caption, sizeof(caption), "Events in %s",
                room && room[0] ? room : "?");
  const int captionW = d.getStrWidth(caption);
  const int boxH = kDateH - 2;                // 10 (matches date tabs)
  const int boxW = captionW + 8;              // 4 px padding each side
  const int boxX = (128 - boxW) / 2;
  const int boxY = kDateY + 2;
  d.setDrawColor(1);
  d.drawRBox(boxX, boxY, boxW, boxH, 2);
  d.setDrawColor(0);
  d.drawStr(boxX + 4, boxY + boxH - 2, caption);
  d.setDrawColor(1);

  // Collect filtered events across all days.
  FilterRef refs[kFilterMaxRefs];
  const int total = collectFilteredRefs(room, refs, kFilterMaxRefs);
  const SchedDay* days = sched_days();

  if (total == 0) {
    const char* msg = "No events at this room.";
    const int tw = d.getStrWidth(msg);
    d.drawStr((128 - tw) / 2, kListY + 12, msg);
    OLEDLayout::drawGameFooter(d);
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", nullptr);
    return;
  }

  // Clamp filterCursor_ + filterScroll_.
  if (filterCursor_ < 0) filterCursor_ = 0;
  if (filterCursor_ >= total) filterCursor_ = total - 1;
  const int rows = visibleRows();
  if (filterCursor_ < filterScroll_) filterScroll_ = filterCursor_;
  if (filterCursor_ >= filterScroll_ + rows) {
    filterScroll_ = filterCursor_ - rows + 1;
  }
  if (filterScroll_ < 0) filterScroll_ = 0;

  // Reset selection scroll-text ticker on cursor change so the title
  // marquee restarts from its starting position.
  if (filterCursor_ != prevEvent_) {
    prevEvent_ = filterCursor_;
    prevColumn_ = 0;
    selectionMs_ = millis();
  }

  // Always-on column dividers (matches drawEventList's chrome).
  d.setDrawColor(1);
  constexpr int gx1 = kColEventX - 2;            // 22
  d.drawVLine(gx1, kListY, kListBottom - kListY);

  // Render rows. Col 1 = day label, col 2 = event title (selection
  // scroll), col 3 = talk title (= room, kept for layout symmetry).
  d.setFont(u8g2_font_4x6_tf);
  for (int row = 0; row < rows; row++) {
    const int idx = filterScroll_ + row;
    if (idx >= total) break;
    const FilterRef& ref = refs[idx];
    const SchedEvent& ev = days[ref.day].events[ref.idx];
    const int y = kListY + row * kRowH;
    const bool sel = (idx == filterCursor_);

    // Col 1 — day label ("May X").
    d.setDrawColor(1);
    d.drawStr(kColTimeX, y + kRowH - 2,
              days[ref.day].label ? days[ref.day].label : "");

    // Col 2 + 3 merged — event title spans the full event+talk area
    // (no col-3 talk rendering since the room is implicit when we're
    // already filtered to a single room).
    const int eventColX = kColEventX;
    const int eventColW = kColEventW + kColGutter + kColTalkW;
    const int eventBaseX = eventColX + (sel ? 3 : 2);
    const int eventBoxX  = sel ? eventColX + eventColW - 1 - kArrowBoxW
                               : eventColX + eventColW - kArrowBoxW;
    const int eventClipL = sel ? eventColX + 3 : eventColX;
    const int eventClipR = sel ? eventBoxX - 2 : eventColX + eventColW - 1;
    const int eventAvail = eventClipR - eventBaseX;
    const int eventTextW = ev.title ? d.getStrWidth(ev.title) : 0;
    const bool eventTextFits = (eventTextW <= eventAvail);
    int eventDrawX = eventBaseX;
    if (sel && !eventTextFits) {
      eventDrawX = scrollDrawX(eventColX, eventClipR, eventTextW,
                               selectionMs_);
    }
    d.setClipWindow(eventClipL, y, eventClipR, y + kRowH);
    d.drawStr(eventDrawX, y + kRowH - 2, ev.title ? ev.title : "");
    d.setMaxClipWindow();

    // Selection chrome — rounded frame + arrow tab matching the
    // schedule's regular selector style.
    if (sel) {
      const int colX = eventColX + 1;
      const int colW = eventColW - 2;
      d.setDrawColor(1);
      d.drawBox(eventBoxX, y, kArrowBoxW, kRowH);
      const int rx = colX + colW - 1;
      d.setDrawColor(0);
      d.drawPixel(rx, y);
      d.drawPixel(rx - 1, y);
      d.drawPixel(rx, y + 1);
      d.drawPixel(rx, y + kRowH - 1);
      d.drawPixel(rx - 1, y + kRowH - 1);
      d.drawPixel(rx, y + kRowH - 2);
      d.setDrawColor(1);
      d.drawRFrame(colX, y, colW, kRowH, 2);
      const int arrowX = eventBoxX + (kArrowBoxW - kArrowRightW) / 2;
      const int arrowY = y + (kRowH - kArrowRightH) / 2;
      d.setDrawColor(0);
      d.drawXBM(arrowX, arrowY, kArrowRightW, kArrowRightH,
                kArrowRightBits);
      d.setDrawColor(1);
    }
  }

  // Footer — same chrome as the regular schedule (action rule +
  // back-arrow + a "details" hint on the right when an event is
  // selected, mirroring the drawEventList footer).
  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "detail");
}


// openDetail / closeDetail / locateSelection retired — the detail
// modal is now ScheduleDetailModalScreen (a pushed Screen). See the
// implementation at the bottom of this file. The stubs below are
// kept so the header's declarations don't need a separate cleanup
// pass; remove from ScheduleScreen.h once all call-sites are gone.
void ScheduleScreen::openDetail() {}
void ScheduleScreen::closeDetail() {}
void ScheduleScreen::locateSelection(GUIManager& /*gui*/) {}

void ScheduleScreen::moveDate(int8_t dir) {
  if (dir > 0 && day_ < SCHED_MAX_DAYS - 1) {
    day_++;
  } else if (dir < 0 && day_ > 0) {
    day_--;
  } else {
    return;
  }
  event_ = 0;
  column_ = 0;
  talk_ = 0;
  scroll_ = 0;
  resetSelectionTicker();
}

void ScheduleScreen::moveEvent(int8_t dir) {
  const SchedDay& day = sched_days()[day_];
  if (day.count == 0) return;

  // event_ is the col-0 of the currently-selected paginated row. Navigate
  // one row at a time (across pair boundaries within a slot, and across
  // slot boundaries when the current slot's last pair is reached).
  const int8_t curRow = rowOfEvent(event_);

  if (dir > 0) {
    // Always advance one paginated row at a time, regardless of how
    // long the current event is. Long events (Birds of a Feather,
    // Beginner Bootcamp) span the selector visually across many rows,
    // but the user should still be able to step into the parallel
    // events that share that time window — earlier behavior here
    // skipped past every slot the long event "covered" and landed on
    // the day's last event instead.
    const int8_t target = nextRow(curRow);

    if (target >= 0 && target < static_cast<int8_t>(day.count)) {
      event_ = target;
      if (column_ == 1 && pairedCol1(event_) < 0) column_ = 0;
      const int rows = visibleRows();
      int rowsAhead = 0;
      for (int8_t r = scroll_;
           r < event_ && r < static_cast<int8_t>(day.count);
           r = nextRow(r), ++rowsAhead) {}
      if (rowsAhead >= rows) {
        const int shift = rowsAhead - (rows - 1);
        for (int k = 0; k < shift && scroll_ < event_; ++k) {
          scroll_ = nextRow(scroll_);
        }
      }
    }
  } else if (dir < 0) {
    const int8_t prev = prevRow(curRow);
    if (prev >= 0) {
      event_ = prev;
      if (column_ == 1 && pairedCol1(event_) < 0) column_ = 0;
      if (event_ < scroll_) scroll_ = event_;
    } else {
      mode_ = Mode::kDateTabs;
      column_ = 0;
    }
  }
}

void ScheduleScreen::handleInput(const Inputs& inputs, int16_t /*cursorX*/,
                                 int16_t /*cursorY*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  // Filter mode (entered via the map's section confirm).
  if (sched_filter_active()) {
    if (e.cancelPressed) {
      gui.popScreen();
      return;
    }
    if (e.confirmPressed) {
      // Resolve the filter cursor to the actual event so the detail
      // modal can render via the standard day_/event_ path. Filter
      // events are single-column (no concurrent column).
      FilterRef refs[kFilterMaxRefs];
      const int total = collectFilteredRefs(sched_room_filter(),
                                            refs, kFilterMaxRefs);
      if (filterCursor_ >= 0 && filterCursor_ < total) {
        day_ = refs[filterCursor_].day;
        event_ = refs[filterCursor_].idx;
        column_ = 0;
        prepareScheduleDetailModal(day_, event_, column_);
        gui.pushScreen(kScreenScheduleDetailModal);
      }
      return;
    }

    const int16_t dx = static_cast<int16_t>(inputs.joyX()) - 2047;
    const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
    int8_t dir = 0;
    if (abs(dy) >= abs(dx)) {
      if (dy >  kJoyDeadband) dir = 2;
      if (dy < -kJoyDeadband) dir = -2;
    }
    if (dir == 0) {
      ramp_.reset();
      return;
    }
    if (!ramp_.tick(dir, millis())) return;
    if (dir == 2)  filterCursor_++;
    if (dir == -2) filterCursor_--;
    return;
  }

  if (e.cancelPressed) {
    if (mode_ == Mode::kEvents) {
      mode_ = Mode::kDateTabs;
      column_ = 0;
      talk_ = 0;
      ramp_.reset();
      Haptics::shortPulse();
      return;
    }
    gui.popScreen();
    return;
  }
  if (e.yPressed) {
    if (sched_toggle_mode()) {
      mode_ = Mode::kDateTabs;
      day_ = 0;
      event_ = 0;
      column_ = 0;
      talk_ = 0;
      scroll_ = 0;
      chooseInitialDay();
      ramp_.reset();
      Haptics::shortPulse();
    }
    return;
  }
  if (e.confirmPressed) {
    if (mode_ == Mode::kDateTabs) {
      mode_ = Mode::kEvents;
      column_ = 0;
      event_ = 0;
      scroll_ = 0;
    } else {
      Haptics::shortPulse();
      prepareScheduleDetailModal(day_, event_, column_);
      gui.pushScreen(kScreenScheduleDetailModal);
    }
    return;
  }

  const int16_t dx = static_cast<int16_t>(inputs.joyX()) - 2047;
  const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
  int8_t dir = 0;
  if (abs(dx) > abs(dy)) {
    if (dx > kJoyDeadband) dir = 1;
    else if (dx < -kJoyDeadband) dir = -1;
  } else {
    if (dy > kJoyDeadband) dir = 2;
    else if (dy < -kJoyDeadband) dir = -2;
  }

  if (mode_ == Mode::kDateTabs) {
    if (dir == 0) {
      ramp_.reset();
      return;
    }
    if (dir == 2) {
      mode_ = Mode::kEvents;
      column_ = 0;
      event_ = 0;
      talk_ = 0;
      scroll_ = 0;
      ramp_.reset();
      return;
    }
    if (!ramp_.tick(dir, millis())) return;
    if (dir == 1 || dir == -1) moveDate(dir);
    return;
  }

  if (dir == 0) {
    ramp_.reset();
    return;
  }

  if (column_ == 0) {
    if (dir == -1) {
      // Already at left column — joystick-left is a no-op until the
      // user explicitly backs out via Cancel.
      ramp_.reset();
      return;
    }
    if (dir == 1) {
      // Enter col 1 when this paginated row actually has one.
      if (pairedCol1(event_) >= 0) {
        column_ = 1;
      }
      ramp_.reset();
      return;
    }
    // Up/down moves between paginated rows in column 0.
    if (!ramp_.tick(dir, millis())) return;
    if (dir == 2)  moveEvent(1);
    if (dir == -2) moveEvent(-1);
    return;
  }

  // column_ == 1 — concurrent column. Joystick-left returns to col 0 of
  // the same row. Joystick up/down moves to the prev/next row,
  // preserving column 1 when the destination row also has one;
  // otherwise snaps to col 0 (handled inside moveEvent).
  if (dir == -1) {
    column_ = 0;
    ramp_.reset();
    return;
  }
  if (dir == 1) {
    ramp_.reset();
    return;
  }
  if (!ramp_.tick(dir, millis())) return;
  if (dir == 2)  moveEvent(1);
  if (dir == -2) moveEvent(-1);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScheduleDetailModalScreen — pushed Screen, ModalScreen subclass.
//  Uses the rounded-frame inset variant (boxX/Y/W/H + useFrame=true)
//  so it visually matches the previous in-place modal: scrolling
//  title + right-aligned time-range subhead + word-wrapped body.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

int8_t s_detail_day      = 0;
int8_t s_detail_event    = -1;
int8_t s_detail_column   = 0;
char   s_detail_subhead[14] = {};

const SchedEvent* detailEvent() {
  if (s_detail_event < 0) return nullptr;
  if (s_detail_day >= SCHED_MAX_DAYS) return nullptr;
  const SchedDay* days = sched_days();
  if (!days) return nullptr;
  const SchedDay& day = days[s_detail_day];
  if (s_detail_event >= static_cast<int8_t>(day.count)) return nullptr;
  return &day.events[s_detail_event];
}

}  // namespace

void prepareScheduleDetailModal(int8_t day, int8_t eventIdx, int8_t column) {
  s_detail_day    = day;
  s_detail_event  = eventIdx;
  s_detail_column = column;
  s_detail_subhead[0] = '\0';
  const SchedEvent* event = detailEvent();
  if (event) {
    char t1[6] = {}, t2[6] = {};
    sched_fmt_time(event->start_min, t1);
    sched_fmt_time(event->end_min,   t2);
    std::snprintf(s_detail_subhead, sizeof(s_detail_subhead),
                  "%s-%s", t1, t2);
  }
}

const char* ScheduleDetailModalScreen::title() const {
  const SchedEvent* event = detailEvent();
  return (event && event->title) ? event->title : "";
}

const char* ScheduleDetailModalScreen::subhead() const {
  return s_detail_subhead[0] ? s_detail_subhead : nullptr;
}

void ScheduleDetailModalScreen::drawBody(oled& d,
                                         const OLEDLayout::ModalChrome& chrome) {
  const SchedEvent* event = detailEvent();
  if (!event || !event->desc || !event->desc[0]) return;

  // Word-wrapped description body in 4×6 (matches the section
  // modal). Word wrap is line-by-line, breaking on space.
  d.setFont(u8g2_font_4x6_tf);
  d.setDrawColor(1);
  constexpr int kDescLineH  = 7;
  constexpr int kDescMargin = 2;
  const int descW = chrome.interiorW - kDescMargin * 2;
  const int descBase1 = chrome.bodyTopY + 6;
  const int descClipBot = chrome.bodyBotY;

  d.setClipWindow(chrome.interiorX, chrome.bodyTopY,
                  chrome.interiorX + chrome.interiorW - 1, descClipBot);
  const char* p = event->desc;
  int y = descBase1;
  char line[64] = "";
  auto flush = [&]() {
    if (line[0]) {
      d.drawStr(chrome.interiorX + kDescMargin, y, line);
      line[0] = '\0';
      y += kDescLineH;
    }
  };
  while (*p && y <= descClipBot) {
    if (*p == '\n') { flush(); p++; continue; }
    const char* wend = p;
    while (*wend && *wend != ' ' && *wend != '\n') ++wend;
    int wlen = static_cast<int>(wend - p);
    if (wlen <= 0) { ++p; continue; }
    if (wlen > 63) wlen = 63;
    char word[64];
    std::strncpy(word, p, wlen);
    word[wlen] = '\0';
    char test[128];
    if (line[0]) std::snprintf(test, sizeof(test), "%s %s", line, word);
    else         std::strncpy(test, word, sizeof(test) - 1);
    test[sizeof(test) - 1] = '\0';
    if (line[0] && d.getStrWidth(test) > descW) {
      flush();
      std::strncpy(line, word, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
    } else {
      std::strncpy(line, test, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
    }
    p = (*wend == ' ') ? wend + 1 : wend;
  }
  if (y <= descClipBot) flush();
  d.setMaxClipWindow();
}

const char* ScheduleDetailModalScreen::aChip() const {
  // Locate is hidden in filter mode (already on the room's events
  // list — nowhere to "go") and when the room doesn't resolve on
  // the venue map.
  const SchedEvent* event = detailEvent();
  if (!event) return nullptr;
  if (event->talk_count == 0 || !event->talks[0].title ||
      !event->talks[0].title[0]) {
    return nullptr;
  }
  if (sched_filter_active()) return nullptr;
  int floorIdx = -1;
  if (!MapData::findLocation(event->talks[0].title, &floorIdx, nullptr)) {
    return nullptr;
  }
  return "locate";
}

void ScheduleDetailModalScreen::onConfirm(GUIManager& gui) {
  const SchedEvent* event = detailEvent();
  if (!event) return;
  if (event->talk_count == 0 || !event->talks[0].title ||
      !event->talks[0].title[0]) {
    return;
  }
  int floor_idx = -1;
  int section_idx = -1;
  if (!MapData::findLocation(event->talks[0].title, &floor_idx,
                             &section_idx)) {
    return;
  }
  mapSetTargetFloor(floor_idx);
  mapSetTargetSection(section_idx);
  Haptics::shortPulse();
  // Pop the modal first so cancel-back from the map lands on the
  // schedule list, not back on the modal.
  gui.popScreen();
  gui.pushScreen(kScreenMapSection);
}
