#include "MapScreens.h"

#include <Arduino.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "AboutSponsors.h"
#include "BoothSponsors.h"
#include "MapData.h"
#include "ScheduleData.h"
#include "../api/DataCache.h"
#include "../api/MsgPackReader.h"
#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../ble/BleBeaconScanner.h"
#endif
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/StarIcon.h"

#include <time.h>

using MapData::MAP_FLOORS;
using MapData::MAP_FLOOR_VECS;
using MapData::MapEdge;
using MapData::MapFloor;
using MapData::MapFloorVec;
using MapData::MapSection;
using MapData::MapSectionVec;
using MapData::MapSubsection;

namespace {

// ── Cross-screen state ────────────────────────────────────────────────────
//
// MapScreen pushes MapSectionScreen, MapSectionScreen pushes MapFloorScreen.
// Each downstream screen needs to know the floor index that was selected
// upstream; the reference firmware passes this via small free functions
// (scr_map_section_set / scr_map_floor_set). We mirror that pattern with
// a single static so we don't have to thread the value through GUIManager.
int s_target_floor   = 0;
// -1 means "use the section screen's default (first section)". Set via
// mapSetTargetSection() before pushing kScreenMapSection — typically
// from the schedule's Locate action so the map opens already centered
// on the event's room. Consumed once by MapSectionScreen::onEnter and
// cleared so subsequent navigation behaves normally.
int s_target_section = -1;

constexpr uint16_t kJoyDeadband = 500;
constexpr int kHeaderH    = 7;       // y=0..6 reserved for screen title
constexpr int kContentTop = kHeaderH;
constexpr int kContentBot = OLEDLayout::kFooterTopY - 1;
constexpr int kContentXMax = 127;

// Standard 3×5 arrow XBMs (mirror ScheduleScreen's). Right-pointing
// for the forward-action cue (floor selector / "drill in" footer);
// left-pointing for the back-action cue on the events drill-down.
constexpr uint8_t kArrowRightW = 3;
constexpr uint8_t kArrowRightH = 5;
const uint8_t kArrowRightBits[] PROGMEM = {
    0x01, 0x03, 0x07, 0x03, 0x01
};
constexpr uint8_t kArrowLeftW = 3;
constexpr uint8_t kArrowLeftH = 5;
const uint8_t kArrowLeftBits[] PROGMEM = {
    0x04, 0x06, 0x07, 0x06, 0x04
};

struct BadgeLocationFix {
  bool hasFix;
  int floor;
  int section;
  uint16_t roomUid;
};

BadgeLocationFix currentBadgeLocation() {
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  const uint16_t roomUid = BleBeaconScanner::bestRoomUid();
  return {
      BleBeaconScanner::hasFix(),
      BleBeaconScanner::floorForUid(roomUid),
      BleBeaconScanner::sectionForUid(roomUid),
      roomUid,
  };
#else
  return {false, -1, -1, 0};
#endif
}

// 2-px-thick line by 4-line "brush": draws the segment at four
// offsets {(0,0), (1,0), (0,1), (1,1)}. Works in any direction —
// previous heuristic-perpendicular-offset version produced gappy
// 45° diagonals because the offset axis didn't actually thicken
// the line perpendicular to its travel.
void drawThickLine(oled& d, int x0, int y0, int x1, int y1) {
  d.drawLine(x0,     y0,     x1,     y1);
  d.drawLine(x0 + 1, y0,     x1 + 1, y1);
  d.drawLine(x0,     y0 + 1, x1,     y1 + 1);
  d.drawLine(x0 + 1, y0 + 1, x1 + 1, y1 + 1);
}

// 50% checkerboard fill — same dither cadence as the schedule
// selector. Local copy because ScheduleScreen's helper is file-static.
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

// Dotted rectangle outline with implicit 1-px rounded corners (corner
// pixels skipped). Every other pixel along each edge — visually softer
// than drawRFrame so the section markers don't compete with the floor
// plan's solid outline.
void drawDottedRRect(oled& d, int x, int y, int w, int h) {
  d.setDrawColor(1);
  // Top + bottom edges, skipping the 1-px corner on each end.
  for (int dx = 1; dx <= w - 2; dx += 2) {
    d.drawPixel(x + dx, y);
    d.drawPixel(x + dx, y + h - 1);
  }
  // Left + right edges, skipping the 1-px corner on each end.
  for (int dy = 1; dy <= h - 2; dy += 2) {
    d.drawPixel(x, y + dy);
    d.drawPixel(x + w - 1, y + dy);
  }
}

// Draws the UPPER half of a circle outline (curve above the centre).
// Used as a half-disc bump on the top edge of the scalable map's
// outer rect — the chord (diameter) coincides with the rect's top
// edge; the arc protrudes above it. Midpoint algorithm; 4 octants
// cover the full upper semicircle.
void drawHalfCircleUpper(oled& d, int cx, int cy, int r) {
  int x = 0, y = r;
  int err = 1 - r;
  d.setDrawColor(1);
  while (x <= y) {
    d.drawPixel(cx + x, cy - y);
    d.drawPixel(cx - x, cy - y);
    d.drawPixel(cx + y, cy - x);
    d.drawPixel(cx - y, cy - x);
    x++;
    if (err < 0) {
      err += 2 * x + 1;
    } else {
      y--;
      err += 2 * (x - y) + 1;
    }
  }
}

// 2-px-thick rounded frame: drawRFrame twice, the second one inset
// by 1 px on every side so we get a uniform 2-pixel wall regardless
// of corner radius.
void drawThickRFrame(oled& d, int x, int y, int w, int h, int r) {
  d.setDrawColor(1);
  d.drawRFrame(x, y, w, h, r);
  if (w > 2 && h > 2) {
    d.drawRFrame(x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0);
  }
}

// Forward declaration — drawFooter is defined further down in this
// anonymous namespace but used by the grid renderers below.
void drawFooter(oled& d, const char* msg);

// ── Universal 16×8 floor grid ────────────────────────────────────────────
//
// Each non-special floor (Concourse, Mezzanine, Level 02, Level 03)
// expresses its sections as cell ranges in a fixed 16-col × 8-row
// grid. MapSectionScreen renders the grid in two modes:
//
//   • Overview (default): grid scales to fit the entire content area,
//     section bounding boxes only — no in-rect labels — with the
//     selected section's name shown in the footer.
//   • Focused: grid scales up so multi-cell sections can host their
//     full text label inside; view pans to keep the selected section
//     anchored on the screen centre.
//
// Confirm in Overview promotes to Focused; confirm in Focused pushes
// the schedule with the room filter set. Cancel reverses.
constexpr int kGridCols = 16;
constexpr int kGridRows = 8;

struct GridSection {
  uint8_t col;
  uint8_t row;
  uint8_t cw;
  uint8_t ch;
  // Visual sub-cell nudge in half-cell-step units. Positive = right /
  // down. Renderer applies these as a pixel shift; navigation logic
  // ignores them, so adjacency stays driven by the integer (col, row)
  // grid even when a section is visually offset.
  int8_t  halfX = 0;
  int8_t  halfY = 0;
};

// Floor 0 — Level 00. Simple 2-section layout. Order matches
// f0v_sections / floors.md (Hangar first, Swag/Lunch second).
constexpr GridSection kLevel00Grid[] = {
    {8, 4, 4, 4},  // 0 The Hangar — shifted +2 cols right, +1 ch tall;
                   // row pulled up to 4 so the new ch=4 still fits in
                   // the 8-row grid.
    {7, 0, 3, 3},  // 1 Swag / Lunch — top, 3×3
};

// Floor 1 — Concourse. Latest nudge: Gallery widened to 3 cw so it
// reaches the right outline, Sponsors B slid one cell left, Help
// Desk now offset half-cell right AND half-cell down so it floats
// off the grid lines around it.
constexpr GridSection kConcourseGrid[] = {
    { 1, 1, 3, 2},          // 0 Persist
    { 2, 3, 4, 2},          // 1 Ground Control
    { 6, 1, 2, 2},          // 2 Sponsors A
    { 7, 6, 2, 2, 1, 0},    // 3 Sponsors B  (-1 col, +½ col offset)
    { 9, 1, 2, 2},          // 4 Sponsors C
    {12, 4, 3, 3},          // 5 Debarkment
    { 9, 4, 1, 2, 1, 1},    // 6 Help Desk   (+½ col, +½ row)
    {13, 1, 3, 2},          // 7 The Gallery (+1 cw)
};

// Floor 2 — Mezzanine. The original Mezzanine layout was 4-col ×
// 8-row portrait; here it's rotated 90° CW into an 8-col × 4-row
// landscape and centred in the 16×8 grid (cols 4–11, rows 2–5).
// Treated like the other floors — same uniform cell metrics — the
// "rotation" lives in the data, not in the renderer. The original
// adjacency is preserved (Expanse anchors the right edge; Hyperion +
// Persenne tile the left half top/bottom; Elara + Zenith tile the
// mid-right top/bottom).
constexpr GridSection kMezzanineGrid[] = {
    {11, 2, 1, 4},  // 0 The Expanse — right edge column-strip
    { 8, 2, 3, 2},  // 1 Elara — top mid-right
    {9,  4, 2, 2},  // 2 Zenith — bottom mid-right
    { 4, 2, 4, 2},  // 3 Hyperion — top-left block
    { 4, 4, 5, 2},  // 4 Persenne — bottom-left strip (longer)
};

// Floor 3 — Level 02. Nova anchors the right edge (cols 13–15);
// Pulsar/Quasar/Nebula are flush-packed against Nova so all four
// sections share borders edge-to-edge. Row band sits at rows 2–5 to
// centre the floor in the outline. Per-section icons (e.g. the
// star1 glyph for "temporal-icon") are pulled from the floors
// msgpack at render time — see lookupSectionIcon() — so adding or
// retiring an icon only requires editing data/in/floors.md, not
// touching this table.
constexpr GridSection kLevel02Grid[] = {
    { 4, 2, 3, 4},  // 0 Pulsar
    { 7, 2, 3, 4},  // 1 Quasar
    {10, 2, 3, 4},  // 2 Nebula
    {13, 2, 3, 4},  // 3 Nova
};

// Floor 4 — Level 03. Three full-height rooms: speaker work room,
// Temporal Film Studio, and Java Workshop (Despina).
constexpr GridSection kLevel03Grid[] = {
    { 0, 0, 5, 8},  // 0 Speaker Work Room
    { 5, 0, 6, 8},  // 1 Temporal Film Studio
    {11, 0, 5, 8},  // 2 Java Workshop (Despina)
};

const GridSection* gridForFloor(int floor_idx, int* count_out) {
  switch (floor_idx) {
    case 0: if (count_out) *count_out = 2; return kLevel00Grid;
    case 1: if (count_out) *count_out = 8; return kConcourseGrid;
    case 2: if (count_out) *count_out = 5; return kMezzanineGrid;
    case 3: if (count_out) *count_out = 4; return kLevel02Grid;
    case 4: if (count_out) *count_out = 3; return kLevel03Grid;
    default:
      if (count_out) *count_out = 0;
      return nullptr;
  }
}

// Per-floor outline rect (16×8 cell coords). Sections live inside.
//   Mezzanine — sideways 8×4 block centred in the 16×8 (cols 4–11,
//   rows 2–5). Tight bounding around the rotated section data.
//   Other floors default to the full 16×8 grid.
// Adjacent-section nav. Joystick input is always pure-axis (see
// joyDirection() — dx/dy is one of the four unit vectors), so the
// algorithm splits into two cases:
//
//   Pure horizontal (dy == 0): consider only candidates whose row
//     range overlaps the source's row range, then pick the smallest
//     forward column distance in the dx direction. This is the
//     "directional grid" feel users expect — Persist→right lands on
//     Sponsors A (same row band) instead of jumping diagonally down
//     to Ground Control.
//
//   Pure vertical (dx == 0): symmetric — column-overlap filter,
//     smallest forward row distance.
//
//   No overlap candidate found: fall back to the centroid-projection
//     scoring used previously, so floors with no perpendicular-
//     aligned neighbour (e.g. a stray off-grid section) still let the
//     cursor move.
//
// Half-cell visual nudges (GridSection::halfX/halfY) are intentionally
// ignored here so adjacency follows the integer grid, not whatever
// pixel-level offset the renderer applies.
namespace {

bool rangesOverlap(int a0, int aLen, int b0, int bLen) {
  // [a0, a0+aLen) vs [b0, b0+bLen) — half-open intervals.
  return (a0 < b0 + bLen) && (b0 < a0 + aLen);
}

int8_t adjacentByAxis(const GridSection* sections, int count, int8_t cur,
                      bool horiz, bool forward) {
  const GridSection& cs = sections[cur];
  // Source range on the *travel* axis (cols for horizontal travel)
  // and the *perpendicular* axis (rows for horizontal travel).
  const int trav0 = horiz ? cs.col : cs.row;
  const int travL = horiz ? cs.cw  : cs.ch;
  const int perp0 = horiz ? cs.row : cs.col;
  const int perpL = horiz ? cs.ch  : cs.cw;

  int8_t best = cur;
  int    best_dist = INT_MAX;
  for (int i = 0; i < count; i++) {
    if (i == cur) continue;
    const GridSection& os = sections[i];
    const int oTrav0 = horiz ? os.col : os.row;
    const int oTravL = horiz ? os.cw  : os.ch;
    const int oPerp0 = horiz ? os.row : os.col;
    const int oPerpL = horiz ? os.ch  : os.cw;
    if (!rangesOverlap(perp0, perpL, oPerp0, oPerpL)) continue;
    const int dist = forward ? (oTrav0 - (trav0 + travL))
                             : (trav0 - (oTrav0 + oTravL));
    if (dist < 0) continue;             // candidate not in travel direction
    if (dist < best_dist) {
      best_dist = dist;
      best = (int8_t)i;
    }
  }
  return best;
}

int8_t adjacentByCentroid(const GridSection* sections, int count, int8_t cur,
                          float dx, float dy) {
  const GridSection& cs = sections[cur];
  const float cx = cs.col + cs.cw / 2.0f;
  const float cy = cs.row + cs.ch / 2.0f;
  int8_t best = cur;
  float  best_score = 1e9f;
  for (int i = 0; i < count; i++) {
    if (i == cur) continue;
    const GridSection& os = sections[i];
    const float ox = os.col + os.cw / 2.0f;
    const float oy = os.row + os.ch / 2.0f;
    const float rx = ox - cx;
    const float ry = oy - cy;
    const float dot = rx * dx + ry * dy;
    if (dot <= 0.0f) continue;
    const float perp = fabsf(rx * (-dy) + ry * dx);
    const float score = dot + perp * 0.75f;
    if (score < best_score) {
      best_score = score;
      best = (int8_t)i;
    }
  }
  return best;
}

}  // namespace

// ── Pixel-layout adjacency (mirror of the grid pair above) ─────────
// Same range-overlap-then-distance algorithm as adjacentByAxis /
// adjacentByCentroid; just operates on PixelRect's (x,y,w,h) instead of
// GridSection's (col,row,cw,ch). Used for floors that have opted into
// MapData::PIXEL_LAYOUTS.
namespace {

int8_t pixelAdjacentByAxis(const MapData::PixelRect* sections, int count,
                           int8_t cur, bool horiz, bool forward) {
  const MapData::PixelRect& cs = sections[cur];
  const int trav0 = horiz ? cs.x : cs.y;
  const int travL = horiz ? cs.w : cs.h;
  const int perp0 = horiz ? cs.y : cs.x;
  const int perpL = horiz ? cs.h : cs.w;
  int8_t best      = cur;
  int    best_dist = INT_MAX;
  for (int i = 0; i < count; i++) {
    if (i == cur) continue;
    const MapData::PixelRect& os = sections[i];
    const int oTrav0 = horiz ? os.x : os.y;
    const int oTravL = horiz ? os.w : os.h;
    const int oPerp0 = horiz ? os.y : os.x;
    const int oPerpL = horiz ? os.h : os.w;
    if (!rangesOverlap(perp0, perpL, oPerp0, oPerpL)) continue;
    const int dist = forward ? (oTrav0 - (trav0 + travL))
                             : (trav0 - (oTrav0 + oTravL));
    if (dist < 0) continue;
    if (dist < best_dist) {
      best_dist = dist;
      best = (int8_t)i;
    }
  }
  return best;
}

int8_t pixelAdjacentByCentroid(const MapData::PixelRect* sections, int count,
                               int8_t cur, float dx, float dy) {
  const MapData::PixelRect& cs = sections[cur];
  const float cx = cs.x + cs.w / 2.0f;
  const float cy = cs.y + cs.h / 2.0f;
  int8_t best      = cur;
  float  best_score = 1e9f;
  for (int i = 0; i < count; i++) {
    if (i == cur) continue;
    const MapData::PixelRect& os = sections[i];
    const float ox = os.x + os.w / 2.0f;
    const float oy = os.y + os.h / 2.0f;
    const float rx = ox - cx;
    const float ry = oy - cy;
    const float dot = rx * dx + ry * dy;
    if (dot <= 0.0f) continue;
    const float perp = fabsf(rx * (-dy) + ry * dx);
    const float score = dot + perp * 0.75f;
    if (score < best_score) {
      best_score = score;
      best = (int8_t)i;
    }
  }
  return best;
}

}  // namespace

int8_t adjacentSection(int floor_idx, int8_t cur, float dx, float dy) {
  // Pixel-layout floors first — they bypass the grid entirely.
  if (floor_idx >= 0 && floor_idx < MapData::kFloorCount) {
    const MapData::PixelFloorLayout* layout =
        MapData::PIXEL_LAYOUTS[floor_idx];
    if (layout) {
      const int count = layout->section_count;
      if (cur < 0 || cur >= count) return cur;
      if (dy == 0.0f && dx != 0.0f) {
        int8_t pick = pixelAdjacentByAxis(layout->sections, count, cur,
                                          /*horiz=*/true,
                                          /*forward=*/dx > 0);
        if (pick != cur) return pick;
      } else if (dx == 0.0f && dy != 0.0f) {
        int8_t pick = pixelAdjacentByAxis(layout->sections, count, cur,
                                          /*horiz=*/false,
                                          /*forward=*/dy > 0);
        if (pick != cur) return pick;
      }
      return pixelAdjacentByCentroid(layout->sections, count, cur, dx, dy);
    }
  }

  int count = 0;
  const GridSection* sections = gridForFloor(floor_idx, &count);
  if (!sections || cur < 0 || cur >= count) return cur;

  if (dy == 0.0f && dx != 0.0f) {
    int8_t pick = adjacentByAxis(sections, count, cur,
                                 /*horiz=*/true, /*forward=*/dx > 0);
    if (pick != cur) return pick;
  } else if (dx == 0.0f && dy != 0.0f) {
    int8_t pick = adjacentByAxis(sections, count, cur,
                                 /*horiz=*/false, /*forward=*/dy > 0);
    if (pick != cur) return pick;
  }
  // Pure-axis pass found nothing aligned with the source's row/col
  // band — fall back to centroid projection so the cursor isn't
  // stuck. Also handles the (currently unreachable) diagonal case.
  return adjacentByCentroid(sections, count, cur, dx, dy);
}

// ── Section data lookup ───────────────────────────────────────────
//
// Source of truth is the floors msgpack served by DataCache (built
// from data/in/floors.md). On first use we walk that blob once and
// cache per-section (icon, desc) keyed by (floor_idx, section_idx);
// subsequent renders are O(table size) lookups.
//
// The only icon-name → glyph mapping that lives in source is the
// "temporal-icon" → star1 bitmap rule in drawSectionIcon below.
// Everything else (which sections carry which icon names, plus the
// description copy used by the section modal) is data.
struct FloorSectionEntry {
  int8_t floor_idx;
  int8_t section_idx;
  char   icon[16];
  char   desc[80];
};
constexpr uint8_t kSectionCacheCap = 40;
FloorSectionEntry s_sectionCache[kSectionCacheCap];
uint8_t           s_sectionCount = 0;
bool              s_sectionCacheBuilt = false;

void buildSectionCache() {
  // Mark built up-front so a parse failure (or empty bundle) doesn't
  // re-trigger the walk on every frame. Restarting the badge after a
  // bundle refresh re-runs this from scratch.
  s_sectionCacheBuilt = true;
  s_sectionCount = 0;

  DataCache::ReadLock lock;
  DataCache::Span span = DataCache::floors();
  if (!span.data || span.len == 0) return;

  MsgPack::Reader rd(span.data, span.len);
  uint32_t floorCount = 0;
  if (!rd.readArray(floorCount)) return;

  for (uint32_t f = 0; f < floorCount; f++) {
    uint32_t floorPairs = 0;
    if (!rd.readMap(floorPairs)) return;
    int8_t floor_idx = -1;
    for (uint32_t p = 0; p < floorPairs; p++) {
      char key[16] = "";
      if (!rd.readString(key, sizeof(key))) return;
      if (strcmp(key, "idx") == 0) {
        int64_t v = 0;
        if (!rd.readInt(v)) return;
        floor_idx = static_cast<int8_t>(v);
      } else if (strcmp(key, "sections") == 0) {
        uint32_t secCount = 0;
        if (!rd.readArray(secCount)) return;
        for (uint32_t s = 0; s < secCount; s++) {
          uint32_t secPairs = 0;
          if (!rd.readMap(secPairs)) return;
          int8_t section_idx = -1;
          char   icon[sizeof(s_sectionCache[0].icon)] = "";
          char   desc[sizeof(s_sectionCache[0].desc)] = "";
          for (uint32_t q = 0; q < secPairs; q++) {
            char key2[16] = "";
            if (!rd.readString(key2, sizeof(key2))) return;
            if (strcmp(key2, "idx") == 0) {
              int64_t v = 0;
              if (!rd.readInt(v)) return;
              section_idx = static_cast<int8_t>(v);
            } else if (strcmp(key2, "icon") == 0) {
              if (!rd.readString(icon, sizeof(icon))) return;
            } else if (strcmp(key2, "desc") == 0) {
              if (!rd.readString(desc, sizeof(desc))) return;
            } else {
              if (!rd.skip()) return;
            }
          }
          if ((icon[0] || desc[0]) && floor_idx >= 0 && section_idx >= 0
              && s_sectionCount < kSectionCacheCap) {
            FloorSectionEntry& e = s_sectionCache[s_sectionCount++];
            e.floor_idx   = floor_idx;
            e.section_idx = section_idx;
            strncpy(e.icon, icon, sizeof(e.icon));
            e.icon[sizeof(e.icon) - 1] = 0;
            strncpy(e.desc, desc, sizeof(e.desc));
            e.desc[sizeof(e.desc) - 1] = 0;
          }
        }
      } else {
        if (!rd.skip()) return;
      }
    }
  }
}

const FloorSectionEntry* lookupSection(int floor_idx, int section_idx) {
  if (!s_sectionCacheBuilt) buildSectionCache();
  for (uint8_t i = 0; i < s_sectionCount; i++) {
    if (s_sectionCache[i].floor_idx   == floor_idx &&
        s_sectionCache[i].section_idx == section_idx) {
      return &s_sectionCache[i];
    }
  }
  return nullptr;
}

const char* lookupSectionIcon(int floor_idx, int section_idx) {
  if (floor_idx == 4) {
    return section_idx == 1 ? "temporal-icon" : nullptr;
  }
  const FloorSectionEntry* e = lookupSection(floor_idx, section_idx);
  return (e && e->icon[0]) ? e->icon : nullptr;
}

const char* lookupSectionDesc(int floor_idx, int section_idx) {
  if (floor_idx == 4 && section_idx >= 0 &&
      section_idx < MAP_FLOOR_VECS[floor_idx].section_count) {
    return MAP_FLOOR_VECS[floor_idx].sections[section_idx].desc;
  }
  const FloorSectionEntry* e = lookupSection(floor_idx, section_idx);
  return (e && e->desc[0]) ? e->desc : nullptr;
}

// Paint a section's `icon:` glyph centred on the given bbox. Drawn
// AFTER the box so the glyph sits on top of the selected-fill or the
// dotted-outline. Uses inverse draw colour on the selected (filled)
// section so the glyph punches through to background.
//
// Recognised icon names (everything else is a silent no-op — add a
// branch here when wiring up a new glyph from floors.md):
//   "temporal-icon"  → star1 bitmap
//   "icon-info"      → 5×5 vertically-flipped exclamation (dot top,
//                      vertical bar bottom). Bits sourced from the
//                      kFailed glyph in MessageStatusGlyph.cpp.
void drawSectionIcon(oled& d, int floor_idx, int section_idx,
                     int x, int y, int w, int h, bool selected) {
  const char* icon = lookupSectionIcon(floor_idx, section_idx);
  if (!icon) return;
  d.setDrawColor(selected ? 0 : 1);
  if (strcmp(icon, "temporal-icon") == 0) {
    const int gx = x + (w - StarIcon::kWidth) / 2;
    const int gy = y + (h - StarIcon::kHeight) / 2;
    d.drawXBM(gx, gy, StarIcon::kWidth, StarIcon::kHeight, StarIcon::kBits);
  } else if (strcmp(icon, "icon-info") == 0) {
    // 5×5 footprint, dot at top, vertical bar at bottom — i.e.
    // MessageStatusGlyph kFailed flipped about its horizontal axis.
    constexpr int kInfoW = 5;
    constexpr int kInfoH = 5;
    const int gx = x + (w - kInfoW) / 2;
    const int gy = y + (h - kInfoH) / 2;
    d.drawPixel(gx + 2, gy + 0);
    d.drawVLine(gx + 2, gy + 2, 3);
  }
  d.setDrawColor(1);
}

struct OutlineRect { int col; int row; int cw; int ch; };
OutlineRect outlineForFloor(int floor_idx) {
  switch (floor_idx) {
    case 2: return {4, 2, 8, 4};  // Mezzanine — sideways landscape
    default: return {0, 0, kGridCols, kGridRows};
  }
}

// Overview rendering — 16×8 grid scaled per-floor:
//   • Mezzanine (floor 2) is the special exception — its mini outline
//     touches the header (y=7) and scales cells up to fill the entire
//     vertical band, so the building reads larger than its 4-cell
//     width would otherwise allow.
//   • Every other floor leaves a 2-px "no art" buffer below the
//     header AND a 2-px buffer above the footer rule.
// Each floor draws ONLY its per-floor outline rect (a sub-region of
// the 16×8 grid); rooms whose outline sub-region is smaller than the
// grid get a smaller bounding box surrounded by empty drawing space.
// The currently-selected section is drawn as a solid filled rect,
// others as dotted rounded outlines. Footer = selected section's
// name + a forward-arrow chip hinting that confirm zooms in.
// 4x2 stair sprite tiled vertically — XBM packing is row-major with bit 0
// being the leftmost pixel of each 8-px column. Row 0 = "####" (lit
// across), Row 1 = "#..#" (left + right post). Repeating downwards reads
// as a staircase, matching the bit pattern the floor layout spec calls
// for: {1111, 1001}.
static const uint8_t kStairBits[] PROGMEM = {0x0F, 0x09};
constexpr uint8_t kStairW = 4;
constexpr uint8_t kStairH = 2;

// 5x3 down-arrow head — solid triangle:
//   #####
//   .###.
//   ..#..
static const uint8_t kArrowDownBits[] PROGMEM = {0x1F, 0x0E, 0x04};
constexpr uint8_t kArrowDownW = 5;
constexpr uint8_t kArrowDownH = 3;

// Mirrored up-arrow head — same bits, flipped row order.
static const uint8_t kArrowUpBits[] PROGMEM = {0x04, 0x0E, 0x1F};
constexpr uint8_t kArrowUpW = 5;
constexpr uint8_t kArrowUpH = 3;

// Selected sections blink between filled-solid and frame-only every
// kBlinkPeriodMs to draw the eye, matching the design spec's "blink
// invert".
constexpr uint32_t kBlinkPeriodMs = 400;

void drawDashedVLine(oled& d, int x, int y, int h) {
  // 1-on-1-off pattern starting lit, so a 14-px line reads as 7 evenly
  // spaced dots. Cheap, predictable, and avoids pulling in a stipple
  // helper for a single decorative element.
  d.setDrawColor(1);
  for (int i = 0; i < h; i += 2) d.drawPixel(x, y + i);
}

// Horizontal sibling of drawDashedVLine — same 1-on-1-off cadence so
// the two visually rhyme when both appear on the same floor (e.g.
// Level 02 corridor wall + L-bracket headers).
void drawDashedHLine(oled& d, int x, int y, int w) {
  d.setDrawColor(1);
  for (int i = 0; i < w; i += 2) d.drawPixel(x + i, y);
}

// Plot an axis-aligned line as alternating-pixel "dotted" stipple.
// Bresenham-style for diagonals so callers don't have to special-case
// shape mixes; the existing pixel layouts only need axis-aligned but
// keeping it general means non-axis-aligned outlines work identically.
void drawDottedLine(oled& d, int x0, int y0, int x1, int y1) {
  d.setDrawColor(1);
  int dx = abs(x1 - x0);
  int dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  int step = 0;
  while (true) {
    if ((step & 1) == 0) d.drawPixel(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
    ++step;
  }
}

void drawPixelLayout(oled& d, int floor_idx, int selected,
                     const MapData::PixelFloorLayout& layout) {
  d.setDrawColor(1);

  // Outline polyline — dotted so the floor boundary reads as a soft
  // suggestion rather than a hard wall.
  for (uint8_t i = 0; i < layout.outline_n; ++i) {
    const MapData::MapEdge& e = layout.outline_edges[i];
    drawDottedLine(d, e.a.x, e.a.y, e.b.x, e.b.y);
  }

  // Decorative shapes — drawn before sections so any selected-fill
  // section ends up on top.
  for (uint8_t i = 0; i < layout.shape_count; ++i) {
    const MapData::PixelShape& s = layout.shapes[i];
    switch (s.kind) {
      case MapData::PixelShapeKind::RectFrame:
        d.drawRFrame(s.x, s.y, s.w, s.h, 0);
        break;
      case MapData::PixelShapeKind::DashedVLine:
        drawDashedVLine(d, s.x, s.y, s.h);
        break;
      case MapData::PixelShapeKind::DashedHLine:
        drawDashedHLine(d, s.x, s.y, s.w);
        break;
      case MapData::PixelShapeKind::Line:
        d.drawLine(s.x, s.y, s.x + s.w, s.y + s.h);
        break;
      case MapData::PixelShapeKind::Pixel:
        d.drawPixel(s.x, s.y);
        break;
      case MapData::PixelShapeKind::ArrowDown:
        d.drawXBM(s.x, s.y, kArrowDownW, kArrowDownH, kArrowDownBits);
        break;
      case MapData::PixelShapeKind::ArrowUp:
        d.drawXBM(s.x, s.y, kArrowUpW, kArrowUpH, kArrowUpBits);
        break;
      case MapData::PixelShapeKind::DottedRectLabel: {
        drawDottedRRect(d, s.x, s.y, s.w, s.h);
        if (s.label && s.label[0]) {
          d.setFontPreset(FONT_TINY);
          const int tw = d.getStrWidth(s.label);
          const int th = d.getMaxCharHeight();
          const int tx = s.x + (static_cast<int>(s.w) - tw) / 2;
          // Anchor on the baseline; getAscent puts the cap-height + 1 px
          // padding back so the glyph centres optically inside the box.
          const int ty = s.y + (static_cast<int>(s.h) - th) / 2 +
                          d.getAscent();
          d.drawStr(tx, ty, s.label);
        }
        break;
      }
    }
  }

  // Stairs — repeat the 4x2 sprite `count` times downward at each anchor,
  // then close the column with a 4-px horizontal cap below the last
  // tile so the staircase reads as a sealed shape rather than open
  // posts.
  for (uint8_t i = 0; i < layout.stair_count; ++i) {
    const MapData::PixelStairs& st = layout.stairs[i];
    for (uint8_t k = 0; k < st.count; ++k) {
      d.drawXBM(st.x, st.y + k * kStairH, kStairW, kStairH, kStairBits);
    }
    d.drawHLine(st.x, st.y + st.count * kStairH, kStairW);
  }

  // Sections — frame for unselected; selected section blinks between
  // a filled box and the same frame so the highlight pulses without
  // permanently masking whatever sits inside the bbox.
  const bool blink_filled = ((millis() / kBlinkPeriodMs) & 1) == 0;
  for (uint8_t i = 0; i < layout.section_count; ++i) {
    const MapData::PixelRect& r = layout.sections[i];
    const bool sel    = (static_cast<int>(i) == selected);
    const bool filled = sel && blink_filled;
    if (filled) {
      d.drawBox(r.x, r.y, r.w, r.h);
    } else {
      d.drawRFrame(r.x, r.y, r.w, r.h, 0);
    }
    // Icon glyph rides on top; drawSectionIcon inverts its draw color
    // for the filled phase so any registered glyph still reads against
    // the inverted background.
    drawSectionIcon(d, floor_idx, i, r.x, r.y, r.w, r.h, filled);

    // Optional centered label — same color-flip rule as the icon.
    if (r.label && r.label[0]) {
      d.setFontPreset(FONT_TINY);
      const int tw = d.getStrWidth(r.label);
      const int th = d.getMaxCharHeight();
      const int tx = r.x + (static_cast<int>(r.w) - tw) / 2;
      const int ty = r.y + (static_cast<int>(r.h) - th) / 2 + d.getAscent();
      d.setDrawColor(filled ? 0 : 1);
      d.drawStr(tx, ty, r.label);
      d.setDrawColor(1);
    }
  }
}

void drawFloorOverview(oled& d, int floor_idx, int selected) {
  // Pixel-layout floors paint at literal screen coords and skip the
  // grid math entirely. Other floors fall through to the legacy 16x8
  // grid renderer below.
  if (floor_idx >= 0 && floor_idx < MapData::kFloorCount) {
    const MapData::PixelFloorLayout* layout =
        MapData::PIXEL_LAYOUTS[floor_idx];
    if (layout) {
      drawPixelLayout(d, floor_idx, selected, *layout);
      return;
    }
  }

  int count = 0;
  const GridSection* sections = gridForFloor(floor_idx, &count);
  if (!sections || count == 0) return;

  // Uniform cell metrics across all floors — Mezzanine no longer
  // gets a special scale; its rotated landscape data uses the same
  // 6×4 cells everyone else does. The mini selector for Mezzanine is
  // a scaled-down sideways view of the same data, courtesy of the
  // rotated layout in kMezzanineGrid.
  const int kCellW  = 6;
  const int kCellH  = 4;
  const int kGutter = 1;
  const int kRectR  = 2;
  (void)floor_idx;  // currently unused beyond the section/outline lookups

  const int gridW = kGridCols * kCellW + (kGridCols - 1) * kGutter;
  const int gridH = kGridRows * kCellH + (kGridRows - 1) * kGutter;
  // Drawing band: 2-px-no-art buffer below the header, then a 3-px
  // additional drop to anchor the bounding boxes lower in the band.
  const int gridX = (128 - gridW) / 2;
  const int gridY = kContentTop + 2 + 3;

  // Floor outline — per-floor sub-rect of the 16×8 grid. Drawn as
  // the floor's bounding box; sections live inside.
  const OutlineRect ol = outlineForFloor(floor_idx);
  const int outlineX  = gridX + ol.col * (kCellW + kGutter);
  const int outlineY  = gridY + ol.row * (kCellH + kGutter);
  const int outlineW  = ol.cw * kCellW + (ol.cw - 1) * kGutter;
  const int outlineH  = ol.ch * kCellH + (ol.ch - 1) * kGutter;

  d.setDrawColor(1);
  drawThickRFrame(d, outlineX, outlineY, outlineW, outlineH, kRectR);

  // Floor 0 (Level 00): Howard ↔ Folsom flanking roads. 7×4 road
  // XBM tiled vertically on each side of the floor outline,
  // vertically centred on the outline's mid-y. Drawn AFTER the
  // outline so the road can extend up to the outline's edges
  // without leaving an erase-band gap.
  if (floor_idx == 0) {
    constexpr uint8_t kRoadW = 7;
    constexpr uint8_t kRoadH = 4;
    static const uint8_t kRoadBits[] PROGMEM =
        {0x41, 0x49, 0x49, 0x41};
    // Pick a tile count that fills most of the outline's height,
    // then anchor the strip so its mid-y matches the outline's
    // mid-y.
    const int outlineMidY = outlineY + outlineH / 2;
    const int tiles      = outlineH / kRoadH;
    const int stripH     = tiles * kRoadH;
    const int roadY      = outlineMidY - stripH / 2;
    // Left and right strip x positions, hugging the outline edges
    // with a 1 px breathing gap.
    const int leftRoadX  = outlineX - kRoadW - 1;
    const int rightRoadX = outlineX + outlineW + 1;
    for (int i = 0; i < tiles; i++) {
      d.drawXBM(leftRoadX,  roadY + i * kRoadH,
                kRoadW, kRoadH, kRoadBits);
      d.drawXBM(rightRoadX, roadY + i * kRoadH,
                kRoadW, kRoadH, kRoadBits);
    }
  }

  for (int i = 0; i < count; i++) {
    const GridSection& s = sections[i];
    const int stepX = kCellW + kGutter;
    const int stepY = kCellH + kGutter;
    const int x = gridX + s.col * stepX + s.halfX * stepX / 2;
    const int y = gridY + s.row * stepY + s.halfY * stepY / 2;
    const int w = s.cw * kCellW + (s.cw - 1) * kGutter;
    const int h = s.ch * kCellH + (s.ch - 1) * kGutter;
    const bool sel = (i == selected);
    if (sel) {
      d.drawRBox(x, y, w, h, 1);
    } else {
      drawDottedRRect(d, x, y, w, h);
    }
    drawSectionIcon(d, floor_idx, i, x, y, w, h, sel);
  }
  // Footer is the caller's responsibility — MapSectionScreen::render
  // chooses between the default section label and a "You're at <X>!"
  // override when the BLE fix lines up with the cursor's section.
}

// Forward decl — used by SectionModalScreen's event-count fallback.
int countEventsAtRoom(const char* roomName);

void drawHeader(oled& d, const char* title) {
  // Use the standard header element so the map app matches the
  // visual chrome of the schedule, settings and other top-level
  // screens (left-aligned title + optional right slot).
  OLEDLayout::drawHeader(d, title, nullptr);
}

void drawFooter(oled& d, const char* msg) {
  // Clear the footer band so polygon strokes / room labels above don't
  // bleed under the divider, then draw the no-rule star footer
  // (bookend stars + 8 px-padded text band that centres / scrolls the
  // section label). The map-app variant skips the horizontal divider
  // line so it doesn't slice through the section outlines drawn just
  // above. Clear starts at kFooterTopY+1 (=55) so a 2-px-thick floor
  // outline whose bottom row lands on y=54 is not erased.
  d.setDrawColor(0);
  d.drawBox(0, OLEDLayout::kFooterTopY + 1,
            128, 64 - (OLEDLayout::kFooterTopY + 1));
  OLEDLayout::drawStarFooterNoLine(d, msg);
}

int8_t joyDirection(const Inputs& inputs) {
  const int16_t dx = static_cast<int16_t>(inputs.joyX()) - 2047;
  const int16_t dy = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(dx) > abs(dy)) {
    if (dx > kJoyDeadband)  return 2;
    if (dx < -kJoyDeadband) return -2;
  } else {
    if (dy > kJoyDeadband)  return 1;
    if (dy < -kJoyDeadband) return -1;
  }
  return 0;
}

}  // namespace

void mapSetTargetFloor(int floor_idx) { s_target_floor = floor_idx; }
void mapSetTargetSection(int section_idx) { s_target_section = section_idx; }

// ═══════════════════════════════════════════════════════════════════════════
//  MapScreen — floor selector
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Floor parallelogram geometry. Compact stagger for the public venue
// floors. Level 03 (data idx 4) and Off-Site (data idx 5) remain in
// the map data for schedule lookups but are not part of the visible
// map stack — Level 03 is staff-only this year and Off-Site is only
// reachable via the schedule's Locate action.
constexpr int kStackFloorCount = 4;   // Level 00, Concourse, Mezzanine, Level 02

constexpr int kFloorOriginX  = 2;     // shifted 5 px left to widen the
                                      // gap to the floor-label column.
constexpr int kFloorOriginY  = 17;    // whole stack + label column slid
                                      // down by net +5 from the original
                                      // (12 → 19, then trimmed by 2 to 17)
                                      // for vertical centering above the
                                      // footer.
constexpr int kFloorVStep    = 7;     // vertical stagger between floors
constexpr int kFloorH        = 12;
constexpr int kFloorSlant    = 18;
constexpr int kFloorIntW     = 50;

constexpr int kLabelX        = 80;
constexpr int kLabelStep     = 9;   // +1 px row spacing
constexpr int kLabelYOff     = 6;

// Map a data-space floor index to the stack-visual index. Off-Site
// (kFloorCount-1) is excluded from the stack and returns -1.
inline int dataToVis(int data_idx) {
  if (data_idx < 0 || data_idx >= kStackFloorCount) return -1;
  return kStackFloorCount - 1 - data_idx;
}

// Counts events whose talks[0].title matches the given room name.
// Used by the section screen's footer fallback when a section has no
// description of its own.
int countEventsAtRoom(const char* roomName) {
  using namespace ScheduleData;
  if (!roomName || !roomName[0]) return 0;
  const SchedDay* days = sched_days();
  if (!days) return 0;
  int count = 0;
  for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
    const SchedDay& day = days[d];
    for (uint8_t i = 0; i < day.count; i++) {
      const SchedEvent& ev = day.events[i];
      if (ev.talk_count > 0 && ev.talks[0].title &&
          strcmp(ev.talks[0].title, roomName) == 0) {
        count++;
      }
    }
  }
  return count;
}

// Returns the data-space floor index of the event currently in
// progress (start_min ≤ now < end_min) — used as the "you are here"
// hint that gets dithered in the floor stack. Returns -1 when the
// clock isn't ready, no event matches, or the event is off-site
// (since Off-Site isn't a stacked floor).
//
// Currently unused — superseded by BLE proximity detection, but kept
// as a documented schedule-based fallback for future use.
[[maybe_unused]] int detectCurrentFloor() {
  using namespace ScheduleData;
  const time_t now = time(nullptr);
  if (now < 1700000000) return -1;
  struct tm tmBuf;
  localtime_r(&now, &tmBuf);
  const uint16_t nowMin =
      static_cast<uint16_t>(tmBuf.tm_hour * 60 + tmBuf.tm_min);

  const SchedDay* days = sched_days();
  if (!days) return -1;
  for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
    const SchedDay& day = days[d];
    for (uint8_t i = 0; i < day.count; i++) {
      const SchedEvent& ev = day.events[i];
      if (ev.start_min <= nowMin && nowMin < ev.end_min) {
        if (ev.talk_count > 0 && ev.talks[0].title) {
          const int f = MapData::findFloor(ev.talks[0].title);
          if (f >= 0 && f < kStackFloorCount) return f;
        }
      }
    }
  }
  return -1;
}

void floorOutline(oled& d, int ox, int oy) {
  const int tl = ox + kFloorSlant;
  const int tr = ox + kFloorSlant + kFloorIntW - 1;
  const int bl = ox;
  const int br = ox + kFloorIntW - 1;
  d.drawLine(tl, oy,             tr, oy);
  d.drawLine(bl, oy + kFloorH - 1, br, oy + kFloorH - 1);
  d.drawLine(tl, oy,             bl, oy + kFloorH - 1);
  d.drawLine(tr, oy,             br, oy + kFloorH - 1);
}

void floorFilled(oled& d, int ox, int oy) {
  for (int dy = 0; dy < kFloorH; dy++) {
    int xl = ox + kFloorSlant - (kFloorSlant * dy) / (kFloorH - 1);
    d.drawHLine(xl, oy + dy, kFloorIntW);
  }
}

void floorDither(oled& d, int ox, int oy) {
  for (int dy = 0; dy < kFloorH; dy++) {
    int xl    = ox + kFloorSlant - (kFloorSlant * dy) / (kFloorH - 1);
    int abs_y = oy + dy;
    int start = (xl + abs_y) % 2;
    for (int dx = start; dx < kFloorIntW; dx += 2) {
      d.drawPixel(xl + dx, abs_y);
    }
  }
}

void drawFloorStack(oled& d, int solid_vis, int dither_vis) {
  // 400 ms blink for the case where the selected floor IS also the
  // detected "you are here" floor — same cadence as the reference.
  const bool animSolid = ((millis() / 400) % 2) == 0;
  for (int i = kStackFloorCount - 1; i >= 0; i--) {
    int oy = kFloorOriginY + i * kFloorVStep;
    const bool is_solid  = (i == solid_vis);
    const bool is_dither = (i == dither_vis);

    // Erase the upper floor's footprint into anything drawn behind it so
    // that selecting an upper floor visually punches a notch in the
    // lower floor below.
    if (i < kStackFloorCount - 1) {
      d.setDrawColor(0);
      floorFilled(d, kFloorOriginX, oy);
    }
    d.setDrawColor(1);
    if (is_solid && is_dither) {
      if (animSolid) {
        floorFilled(d, kFloorOriginX, oy);
      } else {
        floorDither(d, kFloorOriginX, oy);
        floorOutline(d, kFloorOriginX, oy);
      }
    } else if (is_solid) {
      floorFilled(d, kFloorOriginX, oy);
    } else if (is_dither) {
      floorDither(d, kFloorOriginX, oy);
      floorOutline(d, kFloorOriginX, oy);
    } else {
      floorOutline(d, kFloorOriginX, oy);
    }
  }
}

void drawFloorLabels(oled& d, int selected_vis) {
  d.setFontPreset(FONT_TINY);
  d.setDrawColor(1);
  for (int i = 0; i < kStackFloorCount; i++) {
    const char* lbl = MAP_FLOORS[kStackFloorCount - 1 - i].label;
    int yb = kFloorOriginY + i * kLabelStep + kLabelYOff;

    if (i == selected_vis) {
      int tw    = d.getStrWidth(lbl);
      int box_x = kLabelX - 2;
      // 1 px extra padding above the text — frame top moved up by 1 px
      // and overall height grown by 1 px so the bottom edge stays in
      // the same place relative to the label baseline.
      int box_y = yb - 8;
      int box_w = tw + 4 + kArrowRightW + 2;  // padding + arrow XBM
      d.drawRFrame(box_x, box_y, box_w, 10, 1);
    }
    d.drawStr(kLabelX, yb, lbl);
    if (i == selected_vis) {
      int tw = d.getStrWidth(lbl);
      int ax = kLabelX + tw + 2;
      int ay = yb - 5;  // 1 px lower than before — tucks under the
                        // label baseline so the chip is visually
                        // centred on the label's x-height instead of
                        // riding over its ascenders.
      d.drawXBM(ax, ay, kArrowRightW, kArrowRightH, kArrowRightBits);
    }
  }
}

}  // namespace

void MapScreen::onEnter(GUIManager& /*gui*/) {
  selectedVis_ = 0;
  ramp_.reset();
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  // Open a BLE session ASYNCHRONOUSLY — beginSession() spawns a
  // Core 0 task to do the WiFi-stop + BLE-init work and returns
  // immediately so the OLED contrast fade keeps animating. We then
  // arm the scan unconditionally; if the begin task is still in
  // flight, startScan() latches s_scanPending and the task starts
  // scanning the moment the controller comes up.
  BleBeaconScanner::beginSession();
  BleBeaconScanner::startScan();
#endif
  enteredMs_          = millis();
  lastBleHeartbeatMs_ = enteredMs_;
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  lastBleAdvCount_    = BleBeaconScanner::advCount();
#else
  lastBleAdvCount_    = 0;
#endif
}

void MapScreen::onExit(GUIManager& /*gui*/) {
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  // Tear down BLE and bring WiFi back up — also async so the loop
  // task can finish the pop transition fade. The previous build
  // blocked here for the full WiFi reassociation (3-10 s) which
  // tripped the loop watchdog mid-fade and rebooted the badge.
  BleBeaconScanner::endSession();
#endif
}

void MapScreen::render(oled& d, GUIManager& /*gui*/) {
  drawHeader(d, "MAP");

  // Dither the floor the user is currently in (per BLE proximity).
  // No fix → no dither. Off-site / Level 03 aren't in the visible
  // stack so dataToVis() returns -1 and the dither is suppressed.
  const BadgeLocationFix fix = currentBadgeLocation();
  const int      dither_vis = (fix.floor >= 0) ? dataToVis(fix.floor) : -1;
  drawFloorStack(d, selectedVis_, dither_vis);
  drawFloorLabels(d, selectedVis_);

  // Footer surfaces BLE scan state in two transitions:
  //   • on entry / pre-fix, within grace window  → "Locating your badge..."
  //   • after grace, still no usable fix         → "Looks like you're offline!"
  //   • valid floor decoded                      → "You're at <Floor>"
  // Foreign / undecodable iBeacons fall into the offline bucket — from
  // the user's POV those are indistinguishable from no fix.
  constexpr uint32_t kLocatingGraceMs = 5000;
  static char footerBuf[40];
  const bool haveFloor = fix.hasFix &&
                         fix.floor >= 0 &&
                         fix.floor < (int)MapData::kFloorCount;
  if (haveFloor) {
    const char* label = MAP_FLOORS[fix.floor].label;
    if (label && strncmp(label, "Level ", 6) == 0) label += 6;
    snprintf(footerBuf, sizeof(footerBuf), "You're at %s",
             label ? label : "");
  } else if (
#ifdef BADGE_ENABLE_BLE_PROXIMITY
             (uint32_t)millis() - enteredMs_ < kLocatingGraceMs
#else
             false
#endif
            ) {
    snprintf(footerBuf, sizeof(footerBuf), "Locating your badge...");
  } else {
    snprintf(footerBuf, sizeof(footerBuf), "Looks like you're offsite!");
  }
  drawFooter(d, footerBuf);
}

void MapScreen::handleInput(const Inputs& inputs, int16_t /*cursorX*/,
                            int16_t /*cursorY*/, GUIManager& gui) {
  // 2-second BLE heartbeat — only logs while scanning so non-MAP
  // screens stay quiet on the serial console. The delta tells us
  // whether the radio is actually receiving advertisements.
  const uint32_t nowMs = millis();
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  if (BleBeaconScanner::isScanning() &&
      (nowMs - lastBleHeartbeatMs_) >= 2000) {
    const uint32_t adv = BleBeaconScanner::advCount();
    uint8_t obs[8] = {};
    BleBeaconScanner::copyLastObservedUuid(obs);
    Serial.printf("[map/ble] %lus  advs+%lu (total=%lu)  iBeacons=%lu  authOk=%lu  noTime=%lu mismatch=%lu  uid=0x%04X  hasFix=%d  epoch=%lu cached=%lu  obs=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                  (unsigned long)((nowMs - lastBleHeartbeatMs_) / 1000),
                  (unsigned long)(adv - lastBleAdvCount_),
                  (unsigned long)adv,
                  (unsigned long)BleBeaconScanner::iBeaconCount(),
                  (unsigned long)BleBeaconScanner::iBeaconAuthOkCount(),
                  (unsigned long)BleBeaconScanner::authFailNoTime(),
                  (unsigned long)BleBeaconScanner::authFailMismatch(),
                  (unsigned)BleBeaconScanner::bestRoomUid(),
                  BleBeaconScanner::hasFix() ? 1 : 0,
                  (unsigned long)BleBeaconScanner::currentEpoch30(),
                  (unsigned long)BleBeaconScanner::cachedEpoch30(),
                  obs[0], obs[1], obs[2], obs[3],
                  obs[4], obs[5], obs[6], obs[7]);
    lastBleAdvCount_    = adv;
    lastBleHeartbeatMs_ = nowMs;
    // Drain the BLEScan results vector — Arduino-ESP32 keeps every
    // advertisement in there with wantDuplicates=true, and the
    // accumulated objects starve the heap for MAP exit's task spawn
    // and BLE deinit if we don't bound the growth.
    BleBeaconScanner::clearScanCache();
  }
#else
  (void)nowMs;
#endif

  const Inputs::ButtonEdges& e = inputs.edges();
  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }
  if (e.confirmPressed) {
    const int data_idx = kStackFloorCount - 1 - selectedVis_;
    mapSetTargetFloor(data_idx);
    // If the floor the user just confirmed is the floor the BLE fix
    // places them on, also pre-select the section so they land on the
    // "You're at <section>!" footer immediately. Otherwise leave the
    // section pre-selection at its default (-1 → section 0).
    const BadgeLocationFix fix = currentBadgeLocation();
    if (fix.floor == data_idx && fix.section >= 0) {
      mapSetTargetSection(fix.section);
    }
    gui.pushScreen(kScreenMapSection);
    return;
  }

  const int8_t dir = joyDirection(inputs);
  if (dir == 0) {
    ramp_.reset();
    return;
  }
  if (!ramp_.tick(dir, millis())) return;

  // Joy-down moves "down" the visual stack (toward the lowest floor).
  if (dir == 1 && selectedVis_ < kStackFloorCount - 1) selectedVis_++;
  if (dir == -1 && selectedVis_ > 0) selectedVis_--;
}

// ═══════════════════════════════════════════════════════════════════════════
//  MapSectionScreen — vector floor plan with section picker
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Affine transform set per draw call (floor-local → screen pixel space).
float s_secScale = 1.0f;
int   s_secTx = 0;
int   s_secTy = 0;

inline int sx(int16_t x) { return s_secTx + (int)(x * s_secScale); }
inline int sy(int16_t y) { return s_secTy + (int)(y * s_secScale); }

// Bresenham, but lights every other pixel — used for unselected section
// borders so they fade into the background while still being legible.
void dottedLine(oled& d, int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), dy = abs(y1 - y0);
  int sxs = x0 < x1 ? 1 : -1, sys = y0 < y1 ? 1 : -1;
  int err = dx - dy, step = 0;
  for (;;) {
    if ((step & 1) == 0) d.drawPixel(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sxs; }
    if (e2 <  dx) { err += dx; y0 += sys; }
    step++;
  }
}

void solidStroke(oled& d, const MapEdge* edges, uint8_t n) {
  for (int i = 0; i < n; i++) {
    d.drawLine(sx(edges[i].a.x), sy(edges[i].a.y),
               sx(edges[i].b.x), sy(edges[i].b.y));
  }
}

void dotStroke(oled& d, const MapEdge* edges, uint8_t n) {
  for (int i = 0; i < n; i++) {
    dottedLine(d, sx(edges[i].a.x), sy(edges[i].a.y),
                  sx(edges[i].b.x), sy(edges[i].b.y));
  }
}

// Scan-line fill of a closed polygon described as an edge list. Only used
// for the currently-selected section so the user sees a clear highlight.
void polyFill(oled& d, const MapEdge* edges, uint8_t n) {
  int y_min = kContentBot, y_max = kContentTop;
  for (int i = 0; i < n; i++) {
    int ay = sy(edges[i].a.y), by = sy(edges[i].b.y);
    if (ay < y_min) y_min = ay;
    if (by < y_min) y_min = by;
    if (ay > y_max) y_max = ay;
    if (by > y_max) y_max = by;
  }
  if (y_min < kContentTop) y_min = kContentTop;
  if (y_max > kContentBot) y_max = kContentBot;

  for (int y = y_min; y <= y_max; y++) {
    int xi[16];
    int nxi = 0;
    for (int i = 0; i < n && nxi < 15; i++) {
      int ay = sy(edges[i].a.y), by = sy(edges[i].b.y);
      int ax = sx(edges[i].a.x), bx = sx(edges[i].b.x);
      if ((ay <= y && by > y) || (by <= y && ay > y)) {
        xi[nxi++] = ax + (bx - ax) * (y - ay) / (by - ay);
      }
    }
    for (int a = 0; a < nxi - 1; a++) {
      for (int b = a + 1; b < nxi; b++) {
        if (xi[a] > xi[b]) { int t = xi[a]; xi[a] = xi[b]; xi[b] = t; }
      }
    }
    d.setDrawColor(1);
    for (int p = 0; p + 1 < nxi; p += 2) {
      d.drawHLine(xi[p], y, xi[p + 1] - xi[p] + 1);
    }
  }
}

void sectionCentroid(const MapSectionVec& sec, float& cx, float& cy) {
  int16_t xlo = INT16_MAX, xhi = INT16_MIN;
  int16_t ylo = INT16_MAX, yhi = INT16_MIN;
  for (int i = 0; i < sec.shape.n; i++) {
    int16_t ax = sec.shape.edges[i].a.x, bx = sec.shape.edges[i].b.x;
    int16_t ay = sec.shape.edges[i].a.y, by = sec.shape.edges[i].b.y;
    if (ax < xlo) xlo = ax; if (bx < xlo) xlo = bx;
    if (ax > xhi) xhi = ax; if (bx > xhi) xhi = bx;
    if (ay < ylo) ylo = ay; if (by < ylo) ylo = by;
    if (ay > yhi) yhi = ay; if (by > yhi) yhi = by;
  }
  cx = (xlo + xhi) * 0.5f;
  cy = (ylo + yhi) * 0.5f;
}

void sectionScreenCentre(const MapSectionVec& sec, int& scx, int& scy) {
  float cx, cy;
  sectionCentroid(sec, cx, cy);
  scx = sx((int16_t)cx);
  scy = sy((int16_t)cy) + 2;
}

// "Closest centroid in the joystick direction" navigator. Projects each
// candidate onto the movement vector, penalises lateral offset, picks the
// largest score above zero. Identical heuristic to the reference.
int8_t navigate(const MapFloorVec& fv, int8_t cur, float dx, float dy) {
  float cx, cy;
  sectionCentroid(fv.sections[cur], cx, cy);
  int8_t best = cur;
  float best_score = 0.0f;
  for (int i = 0; i < fv.section_count; i++) {
    if (i == cur) continue;
    float ox, oy;
    sectionCentroid(fv.sections[i], ox, oy);
    float rel_x = ox - cx, rel_y = oy - cy;
    float dot = rel_x * dx + rel_y * dy;
    if (dot <= 0.0f) continue;
    float perp = fabsf(rel_x * (-dy) + rel_y * dx);
    float score = dot - perp * 0.5f;
    if (score > best_score) {
      best_score = score;
      best = (int8_t)i;
    }
  }
  return best;
}

}  // namespace

void MapSectionScreen::onEnter(GUIManager& /*gui*/) {
  // Honour an explicit pre-selection from mapSetTargetSection() once,
  // then reset it so a normal MapScreen → MapSectionScreen push lands
  // on section 0 as before.
  if (s_target_section >= 0 &&
      s_target_floor >= 0 && s_target_floor < MapData::kFloorCount &&
      s_target_section < MAP_FLOOR_VECS[s_target_floor].section_count) {
    selected_ = (int8_t)s_target_section;
  } else {
    selected_ = 0;
  }
  s_target_section = -1;
  ramp_.reset();
}

void MapSectionScreen::render(oled& d, GUIManager& /*gui*/) {
  static char title[24];
  const char* lbl = (s_target_floor >= 0 && s_target_floor < MapData::kFloorCount)
                        ? MAP_FLOORS[s_target_floor].label
                        : "Floor";
  snprintf(title, sizeof(title), "%s", lbl);
  drawHeader(d, title);

  const MapFloorVec& fv = MAP_FLOOR_VECS[s_target_floor];


  // Off-Site is not a real floor — Locate routed us here just to
  // surface the venue's name. Render that name centered with a small
  // "Off-Site" caption above; skip the polygon and section list.
  if (s_target_floor == MapData::kFloorCount - 1) {
    d.setDrawColor(1);
    const int idx = (selected_ >= 0 && selected_ < fv.section_count)
                        ? selected_ : 0;
    const char* name = (fv.section_count > 0)
                           ? fv.sections[idx].label
                           : "Off-Site";
    d.setFontPreset(FONT_TINY);
    const char* caption = "Off-Site";
    const int capW = d.getStrWidth(caption);
    d.drawStr((128 - capW) / 2, kContentTop + 12, caption);
    d.setFont(u8g2_font_6x10_tf);
    const int nameW = d.getStrWidth(name);
    d.drawStr((128 - nameW) / 2, kContentTop + 30, name);
    return;
  }

  // All other floors (Concourse / Mezzanine / Level 02 / Level 03)
  // share the unified 16×8 grid system. Confirm pushes the section
  // modal screen — see handleInput below.
  drawFloorOverview(d, s_target_floor, selected_);

  // Footer: default to the selected section's label, but when the
  // cursor sits exactly on the BLE-detected (floor, section), surface
  // a "You're at <section>!" hint instead so the user can confirm the
  // section the badge thinks they're in.
  const BadgeLocationFix fix = currentBadgeLocation();
  const bool youAreHere = (fix.floor == s_target_floor) &&
                          (fix.section == (int)selected_) &&
                          (selected_ >= 0 &&
                           selected_ < (int)fv.section_count);
  static char footerBuf[32];
  const char* label =
      (selected_ >= 0 && selected_ < (int)fv.section_count)
          ? fv.sections[selected_].label
          : "";
  if (youAreHere) {
    snprintf(footerBuf, sizeof(footerBuf), "You're at %s!", label);
    drawFooter(d, footerBuf);
  } else {
    drawFooter(d, label);
  }
}

void MapSectionScreen::handleInput(const Inputs& inputs, int16_t /*cursorX*/,
                                   int16_t /*cursorY*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  // Flow:
  //   Overview  → confirm → push SectionModalScreen (full-screen
  //                          modal, gets contrast-fade transition)
  //   Overview  → cancel  → pop to MapScreen
  // Off-Site is the only non-grid floor — confirm is a no-op there.
  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }
  if (e.confirmPressed) {
    if (s_target_floor == MapData::kFloorCount - 1) return;  // Off-Site
    const MapFloorVec& fv = MAP_FLOOR_VECS[s_target_floor];
    if (selected_ >= 0 && selected_ < fv.section_count) {
      prepareSectionModal(s_target_floor, selected_,
                          fv.sections[selected_].label);
      gui.pushScreen(kScreenSectionModal);
    }
    return;
  }

  const MapFloorVec& fv = MAP_FLOOR_VECS[s_target_floor];
  if (fv.section_count <= 1) return;

  const int8_t dir = joyDirection(inputs);
  if (dir == 0) {
    ramp_.reset();
    return;
  }
  if (!ramp_.tick(dir, millis())) return;

  // Spatial best-estimate nav: pick the section nearest in the
  // joystick direction. Up/down/left/right map to a unit vector;
  // adjacentSection() scores candidates by projection along the
  // vector minus a lateral-offset penalty.
  float dx_f = 0.f, dy_f = 0.f;
  switch (dir) {
    case  1: dy_f =  1.f; break;  // down
    case -1: dy_f = -1.f; break;  // up
    case  2: dx_f =  1.f; break;  // right
    case -2: dx_f = -1.f; break;  // left
    default: return;
  }
  selected_ = adjacentSection(s_target_floor, selected_, dx_f, dy_f);
  (void)fv;
}

// ═══════════════════════════════════════════════════════════════════════════
//  MapFloorScreen — events scheduled in the selected section
// ═══════════════════════════════════════════════════════════════════════════
//
// Scoped by the section label captured from MapSectionScreen via
// s_target_section. Walks ScheduleData::sched_days() and surfaces every
// event whose room (talks[0].title) matches the section label — i.e.
// the actual conference activity at that room, not a static directory.

namespace {

constexpr int kEventRowH = 9;
constexpr int kEventListTop = kContentTop + 1;
constexpr int kEventListBot = kContentBot - 1;
constexpr int kVisibleRows =
    (kEventListBot - kEventListTop) / kEventRowH;  // 4 rows in y=8..52

struct EventRef {
  uint8_t day;
  uint8_t idx;
};

// Collect up to `cap` events whose room (talks[0].title) matches the
// supplied label. Returns the number actually written. Days/events are
// scanned in order so the caller's list is already day-grouped.
int collectEventsForRoom(const char* roomLabel, EventRef* out, int cap) {
  using namespace ScheduleData;
  if (!roomLabel || !roomLabel[0] || !out || cap <= 0) return 0;
  const SchedDay* days = sched_days();
  if (!days) return 0;
  int n = 0;
  for (uint8_t d = 0; d < SCHED_MAX_DAYS && n < cap; d++) {
    const SchedDay& day = days[d];
    for (uint8_t i = 0; i < day.count && n < cap; i++) {
      const SchedEvent& ev = day.events[i];
      if (ev.talk_count > 0 && ev.talks[0].title &&
          strcmp(ev.talks[0].title, roomLabel) == 0) {
        out[n++] = {d, i};
      }
    }
  }
  return n;
}

const char* sectionLabelForTarget() {
  if (s_target_floor < 0 || s_target_floor >= (int)MapData::kFloorCount) {
    return nullptr;
  }
  const MapFloorVec& fv = MAP_FLOOR_VECS[s_target_floor];
  if (s_target_section < 0 || s_target_section >= fv.section_count) {
    return nullptr;
  }
  return fv.sections[s_target_section].label;
}

const char* sectionDescForTarget() {
  if (s_target_floor < 0 || s_target_floor >= (int)MapData::kFloorCount) {
    return nullptr;
  }
  const MapFloorVec& fv = MAP_FLOOR_VECS[s_target_floor];
  if (s_target_section < 0 || s_target_section >= fv.section_count) {
    return nullptr;
  }
  return fv.sections[s_target_section].desc;
}

// Format a section label for use as a screen header — strip an
// optional "The " prefix and uppercase. "The Hangar" → "HANGAR",
// "Swag / Lunch" → "SWAG / LUNCH".
void formatSectionTitle(const char* label, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!label) return;
  const char* src = label;
  if (strncmp(src, "The ", 4) == 0) src += 4;
  size_t i = 0;
  while (*src && i + 1 < cap) {
    char c = *src++;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    out[i++] = c;
  }
  out[i] = '\0';
}

}  // namespace

void MapFloorScreen::onEnter(GUIManager& /*gui*/) {
  cursor_ = 0;
  scroll_ = 0;
  ramp_.reset();
}

void MapFloorScreen::render(oled& d, GUIManager& /*gui*/) {
  const char* roomLabel = sectionLabelForTarget();
  // Header is the section title ("HANGAR", "SWAG / LUNCH" …) — no
  // longer prefixed with "MAP:" so it visually matches the rect
  // labels on the previous screen.
  static char title[24];
  formatSectionTitle(roomLabel, title, sizeof(title));
  drawHeader(d, title[0] ? title : "ROOM");

  if (!roomLabel) return;

  EventRef refs[SCHED_MAX_EVENTS * SCHED_MAX_DAYS];
  const int cap = sizeof(refs) / sizeof(refs[0]);
  const int total = collectEventsForRoom(roomLabel, refs, cap);

  // Footer copy is the section description (or a neutral fallback).
  // Drawn last (after the events list) so it overlays cleanly.
  const char* desc = sectionDescForTarget();
  const char* footMsg = (desc && desc[0]) ? desc : "More info";

  if (total == 0) {
    d.setFontPreset(FONT_TINY);
    d.setDrawColor(1);
    const char* msg = "No scheduled events.";
    const int tw = d.getStrWidth(msg);
    d.drawStr((128 - tw) / 2, kContentTop + 16, msg);
    drawFooter(d, "");
    // Footer text shifted right of the back-arrow chip.
    d.setDrawColor(1);
    d.drawXBM(0, OLEDLayout::kFooterBaseY - kArrowLeftH + 1,
              kArrowLeftW, kArrowLeftH, kArrowLeftBits);
    d.setFontPreset(FONT_TINY);
    d.drawStr(kArrowLeftW + 2, OLEDLayout::kFooterBaseY, footMsg);
    return;
  }

  // Clamp cursor + scroll window so the cursor row is always visible.
  if (cursor_ >= total) cursor_ = total - 1;
  if (cursor_ < 0)      cursor_ = 0;
  if (cursor_ < scroll_) scroll_ = cursor_;
  if (cursor_ >= scroll_ + kVisibleRows) {
    scroll_ = cursor_ - kVisibleRows + 1;
  }

  const ScheduleData::SchedDay* days = ScheduleData::sched_days();

  d.setFontPreset(FONT_TINY);
  for (int row = 0; row < kVisibleRows; row++) {
    const int idx = scroll_ + row;
    if (idx >= total) break;
    const EventRef& ref = refs[idx];
    const ScheduleData::SchedEvent& ev = days[ref.day].events[ref.idx];

    const int y = kEventListTop + row * kEventRowH;
    const bool sel = (idx == cursor_);
    if (sel) {
      d.setDrawColor(1);
      d.drawBox(0, y, 128, kEventRowH);
      d.setDrawColor(0);
    } else {
      d.setDrawColor(1);
    }

    char tbuf[6];
    ScheduleData::sched_fmt_time(ev.start_min, tbuf);
    // "D1 07:30" prefix (1-based day index) + scrolling title.
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "D%u %s",
             (unsigned)(ref.day + 1), tbuf);
    d.drawStr(2, y + 7, prefix);
    const int titleX = 2 + d.getStrWidth(prefix) + 3;
    if (ev.title) {
      d.setClipWindow(titleX, y, 127, y + kEventRowH - 1);
      d.drawStr(titleX, y + 7, ev.title);
      d.setMaxClipWindow();
    }
    d.setDrawColor(1);
  }

  // Scroll-position indicator on the right edge when the list overflows.
  if (total > kVisibleRows) {
    const int trackTop = kEventListTop;
    const int trackH   = kVisibleRows * kEventRowH;
    const int thumbH   = trackH * kVisibleRows / total;
    const int thumbY   = trackTop + (trackH - thumbH) * scroll_ /
                                        (total - kVisibleRows);
    d.setDrawColor(1);
    d.drawVLine(127, trackTop, trackH);
    d.drawVLine(126, thumbY, thumbH > 1 ? thumbH : 2);
  }

  // Footer: section description with a left-arrow back-chip on the
  // left edge. The chip cues that pressing back returns to the floor
  // section selection screen.
  drawFooter(d, "");
  d.setDrawColor(1);
  d.drawXBM(0, OLEDLayout::kFooterBaseY - kArrowLeftH + 1,
            kArrowLeftW, kArrowLeftH, kArrowLeftBits);
  d.setFontPreset(FONT_TINY);
  d.drawStr(kArrowLeftW + 2, OLEDLayout::kFooterBaseY, footMsg);
}

void MapFloorScreen::handleInput(const Inputs& inputs, int16_t /*cursorX*/,
                                 int16_t /*cursorY*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }
  // Confirm currently has no per-event detail; pop back to the floor
  // plan so the user can pick another section.
  if (e.confirmPressed) {
    gui.popScreen();
    return;
  }

  const int8_t dir = joyDirection(inputs);
  if (dir == 0) {
    ramp_.reset();
    return;
  }
  if (!ramp_.tick(dir, millis())) return;

  if (dir == 1)  cursor_++;
  if (dir == -1) cursor_--;
}

// ═══════════════════════════════════════════════════════════════════════════
//  SectionModalScreen — full-screen "modal" rendered as a real Screen
//  so it inherits the GUIManager's contrast-fade transition on push/pop.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

int   s_modal_floor   = -1;
int   s_modal_section = -1;
char  s_modal_label[32] = {};

void wrapAndCentreBody(oled& d, const char* text,
                       int interiorX, int interiorW,
                       int bodyTopY, int bodyBotY) {
  if (!text || !text[0]) return;
  // Match the schedule modal's body font for cross-modal consistency.
  d.setFont(u8g2_font_4x6_tf);
  constexpr int kLineH    = 7;
  constexpr int kCapLines = 5;
  const int maxLines = (bodyBotY - bodyTopY) / kLineH;
  const int lineCap  = maxLines < kCapLines ? maxLines : kCapLines;

  char lines[kCapLines][40] = {};
  int  lineCount = 0;
  const int maxW = interiorW - 4;
  char cur[40] = "";
  const char* p = text;
  while (*p && lineCount < lineCap) {
    const char* wend = p;
    while (*wend && *wend != ' ' && *wend != '\n') ++wend;
    int wlen = static_cast<int>(wend - p);
    if (wlen <= 0) { ++p; continue; }
    if (wlen >= 39) wlen = 39;
    char word[40];
    strncpy(word, p, wlen); word[wlen] = '\0';
    char test[80];
    if (cur[0]) snprintf(test, sizeof(test), "%s %s", cur, word);
    else        strncpy(test, word, sizeof(test) - 1), test[sizeof(test) - 1] = '\0';
    if (cur[0] && d.getStrWidth(test) > maxW) {
      strncpy(lines[lineCount], cur, sizeof(lines[lineCount]) - 1);
      lines[lineCount][sizeof(lines[lineCount]) - 1] = '\0';
      ++lineCount;
      strncpy(cur, word, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';
    } else {
      strncpy(cur, test, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';
    }
    p = (*wend && *wend != '\0') ? wend + 1 : wend;
  }
  if (cur[0] && lineCount < lineCap) {
    strncpy(lines[lineCount], cur, sizeof(lines[lineCount]) - 1);
    lines[lineCount][sizeof(lines[lineCount]) - 1] = '\0';
    ++lineCount;
  }

  const int blockH = lineCount * kLineH;
  int by = bodyTopY + ((bodyBotY - bodyTopY) - blockH) / 2 + 5;
  d.setDrawColor(1);
  for (int i = 0; i < lineCount; i++) {
    const int tw = d.getStrWidth(lines[i]);
    d.drawStr(interiorX + (interiorW - tw) / 2, by, lines[i]);
    by += kLineH;
  }
}

// Sponsor-booth marquee — vertically-centred 26-px row scrolling
// right→left, mirroring AboutSponsorsScreen's scroll feel but
// constrained to the modal body region. Uses the BoothSponsors set
// (rendered to 26 px by scripts/gen_sponsor_xbms.py) so multiple logos
// fit inside the modal chrome with vertical breathing room. Same wrap
// trick (draw twice at base and base+totalW) keeps the strip seamless
// across the edge. Sponsor index space is the same as AboutSponsors —
// the two arrays are generated from the same SPONSORS list in alphabetic
// order so booth.sponsor_indices remains valid against either set.
void drawSponsorBoothBody(oled& d, const OLEDLayout::ModalChrome& chrome,
                          const MapData::SponsorBooth& booth,
                          uint32_t openedMs) {
  if (booth.count == 0) return;

  constexpr int kLogoGapPx       = 16;
  constexpr int kScrollPxPerSec  = 22;
  constexpr uint32_t kSettleMs   = 240;  // hold static during fade-up

  int totalW = 0;
  for (uint8_t i = 0; i < booth.count; ++i) {
    totalW += BoothSponsors::kSponsors[booth.sponsor_indices[i]].width;
  }
  totalW += booth.count * kLogoGapPx;
  if (totalW <= 0) return;

  const int bodyH = chrome.bodyBotY - chrome.bodyTopY;
  const int rowY  = chrome.bodyTopY + (bodyH - BoothSponsors::kHeight) / 2;

  const uint32_t now      = millis();
  const uint32_t elapsed  = now - openedMs;
  const uint32_t motionMs = elapsed > kSettleMs ? elapsed - kSettleMs : 0;
  const int      scroll   =
      static_cast<int>(motionMs * kScrollPxPerSec / 1000U);

  int s = scroll % totalW;
  if (s < 0) s += totalW;
  const int base = -s;

  d.setDrawColor(1);
  for (int pass = 0; pass < 2; ++pass) {
    int x = base + (pass == 0 ? 0 : totalW);
    for (uint8_t i = 0; i < booth.count; ++i) {
      const auto& sp = BoothSponsors::kSponsors[booth.sponsor_indices[i]];
      if (x + static_cast<int>(sp.width) > chrome.interiorX &&
          x < chrome.interiorX + chrome.interiorW) {
        d.drawXBM(x, rowY, sp.width, BoothSponsors::kHeight,
                  BoothSponsors::kSponsorBits + sp.byteOffset);
      }
      x += sp.width + kLogoGapPx;
    }
  }
}

}  // namespace

void prepareSectionModal(int floor_idx, int section_idx, const char* label) {
  s_modal_floor   = floor_idx;
  s_modal_section = section_idx;
  if (label) {
    strncpy(s_modal_label, label, sizeof(s_modal_label) - 1);
    s_modal_label[sizeof(s_modal_label) - 1] = '\0';
  } else {
    s_modal_label[0] = '\0';
  }
}

const char* SectionModalScreen::title() const {
  return s_modal_label;
}

void SectionModalScreen::drawBody(oled& d,
                                  const OLEDLayout::ModalChrome& chrome) {
  // Sponsor booth sections render their member logos as a single
  // horizontally scrolling marquee — same vibe as AboutSponsorsScreen
  // but constrained to the modal body band.
  const MapData::SponsorBooth* booth =
      MapData::findSponsorBooth(s_modal_floor, s_modal_section);
  if (booth && booth->count > 0) {
    drawSponsorBoothBody(d, chrome, *booth, openedMs_);
    return;
  }

  const char* desc = lookupSectionDesc(s_modal_floor, s_modal_section);
  char fallback[64];
  if (!desc || !desc[0]) {
    const int n = countEventsAtRoom(s_modal_label);
    snprintf(fallback, sizeof(fallback),
             "There %s %d event%s in %s.",
             n == 1 ? "is" : "are", n,
             n == 1 ? "" : "s",
             s_modal_label[0] ? s_modal_label : "?");
    desc = fallback;
  }
  wrapAndCentreBody(d, desc, chrome.interiorX, chrome.interiorW,
                    chrome.bodyTopY + 1, chrome.bodyBotY);
}

void SectionModalScreen::onConfirm(GUIManager& gui) {
  // EVENTS — push the schedule scoped to this room. Cancel from the
  // schedule pops back through the modal to the section overview.
  if (s_modal_label[0]) {
    ScheduleData::sched_set_room_filter(s_modal_label);
  }
  gui.pushScreen(kScreenSchedule);
}
