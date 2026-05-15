#pragma once

class GUIManager;

// ─── FileOpener ────────────────────────────────────────────────────────────
//
// Type-aware dispatcher for file selection. Maps a path's extension to a
// concrete viewer / editor screen and pushes it onto the GUI stack. All
// extensions are matched case-insensitive.
//
//   .py / .json / .txt / .md / .csv / .ini / .log / .cfg
//   .toml / .yaml / .yml / .xml / .html / .h / .c / .cpp / .ino
//                                                          → EditorScreen
//   .xbm / .fb / .bin (credit prebinned layout)               → AnimTestScreen
//   anything else (binary, .bmp/.png/.jpg/.gif/.wad/...)    → HexViewScreen
//
// Lives in `screens/` rather than `infra/` because it only references
// the screen globals (`sEditor`, `sHexView`, `sAnimTest`) wired up in
// `GUI.cpp` — no business logic of its own.

namespace FileOpener {

void open(const char* path, GUIManager& gui);

}  // namespace FileOpener
