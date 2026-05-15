#pragma once
#include "Screen.h"

// ─── Files screen (recursive FAT browser with collapsible directories) ──────
//
// Walks the FAT root once into a flat array of `Entry`s tagged with a
// depth and (for directories) an `expanded` flag. `itemCount()` only
// counts entries whose ancestor chain is fully expanded, mirroring the
// SettingsScreen `kGroups` / `activeGroup_` pattern but generalized to
// arbitrary depth.
//
// Selection on a directory toggles its `expanded` flag and (when
// collapsing) shifts `cursor_` / `scroll_` so they don't fall into the
// hidden child range — same fixup as `SettingsScreen::maybeAutoCollapse`.
//
// Selection on a file dispatches through `FileOpener::open(path, gui)`.
//
// A synthetic "[+ New file]" row at the very top opens the keyboard,
// then drops the typed name onto `EditorScreen` in new-file mode.
//
// Storage:
//   `entries_` is allocated from PSRAM via `BadgeMemory::allocPreferPsram`.
//   `kMaxEntries = 256` keeps a deep tree in scope without unbounded RAM.

class FilesScreen : public ListMenuScreen {
 public:
  FilesScreen(ScreenId sid);

  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  void onItemSelect(uint8_t index, GUIManager& gui) override;
  void onEnter(GUIManager& gui) override;
  void onResume(GUIManager& gui) override;
  bool navigableItems() const override { return true; }
  const char* hintText() const override;

  // Entry-point for the "[+ New file]" trampoline. Public so the
  // TextInputScreen submit callback (defined in FilesScreen.cpp) can
  // call back into us once the user types a name.
  void onNewFileSubmit(const char* name, GUIManager& gui);

 private:
  enum class EntryKind : uint8_t { kFile, kDir };

  // Each row in the flat tree. `depth` is the indent level (0 = root).
  // `parent` is the index of the directory entry this child lives in,
  // or -1 for top-level entries. `expanded` is only meaningful for
  // directories; files leave it at false.
  struct Entry {
    char name[28];
    uint32_t size;
    int16_t parent;     // index into entries_ or -1 (root)
    uint8_t depth;      // 0 .. kMaxDepth
    EntryKind kind;
    bool expanded;
  };

  // Storage limits — generous given each Entry is ~36 bytes.
  static constexpr uint16_t kMaxEntries = 256;
  static constexpr uint8_t kMaxDepth = 6;

  Entry* entries_ = nullptr;
  uint16_t entryCount_ = 0;

  // True after `entries_` has been allocated at least once. Lazily on
  // first `onEnter` so we don't burn PSRAM until the user opens Files.
  bool storageReady_ = false;

  // Cached count of currently-visible rows (those whose ancestor chain
  // is fully expanded), refreshed on every structural change.
  mutable uint8_t visibleCount_ = 0;

  bool ensureStorage();
  void rescan();
  void scanDir(const char* path, int16_t parentIdx, uint8_t depth);
  void recountVisible() const;
  bool isVisible(uint16_t idx) const;
  // Index of the Nth visible row, or kMaxEntries if out of range.
  uint16_t visibleAt(uint8_t visiblePos) const;
  // Build full path "/foo/bar/name" for the entry at `idx` into out.
  void buildPath(uint16_t idx, char* out, size_t outCap) const;
  // Children-only descendant range of a directory entry. Returns the
  // count of (recursively) descendant entries; they are always
  // contiguous starting at `dirIdx + 1`.
  uint16_t descendantCount(uint16_t dirIdx) const;
  // Visible-row count within the descendant range (used to fix up
  // cursor/scroll on collapse, mirroring SettingsScreen).
  uint8_t visibleDescendantCount(uint16_t dirIdx) const;
  // Mark `dirIdx` as collapsed AND clear `expanded` on every nested
  // descendant so their children re-collapse next time too.
  void collapseSubtree(uint16_t dirIdx);

  // True when the cursor is on the synthetic "+ New file" row.
  bool isNewFileRow(uint8_t cursor) const;
};
