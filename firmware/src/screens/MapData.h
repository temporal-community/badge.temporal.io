#pragma once

#include <stdint.h>

// ============================================================
//  Venue map data — Replay 2026
//
//  Hierarchy: floor > section > subsection.  Subsections hold a room
//  / area name that matches the schedule's `room` strings, plus an
//  optional pixel position (reserved for future heatmap overlays).
//
//  Index 0 = Lobby (data_idx 0), index 3 = Floor 4.  The vector
//  floor plans below are scaled to fit the 128 × 64 OLED content
//  area (y = 7..55, x = 0..127).
//
//  Ported from Replay-26-Badge_QAFW/Main-Firmware/map_data.{h,cpp};
//  BLE beacon proximity is omitted because that subsystem is not
//  present in this firmware.
// ============================================================

namespace MapData {

struct MapSubsection {
  const char* name;   // matches ScheduleData::SchedTalk.title
  uint8_t     x, y;   // floor-local pixel hint (reserved)
};

struct MapSection {
  const char*          name;
  const MapSubsection* subs;
  uint8_t              sub_count;
};

struct MapFloor {
  uint8_t           number;        // display number (1-based)
  const char*       label;         // short label, e.g. "Floor 3"
  const MapSection* sections;
  uint8_t           section_count;
  const char*       desc;          // optional short description for footer
};

// ── Vector floor plans (polygon outlines + section boundaries) ────
struct MapPt   { int16_t x, y; };
struct MapEdge { MapPt a, b; };

struct MapShape {
  const MapEdge* edges;
  uint8_t        n;
};

struct MapSectionVec {
  const char* label;
  uint8_t     sub_count;
  MapShape    shape;
  const char* desc;     // optional short description for footer
};

struct MapFloorVec {
  uint8_t              number;
  const char*          label;
  MapShape             outline;
  const MapSectionVec* sections;
  uint8_t              section_count;
};

// ── Manual pixel layout (per-floor opt-in override of the 16x8 grid) ─
// Floors that opt into a pixel layout publish:
//   * an outline polyline (open or closed) drawn 1:1 in screen coords
//   * a list of section bounding boxes — order MUST match MAP_FLOOR_VECS
//     for the floor so cursor / lookup logic stays consistent
//   * decorative stair stamps (4x2 sprite repeated vertically)
//   * decorative shapes (rectangles, dashed lines, etc.)
// All coordinates are in raw 128x64 screen pixels. Rendering and adjacency
// for these floors live entirely in screen-space; the grid system in
// MapScreens.cpp is bypassed.

struct PixelRect {
  int16_t     x, y;
  uint16_t    w, h;
  const char* label = nullptr;  // optional centered label drawn inside the rect
};

struct PixelStairs {
  int16_t x, y;
  uint8_t count;        // copies of the 4x2 stair sprite stacked down
};

enum class PixelShapeKind : uint8_t {
  RectFrame,            // hollow rectangle outline (size = w x h)
  DashedVLine,          // 1-px-wide vertical dashed line of length h
  DashedHLine,          // 1-px-tall horizontal dashed line of length w
  Line,                 // solid line from (x, y) to (x + w, y + h)
  Pixel,                // single pixel at (x, y); w/h ignored
  ArrowDown,            // 5x3 down-pointing arrow head; w/h ignored
  ArrowUp,              // 5x3 up-pointing arrow head; w/h ignored
  DottedRectLabel,      // dotted rectangle with single-char label centered;
                        // size = w x h, label points at the label string
};

struct PixelShape {
  PixelShapeKind kind;
  int16_t        x, y;
  uint16_t       w, h;
  const char*    label = nullptr;   // used by DottedRectLabel; ignored otherwise
};

struct PixelFloorLayout {
  const MapEdge*     outline_edges;
  uint8_t            outline_n;
  const PixelRect*   sections;
  uint8_t            section_count;
  const PixelStairs* stairs;
  uint8_t            stair_count;
  const PixelShape*  shapes;
  uint8_t            shape_count;
};

constexpr uint8_t kFloorCount = 6;

extern const MapFloor    MAP_FLOORS[kFloorCount];
extern const MapFloorVec MAP_FLOOR_VECS[kFloorCount];

// Indexed by floor_idx. nullptr → that floor still uses the 16x8 grid in
// MapScreens.cpp (gridForFloor). Floors are migrated to pixel layouts one
// at a time as their hand-tuned coords land.
extern const PixelFloorLayout* PIXEL_LAYOUTS[kFloorCount];

// ── Sponsor booths ─────────────────────────────────────────────────
// Some sections (the lobby Sponsors A/B/C trio) are booths populated by
// a fixed list of sponsors whose 32-px logos live in
// firmware/src/screens/AboutSponsors.h. The section modal swaps its
// description body for a scrolling marquee of those logos when one of
// these mappings matches the (floor, section) being shown.

struct SponsorBooth {
  int8_t        floor_idx;
  int8_t        section_idx;
  const uint8_t* sponsor_indices;  // indices into AboutSponsors::kSponsors
  uint8_t        count;
};

// Returns the booth descriptor for the given (floor, section), or
// nullptr if that section isn't a sponsor booth.
const SponsorBooth* findSponsorBooth(int floor_idx, int section_idx);

// Returns the 0-based floor index containing the named subsection,
// or -1 if not found. Used to jump from a schedule row's room name
// onto the corresponding map floor.
int findFloor(const char* subsection_name);

// Returns the first subsection on a floor (for display-only fallback
// when proximity data is unavailable).
const MapSubsection* nearestSub(int floor_idx);

// Resolve a room name (matching ScheduleData::SchedTalk.title) to a
// (floor_idx, section_idx) pair on MAP_FLOOR_VECS so the schedule
// "Locate" action can highlight the room on the map. Returns true on
// success; section_out is -1 when the floor was found but the room
// doesn't have a vector boundary entry (the caller should still push
// the section screen with the floor pre-selected).
bool findLocation(const char* room_name, int* floor_out, int* section_out);

}  // namespace MapData
