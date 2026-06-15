#include "FileOpener.h"

#include <Arduino.h>
#include <cctype>
#include <cstring>

#include "../ui/GUI.h"
#include "AnimTestScreen.h"
#include "EditorScreen.h"
#include "HexViewScreen.h"
#include "Screen.h"

extern AnimTestScreen sAnimTest;
extern EditorScreen sEditor;
extern HexViewScreen sHexView;

namespace {

// Case-insensitive extension match. Returns true iff `path` ends in
// `ext` (which must include the leading '.').
bool extEq(const char* path, const char* ext) {
  size_t pl = std::strlen(path);
  size_t el = std::strlen(ext);
  if (pl < el) return false;
  const char* tail = path + (pl - el);
  for (size_t i = 0; i < el; i++) {
    char a = static_cast<char>(std::tolower(
        static_cast<unsigned char>(tail[i])));
    char b = static_cast<char>(std::tolower(
        static_cast<unsigned char>(ext[i])));
    if (a != b) return false;
  }
  return true;
}

// Extension table for the editor. Keep ordered roughly by
// expected hit frequency — early returns on the common cases.
constexpr const char* kEditorExts[] = {
    ".py",   ".json", ".txt", ".md",   ".csv", ".ini", ".log",
    ".cfg",  ".toml", ".yaml", ".yml", ".xml", ".html",
    ".h",    ".c",    ".cpp", ".ino",  ".js",
};

bool isEditorExt(const char* path) {
  for (const char* ext : kEditorExts) {
    if (extEq(path, ext)) return true;
  }
  return false;
}

}  // namespace

namespace FileOpener {

void open(const char* path, GUIManager& gui) {
  if (!path || !path[0]) return;

  // Reuse the existing animation-test screen for image viewing — same
  // scale ladder, same frame stepping, same chrome.
  if (extEq(path, ".xbm")) {
    Serial.printf("[FileOpener] %s -> AnimTest (xbm)\n", path);
    sAnimTest.loadXBM(path);
    gui.pushScreen(kScreenAnimTest);
    return;
  }
  // Raw 1bpp framebuffer dumps written by the draw composer
  // (`/composer/<anim>/{fNN,oNN}.fb`). Dimensions are pulled from the
  // sibling `info.json` when available.
  if (extEq(path, ".fb")) {
    Serial.printf("[FileOpener] %s -> AnimTest (fb)\n", path);
    sAnimTest.loadFB(path);
    gui.pushScreen(kScreenAnimTest);
    return;
  }
  // Credit prebinned bitmaps (`width LE16` + `height LE16` + MSB-first
  // packed rows — same layout as scripts/gen_credit_xbms.py). Unrelated
  // `.bin` blobs fail validation and fall through to the hex viewer.
  if (extEq(path, ".bin")) {
    if (sAnimTest.loadCreditBin(path)) {
      Serial.printf("[FileOpener] %s -> AnimTest (bin)\n", path);
      gui.pushScreen(kScreenAnimTest);
    } else {
      Serial.printf("[FileOpener] %s -> HexView (bin not credit image)\n",
                    path);
      sHexView.loadFile(path);
      gui.pushScreen(kScreenHexView);
    }
    return;
  }

  if (isEditorExt(path)) {
    Serial.printf("[FileOpener] %s -> Editor\n", path);
    sEditor.loadFile(path);
    gui.pushScreen(kScreenEditor);
    return;
  }

  // Unknown / binary → hex viewer.
  Serial.printf("[FileOpener] %s -> HexView (no match)\n", path);
  sHexView.loadFile(path);
  gui.pushScreen(kScreenHexView);
}

}  // namespace FileOpener
