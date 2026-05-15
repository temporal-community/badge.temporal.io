#include "FilesScreen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"
#include "../ui/GUI.h"
#include "FileOpener.h"
#include "EditorScreen.h"
#include "TextInputScreen.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

extern TextInputScreen sTextInput;
extern EditorScreen sEditor;

// ─── New-file trampoline (TextInputScreen submit → FilesScreen) ────────────
//
// TextInputScreen's submit signature is `void(const char*, void*)`, so
// we route through a free function that forwards to the FilesScreen
// instance saved in the `user` slot.

namespace {
char g_newFileBuf[64];
GUIManager* g_pendingNewFileGui = nullptr;

void newFileSubmitTrampoline(const char* text, void* user) {
  FilesScreen* self = static_cast<FilesScreen*>(user);
  if (!self || !g_pendingNewFileGui) return;
  self->onNewFileSubmit(text, *g_pendingNewFileGui);
  g_pendingNewFileGui = nullptr;
}
}  // namespace

FilesScreen::FilesScreen(ScreenId sid) : ListMenuScreen(sid, "FILES") {}

void FilesScreen::onEnter(GUIManager& gui) {
  ListMenuScreen::onEnter(gui);
  rescan();
}

// Don't rebuild the tree on resume — preserves expanded state when
// popping back from the editor / hex / image viewer.
void FilesScreen::onResume(GUIManager& /*gui*/) {
  recountVisible();
  if (visibleCount_ == 0) {
    cursor_ = 0;
    scroll_ = 0;
  } else if (cursor_ >= visibleCount_ + 1 /* synthetic +new row */) {
    cursor_ = visibleCount_;  // last visible row
  }
}

uint8_t FilesScreen::itemCount() const {
  recountVisible();
  // +1 for the synthetic "[+ New file]" row pinned at the top.
  return visibleCount_ + 1;
}

bool FilesScreen::isNewFileRow(uint8_t cursor) const {
  return cursor == 0;
}

void FilesScreen::formatItem(uint8_t index, char* buf,
                             uint8_t bufSize) const {
  if (bufSize == 0) return;
  if (isNewFileRow(index)) {
    std::snprintf(buf, bufSize, "[+ New file]");
    return;
  }

  const uint16_t entryIdx = visibleAt(index - 1);
  if (entryIdx >= entryCount_) {
    buf[0] = '\0';
    return;
  }
  const Entry& e = entries_[entryIdx];

  // Indent by depth*2 spaces.
  char indent[16];
  uint8_t pad = e.depth * 2;
  if (pad >= sizeof(indent)) pad = sizeof(indent) - 1;
  for (uint8_t i = 0; i < pad; i++) indent[i] = ' ';
  indent[pad] = '\0';

  if (e.kind == EntryKind::kDir) {
    const char* glyph = e.expanded ? "v" : ">";
    std::snprintf(buf, bufSize, "%s%s %s/", indent, glyph, e.name);
    return;
  }

  // File row: name + right-aligned size, fitting in ~21 tiny-font cols.
  char sizeBuf[8];
  if (e.size < 1024)
    std::snprintf(sizeBuf, sizeof(sizeBuf), "%luB",
                  (unsigned long)e.size);
  else
    std::snprintf(sizeBuf, sizeof(sizeBuf), "%luK",
                  (unsigned long)(e.size / 1024));

  const int kRowCols = 21;
  int nameLen = static_cast<int>(strlen(e.name));
  int sizeLen = static_cast<int>(strlen(sizeBuf));
  int gap = kRowCols - pad - nameLen - sizeLen;
  if (gap < 1) gap = 1;
  std::snprintf(buf, bufSize, "%s%s%*s%s", indent, e.name, gap, "",
                sizeBuf);
}

const char* FilesScreen::hintText() const {
  if (isNewFileRow(cursor_)) return "a:back b:new";
  if (visibleCount_ == 0) return "b:back";
  const uint16_t entryIdx = visibleAt(cursor_ - 1);
  if (entryIdx < entryCount_ &&
      entries_[entryIdx].kind == EntryKind::kDir) {
    return entries_[entryIdx].expanded ? "a:back b:hide "
                                       : "a:back b:show ";
  }
  return "a:back b:open";
}

void FilesScreen::onItemSelect(uint8_t index, GUIManager& gui) {
  if (isNewFileRow(index)) {
    g_newFileBuf[0] = '\0';
    g_pendingNewFileGui = &gui;
    sTextInput.configure("New file name", g_newFileBuf,
                         sizeof(g_newFileBuf), &newFileSubmitTrampoline,
                         this);
    gui.pushScreen(kScreenTextInput);
    return;
  }

  const uint16_t entryIdx = visibleAt(index - 1);
  if (entryIdx >= entryCount_) return;
  Entry& e = entries_[entryIdx];

  if (e.kind == EntryKind::kDir) {
    if (e.expanded) {
      // Collapse — apply Settings-style cursor / scroll fixup so the
      // cursor doesn't fall into the now-hidden child range. The
      // currently-selected row IS the directory header, so cursor_
      // itself stays put; only rows after the subtree shift up.
      const uint8_t hiddenChildren = visibleDescendantCount(entryIdx);
      collapseSubtree(entryIdx);
      // Header row position equals the current cursor.
      const uint8_t headerPos = index;
      const uint8_t subtreeEndPos = headerPos + 1 + hiddenChildren;
      if (scroll_ >= subtreeEndPos) {
        scroll_ -= hiddenChildren;
      } else if (scroll_ > headerPos) {
        scroll_ = headerPos;
      }
      const uint8_t total = itemCount();
      if (total > 0 && cursor_ >= total) cursor_ = total - 1;
      if (scroll_ > cursor_) scroll_ = cursor_;
    } else {
      e.expanded = true;
      recountVisible();
    }
    return;
  }

  // File: dispatch through the opener.
  char path[128];
  buildPath(entryIdx, path, sizeof(path));
  FileOpener::open(path, gui);
}

void FilesScreen::onNewFileSubmit(const char* name, GUIManager& gui) {
  if (!name || !name[0]) return;
  // Pre-strip leading slashes the user might type.
  while (*name == '/') name++;
  if (!*name) return;
  char path[128];
  std::snprintf(path, sizeof(path), "/%s", name);
  // Open the editor in new-file mode (loadFile with empty content).
  sEditor.loadNewFile(path);
  gui.pushScreen(kScreenEditor);
}

// ─── Storage / scan ─────────────────────────────────────────────────────────

bool FilesScreen::ensureStorage() {
  if (storageReady_ && entries_) return true;
  entries_ = static_cast<Entry*>(
      BadgeMemory::allocPreferPsram(kMaxEntries * sizeof(Entry)));
  if (!entries_) {
    Serial.println("[Files] entry buffer alloc failed");
    return false;
  }
  storageReady_ = true;
  return true;
}

void FilesScreen::rescan() {
  if (!ensureStorage()) {
    entryCount_ = 0;
    visibleCount_ = 0;
    return;
  }
  entryCount_ = 0;
  scanDir("/", -1, 0);
  recountVisible();
  if (visibleCount_ + 1 > 0 && cursor_ > visibleCount_) {
    cursor_ = visibleCount_;
  }
}

// Recursive descent. To make a directory's children sit *contiguously
// immediately after* the parent entry (so expand drops kids inline
// under the folder, not at the bottom of the level), we snapshot one
// directory level into a PSRAM scratch buffer, close the FATFS dir
// handle, then emit each dir followed by its full subtree before
// emitting any sibling files. `descendantCount()` and the visibility
// walk both rely on this ordering invariant.
void FilesScreen::scanDir(const char* path, int16_t parentIdx,
                          uint8_t depth) {
  if (depth > kMaxDepth) return;
  if (entryCount_ >= kMaxEntries) return;

  // Snapshot the level into a local PSRAM buffer so we can release the
  // FATFS dir handle before recursing (each scanDir call opens its own
  // handle; keeping the parent's open across recursion would burn FATFS
  // dir slots and complicate error paths).
  struct LevelEntry {
    char name[28];
    uint32_t size;
    bool isDir;
  };
  constexpr uint16_t kMaxPerLevel = 64;
  LevelEntry* snap = static_cast<LevelEntry*>(
      BadgeMemory::allocPreferPsram(kMaxPerLevel * sizeof(LevelEntry)));
  if (!snap) return;
  uint16_t snapCount = 0;

  {
    Filesystem::IOLock fsLock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) {
      free(snap);
      return;
    }
    FF_DIR dir;
    if (f_opendir(fs, &dir, path) != FR_OK) {
      free(snap);
      return;
    }
    FILINFO fno;
    while (snapCount < kMaxPerLevel) {
      if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
      if (fno.fname[0] == '.') continue;  // skip ./.. and dotfiles
      LevelEntry& s = snap[snapCount++];
      strncpy(s.name, fno.fname, sizeof(s.name) - 1);
      s.name[sizeof(s.name) - 1] = '\0';
      s.isDir = (fno.fattrib & AM_DIR) != 0;
      s.size = static_cast<uint32_t>(fno.fsize);
    }
    f_closedir(&dir);
  }

  // Pass 1: emit each directory header and immediately recurse into
  // it so its descendants land directly under the header. This is what
  // makes expansion show children inline rather than scrolling to the
  // bottom of the level.
  for (uint16_t i = 0; i < snapCount; i++) {
    if (!snap[i].isDir) continue;
    if (entryCount_ >= kMaxEntries) break;
    uint16_t myIdx = entryCount_;
    Entry& e = entries_[entryCount_++];
    strncpy(e.name, snap[i].name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.size = 0;
    e.parent = parentIdx;
    e.depth = depth;
    e.kind = EntryKind::kDir;
    e.expanded = false;

    char childPath[256];
    if (path[0] == '/' && path[1] == '\0') {
      std::snprintf(childPath, sizeof(childPath), "/%s", snap[i].name);
    } else {
      std::snprintf(childPath, sizeof(childPath), "%s/%s", path,
                    snap[i].name);
    }
    scanDir(childPath, static_cast<int16_t>(myIdx), depth + 1);
  }

  // Pass 2: emit sibling files after all sibling-directory subtrees,
  // so files appear below folders within each level.
  for (uint16_t i = 0; i < snapCount; i++) {
    if (snap[i].isDir) continue;
    if (entryCount_ >= kMaxEntries) break;
    Entry& e = entries_[entryCount_++];
    strncpy(e.name, snap[i].name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.size = snap[i].size;
    e.parent = parentIdx;
    e.depth = depth;
    e.kind = EntryKind::kFile;
    e.expanded = false;
  }

  free(snap);
}

// ─── Visibility / path helpers ──────────────────────────────────────────────

bool FilesScreen::isVisible(uint16_t idx) const {
  if (idx >= entryCount_) return false;
  // Walk up the parent chain; an entry is visible only if every
  // ancestor directory is expanded.
  int16_t p = entries_[idx].parent;
  while (p >= 0) {
    if (!entries_[p].expanded) return false;
    p = entries_[p].parent;
  }
  return true;
}

void FilesScreen::recountVisible() const {
  uint8_t count = 0;
  for (uint16_t i = 0; i < entryCount_; i++) {
    if (isVisible(i)) count++;
  }
  visibleCount_ = count;
}

uint16_t FilesScreen::visibleAt(uint8_t visiblePos) const {
  uint8_t seen = 0;
  for (uint16_t i = 0; i < entryCount_; i++) {
    if (!isVisible(i)) continue;
    if (seen == visiblePos) return i;
    seen++;
  }
  return kMaxEntries;
}

void FilesScreen::buildPath(uint16_t idx, char* out, size_t outCap) const {
  if (idx >= entryCount_ || outCap == 0) {
    if (out && outCap) out[0] = '\0';
    return;
  }
  // Walk parent chain to root, push names in reverse, then join.
  const Entry* chain[kMaxDepth + 2] = {};
  uint8_t depth = 0;
  int16_t cur = static_cast<int16_t>(idx);
  while (cur >= 0 && depth < (sizeof(chain) / sizeof(chain[0]))) {
    chain[depth++] = &entries_[cur];
    cur = entries_[cur].parent;
  }
  size_t pos = 0;
  out[0] = '\0';
  for (int8_t i = depth - 1; i >= 0; i--) {
    int n = std::snprintf(out + pos, outCap - pos, "/%s", chain[i]->name);
    if (n <= 0 || (size_t)n >= outCap - pos) {
      out[outCap - 1] = '\0';
      return;
    }
    pos += n;
  }
}

uint16_t FilesScreen::descendantCount(uint16_t dirIdx) const {
  if (dirIdx >= entryCount_) return 0;
  // Children are contiguous after dirIdx; the range ends when we hit
  // an entry whose ancestor chain doesn't include dirIdx.
  uint16_t count = 0;
  for (uint16_t i = dirIdx + 1; i < entryCount_; i++) {
    int16_t p = entries_[i].parent;
    bool isDescendant = false;
    while (p >= 0) {
      if (static_cast<uint16_t>(p) == dirIdx) {
        isDescendant = true;
        break;
      }
      p = entries_[p].parent;
    }
    if (!isDescendant) break;
    count++;
  }
  return count;
}

uint8_t FilesScreen::visibleDescendantCount(uint16_t dirIdx) const {
  if (dirIdx >= entryCount_) return 0;
  uint16_t total = descendantCount(dirIdx);
  uint8_t visible = 0;
  for (uint16_t i = dirIdx + 1; i < dirIdx + 1 + total; i++) {
    if (isVisible(i)) visible++;
  }
  return visible;
}

void FilesScreen::collapseSubtree(uint16_t dirIdx) {
  if (dirIdx >= entryCount_) return;
  uint16_t total = descendantCount(dirIdx);
  for (uint16_t i = dirIdx; i < dirIdx + 1 + total; i++) {
    if (entries_[i].kind == EntryKind::kDir) {
      entries_[i].expanded = false;
    }
  }
  recountVisible();
}
