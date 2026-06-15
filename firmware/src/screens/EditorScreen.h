#pragma once
#include <stdint.h>

#include "Screen.h"

// ─── EditorScreen ──────────────────────────────────────────────────────────
//
// Inline text editor based on the eKilo data model (antirez/kilo via the
// Architeuthis-Flux/JumperlOS port). Adapted for the badge:
//   - Display is U8G2 128x64 mono with FONT_TINY (~21 cols × 6 rows).
//   - Input is dpad + joystick + A/B/X/Y, NOT a terminal stream.
//   - Storage is PSRAM-backed via BadgeMemory::allocPreferPsram, no
//     hard cap beyond available memory.
//   - Saves go through Filesystem::writeFileAtomic for power-cut safety.
//
// Character entry routes through the existing TextInputScreen keyboard:
// pressing A pushes the keyboard, the submit callback splices the typed
// string into the row at the cursor.
//
// Y opens a small action menu (Save, Save As, New Line, Delete Line,
// Goto Line, Run for .py files).

class EditorRow;  // private impl detail; lives in the .cpp

class EditorScreen : public Screen {
 public:
  EditorScreen();
  ~EditorScreen() override;

  // Read `path` into the editor in normal (existing-file) mode. Path
  // must already exist. Empty / missing → 1 empty row, marked dirty so
  // the user is prompted to save on exit.
  void loadFile(const char* path);

  // Open the editor in "new file" mode — empty buffer, `path` stored
  // as the save target. Used by FilesScreen's "[+ New file]" row.
  void loadNewFile(const char* path);

  // ScreenAccess: kPairedOnly is fine; same as FilesScreen.
  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void onResume(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenEditor; }
  bool showCursor() const override { return false; }

  // Public so the TextInputScreen submit trampoline can call back in
  // with the typed string after the keyboard pops.
  void onTextSubmit(const char* text);
  // Public so the action-menu trampoline can dispatch verbs.
  enum class Action : uint8_t {
    kNone, kSave, kSaveAs, kNewLine, kDeleteLine, kGotoLine, kRun,
  };
  void doAction(Action action, GUIManager& gui);

 private:
  // ── Rows (eKilo's EditorRow, simplified) ───────────────────────────
  //
  // `chars` is the live text (no trailing \0 stored, but we keep one
  //   for c-str safety; `size` is the byte count without it).
  // `render` is identical to `chars` for now (we don't expand tabs or
  //   apply any visual transform).
  //
  // Both buffers live in PSRAM via BadgeMemory::allocPreferPsram.
  EditorRow* rows_ = nullptr;
  int rowCount_ = 0;
  int rowCapacity_ = 0;

  // Cursor in document coordinates (cy = row index, cx = byte index
  // within that row). cx may be == rows_[cy].size when the cursor sits
  // at end-of-line.
  int cy_ = 0;
  int cx_ = 0;

  // Scroll offsets — top-left of the visible viewport in document
  // coordinates. coloff_ is byte index within the cursor's row.
  int rowoff_ = 0;
  int coloff_ = 0;

  // Render geometry, recomputed in render() based on the actual font
  // metrics. Conservative defaults match FONT_TINY on 128x64.
  uint8_t screenRows_ = 6;
  uint8_t screenCols_ = 21;

  bool dirty_ = false;

  // Path the editor saves to. Empty string == ask via Save As.
  char path_[96] = {};

  // Bottom status / message line — flashed after save / errors. Cleared
  // automatically after kStatusFadeMs.
  char statusMsg_[40] = {};
  uint32_t statusMsgMs_ = 0;
  static constexpr uint32_t kStatusFadeMs = 2500;

  // Edit modal state — pending action from the Y-menu, plus a tiny
  // staging buffer the keyboard writes into.
  enum class PendingPrompt : uint8_t {
    kNone,
    kInsertText,    // A: keyboard splices into current row
    kSaveAs,        // Y-menu: keyboard captures new path
    kGotoLine,      // Y-menu: keyboard captures line number
  };
  PendingPrompt pending_ = PendingPrompt::kNone;
  char inputBuf_[96] = {};

  // Y-menu state. While `actionMenuOpen_` is true, render() draws a
  // small overlay and handleInput() drives selection within it instead
  // of the editor body.
  bool actionMenuOpen_ = false;
  uint8_t actionMenuCursor_ = 0;

  // Save-on-exit confirm modal. 0 = Save, 1 = Discard, 2 = Cancel.
  bool saveModalOpen_ = false;
  uint8_t saveModalCursor_ = 0;

  // Joystick repeat trackers.
  uint32_t lastJoyVMs_ = 0;
  uint32_t lastJoyHMs_ = 0;

  // ── Row primitives (eKilo) ────────────────────────────────────────
  void rowsClear();
  bool ensureRowCapacity(int need);
  void rowAppendString(EditorRow& row, const char* s, int len);
  void rowInsertChar(EditorRow& row, int at, char c);
  void rowDeleteChar(EditorRow& row, int at);
  void rowFreeBuffers(EditorRow& row);
  bool rowResize(EditorRow& row, int newSize);

  void editorInsertRow(int at, const char* s, int len);
  void editorDelRow(int at);
  void editorInsertChar(char c);
  void editorInsertNewline();
  void editorDelChar();
  void editorAppendString(const char* s, int len);

  // Cursor / scroll bookkeeping.
  void clampCursorCol();
  void scrollIntoView();
  void cursorMoveLeft();
  void cursorMoveRight();
  void cursorMoveUp();
  void cursorMoveDown();
  void cursorPageUp();
  void cursorPageDown();

  // Save through Filesystem::writeFileAtomic. Concatenates rows with
  // '\n' into a PSRAM scratch buffer, writes, frees, sets dirty_=false.
  bool save();

  // Status line setter — buf is copied; pass nullptr to clear.
  void setStatus(const char* fmt, ...);

  // Draw helpers.
  void renderHeader(oled& d);
  void renderBody(oled& d);
  void renderStatus(oled& d);
  void renderActionMenu(oled& d);
  void renderSaveModal(oled& d);

  // Push the keyboard pre-loaded with `prefill`. After the keyboard
  // submits, `pending_` decides what we do with the typed text.
  void pushKeyboard(const char* title, const char* prefill,
                    PendingPrompt next, GUIManager& gui);

  // True iff the current file is a .py and "Run" should be available.
  bool isPython() const;
};
