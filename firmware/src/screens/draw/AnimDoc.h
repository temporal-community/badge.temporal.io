// AnimDoc — data model + on-disk serializer for the Draw composer.
//
// Storage layout (FAT VFS):
//   /composer/<anim_id>/info.json         — metadata + objects + per-frame placements
//   /composer/<anim_id>/f00.fb, f01.fb…   — raw 1bpp framebuffer bytes (w*h/8 each,
//                                            same XBM bit order as u8g2: LSB = leftmost)
//
// Caps: 32 anims total, 24 frames per anim, 50–2000 ms per frame.
// `frame_count == 1` is a static image; the timeline UI still treats it as a
// length-1 animation so the editor model stays uniform.
//
// Thread model: editor sessions are single-threaded on the GUI task. All FAT
// access goes through `Filesystem::IOLock`.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace draw {

// XBM buffer size: rows are padded to full bytes.
// Always use this instead of w*h/8 to avoid reading past the end of
// non-multiple-of-8 widths (e.g. a 10px-wide bbox has 2 bytes/row, not 1.25).
inline size_t xbmBytes(uint16_t w, uint16_t h) {
    return (size_t)((w + 7u) / 8u) * h;
}

static constexpr uint8_t  kAnimIdLen        = 8;   // hex chars (NUL terminated -> 9 bytes)
static constexpr uint8_t  kObjIdLen         = 4;   // hex chars (NUL terminated -> 5 bytes)
static constexpr uint8_t  kAnimNameMax      = 28;
static constexpr uint8_t  kMaxAnimations    = 32;
static constexpr uint8_t  kMaxFrames        = 24;
static constexpr uint16_t kMinDurationMs    = 50;
static constexpr uint16_t kMaxDurationMs    = 2000;
static constexpr uint16_t kDefaultDurationMs = 100;

// Canvas sizes supported in Phase 1.
static constexpr uint16_t kCanvasFullW  = 128;
static constexpr uint16_t kCanvasFullH  = 64;
static constexpr uint16_t kCanvasZigW   = 48;
static constexpr uint16_t kCanvasZigH   = 48;
static constexpr uint8_t  kTextContentMax = 63;

enum class ObjectType : uint8_t {
  Catalog   = 0,
  SavedAnim = 1,
  DrawnAnim = 2,  // user-drawn pixel data; pixels owned by ObjectDef.
  Text      = 3,  // reserved for Phase 2 (Nametag composer)
};

enum class TextStackMode : uint8_t {
  OR         = 0,  // transparent text: 1-bits set pixels, 0-bits pass through
  XOR        = 1,  // text inverts pixels under glyph strokes
  RoundedBox = 2,  // black rounded rect behind white text
};

struct ObjectDef {
  char       id[kObjIdLen + 1] = {};
  ObjectType type = ObjectType::Catalog;
  int16_t    catalogIdx = -1;                  // ObjectType::Catalog
  char       savedAnimId[kAnimIdLen + 1] = {}; // ObjectType::SavedAnim

  // ── DrawnAnim payload (full canvas-sized; one frame for now) ───────────
  // `drawnW` × `drawnH` define the bounding box. `pixels` is a PSRAM-
  // allocated buffer of `xbmBytes(drawnW, drawnH)` bytes (raw 1bpp,
  // same XBM bit order as u8g2). `dirty` flags pending writes to disk.
  uint16_t   drawnW = 0;
  uint16_t   drawnH = 0;
  uint8_t*   drawnPixels = nullptr;
  bool       drawnDirty = false;

  // ── Text payload ────────────────────────────────────────────────────────
  char       textContent[kTextContentMax + 1] = {};
  uint8_t    textFontFamily = 0;
  uint8_t    textFontSlot = 2;
  TextStackMode textStackMode = TextStackMode::OR;
};

struct ObjectPlacement {
  char     objId[kObjIdLen + 1] = {};
  int16_t  x = 0;
  int16_t  y = 0;
  int8_t   z = 0;        // signed depth; higher renders on top.
  uint8_t  scale = 0;    // destination dimension px; 0 = native.
  uint32_t phaseMs = 0;  // free-running animation phase offset.
};

struct Frame {
  uint16_t durationMs = kDefaultDurationMs;
  std::vector<ObjectPlacement> placements;
};

struct AnimDoc {
  char     animId[kAnimIdLen + 1] = {};
  char     name[kAnimNameMax + 1] = {};
  uint16_t w = 0;
  uint16_t h = 0;
  uint32_t createdAt = 0;
  uint32_t editedAt  = 0;
  std::vector<Frame>     frames;
  std::vector<ObjectDef> objects;
  bool     dirty = false;                      // doc changed since last save.

  size_t pixelBytes() const { return xbmBytes(w, h); }
};

struct AnimSummary {
  char     animId[kAnimIdLen + 1] = {};
  char     name[kAnimNameMax + 1] = {};
  uint16_t w = 0;
  uint16_t h = 0;
  uint8_t  frameCount = 0;
  uint32_t totalMs = 0;
  uint32_t editedAt = 0;
};

// ── Filesystem ops (all take Filesystem::IOLock internally) ────────────────

// Enumerate every animation in /composer/. Caller may inspect `out` by
// editedAt-desc for picker ordering. Skips entries with malformed info.json.
bool listAll(std::vector<AnimSummary>& out);

// Read info.json + every fNN.fb into `doc`. On success the caller owns the
// PSRAM frame buffers and must call freeAll() when done.
bool load(const char* animId, AnimDoc& doc);

// Write info.json + every dirty frame's fNN.fb. Skips frames whose
// `dirtyOnDisk` is false. Updates `editedAt` and clears `dirty`.
bool save(AnimDoc& doc);

bool removeAnim(const char* animId);
bool duplicateAnim(const char* animId, char* newIdOut, size_t newIdCap);

// Free PSRAM buffers; clears the doc to a fresh state.
void freeAll(AnimDoc& doc);

// Allocate (or reallocate) a DrawnAnim's pixel buffer to `bytes` and zero it.
bool allocDrawnPixels(ObjectDef& def, size_t bytes);

// Identifier helpers
void newAnimId(char* out, size_t cap);
void newObjectId(const AnimDoc& doc, char* out, size_t cap);
void defaultName(char* out, size_t cap);

/// Parse `settings.txt` `[nametag] nametag = …` value.
enum class NametagSettingParse : uint8_t {
  Default = 0,  // empty / "default" → built-in text nametag only
  AnimId = 1,   // `animIdOut` holds an 8-char hex composer id
  Invalid = 2,  // non-empty but not default and not a valid id/path
};
NametagSettingParse parseNametagSetting(const char* raw,
                                         char animIdOut[kAnimIdLen + 1]);

// Path helpers
void animDirPath(const char* animId, char* out, size_t cap);
void infoJsonPath(const char* animId, char* out, size_t cap);
// Legacy (pre-restructure) per-frame pixel file path. Still used during load
// to migrate older docs into DrawnAnim objects, then deleted on first save.
void legacyFramePath(const char* animId, uint8_t idx, char* out, size_t cap);
// Per-DrawnAnim pixel file path: `/composer/<anim>/o<obj_id>.fb`.
void drawnObjectPath(const char* animId, const char* objId,
                     char* out, size_t cap);

}  // namespace draw
