#include "MapData.h"

#include <string.h>

namespace MapData {

// ============================================================
//  Venue map — Replay 2026
//
//  Layout mirrors data/in/floors.md (the same source the floors
//  msgpack bundle is generated from). Stack order is bottom→top of
//  the building:
//
//    [0]  Level 00     — basement / main hall
//    [1]  Concourse    — lobby level (aliased "Lobby")
//    [2]  Mezzanine    — breakout rooms
//    [3]  Level 02     — additional sessions
//    [4]  Level 03     — corporate / production
//    [5]  Off-Site     — external venues (Cupid's Span etc.)
//
//  Section/subsection room names match the schedule's `room`
//  strings, which the schedule's "Locate" action uses to jump
//  directly into the matching floor.
// ============================================================

// ── Level 00 ────────────────────────────────────────────────────────
// Section order mirrors data/in/floors.md (1: The Hangar, 2: Swag / Lunch).
static const MapSubsection f0_subs[] = {
    {"The Hangar",   80, 7},
    {"Swag / Lunch", 30, 7},
};
static const MapSection f0_sections[] = {
    {"Level 00", f0_subs, 2},
};

// ── Concourse / Lobby ──────────────────────────────────────────────
static const MapSubsection f1_subs[] = {
    {"Debarkment",     8,  7},
    {"Check-In",       8,  7},
    {"Persist",        24, 7},
    {"AI Museum",      24, 7},
    {"Ground Control", 40, 7},
    {"The Gallery",    56, 7},
    {"Cafe",           56, 7},
    {"Help Desk",      72, 7},
    {"Sponsors A",     88, 7},
    {"Sponsors B",     104, 7},
    {"Sponsors C",     120, 7},
    {"Lobby",          64, 7},
    {"Lobby/2nd Fl.",  64, 7},
    {"Lobby/Downstairs", 64, 7},
    {"Downstairs",     64, 7},
};
static const MapSection f1_sections[] = {
    {"Concourse", f1_subs, 15},
};

// ── Mezzanine ──────────────────────────────────────────────────────
static const MapSubsection f2_subs[] = {
    {"The Expanse", 12, 7},
    {"Elara",       36, 7},
    {"Zenith",      60, 7},
    {"Hyperion",    84, 7},
    {"Persenne",    108, 7},
};
static const MapSection f2_sections[] = {
    {"Mezzanine", f2_subs, 5},
};

// ── Level 02 ───────────────────────────────────────────────────────
static const MapSubsection f3_subs[] = {
    {"Pulsar",                10, 7},
    {"Quasar",                40, 7},
    {"Nebula",                70, 7},
    {"Nova",                  100, 7},
    {"Level 02",              60, 7},
    // Workshop label aliases used by schedule entries.
    {"Go",                    25, 7},
    {"Python AI/Versioning",  55, 7},
    {"Python Nexus/AI",       85, 7},
};
static const MapSection f3_sections[] = {
    {"Level 02", f3_subs, 8},
};

// ── Level 03 ───────────────────────────────────────────────────────
static const MapSubsection f4_subs[] = {
    {"Speaker Work Room",       20, 7},
    {"Temporal Film Studio",    64, 7},
    {"Java Workshop (Despina)", 108, 7},
    // Schedule aliases for the visible Java Workshop room.
    {"Despina",                 108, 7},
    {"Workshop",                108, 7},
    {"Java",                    108, 7},
    {"Java [sold out]",         108, 7},
};
static const MapSection f4_sections[] = {
    {"Level 03", f4_subs, 7},
};

// ── Off-Site ───────────────────────────────────────────────────────
static const MapSubsection f5_subs[] = {
    {"Cupid's Span", 64, 7},
};
static const MapSection f5_sections[] = {
    {"Off-Site", f5_subs, 1},
};

const MapFloor MAP_FLOORS[kFloorCount] = {
    {0, "Level 00",  f0_sections, 1, "Below street level — swag + hall."},
    {1, "Lobby",     f1_sections, 1, "Lobby level — find cool stuff."},
    {2, "Mezzanine", f2_sections, 1, "Breakout floor — workshops."},
    {3, "Level 02",  f3_sections, 1, "Workshop & session rooms."},
    {4, "Level 03",  f4_sections, 1, "Corporate + production."},
    {5, "Off-Site",  f5_sections, 1, "Activities outside the venue."},
};

// ============================================================
//  Vector floor plans
//
//  Floor-local coordinate space: x=0..127, y=1..47 max.
// ============================================================

static const MapEdge kRectOutline[] = {
    {{0, 1},   {127, 1}},  {{127, 1}, {127, 47}},
    {{127, 47}, {0, 47}},  {{0, 47},  {0, 1}},
};

// ── Level 00 — Hangar + Swag/Lunch ──────────────────────────────────
static const MapEdge f0_sec_swag[] = {  // Swag / Lunch (left)
    {{4, 4},  {58, 4}},  {{58, 4},  {58, 44}},
    {{58, 44}, {4, 44}}, {{4, 44},  {4, 4}},
};
static const MapEdge f0_sec_hangar[] = {  // The Hangar (right, larger)
    {{62, 4},  {123, 4}},  {{123, 4},  {123, 44}},
    {{123, 44}, {62, 44}}, {{62, 44},  {62, 4}},
};
// Order mirrors floors.md (Swag / Lunch listed first, Hangar second).
static const MapSectionVec f0v_sections[] = {
    {"Swag / Lunch", 1, {f0_sec_swag,   4}, "Temporal swag and lunch."},
    {"The Hangar",   1, {f0_sec_hangar, 4}, "Primary keynote hall."},
};

// ── Concourse / Lobby — 4 sponsor cells + lobby strip ──────────────
static const MapEdge f1_sec0[] = {  // Debarkment / Check-In
    {{4, 4},   {38, 4}},  {{38, 4},  {38, 22}},
    {{38, 22}, {4, 22}},  {{4, 22},  {4, 4}},
};
static const MapEdge f1_sec1[] = {  // Lobby (centre band)
    {{42, 4},  {85, 4}},  {{85, 4},  {85, 22}},
    {{85, 22}, {42, 22}}, {{42, 22}, {42, 4}},
};
static const MapEdge f1_sec2[] = {  // Help Desk / Cafe
    {{89, 4},  {123, 4}},  {{123, 4},  {123, 22}},
    {{123, 22}, {89, 22}}, {{89, 22},  {89, 4}},
};
static const MapEdge f1_sec3[] = {  // Sponsors A
    {{4, 25},  {38, 25}},  {{38, 25},  {38, 44}},
    {{38, 44}, {4, 44}},   {{4, 44},   {4, 25}},
};
static const MapEdge f1_sec4[] = {  // Sponsors B
    {{42, 25}, {85, 25}},  {{85, 25},  {85, 44}},
    {{85, 44}, {42, 44}},  {{42, 44},  {42, 25}},
};
static const MapEdge f1_sec5[] = {  // Sponsors C
    {{89, 25}, {123, 25}}, {{123, 25}, {123, 44}},
    {{123, 44}, {89, 44}}, {{89, 44},  {89, 25}},
};
// Concourse — 8 sections in floors.md UID order (also matches the
// custom 13×5-grid layout's section indexing). Polygon shapes are
// retained for findLocation lookups (any of f1_sec0..f1_sec5 is
// fine — the grid render ignores them and uses cell-index bboxes).
static const MapSectionVec f1v_sections[] = {
    {"Persist",        1, {f1_sec1, 4}, "AI Museum installation."},
    {"Ground Control", 1, {f1_sec2, 4}, "Talk to an expert."},
    {"Sponsors A",     1, {f1_sec3, 4}, nullptr},
    {"Sponsors B",     1, {f1_sec4, 4}, nullptr},
    {"Sponsors C",     1, {f1_sec5, 4}, nullptr},
    {"Debarkment",     2, {f1_sec0, 4}, "Check-in & badge pickup."},
    {"Help Desk",      1, {f1_sec2, 4}, "On-site support."},
    {"The Gallery",    1, {f1_sec1, 4}, "Cafe — grab a treat."},
};

// ── Mezzanine — 5 breakout cells in a row ──────────────────────────
static const MapEdge f2_sec0[] = {  // The Expanse
    {{4, 4},   {26, 4}},   {{26, 4},   {26, 44}},
    {{26, 44}, {4, 44}},   {{4, 44},   {4, 4}},
};
static const MapEdge f2_sec1[] = {  // Elara
    {{30, 4},  {52, 4}},   {{52, 4},   {52, 44}},
    {{52, 44}, {30, 44}},  {{30, 44},  {30, 4}},
};
static const MapEdge f2_sec2[] = {  // Zenith
    {{56, 4},  {78, 4}},   {{78, 4},   {78, 44}},
    {{78, 44}, {56, 44}},  {{56, 44},  {56, 4}},
};
static const MapEdge f2_sec3[] = {  // Hyperion
    {{82, 4},  {104, 4}},  {{104, 4},  {104, 44}},
    {{104, 44}, {82, 44}}, {{82, 44},  {82, 4}},
};
static const MapEdge f2_sec4[] = {  // Persenne
    {{108, 4}, {123, 4}},  {{123, 4},  {123, 44}},
    {{123, 44}, {108, 44}},{{108, 44}, {108, 4}},
};
static const MapSectionVec f2v_sections[] = {
    {"The Expanse", 1, {f2_sec0, 4}, nullptr},
    {"Elara",       1, {f2_sec1, 4}, nullptr},
    {"Zenith",      1, {f2_sec2, 4}, nullptr},
    {"Hyperion",    1, {f2_sec3, 4}, nullptr},
    {"Persenne",    1, {f2_sec4, 4}, nullptr},
};

// ── Level 02 — 4 cells in a 2×2 grid ───────────────────────────────
static const MapEdge f3_sec0[] = {  // Pulsar
    {{4, 4},   {62, 4}},   {{62, 4},   {62, 22}},
    {{62, 22}, {4, 22}},   {{4, 22},   {4, 4}},
};
static const MapEdge f3_sec1[] = {  // Quasar
    {{66, 4},  {123, 4}},  {{123, 4},  {123, 22}},
    {{123, 22}, {66, 22}}, {{66, 22},  {66, 4}},
};
static const MapEdge f3_sec2[] = {  // Nebula
    {{4, 25},  {62, 25}},  {{62, 25},  {62, 44}},
    {{62, 44}, {4, 44}},   {{4, 44},   {4, 25}},
};
static const MapEdge f3_sec3[] = {  // Nova
    {{66, 25}, {123, 25}}, {{123, 25}, {123, 44}},
    {{123, 44}, {66, 44}}, {{66, 44},  {66, 25}},
};
static const MapSectionVec f3v_sections[] = {
    {"Pulsar", 1, {f3_sec0, 4}, nullptr},
    {"Quasar", 1, {f3_sec1, 4}, nullptr},
    {"Nebula", 1, {f3_sec2, 4}, nullptr},
    {"Nova",   1, {f3_sec3, 4}, nullptr},
};

// ── Level 03 — 3 visible rooms ─────────────────────────────────────
static const MapEdge f4_sec0[] = {  // Speaker Work Room
    {{4, 4},   {41, 4}},   {{41, 4},   {41, 44}},
    {{41, 44}, {4, 44}},   {{4, 44},   {4, 4}},
};
static const MapEdge f4_sec1[] = {  // Temporal Film Studio
    {{45, 4},  {83, 4}},   {{83, 4},   {83, 44}},
    {{83, 44}, {45, 44}},  {{45, 44},  {45, 4}},
};
static const MapEdge f4_sec2[] = {  // Java Workshop (Despina)
    {{87, 4},  {123, 4}},  {{123, 4},  {123, 44}},
    {{123, 44}, {87, 44}}, {{87, 44},  {87, 4}},
};
static const MapSectionVec f4v_sections[] = {
    {"Speaker Work Room",     1, {f4_sec0, 4}, "Prep and green room for presenters."},
    {"Temporal Film Studio",  1, {f4_sec1, 4}, "Video production and interview space."},
    {"Java Workshop (Despina)", 1, {f4_sec2, 4}, "Dedicated Java workshop room."},
};

// ── Off-Site — single cell representing an external venue ──────────
static const MapEdge f5_sec0[] = {  // Cupid's Span
    {{16, 8},  {111, 8}},  {{111, 8},  {111, 40}},
    {{111, 40}, {16, 40}}, {{16, 40},  {16, 8}},
};
static const MapSectionVec f5v_sections[] = {
    {"Cupid's Span", 1, {f5_sec0, 4}, "Waterfront 5K start."},
};

const MapFloorVec MAP_FLOOR_VECS[kFloorCount] = {
    {0, "Level 00",  {kRectOutline, 4}, f0v_sections, 2},
    {1, "Lobby",     {kRectOutline, 4}, f1v_sections, 8},
    {2, "Mezzanine", {kRectOutline, 4}, f2v_sections, 5},
    {3, "Level 02",  {kRectOutline, 4}, f3v_sections, 4},
    {4, "Level 03",  {kRectOutline, 4}, f4v_sections, 3},
    {5, "Off-Site",  {kRectOutline, 4}, f5v_sections, 1},
};

// ============================================================
//  Pixel layouts — manual hand-tuned coords on the 128x64 canvas.
//  See MapData.h for the data shape contract.
// ============================================================

// ── Lobby (floor_idx = 1) ──────────────────────────────────────
// Outline polyline traced from assets/lobby.xbm via
// scripts/trace_xbm_polyline.py — open horseshoe shape with the two
// openings on the left edge.
static const MapEdge kLobbyOutlineEdges[] = {
    {{  0,  8}, { 15,  8}},
    {{ 15,  8}, { 15, 11}},
    {{ 15, 11}, {114, 11}},
    {{114, 11}, {114,  8}},
    {{114,  8}, {125,  8}},
    {{125,  8}, {125, 52}},
    {{125, 52}, { 48, 52}},
    {{ 48, 52}, { 48, 44}},
    {{ 48, 44}, { 23, 44}},
    {{ 23, 44}, { 23, 51}},
    {{ 23, 51}, {  0, 51}},
};

// Section order matches f1v_sections above (Persist, Ground Control,
// Sponsors A/B/C, Debarkment, Help Desk, The Gallery).
static const PixelRect kLobbySections[] = {
    { 17, 13, 16,  5},   // 0 Persist
    { 25, 21, 25,  9},   // 1 Ground Control      (-2 w)
    { 52, 13, 12,  6},   // 2 Sponsors A          (+1 h)
    { 60, 45, 14,  6},   // 3 Sponsors B          (-1 h)
    { 73, 13, 12,  6},   // 4 Sponsors C          (+1 h)
    { 78, 29,  3,  7},   // 5 Debarkment
    { 85, 27, 20, 13},   // 6 Help Desk
    { 96, 13, 17,  7},   // 7 The Gallery
};

static const PixelStairs kLobbyStairs[] = {
    { 14, 29, 5},        // -1 px on x to nudge the stack off the dashed line
    {109, 30, 4},
    { 56, 25, 8},
    { 59, 25, 8},
    { 72, 25, 8},
    { 75, 25, 8},
};

static const PixelShape kLobbyShapes[] = {
    {PixelShapeKind::RectFrame,    61, 25, 12, 17},  // arrow surround: +1 w, +1 h
    {PixelShapeKind::DashedVLine,  15, 14,  1, 14},
    // Arrow stem + head pointing down — both moved up 2 px (line shaft
    // now y=29..33; 5x3 head anchored at (65, 34)) so the arrow nests
    // higher inside the surround rect.
    {PixelShapeKind::Line,         67, 29,  0,  4},
    {PixelShapeKind::ArrowDown,    65, 34,  0,  0},
};

static const PixelFloorLayout kLobbyLayout = {
    kLobbyOutlineEdges,
    sizeof(kLobbyOutlineEdges) / sizeof(kLobbyOutlineEdges[0]),
    kLobbySections,
    sizeof(kLobbySections) / sizeof(kLobbySections[0]),
    kLobbyStairs,
    sizeof(kLobbyStairs) / sizeof(kLobbyStairs[0]),
    kLobbyShapes,
    sizeof(kLobbyShapes) / sizeof(kLobbyShapes[0]),
};

// ── Level 00 (floor_idx = 0) ───────────────────────────────────
// Open polyline outline. Bottom edge is split by a wide gap (x=59..81)
// where the staircase column + elevator block live; the polyline traces
// up the left side, across the top, and back down the right side. The
// start vertex (left side of the bottom gap) sits 2 px further left
// than the previous design so the gap aligns with the wider elevator
// surround below.
static const MapEdge kLevel00OutlineEdges[] = {
    {{ 59, 53}, {  0, 53}},
    {{  0, 53}, {  0,  9}},
    {{  0,  9}, {127,  9}},
    {{127,  9}, {127, 53}},
    {{127, 53}, { 81, 53}},
};

// Section order matches f0v_sections (Swag / Lunch first, The Hangar
// second) and floors.md. The big top-row box is The Hangar (the
// keynote hall), the small bottom-right box is Swag / Lunch. The "A"
// label rides inside The Hangar's bbox so it lines up with the dotted
// "B" / "C" placeholder columns to its right. A/B/C sit at y=12
// (1 px lower than the previous y=11) so the row reads as floating
// just under the top outline rather than touching it.
static const PixelRect kLevel00Sections[] = {
    {82, 39, 12, 12, nullptr},   // 0 Swag / Lunch — small bottom-right
    {55, 12, 20, 20, "A"},       // 1 The Hangar   — big top-row, +1 y
};

static const PixelStairs kLevel00Stairs[] = {
    {62, 39, 7},
    {75, 39, 7},
};

static const PixelShape kLevel00Shapes[] = {
    // Sponsor / room placeholders B and C — dotted-frame boxes with the
    // single-letter label centred inside. Both shifted +1 y to keep
    // vertical alignment with the A rect above.
    {PixelShapeKind::DottedRectLabel, 79,  12, 20, 20, "B"},
    {PixelShapeKind::DottedRectLabel, 103, 12, 20, 20, "C"},
    // Elevator block: frame + centred up-arrow head + 4-px shaft below.
    // Surround widened 1 px on each side (was x=66 w=9, now x=65 w=11)
    // so the arrow sits with more breathing room from the staircase
    // columns either side. Arrow head + shaft remain centred at x=70.
    {PixelShapeKind::RectFrame,        65, 39, 11, 15},
    {PixelShapeKind::ArrowUp,          68, 43,  0,  0},
    {PixelShapeKind::Line,             70, 46,  0,  3},
};

static const PixelFloorLayout kLevel00Layout = {
    kLevel00OutlineEdges,
    sizeof(kLevel00OutlineEdges) / sizeof(kLevel00OutlineEdges[0]),
    kLevel00Sections,
    sizeof(kLevel00Sections) / sizeof(kLevel00Sections[0]),
    kLevel00Stairs,
    sizeof(kLevel00Stairs) / sizeof(kLevel00Stairs[0]),
    kLevel00Shapes,
    sizeof(kLevel00Shapes) / sizeof(kLevel00Shapes[0]),
};

// ── Mezzanine (floor_idx = 2) ──────────────────────────────────
// Cluster of 5 breakout rooms inside a centred dotted boundary box.
// Section order matches f2v_sections (Expanse, Elara, Zenith,
// Hyperion, Persenne) — those slots are the ones the schedule's
// Locate action keys against.
static const MapEdge kMezzanineOutlineEdges[] = {
    {{44, 54}, {44, 12}},
    {{44, 12}, {80, 12}},
    {{80, 12}, {80, 54}},
};

static const PixelRect kMezzanineSections[] = {
    {50, 14, 26,  8, nullptr},   // 0 The Expanse — top strip
    {50, 24, 12, 12, nullptr},   // 1 Elara
    {64, 24, 12,  6, nullptr},   // 2 Zenith
    {50, 38, 12, 16, nullptr},   // 3 Hyperion
    {64, 32, 12, 22, nullptr},   // 4 Persenne
};

static const PixelFloorLayout kMezzanineLayout = {
    kMezzanineOutlineEdges,
    sizeof(kMezzanineOutlineEdges) / sizeof(kMezzanineOutlineEdges[0]),
    kMezzanineSections,
    sizeof(kMezzanineSections) / sizeof(kMezzanineSections[0]),
    nullptr, 0,
    nullptr, 0,
};

// ── Level 02 (floor_idx = 3) ───────────────────────────────────
// Single-row band of 4 rooms (Pulsar / Quasar / Nebula / Nova) along
// the right two-thirds of the canvas. A vertical dashed separator at
// x=11 marks the corridor wall on the left; the corridor itself hosts
// a two-column staircase descending from y=26.
//
// No outline polyline — the sections butt up against one another and
// against the dashed wall, so the section frames + the wall together
// form the visible floor boundary.
static const PixelRect kLevel02Sections[] = {
    {23, 19, 21, 31, nullptr},   // 0 Pulsar
    {46, 19, 23, 31, nullptr},   // 1 Quasar
    {71, 19, 23, 31, nullptr},   // 2 Nebula
    {96, 19, 28, 31, nullptr},   // 3 Nova
};

static const PixelStairs kLevel02Stairs[] = {
    {13, 26, 8},
    {17, 26, 8},
};

static const PixelShape kLevel02Shapes[] = {
    // Two horizontal dashed segments at y=13 with a 7-px gap between
    // x=63..69 — visually frames the staircase opening below. The
    // single corner pixels at (62,12) and (70,12) cap the gap edges
    // so the dashed line reads as turning into a doorway rather than
    // just disappearing. Whole assembly nudged down 2 px from the
    // original y=11/10 anchor so it sits closer to the section row.
    {PixelShapeKind::DashedHLine,  0, 13, 63, 1},
    {PixelShapeKind::DashedHLine, 70, 13, 58, 1},
    {PixelShapeKind::Pixel,       62, 12,  0, 0},
    {PixelShapeKind::Pixel,       70, 12,  0, 0},
};

static const PixelFloorLayout kLevel02Layout = {
    nullptr, 0,
    kLevel02Sections,
    sizeof(kLevel02Sections) / sizeof(kLevel02Sections[0]),
    kLevel02Stairs,
    sizeof(kLevel02Stairs) / sizeof(kLevel02Stairs[0]),
    kLevel02Shapes,
    sizeof(kLevel02Shapes) / sizeof(kLevel02Shapes[0]),
};

const PixelFloorLayout* PIXEL_LAYOUTS[kFloorCount] = {
    &kLevel00Layout,    // 0 Level 00
    &kLobbyLayout,      // 1 Lobby
    &kMezzanineLayout,  // 2 Mezzanine
    &kLevel02Layout,    // 3 Level 02
    nullptr,            // 4 Level 03 — grid
    nullptr,            // 5 Off-Site — grid
};

// ── Sponsor booth tables ────────────────────────────────────────
// Indices reference AboutSponsors::kSponsors which is alphabetised
// at generation time:
//   0 Apartment 304   1 Augment Code   2 AWS         3 Bitovi
//   4 Braintrust      5 Google for Startups          6 Grid Dynamics
//   7 Liatro          8 Tailscale      9 TechnoIdentity
// Keep the per-booth index list in the same order as floors.md so the
// scrolling marquee plays out left-to-right in the documented order.

static const uint8_t kBoothA[] = {1};                       // Augment Code
static const uint8_t kBoothB[] = {9, 0, 7, 5, 3};           // Tech, Apt304, Liatro, GFS, Bitovi
static const uint8_t kBoothC[] = {4};                       // Braintrust

// Floor 1 (Concourse) section indices in f1v_sections order:
//   0 Persist, 1 Ground Control, 2 Sponsors A, 3 Sponsors B,
//   4 Sponsors C, 5 Debarkment, 6 Help Desk, 7 The Gallery
static const SponsorBooth kSponsorBooths[] = {
    {1, 2, kBoothA, sizeof(kBoothA) / sizeof(kBoothA[0])},
    {1, 3, kBoothB, sizeof(kBoothB) / sizeof(kBoothB[0])},
    {1, 4, kBoothC, sizeof(kBoothC) / sizeof(kBoothC[0])},
};
constexpr uint8_t kSponsorBoothCount =
    sizeof(kSponsorBooths) / sizeof(kSponsorBooths[0]);

const SponsorBooth* findSponsorBooth(int floor_idx, int section_idx) {
  for (uint8_t i = 0; i < kSponsorBoothCount; i++) {
    const SponsorBooth& b = kSponsorBooths[i];
    if (b.floor_idx == floor_idx && b.section_idx == section_idx) {
      return &b;
    }
  }
  return nullptr;
}

int findFloor(const char* name) {
  if (!name || !name[0]) return -1;
  for (int f = 0; f < kFloorCount; f++) {
    for (int s = 0; s < MAP_FLOORS[f].section_count; s++) {
      const MapSection& sec = MAP_FLOORS[f].sections[s];
      for (int ss = 0; ss < sec.sub_count; ss++) {
        if (strcmp(sec.subs[ss].name, name) == 0) return f;
      }
    }
  }
  return -1;
}

const MapSubsection* nearestSub(int floor_idx) {
  if (floor_idx < 0 || floor_idx >= kFloorCount) return nullptr;
  const MapFloor& f = MAP_FLOORS[floor_idx];
  if (f.section_count == 0 || f.sections[0].sub_count == 0) return nullptr;
  return &f.sections[0].subs[0];
}

bool findLocation(const char* room_name, int* floor_out, int* section_out) {
  if (floor_out)   *floor_out   = -1;
  if (section_out) *section_out = -1;
  if (!room_name || !room_name[0]) return false;

  const int f = findFloor(room_name);
  if (f < 0) return false;
  if (floor_out) *floor_out = f;

  // Try to match against MAP_FLOOR_VECS' section labels for the
  // section-screen pre-highlight. Falls back to -1 (caller still
  // pushes the section screen with the floor selected).
  const MapFloorVec& fv = MAP_FLOOR_VECS[f];
  for (int s = 0; s < fv.section_count; s++) {
    if (strcmp(fv.sections[s].label, room_name) == 0) {
      if (section_out) *section_out = s;
      return true;
    }
  }
  if (f == 4 &&
      (strcmp(room_name, "Despina") == 0 ||
       strcmp(room_name, "Workshop") == 0 ||
       strcmp(room_name, "Java") == 0 ||
       strcmp(room_name, "Java [sold out]") == 0)) {
    if (section_out) *section_out = 2;
    return true;
  }
  return true;
}

}  // namespace MapData
