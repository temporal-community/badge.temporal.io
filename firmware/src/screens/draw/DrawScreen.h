// DrawScreen — the native animation composer.
//
// Owns an in-memory `AnimDoc` plus editor scratch state (cursor, undo,
// thumbnail cache, saved-anim sticker cache). Rendered chrome layout is
// constant (top hint, bottom timeline, left tools, right object list);
// for full-screen 128×64 docs the chrome is hidden by default and revealed
// when the cursor is held against a screen edge.
//
// Lifecycle:
//   1.  Caller (DrawPickerScreen) calls `openNew(w, h)` or `openExisting(id)`.
//   2.  Caller pushes `kScreenDraw`. `onEnter` allocates / loads the doc.
//   3.  User edits; `dirty_` tracks changes. `save` icon writes atomically.
//   4.  `exit` icon prompts if dirty, then `onExit` frees PSRAM buffers and
//       pops back to the picker (which reload()s on its own onEnter).

#pragma once

#include <cstdint>
#include <vector>

#include "../Screen.h"
#include "AnimDoc.h"
#include "StickerPickerScreen.h"

class DrawScreen : public Screen {
 public:
  void openNew(uint16_t w, uint16_t h);
  void openExisting(const char* animId);

  // Stash an opcode for the next `onEnter`. Required because the picker pushes
  // us by ScreenId without a chance to pass parameters through the stack.

  void onEnter(GUIManager& gui) override;
  void onExit(GUIManager& gui) override;
  void onResume(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  // Render the composition of any AnimDoc frame — no chrome, no cursor,
  // no editor state. Clears the screen first. Used by the live nametag.
  void renderDocComposition(oled& d, draw::AnimDoc& doc, uint8_t frameIdx);
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenDraw; }
  bool showCursor() const override { return false; }

  // Sticker pick callback (reached via StickerPickerScreen → ScalePickerScreen).
  void onStickerPicked(const StickerPickerScreen::Result& r);
  void onScalePicked(uint8_t scale);
  void onReplaceStickerPicked(const StickerPickerScreen::Result& r);
  void onCtxScalePicked(uint8_t scale);
  void onTextInputDone(const char* text);

  // Public so anonymous-namespace static tables in the .cpp can reference it.
  enum class ContextAction : uint8_t { Move, Edit, Size, LayerZ, Delete };

  enum class HitZone : uint8_t {
    None,
    Canvas,
    // Left strip tools (single-column). "Draw" is a combined pen/erase
    // tool: CONFIRM strokes draw, CANCEL strokes erase.
    ToolDraw, ToolStickerAdd, ToolTextAdd, ToolFieldAdd, ToolHand, ToolSettings, ToolDuration, ToolEdit, ToolDelFrame,
    // Top-right bar (8x8 icons).
    ToolUndo, ToolRedo, ToolPlay, ToolSave, ToolExit,
    // Bottom timeline.
    TimelineFrame,
    TimelinePlus,
    TimelineDel,
    // Bottom timeline — brush size (extra = size 1..9).
    BrushSize,
  };

 private:
  enum class Tool : uint8_t { Draw, Hand, Edit };

  enum class Mode : uint8_t {
    Tutorial,
    Editing,
    Playing,
    ToolMenu,
    DurationPopup,
    SettingsPopup,
    HelpScroll,
    SaveExitPrompt,
    Ghosted,
    ContextMenu,
    ZAdjust,
    TextAppearance,
    FieldPicker,
    ExitNametagPrompt,
  };

  enum class TextOp : uint8_t { None, Add, Edit };

  enum class PendingOp : uint8_t {
    None,
    OpenNew,
    OpenExisting,
  };

  // Single-level undo. Captures (a) the affected DrawnAnim's pixel buffer
  // (so pen strokes are reversible) AND (b) the frame's placements snapshot
  // (so adds/removes/moves of placements are reversible).
  struct UndoState {
    int8_t  frameIdx = -1;
    char    objId[draw::kObjIdLen + 1] = {};
    uint8_t* pixels = nullptr;
    size_t   pixelBytes = 0;
    std::vector<draw::ObjectPlacement> placements;
    bool    valid = false;
  };

  // Per-(animId, scale) cache for saved-anim stickers used as objects.
  struct SavedStickerCache {
    char     animId[draw::kAnimIdLen + 1] = {};
    uint16_t w = 0;
    uint16_t h = 0;
    uint8_t  frameCount = 0;
    uint16_t durations[draw::kMaxFrames] = {};
    uint8_t  scale = 0;            // dst dim in px
    uint16_t scaledW = 0;
    uint16_t scaledH = 0;
    uint8_t* scaledFrames = nullptr;  // PSRAM, frameCount * (scaledW*scaledH/8)
  };

  struct GhostState {
    bool                              active = false;
    StickerPickerScreen::Result       src;
    uint8_t                           scale = 0;
    int8_t                            z = 0;
    uint32_t                          phaseMs = 0;
    char                              objId[draw::kObjIdLen + 1] = {};
    bool                              moveExisting = false;
    // Cursor offset within the sticker captured at grab time. On drop, the
    // placement's top-left = cursor - (offset). Keeps the grabbed pixel
    // anchored to the cursor instead of teleporting the sticker's center.
    int16_t                           grabOffsetX = 0;
    int16_t                           grabOffsetY = 0;
  };

  struct ContextMenuState {
    char     targetObjId[draw::kObjIdLen + 1] = {};
    uint8_t  targetPlacementIdx = 0;
    uint8_t  cursor = 0;
    uint32_t lastJoyMs = 0;
  };

  struct ZAdjustState {
    char    targetObjId[draw::kObjIdLen + 1] = {};
    uint8_t targetPlacementIdx = 0;
    int8_t  originalZ = 0;
    int8_t  pendingZ = 0;
    uint32_t lastJoyMs = 0;
  };

  struct TextAppearanceState {
    char    targetObjId[draw::kObjIdLen + 1] = {};
    uint8_t fontFamily = 0;
    uint8_t fontSlot = 2;
    draw::TextStackMode stack = draw::TextStackMode::OR;
    /// 0 = font family, 1 = font size slot, 2 = stack mode.
    uint8_t fieldSel = 0;
    uint32_t lastJoyMs = 0;
  };

  // ── Lifecycle helpers ───────────────────────────────────────────────────
  bool ensureDocLoaded();
  void rebuildAllThumbs();
  void freeSession();

  // ── Cursor / chrome ─────────────────────────────────────────────────────
  void integrateCursor(const Inputs& inputs);
  void updateChromeRevealState(const Inputs& inputs, uint32_t nowMs);
  bool chromeVisible() const;
  void clampCursorToScreen();
  bool cursorInCanvas() const;
  void canvasLocalCursor(int16_t* outX, int16_t* outY) const;
  void canvasOriginScreen(int16_t* outX, int16_t* outY) const;

  // ── Tools ───────────────────────────────────────────────────────────────
  void snapshotForUndo(int8_t frameIdx);
  void restoreUndo();
  void restoreRedo();
  // Find the active DrawnAnim for `frameIdx`, creating a new one (placed at
  // (0,0) on that frame) if `createIfMissing`. Returns nullptr only when
  // creation fails. The returned pointer is invalidated by any
  // doc_.objects mutation (push_back / erase). Sets activeObjId_ on success.
  draw::ObjectDef* findOrCreateActiveDrawn(uint8_t frameIdx,
                                           bool createIfMissing);
  // Find a DrawnAnim placement under canvas-local (lx, ly) on the given
  // frame. Returns the placement index in `frameIdx`'s placements vector,
  // or -1 if none. Used by Edit/Hand tool clicks and pen-stroke targeting.
  int findDrawnUnderCursor(uint8_t frameIdx, int16_t lx, int16_t ly,
                           const draw::ObjectDef** outDef) const;
  int findAnyUnderCursor(uint8_t frameIdx, int16_t lx, int16_t ly,
                         const draw::ObjectDef** outDef) const;
  draw::ObjectDef* findObjectDef(const char* objId);
  const draw::ObjectDef* findObjectDef(const char* objId) const;
  draw::ObjectPlacement* findPlacement(uint8_t frameIdx, const char* objId);
  const draw::ObjectPlacement* findPlacement(uint8_t frameIdx, const char* objId) const;
  void plotPixel(uint8_t* fb, uint16_t fbW, uint16_t fbH,
                 int16_t x, int16_t y, bool on);
  // Plots a square stamp at (cx, cy) with current brush size.
  void plotStamp(uint8_t* fb, uint16_t fbW, uint16_t fbH,
                 int16_t cx, int16_t cy, bool on);
  /// Clamped brush diameter in pixels (`brushSize_` plus +1 erase boost when
  /// `paintingErase_`).
  uint8_t effectiveBrushPx() const;
  void plotLine(uint8_t* fb, uint16_t fbW, uint16_t fbH,
                int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool on);
  void rebuildThumb(uint8_t frameIdx);

  // Tighten a DrawnAnim's pixel buffer to the bounding box of its set bits
  // and shift its placement accordingly so the visual position is
  // unchanged. Removes the placement (and the ObjectDef) entirely if the
  // buffer is empty. Called at stroke release.
  void shrinkActiveDrawnToBBox();
  // Inflate the active DrawnAnim's pixel buffer to full canvas size with
  // the existing pixels copied to the (placement.x, placement.y) offset
  // and the placement shifted to (0, 0). Used at stroke begin so the user
  // can paint freely outside the prior bounding box; `shrinkActiveDrawnToBBox`
  // tightens it back at stroke release.
  void expandActiveDrawnToCanvas();

  // ── Frame ops ───────────────────────────────────────────────────────────
  bool addFrame(bool duplicateCurrent);
  bool deleteCurrentFrame();
  void moveToFrame(uint8_t idx);

  // ── Playback ────────────────────────────────────────────────────────────
  void advancePlayback(uint32_t nowMs);

  // ── Object / sticker rendering ──────────────────────────────────────────
  SavedStickerCache* getSavedSticker(const char* animId, uint8_t scale);
  // Resolve scale 0 ("native") to a real scale value for a placement.
  uint8_t resolveScale(const draw::ObjectPlacement& p,
                       const draw::ObjectDef& def) const;
  bool savedAnimDimensions(const char* animId, uint8_t scale,
                           int16_t* outW, int16_t* outH) const;
  void renderObjectInstance(oled& d, int16_t canvasX, int16_t canvasY,
                            const draw::ObjectPlacement& p,
                            const draw::ObjectDef& def, uint32_t nowMs);
  bool objectDimensions(const draw::ObjectPlacement& p,
                        const draw::ObjectDef& def,
                        int16_t* outW, int16_t* outH) const;
  /// Integrate IMU into shared parallax smoother. When `forEditorUi`, honors
  /// cfg "Tilt" off (editor-only); nametag `renderDocComposition` passes false.
  void pumpParallax(bool forEditorUi);
  void parallaxOffset(int8_t z, int16_t* dx, int16_t* dy) const;
  // Hit-test a placed object against canvas-local cursor.
  bool placementHitTest(const draw::ObjectPlacement& p,
                        const draw::ObjectDef& def,
                        int16_t cx, int16_t cy) const;

  // ── Drawer / chrome rendering ───────────────────────────────────────────
  void renderTopBar(oled& d);
  void renderTimeline(oled& d);
  void renderToolStrip(oled& d);
  void renderRightStrip(oled& d);
  void renderContextMenu(oled& d);
  void renderDurationPopup(oled& d);
  void renderSettingsPopup(oled& d);
  void renderZAdjustPopup(oled& d);
  void renderTextAppearancePopup(oled& d);
  void renderFieldPickerPopup(oled& d);
  void renderExitNametagPrompt(oled& d);
  void renderTutorial(oled& d);
  void renderHelpScroll(oled& d);
  void renderSaveExitPrompt(oled& d);
  void renderModeCursor(oled& d);
  void renderFooterActions(oled& d, const char* xLabel, const char* yLabel,
                           const char* bLabel, const char* aLabel);
  // Marquee for hovered tool / hit-zone description; renders into the
  // standard OLED footer band. Skipped while painting or while a popup
  // owns the bottom of the screen.
  void renderHelpMarquee(oled& d);
  // Returns a static description string for a given hit zone, or empty.
  const char* helpForZone(HitZone z, uint8_t extra) const;
  // Hand/Edit hover indicator: blinking XOR outline around the sticker the
  // cursor is currently over. Edit tool only highlights DrawnAnims (the
  // only type it can target); Hand highlights any sticker.
  void renderHoverFlash(oled& d, int16_t ox, int16_t oy);

  // ── Tool / chrome hit zones ─────────────────────────────────────────────
  HitZone hitZoneAtCursor(uint8_t* outExtra) const;
  void onChromeClick(HitZone z, uint8_t extra, GUIManager& gui);
  void activateContextAction(GUIManager& gui);
  void doCtxMove(GUIManager& gui);
  void doCtxDelete(GUIManager& gui);
  void doCtxEdit(GUIManager& gui);
  void doCtxSize(GUIManager& gui);
  void doCtxZ(GUIManager& gui);
  void doCtxReplaceZigmoji(GUIManager& gui);
  void doCtxTextAppearance(GUIManager& gui);

  // ── State ───────────────────────────────────────────────────────────────
  PendingOp     pending_      = PendingOp::None;
  uint16_t      pendingW_     = draw::kCanvasFullW;
  uint16_t      pendingH_     = draw::kCanvasFullH;
  char          pendingId_[draw::kAnimIdLen + 1] = {};

  draw::AnimDoc doc_;
  uint8_t       currentFrame_ = 0;
  Tool          currentTool_  = Tool::Draw;
  uint8_t       brushSize_    = 1;          // 1..8: see plotStamp for geometry
  uint8_t       cursorSpeed_  = 1;          // 0 = slow, 1 = med, 2 = fast
  int8_t        nextPlacementZ_ = 1;        // auto-alternating z: +1,-1,+2,-2,...
  /// Layer tilt / IMU parallax in the composer only (cfg Tilt row); not NVS.
  bool          imuParallaxEnabled_ = true;
  Mode          mode_         = Mode::Editing;
  // Object id of the DrawnAnim that subsequent pen strokes paint into. Empty
  // string means "create a new layer on first stroke". Set by the Edit tool
  // or implicitly by the start of a stroke.
  char          activeObjId_[draw::kObjIdLen + 1] = {};

  float         cursorXf_     = 64.f;
  float         cursorYf_     = 32.f;
  uint32_t      lastCursorMs_ = 0;
  uint32_t      cursorRampStartMs_ = 0;     // non-painting hold-to-accelerate ramp
  int16_t       prevPaintX_   = -1;
  int16_t       prevPaintY_   = -1;
  bool          painting_     = false;
  bool          paintingErase_ = false;   // current stroke is the eraser variant
  /// While true, IMU parallax is frozen (no pump) and offsets are zero during
  /// an active pen stroke so pixels stay aligned with the brush.
  bool          freezeParallaxWhilePainting_ = true;
  /// Use unsmoothed ADC for cursor integration while painting for lower lag.
  bool          immediateJoystickWhilePainting_ = true;

  // Per-edge chrome visibility (128×64 canvas only — 48×48 keeps all four
  // permanently shown). Each side is independently activated by moving the
  // cursor to its edge zone and stays shown while the cursor is anywhere
  // inside that strip's hysteresis range. While `painting_`, all strips
  // freeze in their current state so a stroke that crosses an edge zone
  // doesn't accidentally pop the menu.
  bool          topShown_     = false;
  bool          bottomShown_  = false;
  bool          leftShown_    = false;
  bool          rightShown_   = false;
  bool          rightSuppressUntilEdge_ = false;

  // Playback.
  uint32_t      playFrameStartMs_ = 0;
  uint32_t      previewFrameStartMs_ = 0;
  int8_t        previewSavedFrame_ = -1;

  uint8_t       toolMenuCursor_    = 0;
  uint32_t      toolMenuJoyLastMs_ = 0;

  // Duration popup state.
  uint16_t      popupDurationMs_  = draw::kDefaultDurationMs;
  uint32_t       durationJoyLastMs_ = 0;
  // Settings popup state: 0 = cursor speed, 1 = frame duration.
  uint8_t        settingsSel_ = 0;
  uint32_t       settingsJoyLastMs_ = 0;
  uint8_t        tutorialPage_ = 0;
  uint8_t        helpScroll_ = 0;
  bool           saveFailed_ = false;
  /// Save/discard sheet: 0 = save & exit, 1 = discard, 2 = stay.
  uint8_t        savePromptSel_ = 0;
  uint32_t       saveJoyLastMs_ = 0;

  ContextMenuState ctxMenu_;
  ZAdjustState     zAdjust_;
  TextAppearanceState textAppear_;

  TextOp        textOp_ = TextOp::None;
  char          textEditTargetId_[draw::kObjIdLen + 1] = {};
  char          textEditBuf_[draw::kTextContentMax + 1] = {};
  char          replaceTargetObjId_[draw::kObjIdLen + 1] = {};
  char          scaleTargetObjId_[draw::kObjIdLen + 1] = {};

  // Hold-Cancel-4s = save+exit (skipped when Draw tool owns Cancel = erase).
  uint32_t      cancelHoldStartMs_ = 0;
  static constexpr uint32_t kCancelHoldMs = 4000;

  // Field picker popup state (Mode::FieldPicker).
  uint8_t       fieldPickerCursor_ = 0;
  uint8_t       fieldPickerScroll_ = 0;
  uint32_t      fieldPickerJoyMs_ = 0;

  // Tool strip pagination — when more entries than fit visually, this offset
  // shifts which slots are drawn / hit-tested in the on-canvas left strip.
  uint8_t       toolStripScroll_ = 0;
  uint32_t      toolMenuScrollMs_ = 0;

  // After a successful Save+Exit, prompt the user to set the saved drawing
  // as their nametag before popping the screen.
  uint8_t       exitNametagSel_ = 1;  // 0=Yes, 1=No (default off-by-one)
  uint32_t      exitNametagJoyMs_ = 0;

  // Auto-open TextAppearance popup once after placing freshly-created text.
  bool          autoOpenAppearance_ = false;

  // Undo.
  UndoState     undo_;
  UndoState     redo_;

  // Ghost / sticker placement.
  GhostState    ghost_;

  // Thumb cache.
  uint8_t       thumbs_[draw::kMaxFrames][8] = {};   // 8 bytes packed XBM = 8x8 thumb
  bool          thumbValid_[draw::kMaxFrames] = {};

  // Saved-anim sticker cache.
  std::vector<SavedStickerCache> savedCache_;
};

extern DrawScreen sDrawScreen;
