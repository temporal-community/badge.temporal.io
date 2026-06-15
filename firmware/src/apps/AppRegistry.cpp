#include "AppRegistry.h"

#include <Arduino.h>
#include <cstring>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

namespace AppRegistry {

namespace {

DynamicApp* s_apps = nullptr;
size_t s_count = 0;

void ensureAppsPool() {
  if (s_apps) return;
  s_apps = static_cast<DynamicApp*>(
      BadgeMemory::allocPreferPsram(kMaxDynamicApps * sizeof(DynamicApp)));
  if (s_apps) std::memset(s_apps, 0, kMaxDynamicApps * sizeof(DynamicApp));
}

// Single scratch for FAT reads (max 2048 B). Lives in PSRAM when available
// so AppRegistry::scan does not burn ~5 KiB of the Arduino loop stack.
char* appRegScratch() {
  static char* buf = nullptr;
  if (!buf) {
    buf = static_cast<char*>(BadgeMemory::allocPreferPsram(2048));
  }
  return buf;
}

// Read up to `maxLen` bytes from a FAT file into the caller's buffer,
// preserving NUL-termination. Returns the byte count actually read, or
// -1 on failure.
int32_t readFileChunk(const char* path, char* buf, size_t maxLen) {
  if (!path || !buf || maxLen == 0) return -1;

  Filesystem::IOLock fsLock;
  FATFS* fs = replay_get_fatfs();
  if (!fs) return -1;

  FIL fp;
  if (f_open(fs, &fp, path, FA_READ) != FR_OK) {
    return -1;
  }

  UINT bytesRead = 0;
  FRESULT rc = f_read(&fp, buf, static_cast<UINT>(maxLen - 1), &bytesRead);
  f_close(&fp);
  if (rc != FR_OK) {
    return -1;
  }
  buf[bytesRead] = '\0';
  return static_cast<int32_t>(bytesRead);
}

// Strip surrounding "..." or '...' from a value, copy into `out` with
// NUL-termination. Best-effort — doesn't handle escaped quotes.
void copyQuotedString(const char* start, size_t len, char* out, size_t outCap) {
  if (outCap == 0) return;
  out[0] = '\0';
  if (!start || len == 0) return;

  // Trim leading whitespace.
  while (len > 0 && isspace(static_cast<unsigned char>(*start))) {
    start++;
    len--;
  }
  // Trim trailing whitespace.
  while (len > 0 && isspace(static_cast<unsigned char>(start[len - 1]))) {
    len--;
  }

  if (len >= 2 && (start[0] == '"' || start[0] == '\'') &&
      start[len - 1] == start[0]) {
    start++;
    len -= 2;
  }

  size_t copyLen = len < outCap - 1 ? len : outCap - 1;
  memcpy(out, start, copyLen);
  out[copyLen] = '\0';
}

// Find a Python dunder assignment in a text buffer:
//   __title__ = "Foo"
//   __title__='Foo'
// Returns true and copies the unquoted value into `out` on success.
bool findDunder(const char* text, size_t len, const char* name,
                char* out, size_t outCap) {
  if (!text || !name) return false;
  out[0] = '\0';

  size_t nameLen = strlen(name);
  for (size_t i = 0; i + nameLen < len; i++) {
    if (memcmp(text + i, name, nameLen) != 0) continue;
    // Must be at start of line (or beginning of file) — a leading
    // identifier char would mean it's some other variable.
    if (i > 0) {
      char prev = text[i - 1];
      if (prev != '\n' && prev != '\r') continue;
    }

    size_t cursor = i + nameLen;
    while (cursor < len &&
           (text[cursor] == ' ' || text[cursor] == '\t')) {
      cursor++;
    }
    if (cursor >= len || text[cursor] != '=') continue;
    cursor++;
    while (cursor < len &&
           (text[cursor] == ' ' || text[cursor] == '\t')) {
      cursor++;
    }

    size_t valueStart = cursor;
    while (cursor < len && text[cursor] != '\n' && text[cursor] != '\r') {
      cursor++;
    }
    copyQuotedString(text + valueStart, cursor - valueStart, out, outCap);
    return out[0] != '\0';
  }
  return false;
}

// Parse `DATA = ( 0x0F, 0x01, ... )` from an icon.py text buffer. We
// pack the resulting bytes into `out` until kIconBytes are filled.
// Returns true if we got at least kIconBytes meaningful values.
bool parseIconData(const char* text, size_t len, uint8_t* out) {
  if (!text || len == 0) return false;

  // Locate `DATA` followed by `=`.
  const char* hit = nullptr;
  for (size_t i = 0; i + 4 < len; i++) {
    if (memcmp(text + i, "DATA", 4) != 0) continue;
    if (i > 0) {
      char prev = text[i - 1];
      if (prev != '\n' && prev != '\r' && prev != ' ' && prev != '\t')
        continue;
    }
    size_t cursor = i + 4;
    while (cursor < len &&
           (text[cursor] == ' ' || text[cursor] == '\t')) {
      cursor++;
    }
    if (cursor < len && text[cursor] == '=') {
      hit = text + cursor + 1;
      break;
    }
  }
  if (!hit) return false;

  size_t remaining = len - (hit - text);
  size_t produced = 0;
  while (remaining > 0 && produced < kIconBytes) {
    char c = *hit;
    // Hex literal: 0x.. / 0X..
    if (c == '0' && remaining > 1 && (hit[1] == 'x' || hit[1] == 'X')) {
      char* endp = nullptr;
      long v = strtol(hit, &endp, 16);
      if (endp == hit) break;
      out[produced++] = static_cast<uint8_t>(v & 0xFF);
      remaining -= (endp - hit);
      hit = endp;
      continue;
    }
    // Binary literal: 0b.. / 0B.. — must be checked before the
    // decimal-digit branch below, otherwise that branch eats the
    // leading '0' of "0b01110111" as the decimal number 0, then
    // re-enters and parses "01110111" as decimal 1110111. Project
    // convention writes bitmap byte arrays in 0b00001111 form so the
    // dot pattern is human-readable; this branch keeps the runtime
    // icon scanner in sync with that convention. strtol(base=2) does
    // not accept the "0b" prefix itself, so we step past it manually.
    if (c == '0' && remaining > 1 && (hit[1] == 'b' || hit[1] == 'B')) {
      const char* digits = hit + 2;
      char* endp = nullptr;
      long v = strtol(digits, &endp, 2);
      if (endp == digits) break;
      out[produced++] = static_cast<uint8_t>(v & 0xFF);
      remaining -= (endp - hit);
      hit = endp;
      continue;
    }
    if (isdigit(static_cast<unsigned char>(c))) {
      char* endp = nullptr;
      long v = strtol(hit, &endp, 10);
      if (endp == hit) break;
      out[produced++] = static_cast<uint8_t>(v & 0xFF);
      remaining -= (endp - hit);
      hit = endp;
      continue;
    }
    hit++;
    remaining--;
  }
  return produced >= kIconBytes;
}

bool isAcceptableSlug(const char* name) {
  if (!name || !*name) return false;
  if (name[0] == '.') return false;
  for (const char* p = name; *p; p++) {
    if (!(isalnum(static_cast<unsigned char>(*p)) || *p == '_' ||
          *p == '-')) {
      return false;
    }
  }
  return true;
}

void slugToTitle(const char* slug, char* out, size_t outCap) {
  if (outCap == 0) return;
  size_t i = 0;
  bool capitalizeNext = true;
  for (const char* p = slug; *p && i < outCap - 1; p++) {
    if (*p == '_' || *p == '-') {
      out[i++] = ' ';
      capitalizeNext = true;
    } else if (capitalizeNext) {
      out[i++] = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
      capitalizeNext = false;
    } else {
      out[i++] = *p;
    }
  }
  out[i] = '\0';
}

void resolveIcon(const char* slug, const char* iconValue, DynamicApp& app) {
  app.hasCustomIcon = false;
  memset(app.icon, 0, kIconBytes);
  if (!iconValue || !iconValue[0]) {
    return;
  }

  // Compose the icon path. Three accepted forms:
  //   __icon__ = "icon.py"               -> /apps/<slug>/icon.py
  //   __icon__ = "/apps/<slug>/icon.py"  -> absolute path
  //   __icon__ = "(0xFF,0x..)"           -> inline tuple (rare)
  char path[kEntryPathCap];
  if (iconValue[0] == '/') {
    snprintf(path, sizeof(path), "%s", iconValue);
  } else {
    snprintf(path, sizeof(path), "/apps/%s/%s", slug, iconValue);
  }

  char* buf = appRegScratch();
  if (!buf) {
    if (parseIconData(iconValue, strlen(iconValue), app.icon)) {
      app.hasCustomIcon = true;
    }
    return;
  }
  int32_t bytes = readFileChunk(path, buf, 2048);
  if (bytes < 0) {
    // Inline tuple? Try parsing the value directly.
    if (parseIconData(iconValue, strlen(iconValue), app.icon)) {
      app.hasCustomIcon = true;
    }
    return;
  }
  if (parseIconData(buf, static_cast<size_t>(bytes), app.icon)) {
    app.hasCustomIcon = true;
  }
}

void parseAppMain(const char* slug, DynamicApp& app) {
  memset(&app, 0, sizeof(app));
  strncpy(app.slug, slug, kSlugCap - 1);
  app.slug[kSlugCap - 1] = '\0';
  snprintf(app.entryPath, kEntryPathCap, "/apps/%s/main.py", slug);
  app.orderHint = INT16_MAX;

  char* buf = appRegScratch();
  if (!buf) {
    slugToTitle(slug, app.title, kTitleCap);
    return;
  }
  int32_t bytes = readFileChunk(app.entryPath, buf, 2048);
  if (bytes < 0) {
    return;
  }

  if (!findDunder(buf, bytes, "__title__", app.title, kTitleCap)) {
    slugToTitle(slug, app.title, kTitleCap);
  }
  findDunder(buf, bytes, "__description__", app.description,
             kDescriptionCap);

  // __order__ is a signed integer literal — the dunder scanner returns it
  // as a string; convert and clamp to int16 range.
  char orderValue[16];
  if (findDunder(buf, bytes, "__order__", orderValue, sizeof(orderValue))) {
    char* endp = nullptr;
    long v = strtol(orderValue, &endp, 10);
    if (endp != orderValue) {
      if (v < INT16_MIN + 1) v = INT16_MIN + 1;
      if (v > INT16_MAX - 1) v = INT16_MAX - 1;
      app.orderHint = static_cast<int16_t>(v);
    }
  }

  char iconValue[kEntryPathCap];
  if (findDunder(buf, bytes, "__icon__", iconValue, sizeof(iconValue))) {
    resolveIcon(slug, iconValue, app);
  } else {
    // No __icon__ explicitly — try /apps/<slug>/icon.py opportunistically.
    char fallbackPath[kEntryPathCap];
    snprintf(fallbackPath, kEntryPathCap, "/apps/%s/icon.py", slug);
    char* iconBuf = appRegScratch();
    int32_t iconBytes =
        iconBuf ? readFileChunk(fallbackPath, iconBuf, 1024) : -1;
    if (iconBytes > 0 &&
        parseIconData(iconBuf, static_cast<size_t>(iconBytes), app.icon)) {
      app.hasCustomIcon = true;
    }
  }
}

bool entryPathHasMain(const char* slug) {
  char path[kEntryPathCap];
  snprintf(path, kEntryPathCap, "/apps/%s/main.py", slug);
  return Filesystem::fileExists(path);
}

void detectMatrixApp(const char* slug, DynamicApp& app) {
  app.hasMatrixApp = false;
  app.matrixTitle[0] = '\0';
  char path[kEntryPathCap];
  snprintf(path, kEntryPathCap, "/apps/%s/matrix.py", slug);
  if (!Filesystem::fileExists(path)) return;
  app.hasMatrixApp = true;
  // Best-effort __matrix_title__ extraction; fall back to the app title.
  char* buf = appRegScratch();
  if (!buf) return;
  int32_t bytes = readFileChunk(path, buf, 1024);
  if (bytes > 0) {
    findDunder(buf, bytes, "__matrix_title__", app.matrixTitle, kTitleCap);
  }
  if (!app.matrixTitle[0]) {
    strncpy(app.matrixTitle, app.title, kTitleCap - 1);
    app.matrixTitle[kTitleCap - 1] = '\0';
  }
}

}  // namespace

size_t scan() {
  ensureAppsPool();
  s_count = 0;
  if (!s_apps) return 0;

  Filesystem::IOLock fsLock;
  FATFS* fs = replay_get_fatfs();
  if (!fs) return 0;

  FF_DIR dir;
  FILINFO fno;
  if (f_opendir(fs, &dir, "/apps") != FR_OK) {
    return 0;
  }

  // Collect candidate slugs first (we re-acquire the lock per file
  // read inside parseAppMain, and oofatfs's IOLock is recursive).
  char slugs[kMaxDynamicApps][kSlugCap];
  size_t slugCount = 0;
  while (slugCount < kMaxDynamicApps) {
    FRESULT rc = f_readdir(&dir, &fno);
    if (rc != FR_OK || fno.fname[0] == '\0') break;
    if (!(fno.fattrib & AM_DIR)) continue;
    if (!isAcceptableSlug(fno.fname)) continue;
    strncpy(slugs[slugCount], fno.fname, kSlugCap - 1);
    slugs[slugCount][kSlugCap - 1] = '\0';
    slugCount++;
  }
  f_closedir(&dir);

  for (size_t i = 0; i < slugCount && s_count < kMaxDynamicApps; i++) {
    if (!entryPathHasMain(slugs[i])) continue;
    parseAppMain(slugs[i], s_apps[s_count]);
    detectMatrixApp(slugs[i], s_apps[s_count]);
    s_count++;
  }

  Serial.printf("[apps] AppRegistry: %u app(s) discovered\n",
                static_cast<unsigned>(s_count));
  return s_count;
}

size_t rescan() { return scan(); }

size_t count() { return s_count; }

const DynamicApp* at(size_t index) {
  if (!s_apps || index >= s_count) return nullptr;
  return &s_apps[index];
}

}  // namespace AppRegistry
