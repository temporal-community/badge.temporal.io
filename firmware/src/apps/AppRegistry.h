// Dynamic app registry — scans /apps/<slug>/main.py for Python dunder
// metadata (__title__, __icon__, __description__) and exposes the
// discovered apps to the main-menu grid. JumperIDE writes a script to
// /apps/<slug>/main.py and then calls badge.rescan_apps() to refresh
// the menu without rebooting.
//
// Design contract:
//   - The registry never imports/executes the Python module. It does a
//     fast text scan of the first ~2 KB of main.py and pulls dunders out
//     with handcrafted parsing. Boot must stay snappy even with hundreds
//     of apps.
//   - Icon resolution: if __icon__ is a path string ending in .py, we
//     also scan that file for `DATA = (...)` matching the layout used
//     by AppIcons::* (12x12 packed XBM, 24 bytes total). If __icon__
//     is missing or unparsable, we fall back to AppIcons::apps so the
//     app still shows up.
//   - All discovered apps live in PSRAM where possible — the icon byte
//     copies are small (24 bytes each), but the registry can hold up
//     to kMaxDynamicApps entries.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace AppRegistry {

constexpr size_t kMaxDynamicApps = 32;
constexpr size_t kSlugCap = 24;
constexpr size_t kTitleCap = 20;
constexpr size_t kDescriptionCap = 64;
constexpr size_t kEntryPathCap = 64;
constexpr uint8_t kIconWidth = 12;
constexpr uint8_t kIconHeight = 12;
// Packed XBM-style: ceil(12/8) bytes per row × 12 rows = 2 × 12 = 24 bytes.
constexpr size_t kIconBytes = 24;

struct DynamicApp {
  char slug[kSlugCap];
  char title[kTitleCap];
  char description[kDescriptionCap];
  char entryPath[kEntryPathCap];
  uint8_t icon[kIconBytes];
  bool hasCustomIcon;
  // Set when /apps/<slug>/matrix.py exists. The LEDScreen carousel
  // surfaces these as additional persistent ambient picks.
  bool hasMatrixApp;
  // Display label for the matrix-app entry. Defaults to the foreground
  // app's title when the script omits __matrix_title__.
  char matrixTitle[kTitleCap];
  // `__order__` dunder, parsed as a signed integer. Sentinel
  // INT16_MAX means "unspecified" — the rebuild path falls back to the
  // dynamic-app default order in that case.
  int16_t orderHint;
};

// Scan /apps for self-describing Python apps. Idempotent — re-running
// rebuilds the table. Returns the number of apps discovered.
size_t scan();

// Force a rescan from anywhere (Python helper, GUI rebuild on resume).
size_t rescan();

// Read-only access for the GUI menu builder.
size_t count();
const DynamicApp* at(size_t index);

}  // namespace AppRegistry
