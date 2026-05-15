#include "EditorScreen.h"

#include <Arduino.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>

#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/UIFonts.h"
#include "TextInputScreen.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
void mpy_gui_exec_file(const char* path);
}

extern TextInputScreen sTextInput;

// ─── EditorRow ──────────────────────────────────────────────────────────────
//
// One line of text. `chars` holds raw line content (no trailing newline);
// `size` is its byte length. Buffers live in PSRAM.

class EditorRow {
 public:
  char* chars = nullptr;
  int size = 0;
  int capacity = 0;
};

namespace {

// PSRAM-preferred (re)allocator. malloc fallback for tiny buffers
// matches BadgeMemory::PsramAllocator::reallocate.
void* psRealloc(void* p, size_t newSize) {
  if (!p) return BadgeMemory::allocPreferPsram(newSize);
  if (newSize == 0) {
    free(p);
    return nullptr;
  }
  void* np = heap_caps_realloc(p, newSize,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!np && newSize <= 4096) {
    np = realloc(p, newSize);
  }
  return np;
}

// Editor render area within the 128x64 panel.
constexpr int kHeaderH    = 9;   // title rule sits at y=8
constexpr int kFooterTopY = 54;  // matches OLEDLayout::kFooterTopY
constexpr int kBodyTopY   = kHeaderH + 1;
constexpr int kStatusY    = kFooterTopY - 1;  // tiny status strip
constexpr int kBodyBotY   = kStatusY - 1;

// The keyboard-trampoline shim. `TextInputScreen::Submit` is a free
// function pointer; we need a route from there back to a specific
// EditorScreen instance. Stash a pointer when we push the keyboard.
EditorScreen* g_pendingEditor = nullptr;
GUIManager* g_pendingEditorGui = nullptr;

void editorTextSubmitTrampoline(const char* text, void* user) {
  EditorScreen* self = static_cast<EditorScreen*>(user);
  if (!self) return;
  self->onTextSubmit(text);
  g_pendingEditor = nullptr;
  g_pendingEditorGui = nullptr;
}

}  // namespace

EditorScreen::EditorScreen() = default;

EditorScreen::~EditorScreen() { rowsClear(); }

// ─── Row buffer management ──────────────────────────────────────────────────

void EditorScreen::rowFreeBuffers(EditorRow& row) {
  if (row.chars) free(row.chars);
  row.chars = nullptr;
  row.size = 0;
  row.capacity = 0;
}

void EditorScreen::rowsClear() {
  if (rows_) {
    for (int i = 0; i < rowCount_; i++) rowFreeBuffers(rows_[i]);
    free(rows_);
  }
  rows_ = nullptr;
  rowCount_ = 0;
  rowCapacity_ = 0;
  cy_ = cx_ = 0;
  rowoff_ = coloff_ = 0;
  dirty_ = false;
}

bool EditorScreen::ensureRowCapacity(int need) {
  if (need <= rowCapacity_) return true;
  int newCap = rowCapacity_ == 0 ? 16 : rowCapacity_;
  while (newCap < need) newCap *= 2;
  EditorRow* nr = static_cast<EditorRow*>(
      psRealloc(rows_, newCap * sizeof(EditorRow)));
  if (!nr) return false;
  // Zero-init the freshly grown tail.
  for (int i = rowCapacity_; i < newCap; i++) {
    nr[i].chars = nullptr;
    nr[i].size = 0;
    nr[i].capacity = 0;
  }
  rows_ = nr;
  rowCapacity_ = newCap;
  return true;
}

bool EditorScreen::rowResize(EditorRow& row, int newSize) {
  // Always keep room for a trailing NUL (handy for c-str access).
  int need = newSize + 1;
  if (need <= row.capacity) {
    row.size = newSize;
    if (row.chars) row.chars[newSize] = '\0';
    return true;
  }
  int newCap = row.capacity == 0 ? 32 : row.capacity;
  while (newCap < need) newCap *= 2;
  char* nb = static_cast<char*>(psRealloc(row.chars, newCap));
  if (!nb) return false;
  row.chars = nb;
  row.capacity = newCap;
  row.size = newSize;
  row.chars[newSize] = '\0';
  return true;
}

void EditorScreen::rowAppendString(EditorRow& row, const char* s, int len) {
  if (len <= 0) return;
  int oldSize = row.size;
  if (!rowResize(row, oldSize + len)) return;
  std::memcpy(row.chars + oldSize, s, len);
}

void EditorScreen::rowInsertChar(EditorRow& row, int at, char c) {
  if (at < 0) at = 0;
  if (at > row.size) at = row.size;
  int oldSize = row.size;
  if (!rowResize(row, oldSize + 1)) return;
  if (at < oldSize) {
    std::memmove(row.chars + at + 1, row.chars + at, oldSize - at);
  }
  row.chars[at] = c;
}

void EditorScreen::rowDeleteChar(EditorRow& row, int at) {
  if (at < 0 || at >= row.size) return;
  std::memmove(row.chars + at, row.chars + at + 1, row.size - at - 1);
  rowResize(row, row.size - 1);
}

// ─── Document-level edit ops ────────────────────────────────────────────────

void EditorScreen::editorInsertRow(int at, const char* s, int len) {
  if (at < 0 || at > rowCount_) return;
  if (!ensureRowCapacity(rowCount_ + 1)) return;
  if (at < rowCount_) {
    std::memmove(&rows_[at + 1], &rows_[at],
                 sizeof(EditorRow) * (rowCount_ - at));
  }
  rows_[at].chars = nullptr;
  rows_[at].size = 0;
  rows_[at].capacity = 0;
  if (len > 0) rowAppendString(rows_[at], s, len);
  rowCount_++;
  dirty_ = true;
}

void EditorScreen::editorDelRow(int at) {
  if (at < 0 || at >= rowCount_) return;
  rowFreeBuffers(rows_[at]);
  if (at < rowCount_ - 1) {
    std::memmove(&rows_[at], &rows_[at + 1],
                 sizeof(EditorRow) * (rowCount_ - at - 1));
  }
  rowCount_--;
  // Zero the slot we vacated so a later resize doesn't trip on stale
  // pointer state.
  rows_[rowCount_].chars = nullptr;
  rows_[rowCount_].size = 0;
  rows_[rowCount_].capacity = 0;
  dirty_ = true;
}

void EditorScreen::editorInsertChar(char c) {
  if (cy_ >= rowCount_) {
    editorInsertRow(rowCount_, "", 0);
  }
  rowInsertChar(rows_[cy_], cx_, c);
  cx_++;
  dirty_ = true;
}

void EditorScreen::editorInsertNewline() {
  if (cx_ == 0) {
    editorInsertRow(cy_, "", 0);
  } else {
    EditorRow& row = rows_[cy_];
    // Snapshot the tail before mutating; rowResize may move the buffer.
    int tailLen = row.size - cx_;
    char* tail = nullptr;
    if (tailLen > 0) {
      tail = static_cast<char*>(BadgeMemory::allocPreferPsram(tailLen));
      if (tail) std::memcpy(tail, row.chars + cx_, tailLen);
    }
    rowResize(row, cx_);
    editorInsertRow(cy_ + 1, tail ? tail : "", tailLen > 0 ? tailLen : 0);
    if (tail) free(tail);
  }
  cy_++;
  cx_ = 0;
  dirty_ = true;
}

void EditorScreen::editorDelChar() {
  if (cy_ >= rowCount_) return;
  if (cx_ == 0 && cy_ == 0) return;
  if (cx_ > 0) {
    rowDeleteChar(rows_[cy_], cx_ - 1);
    cx_--;
  } else {
    // Join with previous line.
    int prevLen = rows_[cy_ - 1].size;
    if (rows_[cy_].size > 0) {
      rowAppendString(rows_[cy_ - 1], rows_[cy_].chars, rows_[cy_].size);
    }
    editorDelRow(cy_);
    cy_--;
    cx_ = prevLen;
  }
  dirty_ = true;
}

void EditorScreen::editorAppendString(const char* s, int len) {
  for (int i = 0; i < len; i++) {
    if (s[i] == '\n')
      editorInsertNewline();
    else if (s[i] == '\r')
      continue;  // ignore CR (CRLF normalization)
    else
      editorInsertChar(s[i]);
  }
}

// ─── loadFile / save ────────────────────────────────────────────────────────

void EditorScreen::loadFile(const char* path) {
  rowsClear();
  std::strncpy(path_, path ? path : "", sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';

  if (!path || !path[0]) {
    editorInsertRow(0, "", 0);
    dirty_ = false;
    return;
  }

  // Stream the file in chunks into a PSRAM scratch buffer, then split
  // on '\n'. We do this in a single read where possible so the row
  // splitter is uncomplicated.
  Filesystem::IOLock lock;
  FATFS* fs = replay_get_fatfs();
  if (!fs) {
    editorInsertRow(0, "", 0);
    setStatus("FS unavailable");
    return;
  }
  FIL fil;
  if (f_open(fs, &fil, path, FA_READ) != FR_OK) {
    editorInsertRow(0, "", 0);
    setStatus("Open failed");
    return;
  }

  UINT total = f_size(&fil);
  char* buf = nullptr;
  if (total > 0) {
    buf = static_cast<char*>(BadgeMemory::allocPreferPsram(total + 1));
    if (!buf) {
      f_close(&fil);
      editorInsertRow(0, "", 0);
      setStatus("Alloc fail");
      return;
    }
    UINT got = 0;
    if (f_read(&fil, buf, total, &got) != FR_OK || got != total) {
      f_close(&fil);
      free(buf);
      editorInsertRow(0, "", 0);
      setStatus("Read failed");
      return;
    }
    buf[total] = '\0';
  }
  f_close(&fil);

  if (!buf || total == 0) {
    editorInsertRow(0, "", 0);
    if (buf) free(buf);
    dirty_ = false;
    return;
  }

  // Split on \n; strip optional \r before each \n (CRLF normalization).
  int lineStart = 0;
  for (UINT i = 0; i < total; i++) {
    if (buf[i] == '\n') {
      int lineEnd = (int)i;
      if (lineEnd > lineStart && buf[lineEnd - 1] == '\r') lineEnd--;
      editorInsertRow(rowCount_, buf + lineStart, lineEnd - lineStart);
      lineStart = (int)i + 1;
    }
  }
  // Trailing line without a terminator.
  if (lineStart < (int)total) {
    int lineEnd = (int)total;
    if (lineEnd > lineStart && buf[lineEnd - 1] == '\r') lineEnd--;
    editorInsertRow(rowCount_, buf + lineStart, lineEnd - lineStart);
  } else if (rowCount_ == 0) {
    editorInsertRow(0, "", 0);
  }
  free(buf);
  dirty_ = false;
}

void EditorScreen::loadNewFile(const char* path) {
  rowsClear();
  std::strncpy(path_, path ? path : "", sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';
  editorInsertRow(0, "", 0);
  dirty_ = true;  // unsaved by definition
}

bool EditorScreen::save() {
  if (!path_[0]) {
    setStatus("No path");
    return false;
  }

  // Compute total payload length: sum(row sizes) + (rowCount-1) for
  // newlines between rows.
  size_t total = 0;
  for (int i = 0; i < rowCount_; i++) total += rows_[i].size;
  if (rowCount_ > 0) total += (size_t)(rowCount_ - 1);

  char* buf = static_cast<char*>(
      BadgeMemory::allocPreferPsram(total ? total : 1));
  if (!buf) {
    setStatus("Alloc fail");
    return false;
  }
  size_t pos = 0;
  for (int i = 0; i < rowCount_; i++) {
    if (rows_[i].size > 0) {
      std::memcpy(buf + pos, rows_[i].chars, rows_[i].size);
      pos += rows_[i].size;
    }
    if (i + 1 < rowCount_) buf[pos++] = '\n';
  }
  bool ok = Filesystem::writeFileAtomic(path_, buf, pos);
  free(buf);
  if (ok) {
    dirty_ = false;
    setStatus("Saved %u bytes", (unsigned)pos);
  } else {
    setStatus("Save failed");
  }
  return ok;
}

// ─── Cursor / scroll ────────────────────────────────────────────────────────

void EditorScreen::clampCursorCol() {
  if (cy_ >= rowCount_) cy_ = rowCount_ - 1;
  if (cy_ < 0) cy_ = 0;
  int maxCol = (cy_ < rowCount_) ? rows_[cy_].size : 0;
  if (cx_ > maxCol) cx_ = maxCol;
  if (cx_ < 0) cx_ = 0;
}

void EditorScreen::scrollIntoView() {
  if (cy_ < rowoff_) rowoff_ = cy_;
  if (cy_ >= rowoff_ + screenRows_) rowoff_ = cy_ - screenRows_ + 1;
  if (cx_ < coloff_) coloff_ = cx_;
  if (cx_ >= coloff_ + screenCols_) coloff_ = cx_ - screenCols_ + 1;
  if (rowoff_ < 0) rowoff_ = 0;
  if (coloff_ < 0) coloff_ = 0;
}

void EditorScreen::cursorMoveLeft() {
  if (cx_ > 0) {
    cx_--;
  } else if (cy_ > 0) {
    cy_--;
    cx_ = rows_[cy_].size;
  }
}

void EditorScreen::cursorMoveRight() {
  if (cy_ >= rowCount_) return;
  if (cx_ < rows_[cy_].size) {
    cx_++;
  } else if (cy_ + 1 < rowCount_) {
    cy_++;
    cx_ = 0;
  }
}

void EditorScreen::cursorMoveUp() {
  if (cy_ > 0) {
    cy_--;
    clampCursorCol();
  }
}

void EditorScreen::cursorMoveDown() {
  if (cy_ + 1 < rowCount_) {
    cy_++;
    clampCursorCol();
  }
}

void EditorScreen::cursorPageUp() {
  cy_ -= screenRows_;
  if (cy_ < 0) cy_ = 0;
  clampCursorCol();
}

void EditorScreen::cursorPageDown() {
  cy_ += screenRows_;
  if (cy_ >= rowCount_) cy_ = rowCount_ - 1;
  if (cy_ < 0) cy_ = 0;
  clampCursorCol();
}

// ─── Status line ────────────────────────────────────────────────────────────

void EditorScreen::setStatus(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(statusMsg_, sizeof(statusMsg_), fmt, ap);
  va_end(ap);
  statusMsgMs_ = millis();
}

bool EditorScreen::isPython() const {
  size_t n = std::strlen(path_);
  if (n < 3) return false;
  const char* tail = path_ + n - 3;
  return tail[0] == '.' && (tail[1] | 0x20) == 'p' &&
         (tail[2] | 0x20) == 'y';
}

// ─── Screen lifecycle / render ──────────────────────────────────────────────

void EditorScreen::onEnter(GUIManager& /*gui*/) {
  pending_ = PendingPrompt::kNone;
  actionMenuOpen_ = false;
  actionMenuCursor_ = 0;
  saveModalOpen_ = false;
  saveModalCursor_ = 0;
  lastJoyVMs_ = lastJoyHMs_ = 0;
}

void EditorScreen::onExit(GUIManager& /*gui*/) {
  // Free everything; do NOT retain PSRAM after a back-pop.
  rowsClear();
  path_[0] = '\0';
  statusMsg_[0] = '\0';
}

void EditorScreen::onResume(GUIManager& gui) {
  // Returning from the keyboard: dispatch on `pending_`.
  switch (pending_) {
    case PendingPrompt::kInsertText:
      onTextSubmit(inputBuf_);
      break;
    case PendingPrompt::kSaveAs:
      if (inputBuf_[0]) {
        std::strncpy(path_, inputBuf_, sizeof(path_) - 1);
        path_[sizeof(path_) - 1] = '\0';
        save();
      }
      break;
    case PendingPrompt::kGotoLine: {
      int target = atoi(inputBuf_);
      if (target > 0 && target <= rowCount_) {
        cy_ = target - 1;
        cx_ = 0;
      }
      break;
    }
    default:
      break;
  }
  pending_ = PendingPrompt::kNone;
}

void EditorScreen::renderHeader(oled& d) {
  d.setDrawColor(1);
  d.setFontPreset(FONT_TINY);
  // Filename (left), dirty marker, then "Ln/Col" (right).
  char left[40];
  const char* name = path_[0] ? path_ : "[new]";
  // Trim to last path segment for compactness.
  const char* slash = std::strrchr(name, '/');
  if (slash && slash[1]) name = slash + 1;
  std::snprintf(left, sizeof(left), "%s%s", name, dirty_ ? "*" : "");
  d.drawStr(0, 7, left);

  char right[16];
  std::snprintf(right, sizeof(right), "%d:%d", cy_ + 1, cx_ + 1);
  int rw = d.getStrWidth(right);
  d.drawStr(128 - rw, 7, right);
  d.drawHLine(0, 8, 128);
}

void EditorScreen::renderBody(oled& d) {
  d.setFontPreset(FONT_TINY);
  d.setDrawColor(1);
  d.setTextWrap(false);

  // Row geometry — 7 px per text line gives us 6 visible rows in the
  // 44-px body band (kBodyTopY..kBodyBotY).
  const int rowH = 7;
  const int bodyH = kBodyBotY - kBodyTopY;
  screenRows_ = (uint8_t)(bodyH / rowH);
  if (screenRows_ < 1) screenRows_ = 1;

  // `smallsimple_tr` is proportional, so we can't predict columns by a
  // single divisor. Instead grow each visible slice greedily until one
  // more glyph would overflow 128 px — gives us 30+ chars per row in
  // practice while never clipping the right edge.
  //
  // We still keep `screenCols_` as a worst-case upper bound for cursor
  // bookkeeping (scrollIntoView relies on it). Use the NARROWEST common
  // glyph as the cell estimate so the cap doesn't under-count.
  int cellW = d.getStrWidth("i");
  if (cellW < 3) cellW = 3;
  screenCols_ = (uint8_t)(128 / cellW);
  if (screenCols_ < 16) screenCols_ = 16;
  if (screenCols_ > 60) screenCols_ = 60;

  scrollIntoView();

  // Pre-compute per-row visible spans so the cursor renders aligned to
  // the same widths drawText uses. We also stash the visible-end index
  // for the cursor row so the cursor cell can hop one cell past the
  // last character (end-of-line caret).
  int cursorRowSliceEnd = coloff_;

  for (uint8_t r = 0; r < screenRows_; r++) {
    int docRow = rowoff_ + r;
    if (docRow >= rowCount_) break;
    const EditorRow& row = rows_[docRow];
    int y = kBodyTopY + r * rowH + 6;  // baseline

    int start = coloff_;
    if (start > row.size) continue;
    int avail = row.size - start;
    if (avail < 0) avail = 0;

    char tmp[80];
    int slice = 0;
    int width = 0;
    char one[2] = {0, 0};
    while (slice < avail && slice < (int)sizeof(tmp) - 1) {
      one[0] = row.chars[start + slice];
      int w = d.getStrWidth(one);
      // Approximate kerning by using the actual delta when appending:
      // measure the candidate string instead of summing per-char widths
      // (cheap enough for ~30-char rows).
      tmp[slice] = one[0];
      tmp[slice + 1] = '\0';
      int newWidth = d.getStrWidth(tmp);
      if (newWidth > 128) {
        tmp[slice] = '\0';
        break;
      }
      slice++;
      width = newWidth;
      (void)w;
    }
    tmp[slice] = '\0';
    d.drawStr(0, y, tmp);
    if (docRow == cy_) cursorRowSliceEnd = start + slice;
  }

  // Cursor cell — measure the prefix width up to the cursor column to
  // place it precisely on the same grid the text uses, accounting for
  // proportional glyph widths.
  if (cy_ >= rowoff_ && cy_ < rowoff_ + screenRows_) {
    int rowVis = cy_ - rowoff_;
    const EditorRow& row = rows_[cy_];
    int cellX = 0;
    int caretCol = cx_;
    if (caretCol < coloff_) caretCol = coloff_;
    if (caretCol > cursorRowSliceEnd) caretCol = cursorRowSliceEnd;
    int prefixLen = caretCol - coloff_;
    if (prefixLen > 0 && coloff_ < row.size) {
      char buf[80];
      int n = prefixLen;
      if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
      std::memcpy(buf, row.chars + coloff_, n);
      buf[n] = '\0';
      cellX = d.getStrWidth(buf);
    }
    int caretW = cellW;
    if (caretCol < row.size) {
      char one[2] = {row.chars[caretCol], '\0'};
      caretW = d.getStrWidth(one);
      if (caretW < cellW) caretW = cellW;
    }
    int cellY = kBodyTopY + rowVis * rowH;
    d.setDrawColor(2);
    d.drawBox(cellX, cellY, caretW, rowH - 1);
    d.setDrawColor(1);
  }
}

void EditorScreen::renderStatus(oled& d) {
  if (!statusMsg_[0]) return;
  if (millis() - statusMsgMs_ > kStatusFadeMs) {
    statusMsg_[0] = '\0';
    return;
  }
  d.setFontPreset(FONT_TINY);
  d.setDrawColor(1);
  d.drawHLine(0, kStatusY - 7, 128);
  d.drawStr(0, kStatusY, statusMsg_);
}

void EditorScreen::renderActionMenu(oled& d) {
  // Small bottom-anchored modal with the action verbs.
  static const char* kLabels[] = {
      "Save", "Save As", "Goto Line", "Delete Line", "New Line", "Run",
  };
  const uint8_t kCount = isPython() ? 6 : 5;

  const int boxW = 80;
  const int boxH = 9 * kCount + 4;
  const int boxX = 128 - boxW - 1;
  const int boxY = 64 - boxH - 1;
  d.setDrawColor(0);
  d.drawBox(boxX, boxY, boxW, boxH);
  d.setDrawColor(1);
  d.drawRFrame(boxX, boxY, boxW, boxH, 1);

  d.setFontPreset(FONT_TINY);
  for (uint8_t i = 0; i < kCount; i++) {
    int y = boxY + 2 + i * 9;
    if (i == actionMenuCursor_) {
      d.drawBox(boxX + 1, y, boxW - 2, 9);
      d.setDrawColor(0);
    }
    d.drawStr(boxX + 4, y + 7, kLabels[i]);
    d.setDrawColor(1);
  }
}

void EditorScreen::render(oled& d, GUIManager& /*gui*/) {
  renderHeader(d);
  renderBody(d);

  // Footer hint (always visible). Keep labels short + lowercase so the
  // chip row stays static — long labels marquee-scroll which is jarring
  // mid-edit.
  OLEDLayout::drawGameFooter(d);
  d.setFontPreset(FONT_TINY);
  OLEDLayout::drawFooterActions(d, "del", "menu", "esc", "type");

  renderStatus(d);
  if (actionMenuOpen_) renderActionMenu(d);
  if (saveModalOpen_) renderSaveModal(d);
}

void EditorScreen::renderSaveModal(oled& d) {
  // Centered 2-option prompt: Save / Don't save. B (cancel) dismisses
  // the modal entirely and returns to editing — that's the implicit
  // "stay" path, no third chip needed.
  const int boxW = 110;
  const int boxH = 34;
  const int boxX = (128 - boxW) / 2;
  const int boxY = (64 - boxH) / 2;

  d.setDrawColor(0);
  d.drawBox(boxX, boxY, boxW, boxH);
  d.setDrawColor(1);
  d.drawRFrame(boxX, boxY, boxW, boxH, 1);

  d.setFontPreset(FONT_TINY);
  const char* prompt = "Unsaved changes";
  int pw = d.getStrWidth(prompt);
  d.drawStr(boxX + (boxW - pw) / 2, boxY + 8, prompt);

  const char* hint = "B to keep editing";
  int hw = d.getStrWidth(hint);
  d.drawStr(boxX + (boxW - hw) / 2, boxY + boxH - 2, hint);

  static const char* kLabels[] = {"Save", "Don't save"};
  const int chipW = 46;
  const int chipH = 11;
  const int chipY = boxY + 13;
  const int chipGap = (boxW - chipW * 2) / 3;
  for (uint8_t i = 0; i < 2; i++) {
    int x = boxX + chipGap + i * (chipW + chipGap);
    if (i == saveModalCursor_) {
      d.setDrawColor(1);
      d.drawBox(x, chipY, chipW, chipH);
      d.setDrawColor(0);
    } else {
      d.setDrawColor(1);
      d.drawRFrame(x, chipY, chipW, chipH, 1);
    }
    int lw = d.getStrWidth(kLabels[i]);
    d.drawStr(x + (chipW - lw) / 2, chipY + 8, kLabels[i]);
    d.setDrawColor(1);
  }
}

// ─── Keyboard plumbing ──────────────────────────────────────────────────────

void EditorScreen::pushKeyboard(const char* title, const char* prefill,
                                PendingPrompt next, GUIManager& gui) {
  pending_ = next;
  std::strncpy(inputBuf_, prefill ? prefill : "", sizeof(inputBuf_) - 1);
  inputBuf_[sizeof(inputBuf_) - 1] = '\0';
  g_pendingEditor = this;
  g_pendingEditorGui = &gui;
  sTextInput.configure(title, inputBuf_, sizeof(inputBuf_),
                       &editorTextSubmitTrampoline, this);
  gui.pushScreen(kScreenTextInput);
}

void EditorScreen::onTextSubmit(const char* text) {
  if (!text || !text[0]) return;
  // Splice the typed string into the document at the cursor.
  editorAppendString(text, (int)std::strlen(text));
  // Don't clear inputBuf_ here — the keyboard owns it for as long as
  // it's the active screen. We'll start fresh on the next push.
}

// ─── Y-menu actions ─────────────────────────────────────────────────────────

void EditorScreen::doAction(Action action, GUIManager& gui) {
  switch (action) {
    case Action::kSave:
      save();
      break;
    case Action::kSaveAs:
      pushKeyboard("Save As", path_, PendingPrompt::kSaveAs, gui);
      break;
    case Action::kGotoLine: {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d", cy_ + 1);
      pushKeyboard("Goto Line", buf, PendingPrompt::kGotoLine, gui);
      break;
    }
    case Action::kDeleteLine:
      if (rowCount_ > 0) {
        editorDelRow(cy_);
        if (rowCount_ == 0) editorInsertRow(0, "", 0);
        if (cy_ >= rowCount_) cy_ = rowCount_ - 1;
        cx_ = 0;
        dirty_ = true;
      }
      break;
    case Action::kNewLine:
      editorInsertNewline();
      break;
    case Action::kRun:
      if (isPython() && path_[0]) {
        // If dirty, save first so we run what the user is looking at.
        if (dirty_) save();
        oled& d = gui.oledDisplay();
        d.clearBuffer();
        d.setFontPreset(FONT_SMALL);
        const char* msg = "Running...";
        int tw = d.getStrWidth(msg);
        d.drawStr((128 - tw) / 2, 32, msg);
        d.sendBuffer();
        Serial.printf("[Editor] Run %s\n", path_);
        mpy_gui_exec_file(path_);
      }
      break;
    case Action::kNone:
    default:
      break;
  }
}

// ─── Input handling ─────────────────────────────────────────────────────────

void EditorScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                               int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  // ── Save-on-exit modal ──────────────────────────────────────────
  // Two chips: Save (0) / Don't save (1). B closes the modal and
  // returns to editing — no on-screen "Cancel" chip, so picking
  // either chip always exits.
  if (saveModalOpen_) {
    if (e.leftPressed && saveModalCursor_ > 0) saveModalCursor_--;
    if (e.rightPressed && saveModalCursor_ < 1) saveModalCursor_++;
    {
      uint32_t now = millis();
      int16_t jx = static_cast<int16_t>(inputs.joyX()) - 2047;
      if (abs(jx) > 800) {
        if (lastJoyHMs_ == 0 || now - lastJoyHMs_ >= 220) {
          lastJoyHMs_ = now;
          if (jx > 0 && saveModalCursor_ < 1) saveModalCursor_++;
          else if (jx < 0 && saveModalCursor_ > 0) saveModalCursor_--;
        }
      } else {
        lastJoyHMs_ = 0;
      }
    }
    if (e.cancelPressed) {
      saveModalOpen_ = false;  // back to editing
      return;
    }
    if (e.confirmPressed) {
      uint8_t pick = saveModalCursor_;
      saveModalOpen_ = false;
      Serial.printf("[Editor] save modal pick=%u (0=save 1=discard)\n",
                    (unsigned)pick);
      if (pick == 0) {
        if (save()) {
          dirty_ = false;
          gui.popScreen();
        }
        // If save failed, status line shows the error and we stay.
      } else {
        // Don't save — clear dirty so the pop is unambiguous.
        dirty_ = false;
        gui.popScreen();
      }
    }
    return;
  }

  // ── Action menu modal ────────────────────────────────────────────
  if (actionMenuOpen_) {
    const uint8_t kCount = isPython() ? 6 : 5;
    if (e.upPressed && actionMenuCursor_ > 0) actionMenuCursor_--;
    if (e.downPressed && actionMenuCursor_ + 1 < kCount)
      actionMenuCursor_++;
    // Joystick Y nav — same cadence as the editor cursor.
    {
      uint32_t now = millis();
      int16_t jy = static_cast<int16_t>(inputs.joyY()) - 2047;
      if (abs(jy) > 600) {
        uint32_t repeat = abs(jy) > 1500 ? 110 : 240;
        if (lastJoyVMs_ == 0 || now - lastJoyVMs_ >= repeat) {
          lastJoyVMs_ = now;
          if (jy > 0 && actionMenuCursor_ + 1 < kCount)
            actionMenuCursor_++;
          else if (jy < 0 && actionMenuCursor_ > 0)
            actionMenuCursor_--;
        }
      } else {
        lastJoyVMs_ = 0;
      }
    }
    if (e.cancelPressed || e.yPressed) {
      actionMenuOpen_ = false;
      return;
    }
    if (e.confirmPressed) {
      static const Action kVerbs[] = {
          Action::kSave,       Action::kSaveAs,    Action::kGotoLine,
          Action::kDeleteLine, Action::kNewLine,   Action::kRun,
      };
      Action a = kVerbs[actionMenuCursor_];
      actionMenuOpen_ = false;
      doAction(a, gui);
      return;
    }
    return;
  }

  // ── Top-level button handling ────────────────────────────────────
  if (e.cancelPressed) {
    if (dirty_) {
      saveModalOpen_ = true;
      saveModalCursor_ = 0;  // default to Save
    } else {
      gui.popScreen();
    }
    return;
  }

  if (e.confirmPressed) {
    pushKeyboard("Insert text", "", PendingPrompt::kInsertText, gui);
    return;
  }

  if (e.xPressed) {
    // Backspace at the cursor — delete the byte immediately before
    // `cx_`, joining lines when the cursor is at column 0. Save lives
    // on the Y-menu, so X is freed up for the much more frequent
    // delete operation.
    editorDelChar();
    return;
  }

  if (e.yPressed) {
    actionMenuOpen_ = true;
    actionMenuCursor_ = 0;
    return;
  }

  // ── DPAD cursor movement (auto-repeat already applied by Inputs) ─
  if (e.upPressed) cursorMoveUp();
  if (e.downPressed) cursorMoveDown();
  if (e.leftPressed) cursorMoveLeft();
  if (e.rightPressed) cursorMoveRight();

  // Backspace via DPAD-LEFT-while-holding-Y is overkill; expose
  // delete-char through the action menu and rely on row append from
  // the keyboard for forward edits.

  // ── Joystick: per-character cursor move with auto-repeat ─────────
  // Vertical = single-line up/down; horizontal = single-glyph
  // left/right (which already wraps across line ends via
  // cursorMoveLeft / cursorMoveRight). Page-up/down lives on the
  // Y-menu, not on the stick — the stick is for fine cursor work.
  uint32_t now = millis();
  int16_t jy = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(jy) > 600) {
    uint32_t repeat = abs(jy) > 1500 ? 70 : 180;
    if (lastJoyVMs_ == 0 || now - lastJoyVMs_ >= repeat) {
      lastJoyVMs_ = now;
      if (jy > 0)
        cursorMoveDown();
      else
        cursorMoveUp();
    }
  } else {
    lastJoyVMs_ = 0;
  }

  int16_t jx = static_cast<int16_t>(inputs.joyX()) - 2047;
  if (abs(jx) > 600) {
    uint32_t repeat = abs(jx) > 1500 ? 60 : 160;
    if (lastJoyHMs_ == 0 || now - lastJoyHMs_ >= repeat) {
      lastJoyHMs_ = now;
      if (jx > 0)
        cursorMoveRight();
      else
        cursorMoveLeft();
    }
  } else {
    lastJoyHMs_ = 0;
  }
}
