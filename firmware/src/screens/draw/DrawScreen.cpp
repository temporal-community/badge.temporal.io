#include "DrawScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "../../BadgeGlobals.h"
#include "../../hardware/Haptics.h"
#include "../../hardware/Inputs.h"
#include "../../hardware/IMU.h"
#include "../../hardware/oled.h"
#include "../../identity/BadgeInfo.h"
#include "../../infra/BadgeConfig.h"
#include "../../infra/Filesystem.h"
#include "../../infra/PsramAllocator.h"
#include "../../ui/ButtonGlyphs.h"
#include "../../ui/GUI.h"
#include "../../ui/Images.h"
#include "../../ui/OLEDLayout.h"
#include "../../ui/UIFonts.h"
#include "../EmojiText.h"
#include "../ScreenRefs.h"
#include "DrawIcons.h"
#include "ScalePickerScreen.h"
#include "../../ui/FontCatalog.h"

#include <new>

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

DrawScreen sDrawScreen;

namespace {

// Cursor integration tunables. Joystick raw is 0..4095, center 2047.
constexpr int16_t kJoyCenter   = 2047;
constexpr int16_t kCursorJoyDeadzone = 220;
constexpr int16_t kJoyDeadzonePainting = 300;   // narrower while pen down (less sticky)
constexpr int16_t kJoyMenuDeadzone = 650;       // large deadzone for one-shot menu nav
constexpr uint32_t kOneShotRepeatMs = 800;       // ms before one-shot gating allows repeat
constexpr float   kCursorSlowPxPerSec = 22.f;
constexpr float   kCursorFastPxPerSec = 70.f;
constexpr float   kCursorAccelMaxPxPerSec = 300.f;  // hold-to-accelerate ceiling
constexpr uint32_t kCursorRampDelayMs = 300;    // ms before ramp begins
constexpr uint32_t kCursorRampDurationMs = 3000; // ms to reach full accel
// While painting: speed is 0 at the deadzone edge and scales as defl^3 up to
// this cap at full stick deflection (fine control for short strokes, no constant
// drift that reads as straight segments).
constexpr float   kCursorDrawMaxPxPerSec = 52.f;

// Haptic duty/frequency for draw vs erase strokes
constexpr uint8_t  kDrawHapticDuty = 20;
constexpr uint32_t kDrawHapticFreqHz = 540;
constexpr uint32_t kEraseHapticFreqHz = 320;
constexpr uint32_t kCursorTickMaxMs  = 300;   // clamp dt to avoid jumps after stalls

// Top bar.
constexpr uint8_t kTopBarY = 0;
constexpr uint8_t kTopBarH = 8;
// Top-right icon row: undo/redo, gap, play/save/exit.
constexpr uint8_t kTopRightIconCount = 5;
constexpr int16_t kTopRightIconY     = 0;
constexpr int16_t kTopRightExitX     = 119;   // 127 - 8
constexpr int16_t kTopRightSaveX     = 110;   // 119 - 8 - 1
constexpr int16_t kTopRightPlayX     = 101;   // 110 - 8 - 1
constexpr int16_t kTopRightRedoX     = 89;    // one blank 3px gap before play
constexpr int16_t kTopRightUndoX     = 80;

// Tool strip (single column, left side).
constexpr uint8_t kToolStripX  = 0;
constexpr uint8_t kToolStripY  = 8;
constexpr uint8_t kToolStripW  = 40;
constexpr uint8_t kToolStripH  = 48;
constexpr uint8_t kToolRowH    = 9;   // 5 rows + small bottom gutter
constexpr uint8_t kToolIconW   = 8;
constexpr uint8_t kToolIconH   = 8;

// Right strip (help text / brush selector).
constexpr uint8_t kRightStripX = 88;
constexpr uint8_t kRightStripY = 8;
constexpr uint8_t kRightStripW = 40;
constexpr uint8_t kRightStripH = 48;

// Bottom timeline.
constexpr uint8_t kBottomBarY = 56;
constexpr uint8_t kBottomBarH = 8;

// Per-thumb size in the timeline strip.
constexpr uint8_t kThumbW     = 6;
constexpr uint8_t kThumbH     = 6;
constexpr uint8_t kThumbCellW = kThumbW + 1;
constexpr uint8_t kPlusTileW  = 7;
constexpr uint8_t kCursorCrossLen = 3;

// Tool slot table for the single-column left strip.
struct ToolSlot {
    uint8_t row;
    DrawScreen::HitZone zone;
    const uint8_t* icon;
    const char* label;
};

const ToolSlot kToolSlots[] = {
    {0, DrawScreen::HitZone::ToolDraw,       DrawIcons::pen8,     "draw"},
    {1, DrawScreen::HitZone::ToolStickerAdd, DrawIcons::sticker8, "zig"},
    {2, DrawScreen::HitZone::ToolTextAdd,    DrawIcons::text8,    "text"},
    {3, DrawScreen::HitZone::ToolFieldAdd,   DrawIcons::tag8,     "field"},
    {4, DrawScreen::HitZone::ToolHand,       DrawIcons::hand8,    "hand"},
    {5, DrawScreen::HitZone::ToolSettings,   DrawIcons::gear8,    "cfg"},
};
constexpr uint8_t kToolSlotCount = sizeof(kToolSlots) / sizeof(kToolSlots[0]);
// Visual capacity of the on-canvas left strip in rows. Anything beyond this
// requires the strip to scroll (or the user can use the LEFT-button popup
// which paginates with the joystick).
constexpr uint8_t kToolStripVisibleRows = 5;

// ── Field picker (BadgeInfo fields placed as text) ─────────────────────────
struct FieldDef {
    const char* label;
    size_t      offset;
    size_t      cap;
};
#define BIFIELD(L, M)                                                        \
    {L,                                                                       \
     offsetof(BadgeInfo::Fields, M),                                          \
     sizeof(((BadgeInfo::Fields*)0)->M)}
const FieldDef kFieldDefs[] = {
    BIFIELD("Name",    name),
    BIFIELD("Title",   title),
    BIFIELD("Company", company),
    BIFIELD("Type",    attendeeType),
    BIFIELD("Email",   email),
    BIFIELD("Web",     website),
    BIFIELD("Phone",   phone),
    BIFIELD("Bio",     bio),
    BIFIELD("UUID",    ticketUuid),
};
#undef BIFIELD
constexpr uint8_t kFieldDefCount = sizeof(kFieldDefs) / sizeof(kFieldDefs[0]);
constexpr uint8_t kFieldPickerVisibleRows = 5;

// Top-right icon table (play/save/exit).
struct TopRightSlot {
    int16_t x;
    DrawScreen::HitZone zone;
    const uint8_t* icon;
};

const TopRightSlot kTopRightSlots[] = {
    {kTopRightUndoX, DrawScreen::HitZone::ToolUndo, DrawIcons::undo8},
    {kTopRightRedoX, DrawScreen::HitZone::ToolRedo, DrawIcons::redo8},
    {kTopRightPlayX, DrawScreen::HitZone::ToolPlay, DrawIcons::play8},
    {kTopRightSaveX, DrawScreen::HitZone::ToolSave, DrawIcons::save8},
    {kTopRightExitX, DrawScreen::HitZone::ToolExit, DrawIcons::exitX8},
};
constexpr uint8_t kTopRightSlotCount = sizeof(kTopRightSlots) / sizeof(kTopRightSlots[0]);

void onStickerCb(const StickerPickerScreen::Result& r,
                 GUIManager& gui, void* user) {
    auto* self = static_cast<DrawScreen*>(user);
    if (self) {
        self->onStickerPicked(r);
    }
    gui.popScreen();
    // ScalePickerScreen will be pushed by the screen we returned to (DrawScreen::onEnter).
    // Simpler: push it here directly by configuring + pushing.
    sScalePicker.configure(r,
                           [](uint8_t scale, GUIManager& gui2, void* user2) {
                               auto* s = static_cast<DrawScreen*>(user2);
                               if (s) s->onScalePicked(scale);
                               gui2.popScreen();
                           },
                           user);
    gui.pushScreen(kScreenDrawScalePicker);
}

void onReplaceStickerCb(const StickerPickerScreen::Result& r,
                        GUIManager& gui, void* user) {
    auto* self = static_cast<DrawScreen*>(user);
    if (self) self->onReplaceStickerPicked(r);
    gui.popScreen();
}

void onCtxScaleCb(uint8_t scale, GUIManager& gui, void* user) {
    auto* self = static_cast<DrawScreen*>(user);
    if (self) self->onCtxScalePicked(scale);
    gui.popScreen();
}

void onTextInputCb(const char* text, void* user) {
    auto* self = static_cast<DrawScreen*>(user);
    if (self) self->onTextInputDone(text);
}

constexpr DrawScreen::ContextAction kContextRows[] = {
    DrawScreen::ContextAction::Move,
    DrawScreen::ContextAction::Edit,
    DrawScreen::ContextAction::Size,
    DrawScreen::ContextAction::LayerZ,
    DrawScreen::ContextAction::Delete,
};
constexpr uint8_t kContextRowCount = sizeof(kContextRows) / sizeof(kContextRows[0]);
constexpr float kPxPerZAtFullTilt = 2.0f;
constexpr float kTiltSmoothing = 0.15f;
float sParallaxXSmoothed = 0.f;
float sParallaxYSmoothed = 0.f;

// Composer canvas: parallax is clamped to ±8 px per axis (per layer), keeping
// tilt visible without huge drifts. Live nametag uses the same z scale but a
// looser clamp so the flipped overlay can move more while staying on-screen.
constexpr int16_t kEditorParallaxClampPx = 8;
constexpr int16_t kNametagParallaxClampPx = 128;

inline void nametagCompositionParallax(int8_t z, int16_t* dx, int16_t* dy) {
  const float kPar = kPxPerZAtFullTilt * (float)z / 1000.f;
  int32_t rdx = static_cast<int32_t>(sParallaxXSmoothed * kPar);
  int32_t rdy = static_cast<int32_t>(sParallaxYSmoothed * kPar);
  rdx = std::clamp<int32_t>(rdx, -kNametagParallaxClampPx, kNametagParallaxClampPx);
  rdy = std::clamp<int32_t>(rdy, -kNametagParallaxClampPx, kNametagParallaxClampPx);
  *dx = static_cast<int16_t>(rdx);
  *dy = static_cast<int16_t>(rdy);
}

float clampMg(float v) {
    if (v < -1000.f) return -1000.f;
    if (v > 1000.f) return 1000.f;
    return v;
}

void cubeFaceParallaxVector(float screenXMg, float screenYMg, float screenZMg,
                            float* outX, float* outY) {
    const float ax = std::fabs(screenXMg);
    const float ay = std::fabs(screenYMg);
    const float az = std::fabs(screenZMg);
    float faceX = 0.f;
    float faceY = 0.f;

    // Treat gravity as a point on a cube. The dominant axis is the cube face
    // normal; the other two axes become the local position on that face.
    // This preserves a full [-1000,+1000] response even when the badge is
    // lying flat, where raw screen X/Y tilt can otherwise collapse near zero.
    if (az >= ax && az >= ay && az > 1.f) {
        const float s = screenZMg >= 0.f ? 1.f : -1.f;
        faceX =  s * screenXMg / az;
        faceY =  s * screenYMg / az;
    } else if (ax >= ay && ax > 1.f) {
        const float s = screenXMg >= 0.f ? 1.f : -1.f;
        faceX = -s * screenZMg / ax;
        faceY =  s * screenYMg / ax;
    } else if (ay > 1.f) {
        const float s = screenYMg >= 0.f ? 1.f : -1.f;
        faceX =  s * screenXMg / ay;
        faceY = -s * screenZMg / ay;
    }

    *outX = clampMg(faceX * 1000.f);
    *outY = clampMg(faceY * 1000.f);
}

/// One-shot vertical joystick nav for popup menus. Fires once when the stick
/// leaves the large deadzone, then requires return to center before firing
/// again. After kOneShotRepeatMs of continuous hold, allows a slow auto-repeat.
/// `wasDeflected` must be zeroed when the popup opens. Returns -1 (up),
/// +1 (down), or 0 (no movement). Also accepts D-pad UP (up) and DOWN (down)
/// as single-shot presses.
int8_t menuOneShotNav(const Inputs& inputs, const Inputs::ButtonEdges& e,
                      bool* wasDeflected, uint32_t* holdStartMs = nullptr) {
    if (e.upPressed)   return -1;
    if (e.downPressed) return +1;
    const int16_t jy = -((int16_t)inputs.joyY() - kJoyCenter); // negate so up = positive
    const int ajy = abs(jy);
    if (ajy <= kJoyMenuDeadzone) {
        *wasDeflected = false;
        if (holdStartMs) *holdStartMs = 0;
        return 0;
    }
    if (!*wasDeflected) {
        // First fire.
        *wasDeflected = true;
        if (holdStartMs) *holdStartMs = millis();
        return jy > 0 ? -1 : +1;
    }
    // Already fired. Allow slow repeat after timeout.
    if (holdStartMs && *holdStartMs != 0) {
        const uint32_t now = millis();
        if (now - *holdStartMs >= kOneShotRepeatMs) {
            *holdStartMs = now;  // reset for next repeat interval
            return jy > 0 ? -1 : +1;
        }
    }
    return 0;
}

/// One-shot horizontal joystick / LEFT-RIGHT button step for cycling values in
/// popup menus. Returns -1 (left/decrease), +1 (right/increase), or 0.
/// After kOneShotRepeatMs of continuous hold, allows slow auto-repeat.
int8_t valueOneShotStep(const Inputs& inputs, const Inputs::ButtonEdges& e,
                        bool* wasDeflected, uint32_t* holdStartMs = nullptr) {
    if (e.leftPressed)  return -1;
    if (e.rightPressed) return +1;
    const int16_t jx = (int16_t)inputs.joyX() - kJoyCenter;
    const int ajx = abs(jx);
    if (ajx <= kJoyMenuDeadzone) {
        *wasDeflected = false;
        if (holdStartMs) *holdStartMs = 0;
        return 0;
    }
    if (!*wasDeflected) {
        *wasDeflected = true;
        if (holdStartMs) *holdStartMs = millis();
        return jx > 0 ? 1 : -1;
    }
    if (holdStartMs && *holdStartMs != 0) {
        const uint32_t now = millis();
        if (now - *holdStartMs >= kOneShotRepeatMs) {
            *holdStartMs = now;
            return jx > 0 ? 1 : -1;
        }
    }
    return 0;
}

// ── Help / tutorial content ────────────────────────────────────────────────
struct HelpLine {
    const char* text;
    bool header;
    bool glyphs;
};

struct TutorialLine {
    const char* text;
    bool glyphs;
};

struct TutorialPage {
    const char* title;
    TutorialLine lines[4];
};

static const HelpLine kHelpLines[] = {
    {"CHROME", true, false},
    {"Joystick moves cursor", false, false},
    {"Top row opens bar", false, false},
    {"Bottom row timeline", false, false},
    {"X:tools", false, true},
    {"UP:object menu", false, true},
    {"DRAWING", true, false},
    {"Confirm:draw stroke", false, true},
    {"Cancel:erase", false, true},
    {"Hold + drag paints line", false, false},
    {"Brush size bottom right", false, false},
    {"LEFT TOOLS", true, false},
    {"draw: freehand pen", false, false},
    {"zig: place sticker", false, false},
    {"text: add text object", false, false},
    {"hand: drag object", false, false},
    {"edit: paint layer", false, false},
    {"cfg: settings + help", false, false},
    {"cfg Tilt: IMU layers", false, false},
    {"CONTEXT MENU", true, false},
    {"UP:open on object", false, true},
    {"move: reposition", false, false},
    {"edit: select ink layer", false, false},
    {"size: rescale sticker", false, false},
    {"Z: set layer depth", false, false},
    {"del: remove from frame", false, false},
    {"FRAMES", true, false},
    {"Bottom strip timeline", false, false},
    {"Frame thumb: select", false, false},
    {"+ tile duplicates frame", false, false},
    {"Del tile removes frame", false, false},
    {"cfg Frame ms: duration", false, false},
    {"TOP BAR", true, false},
    {"undo / redo arrows", false, false},
    {"play previews anim", false, false},
    {"save writes to disk", false, false},
    {"exit prompts if unsaved", false, false},
    {"PLACING STICKERS", true, false},
    {"Confirm:drop at cursor", false, true},
    {"Cancel:cancel place", false, true},
    {"ESCAPE", true, false},
    {"Hold all 4 face buttons", false, false},
    {"to force-exit any app", false, false},
};
constexpr uint8_t kHelpLineCount = sizeof(kHelpLines) / sizeof(kHelpLines[0]);
constexpr uint8_t kHelpLineH     = 9;
constexpr uint8_t kHelpHeaderH   = 8;
constexpr uint8_t kHelpFooterH   = 10;
constexpr uint8_t kHelpVisible   =
    (64 - kHelpHeaderH - kHelpFooterH) / kHelpLineH;

static const TutorialPage kTutorialPages[] = {
    {"Draw",
     {{"Joystick moves cursor", false},
      {"Top row opens bar", false},
      {"Bottom row timeline", false},
      {"X:tools", true}}},
    {"Ink",
     {{"Confirm:draw stroke", true},
      {"Cancel:erase", true},
      {"Hold + drag paints line", false},
      {"Brush size bottom right", false}}},
    {"Objects",
     {{"zig: place sticker", false},
      {"text: add text object", false},
      {"hand: drag object", false},
      {"UP:object menu", true}}},
    {"Frames",
     {{"Frame thumb: select", false},
      {"+ tile duplicates frame", false},
      {"play previews anim", false},
      {"save writes to disk", false}}},
};
constexpr uint8_t kTutorialPageCount =
    sizeof(kTutorialPages) / sizeof(kTutorialPages[0]);

void drawHelpTextLine(oled& d, int16_t x, int16_t baseline,
                      const char* text, bool glyphs, int16_t maxW) {
    if (glyphs) {
        ButtonGlyphs::drawInlineHintCompact(d, x, baseline, text);
        return;
    }

    char fitted[32];
    std::snprintf(fitted, sizeof(fitted), "%s", text ? text : "");
    OLEDLayout::fitText(d, fitted, sizeof(fitted), maxW);
    d.drawStr(x, baseline, fitted);
}

}  // namespace

// ── Public entry points ────────────────────────────────────────────────────

void DrawScreen::openNew(uint16_t w, uint16_t h) {
    pending_  = PendingOp::OpenNew;
    pendingW_ = w;
    pendingH_ = h;
    pendingId_[0] = '\0';
}

void DrawScreen::openExisting(const char* animId) {
    pending_ = PendingOp::OpenExisting;
    if (animId) {
        std::strncpy(pendingId_, animId, sizeof(pendingId_) - 1);
        pendingId_[sizeof(pendingId_) - 1] = '\0';
    }
}

void DrawScreen::onEnter(GUIManager& gui) {
    (void)gui;
    if (pending_ == PendingOp::None) {
        // Re-entered from a sub-screen (sticker picker, text input). Don't
        // wipe state.
        return;
    }
    ensureDocLoaded();
    pending_ = PendingOp::None;
}

void DrawScreen::onResume(GUIManager& /*gui*/) {
    // Coming back from a sub-screen (sticker picker, scale picker, text
    // input, …) — hide every chrome strip so the canvas is unobstructed
    // for the next action. The user has to deliberately move the cursor
    // back to an edge to bring the menus back.
    if (doc_.w != draw::kCanvasZigW) {
        topShown_ = bottomShown_ = leftShown_ = rightShown_ = false;
    }
}

void DrawScreen::onExit(GUIManager& /*gui*/) {
    imuParallaxEnabled_ = true;
    // Only free if we're really leaving — not on temporary push to sub-screen.
    // The picker screens push other screens but we keep doc_ alive while they
    // are on top of us. The simplest signal is "ScreenStack is popping us":
    // we conservatively free in onExit. If the picker pushes us again, openExisting
    // will reload.
    // However, when StickerPicker is pushed from our handleInput, we get an
    // onExit call. To avoid wiping state, key off `pending_ == None` AND we
    // skip free on push-to-substack by checking if a known sub-screen pushed.
    //
    // Simplest correct behavior: only freeSession() when a save-or-discard
    // path explicitly closes us. Push to sub-screens will not call onExit
    // because onExit fires on pop, not on cover. Good — the engine only
    // calls onExit when our screen is popped off the stack.
    freeSession();
    pending_ = PendingOp::None;
}

bool DrawScreen::ensureDocLoaded() {
    freeSession();

    if (pending_ == PendingOp::OpenExisting) {
        if (!draw::load(pendingId_, doc_)) {
            doc_.w = draw::kCanvasFullW;
            doc_.h = draw::kCanvasFullH;
            doc_.frames.resize(1);
            draw::newAnimId(doc_.animId, sizeof(doc_.animId));
            draw::defaultName(doc_.name, sizeof(doc_.name));
        }
    } else {
        doc_.w = pendingW_;
        doc_.h = pendingH_;
        draw::newAnimId(doc_.animId, sizeof(doc_.animId));
        draw::defaultName(doc_.name, sizeof(doc_.name));
        doc_.frames.resize(1);
        doc_.dirty = true;
    }

    currentFrame_ = 0;
    currentTool_  = Tool::Draw;
    activeObjId_[0] = '\0';
    mode_         = Mode::Tutorial;
    tutorialPage_ = 0;
    saveFailed_   = false;
    cursorXf_     = doc_.w / 2.f;
    cursorYf_     = doc_.h / 2.f;
    if (doc_.w == draw::kCanvasZigW) {
        // Place cursor at canvas center in screen coords (40+24, 8+24) = (64, 32).
        cursorXf_ = 64.f;
        cursorYf_ = 32.f;
    }
    lastCursorMs_ = millis();
    painting_     = false;
    {
        // Always start with chrome visible so the user sees the toolbars.
        topShown_    = true;
        bottomShown_ = true;
        leftShown_   = true;
        rightShown_  = false;  // right strip is context-menu only
        rightSuppressUntilEdge_ = true;
    }

    for (uint8_t i = 0; i < draw::kMaxFrames; i++) thumbValid_[i] = false;

    if (undo_.pixels) { free(undo_.pixels); undo_.pixels = nullptr; }
    undo_.frameIdx = -1;
    undo_.placements.clear();
    undo_.valid = false;
    if (redo_.pixels) { free(redo_.pixels); redo_.pixels = nullptr; }
    redo_.frameIdx = -1;
    redo_.placements.clear();
    redo_.valid = false;

    rebuildAllThumbs();
    return true;
}

void DrawScreen::rebuildAllThumbs() {
    for (uint8_t i = 0; i < doc_.frames.size(); i++) rebuildThumb(i);
}

void DrawScreen::freeSession() {
    draw::freeAll(doc_);
    if (undo_.pixels) { free(undo_.pixels); undo_.pixels = nullptr; }
    undo_.frameIdx = -1;
    undo_.placements.clear();
    undo_.valid = false;
    if (redo_.pixels) { free(redo_.pixels); redo_.pixels = nullptr; }
    redo_.frameIdx = -1;
    redo_.placements.clear();
    redo_.valid = false;

    for (auto& c : savedCache_) {
        if (c.scaledFrames) { free(c.scaledFrames); c.scaledFrames = nullptr; }
    }
    savedCache_.clear();

    ghost_ = GhostState{};
    ctxMenu_ = ContextMenuState{};
    zAdjust_ = ZAdjustState{};
    textAppear_ = TextAppearanceState{};
    durationJoyLastMs_ = 0;
    saveJoyLastMs_ = 0;
    savePromptSel_ = 0;
    toolMenuCursor_ = 0;
    toolMenuJoyLastMs_ = 0;
    helpScroll_ = 0;
    textOp_ = TextOp::None;
    textEditTargetId_[0] = '\0';
    replaceTargetObjId_[0] = '\0';
    scaleTargetObjId_[0] = '\0';
}

// ── Cursor / chrome ────────────────────────────────────────────────────────

void DrawScreen::integrateCursor(const Inputs& inputs) {
    const uint32_t now = millis();
    uint32_t dt = now - lastCursorMs_;
    if (dt > kCursorTickMaxMs) dt = kCursorTickMaxMs;
    lastCursorMs_ = now;
    if (dt == 0) return;

    // Read both raw and smoothed values: smoothed gives us a stable feel
    // during normal motion, but its IIR tail can keep delivering above-
    // deadzone deflection for tens of ms after the user releases the stick,
    // producing visible "skating" drift. Gating cursor integration on the
    // raw reading being inside a tighter idle threshold kills the drift the
    // moment the stick physically returns to center.
    uint16_t rawX = 0, rawY = 0;
    inputs.readJoystickImmediate(&rawX, &rawY, /*applyDeadzone=*/true);
    uint16_t smX = inputs.joyX();
    uint16_t smY = inputs.joyY();
    if (painting_ && immediateJoystickWhilePainting_) {
        smX = rawX;
        smY = rawY;
    }
    const float fxRaw = (float)((int16_t)rawX - kJoyCenter);
    const float fyRaw = (float)((int16_t)rawY - kJoyCenter);
    const float fx = (float)((int16_t)smX - kJoyCenter);
    const float fy = (float)((int16_t)smY - kJoyCenter);
    const float dz = (float)(painting_ ? kJoyDeadzonePainting : kCursorJoyDeadzone);

    const float magRaw = std::sqrt(fxRaw * fxRaw + fyRaw * fyRaw);
    const float mag = std::sqrt(fx * fx + fy * fy);
    // Idle if the *raw* stick is parked. A 60% threshold is the sweet spot
    // — strict enough to kill smoothed drift, lax enough that legitimate
    // micro-deflections still register.
    const float idleThresh = dz * 0.6f;
    if (magRaw <= idleThresh) {
        cursorRampStartMs_ = 0;
        clampCursorToScreen();
        return;
    }
    if (mag <= dz) {
        cursorRampStartMs_ = 0;  // reset hold-to-accelerate ramp
        clampCursorToScreen();
        return;
    }

    // Normalised direction preserves the joystick's actual angle.
    const float dirX = fx / mag;
    const float dirY = fy / mag;

    // Deflection 0..1 from deadzone edge to full tilt.
    const float maxDefl = (float)kJoyCenter - dz;
    float defl = (mag - dz) / maxDefl;
    if (defl > 1.f) defl = 1.f;

    static constexpr float kSpeedMul[] = {0.5f, 1.0f, 2.0f};
    const float mul = kSpeedMul[cursorSpeed_ < 3 ? cursorSpeed_ : 1];

    float speed;
    if (painting_) {
        speed = kCursorDrawMaxPxPerSec * mul * defl * defl * defl;
    } else {
        // Quadratic response: precise at low deflection, fast at full tilt.
        float maxSpeed = kCursorFastPxPerSec;
        // Hold-to-accelerate: after kCursorRampDelayMs of continuous deflection,
        // linearly ramp maxSpeed up to kCursorAccelMaxPxPerSec over kCursorRampDurationMs.
        if (cursorRampStartMs_ == 0) {
            cursorRampStartMs_ = now;
        }
        const uint32_t holdMs = now - cursorRampStartMs_;
        if (holdMs > kCursorRampDelayMs) {
            float rampT = (float)(holdMs - kCursorRampDelayMs) / (float)kCursorRampDurationMs;
            if (rampT > 1.f) rampT = 1.f;
            maxSpeed = kCursorFastPxPerSec +
                       (kCursorAccelMaxPxPerSec - kCursorFastPxPerSec) * rampT;
        }
        speed = (kCursorSlowPxPerSec +
                 (maxSpeed - kCursorSlowPxPerSec) * defl * defl) * mul;
    }

    const float step = speed * (dt / 1000.f);
    cursorXf_ += dirX * step;
    cursorYf_ += dirY * step;
    clampCursorToScreen();
}

void DrawScreen::clampCursorToScreen() {
    if (cursorXf_ < 0)   cursorXf_ = 0;
    if (cursorXf_ > 127) cursorXf_ = 127;
    if (cursorYf_ < 0)   cursorYf_ = 0;
    if (cursorYf_ > 63)  cursorYf_ = 63;
}

bool DrawScreen::chromeVisible() const {
    return topShown_ || bottomShown_ || leftShown_ || rightShown_;
}

void DrawScreen::canvasOriginScreen(int16_t* outX, int16_t* outY) const {
    if (doc_.w == draw::kCanvasZigW) {
        *outX = 40;
        *outY = 8;
    } else {
        *outX = 0;
        *outY = 0;
    }
}

bool DrawScreen::cursorInCanvas() const {
    const int16_t cx = (int16_t)cursorXf_;
    const int16_t cy = (int16_t)cursorYf_;
    int16_t ox, oy;
    canvasOriginScreen(&ox, &oy);
    const int16_t canvasRight = (doc_.w == draw::kCanvasZigW)
        ? (int16_t)(ox + doc_.w)
        : (int16_t)doc_.w;
    const int16_t canvasBottom = (doc_.w == draw::kCanvasZigW)
        ? (int16_t)(oy + doc_.h)
        : (int16_t)doc_.h;
    // Whichever strips are currently shown obscure their physical regions.
    // Anything outside those obscured regions counts as canvas.
    const int16_t xMin = leftShown_   ? (int16_t)kToolStripW   : (int16_t)0;
    const int16_t xMax = rightShown_  ? (int16_t)kRightStripX  : canvasRight;
    const int16_t yMin = topShown_    ? (int16_t)(kTopBarY + kTopBarH)
                                       : (int16_t)0;
    const int16_t yMax = bottomShown_ ? (int16_t)kBottomBarY
                                       : canvasBottom;
    return cx >= xMin && cx < xMax && cy >= yMin && cy < yMax;
}

void DrawScreen::canvasLocalCursor(int16_t* outX, int16_t* outY) const {
    int16_t ox, oy;
    canvasOriginScreen(&ox, &oy);
    *outX = (int16_t)cursorXf_ - ox;
    *outY = (int16_t)cursorYf_ - oy;
}

void DrawScreen::updateChromeRevealState(const Inputs& /*inputs*/,
                                         uint32_t /*nowMs*/) {
    constexpr int16_t kEdgeActivateNear = 4;
    if (doc_.w == draw::kCanvasZigW) {
        // 48×48 canvas: chrome always permanent.
        topShown_ = bottomShown_ = leftShown_ = true;
        if ((int16_t)cursorXf_ >= 127 - kEdgeActivateNear) {
            rightSuppressUntilEdge_ = false;
        }
        rightShown_ = !rightSuppressUntilEdge_;
        return;
    }
    if (mode_ == Mode::Playing) {
        // Playback: chrome always hidden so the animation gets full screen.
        topShown_ = bottomShown_ = leftShown_ = rightShown_ = false;
        return;
    }
    if (mode_ == Mode::ToolMenu) {
        leftShown_ = true;
        topShown_ = bottomShown_ = true;
        return;
    }
    if (painting_ || mode_ == Mode::Ghosted) {
        return;
    }

    const int16_t cx = (int16_t)cursorXf_;
    const int16_t cy = (int16_t)cursorYf_;

    // Top / bottom bars: show together as a pair. Either edge activates both;
    // leaving both edges hides both.
    const bool atTopRow    = (cy == 0);
    const bool atBottomRow = (cy == 63);

    if (atTopRow || atBottomRow) {
        topShown_ = bottomShown_ = true;
    } else if (cy >= (int16_t)(kTopBarY + kTopBarH) && cy < (int16_t)kBottomBarY) {
        topShown_ = bottomShown_ = false;
    }

    if (cx >= 127 - kEdgeActivateNear) {
        rightSuppressUntilEdge_ = false;
    }

    // Inner viewport (zigmoji layout): still hide both when fully inside the
    // 48×48 canvas rect so left/right edge grazing can't strand chrome on.
    const bool inInnerViewport =
        cx >= (int16_t)kToolStripW && cx < (int16_t)kRightStripX &&
        cy >= (int16_t)(kTopBarY + kTopBarH) && cy < (int16_t)kBottomBarY;
    if (inInnerViewport) {
        topShown_ = bottomShown_ = false;
    }
    // leftShown_ is toggled exclusively by the LEFT button.
    // If it's open, keep top and bottom visible too so the full chrome shows.
    if (leftShown_) topShown_ = bottomShown_ = true;

    // Right strip is only used for the context menu overlay.
    if (mode_ != Mode::ContextMenu) {
        rightShown_ = false;
    } else {
        // Context menu: always show full chrome.
        topShown_ = bottomShown_ = true;
    }
    if (rightSuppressUntilEdge_) {
        rightShown_ = false;
    }
}

// ── Tools ──────────────────────────────────────────────────────────────────

void DrawScreen::snapshotForUndo(int8_t frameIdx) {
    if (frameIdx < 0 || (size_t)frameIdx >= doc_.frames.size()) return;
    if (redo_.pixels) { free(redo_.pixels); redo_.pixels = nullptr; }
    redo_.frameIdx = -1;
    redo_.placements.clear();
    redo_.valid = false;

    undo_.frameIdx = frameIdx;
    undo_.placements = doc_.frames[frameIdx].placements;
    undo_.objId[0] = '\0';
    if (undo_.pixels) { free(undo_.pixels); undo_.pixels = nullptr; }
    undo_.pixelBytes = 0;

    // If a DrawnAnim is active, stash its pixels too.
    if (activeObjId_[0]) {
        for (auto& obj : doc_.objects) {
            if (obj.type != draw::ObjectType::DrawnAnim) continue;
            if (std::strcmp(obj.id, activeObjId_) != 0) continue;
            const size_t bytes = draw::xbmBytes(obj.drawnW, obj.drawnH);
            undo_.pixels = (uint8_t*)BadgeMemory::allocPreferPsram(bytes);
            if (undo_.pixels && obj.drawnPixels) {
                std::memcpy(undo_.pixels, obj.drawnPixels, bytes);
                std::strncpy(undo_.objId, obj.id, sizeof(undo_.objId) - 1);
                undo_.pixelBytes = bytes;
            }
            break;
        }
    }
    undo_.valid = true;
}

void DrawScreen::restoreUndo() {
    if (!undo_.valid || undo_.frameIdx < 0) return;
    if ((size_t)undo_.frameIdx >= doc_.frames.size()) return;

    if (redo_.pixels) { free(redo_.pixels); redo_.pixels = nullptr; }
    redo_.frameIdx = undo_.frameIdx;
    redo_.placements = doc_.frames[undo_.frameIdx].placements;
    redo_.objId[0] = '\0';
    redo_.pixelBytes = 0;
    if (undo_.objId[0]) {
        for (auto& obj : doc_.objects) {
            if (obj.type != draw::ObjectType::DrawnAnim) continue;
            if (std::strcmp(obj.id, undo_.objId) != 0) continue;
            const size_t bytes = draw::xbmBytes(obj.drawnW, obj.drawnH);
            redo_.pixels = (uint8_t*)BadgeMemory::allocPreferPsram(bytes);
            if (redo_.pixels && obj.drawnPixels) {
                std::memcpy(redo_.pixels, obj.drawnPixels, bytes);
                std::strncpy(redo_.objId, obj.id, sizeof(redo_.objId) - 1);
                redo_.pixelBytes = bytes;
            }
            break;
        }
    }
    redo_.valid = true;

    doc_.frames[undo_.frameIdx].placements = undo_.placements;
    if (undo_.objId[0] && undo_.pixels) {
        for (auto& obj : doc_.objects) {
            if (obj.type != draw::ObjectType::DrawnAnim) continue;
            if (std::strcmp(obj.id, undo_.objId) != 0) continue;
            const size_t bytes = draw::xbmBytes(obj.drawnW, obj.drawnH);
            if (bytes == undo_.pixelBytes && obj.drawnPixels) {
                std::memcpy(obj.drawnPixels, undo_.pixels, bytes);
                obj.drawnDirty = true;
            }
            break;
        }
    }
    doc_.dirty = true;
    rebuildThumb(undo_.frameIdx);
    undo_.valid = false;
}

void DrawScreen::restoreRedo() {
    if (!redo_.valid || redo_.frameIdx < 0) return;
    if ((size_t)redo_.frameIdx >= doc_.frames.size()) return;

    if (undo_.pixels) { free(undo_.pixels); undo_.pixels = nullptr; }
    undo_.frameIdx = redo_.frameIdx;
    undo_.placements = doc_.frames[redo_.frameIdx].placements;
    undo_.objId[0] = '\0';
    undo_.pixelBytes = 0;
    if (redo_.objId[0]) {
        for (auto& obj : doc_.objects) {
            if (obj.type != draw::ObjectType::DrawnAnim) continue;
            if (std::strcmp(obj.id, redo_.objId) != 0) continue;
            const size_t bytes = draw::xbmBytes(obj.drawnW, obj.drawnH);
            undo_.pixels = (uint8_t*)BadgeMemory::allocPreferPsram(bytes);
            if (undo_.pixels && obj.drawnPixels) {
                std::memcpy(undo_.pixels, obj.drawnPixels, bytes);
                std::strncpy(undo_.objId, obj.id, sizeof(undo_.objId) - 1);
                undo_.pixelBytes = bytes;
            }
            break;
        }
    }
    undo_.valid = true;

    doc_.frames[redo_.frameIdx].placements = redo_.placements;
    if (redo_.objId[0] && redo_.pixels) {
        for (auto& obj : doc_.objects) {
            if (obj.type != draw::ObjectType::DrawnAnim) continue;
            if (std::strcmp(obj.id, redo_.objId) != 0) continue;
            const size_t bytes = draw::xbmBytes(obj.drawnW, obj.drawnH);
            if (bytes == redo_.pixelBytes && obj.drawnPixels) {
                std::memcpy(obj.drawnPixels, redo_.pixels, bytes);
                obj.drawnDirty = true;
            }
            break;
        }
    }
    doc_.dirty = true;
    rebuildThumb(redo_.frameIdx);
    redo_.valid = false;
}

void DrawScreen::plotPixel(uint8_t* fb, uint16_t fbW, uint16_t fbH,
                           int16_t x, int16_t y, bool on) {
    if (x < 0 || x >= (int16_t)fbW) return;
    if (y < 0 || y >= (int16_t)fbH) return;
    const uint16_t rowBytes = (fbW + 7) / 8;
    uint8_t& byte = fb[y * rowBytes + (x >> 3)];
    const uint8_t mask = 1u << (x & 7);
    if (on) byte |= mask;
    else    byte &= ~mask;
}

uint8_t DrawScreen::effectiveBrushPx() const {
    uint8_t s = brushSize_ < 1 ? 1 : (brushSize_ > 8 ? 8 : brushSize_);
    if (painting_ && paintingErase_ && s < 8) {
        return (uint8_t)(s + 1u);
    }
    return s;
}

void DrawScreen::plotStamp(uint8_t* fb, uint16_t fbW, uint16_t fbH,
                           int16_t cx, int16_t cy, bool on) {
    const uint8_t s = effectiveBrushPx();
    if (s == 1) { plotPixel(fb, fbW, fbH, cx, cy, on); return; }

    // Visual-size-ordered brushes: square, circle, square, circle, ...
    //   D2 = 2×2 square     D3 = circle r=1
    //   D4 = 3×3 square     D5 = circle r=2
    //   D6 = 4×4 square     D7 = circle r=3
    //   D8 = 5×5 square
    if (s % 2 == 0) {
        // Even D-number: filled square. D2→side 2, D4→3, D6→4, D8→5
        const int16_t side = (int16_t)(s / 2 + 1);
        const int16_t half = side / 2;
        for (int16_t dy = 0; dy < side; dy++)
            for (int16_t dx = 0; dx < side; dx++)
                plotPixel(fb, fbW, fbH, cx - half + dx, cy - half + dy, on);
    } else {
        // Odd D-number ≥3: circular stamp. D3→r=1, D5→r=2, D7→r=3
        const int16_t r = (int16_t)(s / 2);
        const int16_t rSq = r * r;
        for (int16_t dy = -r; dy <= r; dy++)
            for (int16_t dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= rSq)
                    plotPixel(fb, fbW, fbH, cx + dx, cy + dy, on);
    }
}

void DrawScreen::plotLine(uint8_t* fb, uint16_t fbW, uint16_t fbH,
                          int16_t x0, int16_t y0,
                          int16_t x1, int16_t y1, bool on) {
    int16_t dx =  abs(x1 - x0);
    int16_t dy = -abs(y1 - y0);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;
    while (true) {
        plotStamp(fb, fbW, fbH, x0, y0, on);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

draw::ObjectDef* DrawScreen::findOrCreateActiveDrawn(uint8_t frameIdx,
                                                     bool createIfMissing) {
    if (frameIdx >= doc_.frames.size()) return nullptr;

    // Honor activeObjId_ if it points to an existing DrawnAnim that still
    // has a placement on this frame. Without the placement check, a context-
    // menu delete leaves the ObjectDef in doc_.objects but removes the
    // placement, causing expandActiveDrawnToCanvas to bail and the stroke
    // to silently fail.
    if (activeObjId_[0]) {
        draw::ObjectDef* match = nullptr;
        bool hasPlacement = false;
        for (auto& obj : doc_.objects) {
            if (obj.type != draw::ObjectType::DrawnAnim) continue;
            if (std::strcmp(obj.id, activeObjId_) == 0) { match = &obj; break; }
        }
        if (match) {
            for (const auto& pl : doc_.frames[frameIdx].placements) {
                if (std::strcmp(pl.objId, activeObjId_) == 0) {
                    hasPlacement = true;
                    break;
                }
            }
        }
        if (match && hasPlacement) return match;
        activeObjId_[0] = '\0';
    }

    if (!createIfMissing) return nullptr;

    // Create a new DrawnAnim and place it at (0,0) on the current frame.
    draw::ObjectDef def{};
    def.type = draw::ObjectType::DrawnAnim;
    def.drawnW = doc_.w;
    def.drawnH = doc_.h;
    draw::newObjectId(doc_, def.id, sizeof(def.id));
    const size_t bytes = draw::xbmBytes(def.drawnW, def.drawnH);
    if (!draw::allocDrawnPixels(def, bytes)) return nullptr;
    def.drawnDirty = true;
    doc_.objects.push_back(std::move(def));

    draw::ObjectPlacement p{};
    std::strncpy(p.objId, doc_.objects.back().id, sizeof(p.objId) - 1);
    p.x = 0;
    p.y = 0;
    p.z = 0;
    p.scale = 0;
    p.phaseMs = 0;
    doc_.frames[frameIdx].placements.push_back(p);

    std::strncpy(activeObjId_, doc_.objects.back().id, sizeof(activeObjId_) - 1);
    doc_.dirty = true;
    return &doc_.objects.back();
}

int DrawScreen::findDrawnUnderCursor(uint8_t frameIdx,
                                     int16_t lx, int16_t ly,
                                     const draw::ObjectDef** outDef) const {
    if (frameIdx >= doc_.frames.size()) return -1;
    const auto& placements = doc_.frames[frameIdx].placements;
    int best = -1;
    int8_t bestZ = -127;
    for (uint8_t i = 0; i < placements.size(); i++) {
        const auto& p = placements[i];
        const draw::ObjectDef* def = nullptr;
        for (const auto& o : doc_.objects) {
            if (std::strcmp(o.id, p.objId) == 0) { def = &o; break; }
        }
        if (!def || def->type != draw::ObjectType::DrawnAnim) continue;
        if (lx < p.x || ly < p.y) continue;
        if (lx >= p.x + def->drawnW || ly >= p.y + def->drawnH) continue;
        if (p.z >= bestZ) {
            best = i;
            bestZ = p.z;
            if (outDef) *outDef = def;
        }
    }
    return best;
}

draw::ObjectDef* DrawScreen::findObjectDef(const char* objId) {
    if (!objId || !objId[0]) return nullptr;
    for (auto& o : doc_.objects) {
        if (std::strcmp(o.id, objId) == 0) return &o;
    }
    return nullptr;
}

const draw::ObjectDef* DrawScreen::findObjectDef(const char* objId) const {
    if (!objId || !objId[0]) return nullptr;
    for (const auto& o : doc_.objects) {
        if (std::strcmp(o.id, objId) == 0) return &o;
    }
    return nullptr;
}

draw::ObjectPlacement* DrawScreen::findPlacement(uint8_t frameIdx, const char* objId) {
    if (frameIdx >= doc_.frames.size() || !objId || !objId[0]) return nullptr;
    for (auto& p : doc_.frames[frameIdx].placements) {
        if (std::strcmp(p.objId, objId) == 0) return &p;
    }
    return nullptr;
}

const draw::ObjectPlacement* DrawScreen::findPlacement(uint8_t frameIdx,
                                                       const char* objId) const {
    if (frameIdx >= doc_.frames.size() || !objId || !objId[0]) return nullptr;
    for (const auto& p : doc_.frames[frameIdx].placements) {
        if (std::strcmp(p.objId, objId) == 0) return &p;
    }
    return nullptr;
}

bool DrawScreen::objectDimensions(const draw::ObjectPlacement& p,
                                  const draw::ObjectDef& def,
                                  int16_t* outW, int16_t* outH) const {
    int16_t w = 0, h = 0;
    if (def.type == draw::ObjectType::DrawnAnim) {
        w = def.drawnW;
        h = def.drawnH;
    } else if (def.type == draw::ObjectType::Catalog) {
        const uint8_t s = resolveScale(p, def);
        w = h = s;
    } else if (def.type == draw::ObjectType::SavedAnim) {
        if (!savedAnimDimensions(def.savedAnimId, p.scale, &w, &h)) {
            w = h = 48;
        }
    } else if (def.type == draw::ObjectType::Text) {
        // Approximate hit box without mutating display font state from a const
        // hit-test. Rendering uses the exact font metrics.
        static constexpr uint8_t kApproxH[kSizeCount] =
            {4, 6, 7, 8, 9, 10, 13, 15, 20, 24};
        const uint8_t slot = def.textFontSlot < kSizeCount ? def.textFontSlot : 2;
        h = kApproxH[slot] + 2;
        const uint8_t charW = (uint8_t)std::max<uint8_t>(3, (uint8_t)(h / 2));
        w = (int16_t)std::min<uint16_t>(127, std::strlen(def.textContent) * charW + 3);
    }
    if (w <= 0 || h <= 0) return false;
    if (outW) *outW = w;
    if (outH) *outH = h;
    return true;
}

bool DrawScreen::savedAnimDimensions(const char* animId, uint8_t scale,
                                     int16_t* outW, int16_t* outH) const {
    if (!animId || !animId[0]) return false;

    for (const auto& c : savedCache_) {
        if (std::strcmp(c.animId, animId) != 0) continue;
        const bool nativeHit = (scale == 0 && c.scaledW == c.w && c.scaledH == c.h);
        if (c.scale == scale || nativeHit) {
            if (outW) *outW = (int16_t)c.scaledW;
            if (outH) *outH = (int16_t)c.scaledH;
            return c.scaledW > 0 && c.scaledH > 0;
        }
    }

    std::vector<draw::AnimSummary> all;
    draw::listAll(all);
    for (const auto& s : all) {
        if (std::strcmp(s.animId, animId) != 0) continue;
        uint16_t w = s.w;
        uint16_t h = s.h;
        if (scale > 0 && scale != s.w) {
            if (s.w == 0 || s.w % scale != 0) return false;
            const uint16_t div = s.w / scale;
            if (div == 0 || s.h % div != 0) return false;
            w = scale;
            h = s.h / div;
        }
        if (w == 0 || h == 0) return false;
        if (outW) *outW = (int16_t)w;
        if (outH) *outH = (int16_t)h;
        return true;
    }
    return false;
}

int DrawScreen::findAnyUnderCursor(uint8_t frameIdx, int16_t lx, int16_t ly,
                                   const draw::ObjectDef** outDef) const {
    if (frameIdx >= doc_.frames.size()) return -1;
    int best = -1;
    int8_t bestZ = -127;
    const auto& placements = doc_.frames[frameIdx].placements;
    for (uint8_t i = 0; i < placements.size(); i++) {
        const auto& p = placements[i];
        const draw::ObjectDef* def = findObjectDef(p.objId);
        if (!def) continue;
        if (!placementHitTest(p, *def, lx, ly)) continue;
        if (p.z >= bestZ) {
            best = i;
            bestZ = p.z;
            if (outDef) *outDef = def;
        }
    }
    return best;
}

void DrawScreen::pumpParallax(bool forEditorUi) {
    if (forEditorUi && !imuParallaxEnabled_) {
        sParallaxXSmoothed = 0.f;
        sParallaxYSmoothed = 0.f;
        return;
    }
    if (forEditorUi && painting_ && freezeParallaxWhilePainting_) {
        return;
    }
    extern IMU imu;
    // IMU tilt fields are screen-oriented but named from legacy callers:
    // tiltXMg is display left/right, tiltYMg is display up/down. Keeping
    // this order matters for the upright cube face; swapping them makes
    // left/right motion show up as up/down parallax.
    float cubeX = 0.f;
    float cubeY = 0.f;
    cubeFaceParallaxVector(imu.tiltXMg(), imu.tiltYMg(), imu.accelZMg(),
                           &cubeX, &cubeY);
    sParallaxXSmoothed += (cubeX - sParallaxXSmoothed) * kTiltSmoothing;
    sParallaxYSmoothed += (cubeY - sParallaxYSmoothed) * kTiltSmoothing;
}

void DrawScreen::parallaxOffset(int8_t z, int16_t* dx, int16_t* dy) const {
    if (painting_ && freezeParallaxWhilePainting_) {
        *dx = 0;
        *dy = 0;
        return;
    }
    const float k = kPxPerZAtFullTilt * (float)z / 1000.f;
    int32_t tx = static_cast<int32_t>(sParallaxXSmoothed * k);
    int32_t ty = static_cast<int32_t>(sParallaxYSmoothed * k);
    tx = std::clamp<int32_t>(tx, -kEditorParallaxClampPx, kEditorParallaxClampPx);
    ty = std::clamp<int32_t>(ty, -kEditorParallaxClampPx, kEditorParallaxClampPx);
    *dx = static_cast<int16_t>(tx);
    *dy = static_cast<int16_t>(ty);
}

void DrawScreen::expandActiveDrawnToCanvas() {
    if (!activeObjId_[0]) return;
    if (currentFrame_ >= doc_.frames.size()) return;

    draw::ObjectDef* def = nullptr;
    for (auto& o : doc_.objects) {
        if (o.type != draw::ObjectType::DrawnAnim) continue;
        if (std::strcmp(o.id, activeObjId_) == 0) { def = &o; break; }
    }
    if (!def) return;

    auto& placements = doc_.frames[currentFrame_].placements;
    draw::ObjectPlacement* placement = nullptr;
    for (auto& pl : placements) {
        if (std::strcmp(pl.objId, activeObjId_) == 0) { placement = &pl; break; }
    }
    if (!placement) return;

    if (def->drawnW == doc_.w && def->drawnH == doc_.h &&
        placement->x == 0 && placement->y == 0) {
        return;
    }

    const size_t newBytes = (size_t)doc_.w * doc_.h / 8u;
    uint8_t* fullBuf = (uint8_t*)BadgeMemory::allocPreferPsram(newBytes);
    if (!fullBuf) return;
    std::memset(fullBuf, 0, newBytes);

    const uint16_t newRowBytes = (doc_.w + 7) / 8;
    const uint16_t oldRowBytes = (def->drawnW + 7) / 8;
    if (def->drawnPixels) {
        for (uint16_t sy = 0; sy < def->drawnH; sy++) {
            const int16_t dy = (int16_t)sy + placement->y;
            if (dy < 0 || dy >= (int16_t)doc_.h) continue;
            for (uint16_t sx = 0; sx < def->drawnW; sx++) {
                const uint8_t bit =
                    (def->drawnPixels[sy * oldRowBytes + (sx >> 3)] >> (sx & 7)) & 1;
                if (!bit) continue;
                const int16_t dx = (int16_t)sx + placement->x;
                if (dx < 0 || dx >= (int16_t)doc_.w) continue;
                fullBuf[dy * newRowBytes + (dx >> 3)] |= (uint8_t)(1u << (dx & 7));
            }
        }
        free(def->drawnPixels);
    }
    def->drawnPixels = fullBuf;
    def->drawnW = doc_.w;
    def->drawnH = doc_.h;
    def->drawnDirty = true;

    placement->x = 0;
    placement->y = 0;
    doc_.dirty = true;
}

void DrawScreen::shrinkActiveDrawnToBBox() {
    if (!activeObjId_[0]) return;
    if (currentFrame_ >= doc_.frames.size()) return;

    // Find the active def + its placement.
    draw::ObjectDef* def = nullptr;
    for (auto& o : doc_.objects) {
        if (o.type != draw::ObjectType::DrawnAnim) continue;
        if (std::strcmp(o.id, activeObjId_) == 0) { def = &o; break; }
    }
    if (!def || !def->drawnPixels) return;

    auto& placements = doc_.frames[currentFrame_].placements;
    int placementIdx = -1;
    for (uint8_t i = 0; i < placements.size(); i++) {
        if (std::strcmp(placements[i].objId, activeObjId_) == 0) {
            placementIdx = i;
            break;
        }
    }
    if (placementIdx < 0) return;
    auto& placement = placements[placementIdx];

    const uint16_t w = def->drawnW;
    const uint16_t h = def->drawnH;
    const uint16_t rowBytes = (w + 7) / 8;

    // Find min/max x/y of set bits.
    int16_t minX = INT16_MAX, minY = INT16_MAX;
    int16_t maxX = -1, maxY = -1;
    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            if ((def->drawnPixels[y * rowBytes + (x >> 3)] >> (x & 7)) & 1) {
                if ((int16_t)x < minX) minX = x;
                if ((int16_t)y < minY) minY = y;
                if ((int16_t)x > maxX) maxX = x;
                if ((int16_t)y > maxY) maxY = y;
            }
        }
    }

    if (maxX < 0) {
        // Empty layer — drop both placement and ObjectDef so the next save
        // garbage-collects the o<id>.fb file.
        placements.erase(placements.begin() + placementIdx);
        // Find the def's index and erase. Have to re-find since pointer would
        // be invalidated by erase otherwise (but we're about to erase).
        for (auto it = doc_.objects.begin(); it != doc_.objects.end(); ++it) {
            if (std::strcmp(it->id, activeObjId_) != 0) continue;
            if (it->drawnPixels) free(it->drawnPixels);
            doc_.objects.erase(it);
            break;
        }
        activeObjId_[0] = '\0';
        doc_.dirty = true;
        return;
    }

    // Already tight? (covers full extent)
    if (minX == 0 && minY == 0 && maxX == (int16_t)w - 1 &&
        maxY == (int16_t)h - 1) {
        return;
    }

    const uint16_t newW = (uint16_t)(maxX - minX + 1);
    const uint16_t newH = (uint16_t)(maxY - minY + 1);
    const uint16_t newRowBytes = (newW + 7) / 8;
    const size_t newBytes = (size_t)newRowBytes * newH;

    uint8_t* newBuf = (uint8_t*)BadgeMemory::allocPreferPsram(newBytes);
    if (!newBuf) return;
    std::memset(newBuf, 0, newBytes);

    // Copy bits from (minX, minY)..(maxX, maxY) into the new buffer at (0,0).
    for (int16_t sy = minY; sy <= maxY; sy++) {
        for (int16_t sx = minX; sx <= maxX; sx++) {
            const uint8_t bit =
                (def->drawnPixels[sy * rowBytes + (sx >> 3)] >> (sx & 7)) & 1;
            if (!bit) continue;
            const int16_t dx = sx - minX;
            const int16_t dy = sy - minY;
            newBuf[dy * newRowBytes + (dx >> 3)] |= (uint8_t)(1u << (dx & 7));
        }
    }

    free(def->drawnPixels);
    def->drawnPixels = newBuf;
    def->drawnW = newW;
    def->drawnH = newH;
    def->drawnDirty = true;

    // Shift placement so the visible drawing stays where it is on screen.
    placement.x = (int16_t)(placement.x + minX);
    placement.y = (int16_t)(placement.y + minY);
    doc_.dirty = true;
}

void DrawScreen::rebuildThumb(uint8_t frameIdx) {
    if (frameIdx >= doc_.frames.size()) return;
    if (frameIdx >= draw::kMaxFrames) return;
    uint8_t* tb = thumbs_[frameIdx];
    std::memset(tb, 0, 8);

    // Composite all DrawnAnim layers into a virtual full-canvas bitmap, then
    // nearest-neighbor downscale. We don't allocate the full bitmap; instead
    // sample each thumb pixel by walking the source pixels lazily.
    const uint16_t scaleX = doc_.w / kThumbW;
    const uint16_t scaleY = doc_.h / kThumbH;
    const uint16_t threshold = (scaleX * scaleY) / 2;
    const auto& placements = doc_.frames[frameIdx].placements;

    for (uint8_t ty = 0; ty < kThumbH; ty++) {
        const uint16_t sy0 = ty * scaleY;
        for (uint8_t tx = 0; tx < kThumbW; tx++) {
            const uint16_t sx0 = tx * scaleX;
            uint16_t count = 0;
            for (uint16_t by = 0; by < scaleY; by++) {
                const uint16_t sy = sy0 + by;
                for (uint16_t bx = 0; bx < scaleX; bx++) {
                    const uint16_t sx = sx0 + bx;
                    bool lit = false;
                    for (const auto& p : placements) {
                        const draw::ObjectDef* def = nullptr;
                        for (const auto& o : doc_.objects) {
                            if (std::strcmp(o.id, p.objId) == 0) { def = &o; break; }
                        }
                        if (!def) continue;
                        if (def->type != draw::ObjectType::DrawnAnim) continue;
                        if (!def->drawnPixels) continue;
                        const int16_t lx = (int16_t)sx - p.x;
                        const int16_t ly = (int16_t)sy - p.y;
                        if (lx < 0 || lx >= (int16_t)def->drawnW) continue;
                        if (ly < 0 || ly >= (int16_t)def->drawnH) continue;
                        const uint16_t rowBytes = (def->drawnW + 7) / 8;
                        const uint8_t byte =
                            def->drawnPixels[ly * rowBytes + (lx >> 3)];
                        if ((byte >> (lx & 7)) & 1) { lit = true; break; }
                    }
                    if (lit) count++;
                }
            }
            if (count > threshold) tb[ty] |= (1u << tx);
        }
    }
    thumbValid_[frameIdx] = true;
}

// ── Frame ops ──────────────────────────────────────────────────────────────

bool DrawScreen::addFrame(bool duplicateCurrent) {
    if (doc_.frames.size() >= draw::kMaxFrames) return false;

    draw::Frame nf;
    nf.durationMs = draw::kDefaultDurationMs;
    if (duplicateCurrent && currentFrame_ < doc_.frames.size()) {
        const draw::Frame& src = doc_.frames[currentFrame_];
        nf.placements = src.placements;
        nf.durationMs = src.durationMs;
    }

    auto it = doc_.frames.begin() + currentFrame_ + 1;
    doc_.frames.insert(it, std::move(nf));
    currentFrame_ += 1;
    doc_.dirty = true;
    rebuildAllThumbs();
    return true;
}

bool DrawScreen::deleteCurrentFrame() {
    if (doc_.frames.size() <= 1) return false;
    snapshotForUndo(currentFrame_);
    doc_.frames.erase(doc_.frames.begin() + currentFrame_);
    if (currentFrame_ >= doc_.frames.size()) {
        currentFrame_ = (uint8_t)(doc_.frames.size() - 1);
    }
    doc_.dirty = true;
    rebuildAllThumbs();
    return true;
}

void DrawScreen::moveToFrame(uint8_t idx) {
    if (idx >= doc_.frames.size()) return;
    currentFrame_ = idx;
    rebuildThumb(currentFrame_);
}

void DrawScreen::advancePlayback(uint32_t nowMs) {
    if (doc_.frames.empty()) return;
    uint16_t dur = doc_.frames[currentFrame_].durationMs;
    if (dur < draw::kMinDurationMs) dur = draw::kMinDurationMs;
    if (nowMs - playFrameStartMs_ >= dur) {
        currentFrame_ = (currentFrame_ + 1) % (uint8_t)doc_.frames.size();
        playFrameStartMs_ = nowMs;
    }
}

// ── Object / sticker render ────────────────────────────────────────────────

DrawScreen::SavedStickerCache* DrawScreen::getSavedSticker(const char* animId,
                                                           uint8_t scale) {
    for (auto& c : savedCache_) {
        if (c.scale == scale && std::strcmp(c.animId, animId) == 0) return &c;
    }

    // Lazy-load this saved anim's metadata + frames at the requested scale.
    draw::AnimDoc src;
    if (!draw::load(animId, src)) return nullptr;

    SavedStickerCache cache{};
    std::strncpy(cache.animId, animId, sizeof(cache.animId) - 1);
    cache.w = src.w;
    cache.h = src.h;
    cache.frameCount = (uint8_t)src.frames.size();
    if (cache.frameCount == 0 || cache.frameCount > draw::kMaxFrames) {
        draw::freeAll(src);
        return nullptr;
    }

    if (scale == 0 || scale == src.w) {
        cache.scale = (uint8_t)src.w;
        cache.scaledW = src.w;
        cache.scaledH = src.h;
    } else {
        if (src.w % scale != 0) {
            draw::freeAll(src);
            return nullptr;
        }
        const uint8_t div = (uint8_t)(src.w / scale);
        if (div == 0 || src.h % div != 0) {
            draw::freeAll(src);
            return nullptr;
        }
        cache.scale = scale;
        cache.scaledW = scale;
        cache.scaledH = (uint16_t)(src.h / div);
    }

    const size_t scaledFrameBytes = ((cache.scaledW + 7) / 8) * cache.scaledH;
    cache.scaledFrames = (uint8_t*)BadgeMemory::allocPreferPsram(
        scaledFrameBytes * cache.frameCount);
    if (!cache.scaledFrames) {
        draw::freeAll(src);
        return nullptr;
    }

    // Per-frame composite: OR together every DrawnAnim layer on this frame
    // (positioned by its placement) into a full-canvas-sized buffer, then
    // optionally scale into the cache slot. Catalog / saved-anim sub-stickers
    // inside the source doc are intentionally skipped — nesting them would
    // recurse and isn't needed for Phase 1 sticker previews.
    const size_t srcBytes = (size_t)src.w * src.h / 8u;
    uint8_t* composite = (uint8_t*)BadgeMemory::allocPreferPsram(srcBytes);
    if (!composite) {
        free(cache.scaledFrames);
        draw::freeAll(src);
        return nullptr;
    }
    const uint16_t srcRowBytes = (src.w + 7) / 8;

    for (uint8_t i = 0; i < cache.frameCount; i++) {
        cache.durations[i] = src.frames[i].durationMs;
        std::memset(composite, 0, srcBytes);
        for (const auto& pl : src.frames[i].placements) {
            const draw::ObjectDef* def = nullptr;
            for (const auto& o : src.objects) {
                if (std::strcmp(o.id, pl.objId) == 0) { def = &o; break; }
            }
            if (!def || def->type != draw::ObjectType::DrawnAnim) continue;
            if (!def->drawnPixels) continue;
            const uint16_t lrowBytes = (def->drawnW + 7) / 8;
            for (uint16_t ly = 0; ly < def->drawnH; ly++) {
                const int16_t cy = (int16_t)pl.y + (int16_t)ly;
                if (cy < 0 || cy >= (int16_t)src.h) continue;
                for (uint16_t lx = 0; lx < def->drawnW; lx++) {
                    const int16_t cx = (int16_t)pl.x + (int16_t)lx;
                    if (cx < 0 || cx >= (int16_t)src.w) continue;
                    const uint8_t srcByte =
                        def->drawnPixels[ly * lrowBytes + (lx >> 3)];
                    if ((srcByte >> (lx & 7)) & 1) {
                        composite[cy * srcRowBytes + (cx >> 3)] |=
                            (1u << (cx & 7));
                    }
                }
            }
        }

        uint8_t* dst = cache.scaledFrames + i * scaledFrameBytes;
        if (cache.scaledW == src.w && cache.scaledH == src.h) {
            std::memcpy(dst, composite, scaledFrameBytes);
        } else {
            ImageScaler::scale(composite,
                               (uint8_t)src.w, (uint8_t)src.h,
                               dst,
                               (uint8_t)cache.scaledW, (uint8_t)cache.scaledH);
        }
    }
    free(composite);

    draw::freeAll(src);
    savedCache_.push_back(std::move(cache));
    return &savedCache_.back();
}

uint8_t DrawScreen::resolveScale(const draw::ObjectPlacement& p,
                                 const draw::ObjectDef& def) const {
    if (p.scale > 0) return p.scale;
    if (def.type == draw::ObjectType::Catalog) {
        // Default = 48 if data48 available, else 64.
        if (def.catalogIdx >= 0 && def.catalogIdx < (int16_t)kImageCatalogCount) {
            const ImageInfo& img = kImageCatalog[def.catalogIdx];
            return img.data48 ? 48 : 64;
        }
        return 48;
    }
    return 0;  // saved-anim native (filled in by getSavedSticker)
}

void DrawScreen::renderObjectInstance(oled& d, int16_t canvasX, int16_t canvasY,
                                      const draw::ObjectPlacement& p,
                                      const draw::ObjectDef& def,
                                      uint32_t nowMs) {
    const int16_t drawX = canvasX + p.x;
    const int16_t drawY = canvasY + p.y;

    if (def.type == draw::ObjectType::Catalog) {
        if (def.catalogIdx < 0 || def.catalogIdx >= (int16_t)kImageCatalogCount) {
            return;
        }
        const ImageInfo& img = kImageCatalog[def.catalogIdx];
        const uint8_t scale = resolveScale(p, def);
        const bool use48 = img.data48 && (scale == 48 || scale == 24 || scale == 12);
        const uint8_t srcDim = use48 ? 48 : 64;

        uint8_t frameIdx = 0;
        if (img.frameCount > 1) {
            uint32_t totalMs = 0;
            for (uint8_t i = 0; i < img.frameCount; i++) {
                totalMs += img.frameTimes ? img.frameTimes[i] : 180;
            }
            if (totalMs == 0) totalMs = 180 * img.frameCount;
            uint32_t t = (nowMs + p.phaseMs) % totalMs;
            for (uint8_t i = 0; i < img.frameCount; i++) {
                uint32_t fd = img.frameTimes ? img.frameTimes[i] : 180;
                if (t < fd) { frameIdx = i; break; }
                t -= fd;
            }
        }

        const uint8_t* fb = ImageScaler::getFrame(img, frameIdx, scale);
        d.drawXBM(drawX, drawY, srcDim, srcDim, fb, scale, scale);
    } else if (def.type == draw::ObjectType::SavedAnim) {
        SavedStickerCache* c = getSavedSticker(def.savedAnimId,
                                               p.scale ? p.scale : 0);
        if (!c) return;
        uint8_t frameIdx = 0;
        if (c->frameCount > 1) {
            uint32_t totalMs = 0;
            for (uint8_t i = 0; i < c->frameCount; i++) totalMs += c->durations[i];
            if (totalMs == 0) totalMs = draw::kDefaultDurationMs * c->frameCount;
            uint32_t t = (nowMs + p.phaseMs) % totalMs;
            for (uint8_t i = 0; i < c->frameCount; i++) {
                uint32_t fd = c->durations[i];
                if (fd == 0) fd = draw::kDefaultDurationMs;
                if (t < fd) { frameIdx = i; break; }
                t -= fd;
            }
        }
        const size_t scaledFrameBytes = ((c->scaledW + 7) / 8) * c->scaledH;
        const uint8_t* bits = c->scaledFrames + frameIdx * scaledFrameBytes;
        d.drawXBM(drawX, drawY, c->scaledW, c->scaledH, bits);
    } else if (def.type == draw::ObjectType::DrawnAnim) {
        if (!def.drawnPixels) return;
        d.drawXBM(drawX, drawY, def.drawnW, def.drawnH, def.drawnPixels);
    } else if (def.type == draw::ObjectType::Text) {
        if (!def.textContent[0]) return;
        const uint8_t prevFamily = d.fontFamilyIndex();
        const uint8_t prevSlot = d.fontSlotIndex();
        d.setFontFamilyAndSlot(def.textFontFamily, def.textFontSlot);
        const int ascent = d.getAscent();
        const int descent = d.getDescent();
        const bool hasEmoji = EmojiText::stringHasEmoji(def.textContent);
        const int textW = hasEmoji
            ? EmojiText::mixedLineWidth(d, def.textContent)
            : d.getUTF8Width(def.textContent);
        const int textH = ascent - descent;
        const int baselineY = drawY + ascent + 1;
        // Emoji baseline sits lower than ASCII to vertically center the
        // taller glyphs within the same line band.
        const int emojiBaseline = baselineY + 4;
        const uint8_t* asciiFont = kFontGrid[def.textFontFamily][def.textFontSlot];

        if (def.textStackMode == draw::TextStackMode::RoundedBox) {
            d.setBitmapTransparent(false);
            d.setDrawColor(0);
            d.drawRBox(drawX - 2, drawY, textW + 4, textH + 3, 2);
            d.setDrawColor(1);
            d.setFontMode(1);
            if (hasEmoji) {
                EmojiText::drawMixedLine(d, drawX, baselineY, emojiBaseline,
                                         def.textContent, asciiFont);
            } else {
                d.drawUTF8(drawX, baselineY, def.textContent);
            }
            d.setFontMode(0);
            d.setBitmapTransparent(true);
        } else {
            d.setFontMode(1);
            d.setDrawColor(def.textStackMode == draw::TextStackMode::XOR ? 2 : 1);
            if (hasEmoji) {
                EmojiText::drawMixedLine(d, drawX, baselineY, emojiBaseline,
                                         def.textContent, asciiFont);
            } else {
                d.drawUTF8(drawX, baselineY, def.textContent);
            }
            d.setDrawColor(1);
            d.setFontMode(0);
        }
        d.setFontFamilyAndSlot(prevFamily, prevSlot);
    }
}

bool DrawScreen::placementHitTest(const draw::ObjectPlacement& p,
                                  const draw::ObjectDef& def,
                                  int16_t cx, int16_t cy) const {
    int16_t w = 0, h = 0;
    if (!objectDimensions(p, def, &w, &h)) return false;
    int16_t dx = 0, dy = 0;
    parallaxOffset(p.z, &dx, &dy);
    const int16_t x = p.x + dx;
    const int16_t y = p.y + dy;
    return cx >= x && cx < x + w && cy >= y && cy < y + h;
}

// ── Render ─────────────────────────────────────────────────────────────────

void DrawScreen::renderDocComposition(oled& d, draw::AnimDoc& doc, uint8_t frameIdx) {
    // Same IMU smoothing as the composer; `nametagCompositionParallax` uses the
    // same z scale as `parallaxOffset` but a looser per-axis clamp (nametag only).

    pumpParallax(/*forEditorUi=*/false);

    d.setDrawColor(0);
    d.drawBox(0, 0, 128, 64);
    d.setDrawColor(1);

    // Canvas origin: 128×64 sits at (0,0); 48×48 zigmoji sits at (40,8).
    const int16_t ox = (doc.w == draw::kCanvasZigW) ? 40 : 0;
    const int16_t oy = (doc.w == draw::kCanvasZigW) ? 8  : 0;

    d.setBitmapTransparent(true);
    if (frameIdx < doc.frames.size()) {
        const auto& placements = doc.frames[frameIdx].placements;
        std::vector<uint8_t> order(placements.size());
        for (uint8_t i = 0; i < placements.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](uint8_t a, uint8_t b) {
                             return placements[a].z < placements[b].z;
                         });
        const uint32_t now = millis();
        for (uint8_t idx : order) {
            auto p = placements[idx];
            int16_t dx = 0, dy = 0;
            nametagCompositionParallax(p.z, &dx, &dy);
            p.x = static_cast<int16_t>(p.x + dx);
            p.y = static_cast<int16_t>(p.y + dy);
            const draw::ObjectDef* def = nullptr;
            for (const auto& o : doc.objects) {
                if (std::strcmp(o.id, p.objId) == 0) { def = &o; break; }
            }
            if (def) renderObjectInstance(d, ox, oy, p, *def, now);
        }
    }
    d.setBitmapTransparent(false);
    d.setFontMode(0);
    d.setDrawColor(1);
}

void DrawScreen::render(oled& d, GUIManager& gui) {
    (void)gui;
    pumpParallax(/*forEditorUi=*/true);
    if (mode_ == Mode::Tutorial) {
        renderTutorial(d);
        return;
    }
    if (mode_ == Mode::Playing) {
        advancePlayback(millis());
    }

    // Clear and paint pixel layer.
    d.setDrawColor(0);
    d.drawBox(0, 0, 128, 64);
    d.setDrawColor(1);

    int16_t ox, oy;
    canvasOriginScreen(&ox, &oy);

    // Z-sorted placements for the active frame. Transparent bitmap mode so
    // overlapping layers OR together (without it u8g2 paints the 0-bits as
    // background, which shows up as a solid black square covering whatever
    // is underneath).
    d.setBitmapTransparent(true);
    if (currentFrame_ < doc_.frames.size()) {
        const auto& placements = doc_.frames[currentFrame_].placements;
        std::vector<uint8_t> order(placements.size());
        for (uint8_t i = 0; i < placements.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](uint8_t a, uint8_t b) {
                             return placements[a].z < placements[b].z;
                         });
        const uint32_t now2 = millis();
        // While the TextAppearance popup is active we re-render the active
        // text object with pending settings inside renderTextAppearancePopup,
        // so suppress the canvas-level render of that placement to avoid the
        // baked-in version sitting underneath the live preview.
        const bool suppressActiveText = (mode_ == Mode::TextAppearance);
        for (uint8_t idx : order) {
            auto p = placements[idx];
            int16_t dx = 0, dy = 0;
            parallaxOffset(p.z, &dx, &dy);
            p.x = (int16_t)(p.x + dx);
            p.y = (int16_t)(p.y + dy);
            const draw::ObjectDef* def = nullptr;
            for (const auto& d2 : doc_.objects) {
                if (std::strcmp(d2.id, p.objId) == 0) { def = &d2; break; }
            }
            if (suppressActiveText && def && def->type == draw::ObjectType::Text &&
                std::strcmp(def->id, textAppear_.targetObjId) == 0) {
                continue;
            }
            if (def) renderObjectInstance(d, ox, oy, p, *def, now2);
        }
    }

    // Ghost (sticker following cursor). The active ObjectDef stays in
    // doc_.objects throughout the drag (we only ever erase the placement
    // from the frame), so we look it up by id rather than rebuilding a
    // synthetic def — that's the only path that lets DrawnAnims render
    // while being dragged, since their pixel data lives on the ObjectDef
    // itself.
    if (mode_ == Mode::Ghosted && ghost_.active) {
        const draw::ObjectDef* def = nullptr;
        for (const auto& o : doc_.objects) {
            if (std::strcmp(o.id, ghost_.objId) == 0) { def = &o; break; }
        }
        if (def) {
            int16_t cx, cy;
            canvasLocalCursor(&cx, &cy);
            draw::ObjectPlacement p{};
            std::strncpy(p.objId, ghost_.objId, sizeof(p.objId) - 1);
            p.scale = ghost_.scale;
            p.x = (int16_t)(cx - ghost_.grabOffsetX);
            p.y = (int16_t)(cy - ghost_.grabOffsetY);
            renderObjectInstance(d, ox, oy, p, *def, millis());
        }
    }

    // Restore solid bitmap mode for the rest of the chrome / cursor / modal
    // rendering — the rest of the GUI relies on u8g2's default solid blits
    // and we don't want our transparent-mode toggle leaking out of this
    // screen.
    d.setBitmapTransparent(false);
    d.setFontMode(0);
    d.setDrawColor(1);
    d.setFont(UIFonts::kText);

    // Hover flash for Hand / Edit before chrome so the chrome z-orders
    // above it (otherwise the highlight peeks out under tool strips).
    renderHoverFlash(d, ox, oy);

    // Chrome on top — each strip renders independently based on its own
    // edge-activation flag.
    if (topShown_)    renderTopBar(d);
    if (leftShown_)   renderToolStrip(d);
    if (rightShown_)  renderRightStrip(d);
    if (bottomShown_) renderTimeline(d);

    // Modal overlays.
    if (mode_ == Mode::DurationPopup) renderDurationPopup(d);
    else if (mode_ == Mode::SettingsPopup) renderSettingsPopup(d);
    else if (mode_ == Mode::HelpScroll) renderHelpScroll(d);
    else if (mode_ == Mode::ZAdjust) renderZAdjustPopup(d);
    else if (mode_ == Mode::TextAppearance) renderTextAppearancePopup(d);
    else if (mode_ == Mode::FieldPicker) renderFieldPickerPopup(d);
    else if (mode_ == Mode::SaveExitPrompt) renderSaveExitPrompt(d);
    else if (mode_ == Mode::ExitNametagPrompt) renderExitNametagPrompt(d);
    else if (mode_ == Mode::ContextMenu) {
        renderFooterActions(d, nullptr, nullptr, nullptr, "ok");
    }
    if (mode_ == Mode::Editing || mode_ == Mode::ToolMenu) {
        // Marquee help under the timeline whenever a chrome icon is hovered.
        renderHelpMarquee(d);
    }

    renderModeCursor(d);
}

void DrawScreen::renderModeCursor(oled& d) {
    // Full-screen overlays — no cursor.
    if (mode_ == Mode::Tutorial ||
        mode_ == Mode::ToolMenu || mode_ == Mode::ContextMenu ||
        mode_ == Mode::HelpScroll) return;
    // While dragging stickers / repositioning ghosts, omit the hotspot so only
    // the semi-transparent object previews the gesture.
    if (mode_ == Mode::Ghosted) return;

    const int16_t cx = (int16_t)cursorXf_;
    const int16_t cy = (int16_t)cursorYf_;

    struct CursorXorGuard {
        oled& dd;
        explicit CursorXorGuard(oled& out) : dd(out) {
            dd.setBitmapTransparent(false);
            dd.setFontMode(0);
            dd.setDrawColor(2);
        }
        ~CursorXorGuard() {
            dd.setFontMode(0);
            dd.setDrawColor(1);
        }
    };
    CursorXorGuard xorGuard{d};

    char badge = '\0';
    if (mode_ == Mode::Playing) {
        badge = 'P';
    } else if (mode_ == Mode::ContextMenu) {
        static constexpr char kBadges[] = {'M', 'E', 'S', 'Z', 'D'};
        badge = kBadges[ctxMenu_.cursor < 5 ? ctxMenu_.cursor : 0];
    } else if (mode_ == Mode::DurationPopup) {
        badge = 'd';
    } else if (mode_ == Mode::ZAdjust) {
        badge = 'Z';
    } else if (mode_ == Mode::TextAppearance) {
        badge = 'T';
    } else if (mode_ == Mode::SaveExitPrompt) {
        badge = '?';
    } else if (mode_ == Mode::Editing && currentTool_ == Tool::Hand) {
        badge = 'H';
    } else if (mode_ == Mode::Editing && currentTool_ == Tool::Edit) {
        badge = 'E';
    }

    // Draw the brush shape as an XOR stamp so the cursor shows exactly what
    // the stroke will paint. For non-paint modes, fall back to a small cross.
    if (mode_ == Mode::Editing &&
        (currentTool_ == Tool::Draw || currentTool_ == Tool::Edit)) {
        const uint8_t s = effectiveBrushPx();
        if (s == 1) {
            d.drawPixel(cx, cy);
        } else if (s % 2 == 0) {
            // Even D: square. D2→side 2, D4→3, D6→4, D8→5
            const int16_t side = (int16_t)(s / 2 + 1);
            const int16_t half = side / 2;
            d.drawBox(cx - half, cy - half, side, side);
        } else {
            // Odd D≥3: circle. D3→r=1, D5→r=2, D7→r=3
            const int16_t r = (int16_t)(s / 2);
            const int16_t rSq = r * r;
            for (int16_t iy = -r; iy <= r; iy++)
                for (int16_t ix = -r; ix <= r; ix++)
                    if (ix * ix + iy * iy <= rSq)
                        d.drawPixel(cx + ix, cy + iy);
        }
    } else {
        d.drawHLine(cx - kCursorCrossLen, cy, kCursorCrossLen * 2 + 1);
        d.drawVLine(cx, cy - kCursorCrossLen, kCursorCrossLen * 2 + 1);
    }

    if (badge) {
        char txt[2] = {badge, '\0'};
        d.setFont(UIFonts::kText);
        d.setFontMode(1);
        int16_t bx = cx + 5;
        int16_t by = cy - 4;
        if (bx > 121) bx = cx - 9;
        if (by < 7) by = cy + 10;
        d.drawStr(bx, by, txt);
    }
}

void DrawScreen::renderHoverFlash(oled& d, int16_t ox, int16_t oy) {
    if (mode_ != Mode::Editing && mode_ != Mode::ContextMenu) return;
    if (currentFrame_ >= doc_.frames.size()) return;

    if (mode_ == Mode::ContextMenu) {
        const draw::ObjectDef* def = findObjectDef(ctxMenu_.targetObjId);
        const draw::ObjectPlacement* p = findPlacement(currentFrame_, ctxMenu_.targetObjId);
        int16_t w = 0, h = 0;
        if (!def || !p || !objectDimensions(*p, *def, &w, &h)) return;
        if ((millis() / 250) & 1) return;
        int16_t dx = 0, dy = 0;
        parallaxOffset(p->z, &dx, &dy);
        const int16_t sx = ox + p->x + dx;
        const int16_t sy = oy + p->y + dy;
        d.setDrawColor(2);
        d.drawHLine(sx, sy, h > 0 ? w : 0);
        d.drawHLine(sx, sy + h - 1, w);
        d.drawVLine(sx, sy, h);
        d.drawVLine(sx + w - 1, sy, h);
        d.setDrawColor(1);
        return;
    }

    // Edit-tool selected-layer indicator: when a DrawnAnim is active, blink
    // its bbox briefly once every 2 seconds so the user knows which layer
    // their next stroke will land on. Cycle: visible for ~120 ms each
    // 2000 ms window.
    if (currentTool_ == Tool::Edit && activeObjId_[0] && !painting_) {
        const draw::ObjectDef* def = nullptr;
        for (const auto& o : doc_.objects) {
            if (std::strcmp(o.id, activeObjId_) == 0) { def = &o; break; }
        }
        if (def && def->type == draw::ObjectType::DrawnAnim) {
            const auto& placements = doc_.frames[currentFrame_].placements;
            for (const auto& p : placements) {
                if (std::strcmp(p.objId, activeObjId_) != 0) continue;
                if ((millis() % 2000) < 120) {
                    d.setDrawColor(2);
                    int16_t dx = 0, dy = 0;
                    parallaxOffset(p.z, &dx, &dy);
                    const int16_t sx = ox + p.x + dx;
                    const int16_t sy = oy + p.y + dy;
                    d.drawHLine(sx, sy,                  def->drawnW);
                    d.drawHLine(sx, sy + def->drawnH-1,  def->drawnW);
                    d.drawVLine(sx,                     sy, def->drawnH);
                    d.drawVLine(sx + def->drawnW - 1,   sy, def->drawnH);
                    d.setDrawColor(1);
                }
                break;
            }
        }
    }

    // Hover flash for Hand / Edit while not painting.
    if (currentTool_ != Tool::Hand && currentTool_ != Tool::Edit) return;
    if (painting_) return;
    if (!cursorInCanvas()) return;

    int16_t lx, ly;
    canvasLocalCursor(&lx, &ly);

    // Topmost (highest-z) sticker the cursor is over. Edit ignores non-
    // DrawnAnim hits so the user can't try to "edit" a catalog sticker.
    const auto& placements = doc_.frames[currentFrame_].placements;
    int hit = -1;
    int8_t hitZ = -127;
    int16_t hitX = 0, hitY = 0, hitW = 0, hitH = 0;
    for (uint8_t i = 0; i < placements.size(); i++) {
        const auto& p = placements[i];
        const draw::ObjectDef* def = nullptr;
        for (const auto& o : doc_.objects) {
            if (std::strcmp(o.id, p.objId) == 0) { def = &o; break; }
        }
        if (!def) continue;
        if (currentTool_ == Tool::Edit &&
            def->type != draw::ObjectType::DrawnAnim) continue;
        if (!placementHitTest(p, *def, lx, ly)) continue;
        if (p.z < hitZ) continue;

        int16_t w = 0, h = 0;
        if (!objectDimensions(p, *def, &w, &h)) continue;
        hit = i;
        hitZ = p.z;
        int16_t dx = 0, dy = 0;
        parallaxOffset(p.z, &dx, &dy);
        hitX = p.x + dx;
        hitY = p.y + dy;
        hitW = w;
        hitH = h;
    }

    if (hit < 0) return;

    // 4 Hz blink — visible half the time so it reads as "flashing".
    if ((millis() / 250) & 1) return;

    d.setDrawColor(2);
    const int16_t sx = ox + hitX;
    const int16_t sy = oy + hitY;
    d.drawHLine(sx, sy,             hitW);
    d.drawHLine(sx, sy + hitH - 1,  hitW);
    d.drawVLine(sx,             sy, hitH);
    d.drawVLine(sx + hitW - 1,  sy, hitH);
    d.setDrawColor(1);
}

void DrawScreen::renderTopBar(oled& d) {
    d.setDrawColor(0);
    d.drawBox(0, kTopBarY, 128, kTopBarH);
    d.setDrawColor(1);
    d.setFont(UIFonts::kText);

    // Status text on the left (truncated to leave room for undo/redo + action icons).
    char buf[24];
    const char* toolName =
        (currentTool_ == Tool::Draw) ? "D" :
        (currentTool_ == Tool::Hand) ? "H" : "E";
    uint16_t dur = (currentFrame_ < doc_.frames.size())
                   ? doc_.frames[currentFrame_].durationMs : 0;
    std::snprintf(buf, sizeof(buf), "F%u/%u %s%u %ums",
                  (unsigned)(currentFrame_ + 1),
                  (unsigned)doc_.frames.size(),
                  toolName, (unsigned)brushSize_,
                  (unsigned)dur);
    OLEDLayout::fitText(d, buf, sizeof(buf), kTopRightUndoX - 2);
    d.drawStr(1, 6, buf);

    // Icon row on the right: play, save, exit.
    HitZone hit = hitZoneAtCursor(nullptr);
    for (uint8_t i = 0; i < kTopRightSlotCount; i++) {
        const TopRightSlot& s = kTopRightSlots[i];
        const bool hot = (hit == s.zone);
        const bool selected = (s.zone == HitZone::ToolPlay && mode_ == Mode::Playing) ||
                              (s.zone == HitZone::ToolUndo && undo_.valid) ||
                              (s.zone == HitZone::ToolRedo && redo_.valid);
        if (hot || selected) {
            d.setDrawColor(1);
            d.drawBox(s.x, kTopRightIconY, 8, 8);
            d.setDrawColor(0);
            d.drawXBM(s.x, kTopRightIconY, 8, 8, s.icon);
            d.setDrawColor(1);
        } else {
            d.drawXBM(s.x, kTopRightIconY, 8, 8, s.icon);
        }
    }
}

void DrawScreen::renderTimeline(oled& d) {
    d.setDrawColor(0);
    d.drawBox(0, kBottomBarY, 128, kBottomBarH);
    d.setDrawColor(1);
    d.drawHLine(0, kBottomBarY, 128);

    constexpr uint8_t kBrushCount = 8;
    constexpr int16_t kBrushAreaW = 48;  // 8 * 6
    constexpr int16_t kBrushCellW = kBrushAreaW / kBrushCount; // 6px each
    const int16_t brushAreaX = 128 - kBrushAreaW;

    HitZone hit = hitZoneAtCursor(nullptr);
    uint8_t hitExtra = 0;
    hitZoneAtCursor(&hitExtra);

    for (uint8_t i = 0; i < kBrushCount; i++) {
        const uint8_t sz = i + 1;
        const int16_t bx = brushAreaX + i * kBrushCellW;
        const bool hot = (hit == HitZone::BrushSize && hitExtra == sz);
        const bool sel = (brushSize_ == sz);
        if (hot || sel) {
            d.setDrawColor(1);
            d.drawBox(bx, kBottomBarY + 1, kBrushCellW, kBottomBarH - 1);
            d.setDrawColor(0);
        }
        // Icon shape matches actual brush: even D = square, odd D≥3 = circle.
        const int16_t ccx = bx + kBrushCellW / 2;
        const int16_t ccy = kBottomBarY + kBottomBarH / 2;
        if (sz == 1) {
            d.drawPixel(ccx, ccy);
        } else if (sz % 2 == 0) {
            // Even D: square icon. D2→side 2, D4→3, D6→4, D8→5
            const int16_t side = (int16_t)(sz / 2 + 1);
            const int16_t clamp = (side > 5) ? 5 : side;
            const int16_t half = clamp / 2;
            d.drawBox(ccx - half, ccy - half, clamp, clamp);
        } else {
            // Odd D≥3: circle icon. D3→r=1, D5→r=2, D7→r=3
            const int16_t r = (int16_t)(sz / 2);
            const int16_t clampR = (r > 3) ? 3 : r;
            for (int16_t iy = -clampR; iy <= clampR; iy++)
                for (int16_t ix = -clampR; ix <= clampR; ix++)
                    if (ix * ix + iy * iy <= clampR * clampR)
                        d.drawPixel(ccx + ix, ccy + iy);
        }
        d.setDrawColor(1);
    }

    d.drawVLine(brushAreaX - 1, kBottomBarY + 1, kBottomBarH - 1);

    // Frame thumbs fill the left portion.
    const int16_t timelineX0 = 0;
    const uint8_t total = (uint8_t)doc_.frames.size();
    const uint8_t maxSlots = (uint8_t)((brushAreaX - 2 - timelineX0) / kThumbCellW);
    uint8_t firstVisible = 0;
    if (currentFrame_ + 2 >= maxSlots) {
        firstVisible = (uint8_t)(currentFrame_ + 2 - maxSlots);
        if (firstVisible + maxSlots > total + 2) {
            firstVisible = total + 2 > maxSlots ? (total + 2 - maxSlots) : 0;
        }
    }

    for (uint8_t slot = 0; slot < maxSlots; slot++) {
        const uint8_t idx = firstVisible + slot;
        const int16_t x = timelineX0 + slot * kThumbCellW;
        if (idx < total) {
            d.drawHLine(x, kBottomBarY + 1, kThumbW);
            d.drawHLine(x, kBottomBarY + 1 + kThumbH, kThumbW);
            d.drawVLine(x, kBottomBarY + 1, kThumbH);
            d.drawVLine(x + kThumbW - 1, kBottomBarY + 1, kThumbH);

            if (idx < draw::kMaxFrames && thumbValid_[idx]) {
                d.drawXBM(x, kBottomBarY + 1, kThumbW, kThumbH, thumbs_[idx]);
            }

            if (idx == currentFrame_) {
                d.drawBox(x, kBottomBarY + 1, 2, 1);
                d.drawBox(x + kThumbW - 2, kBottomBarY + 1, 2, 1);
            }
        } else if (idx == total) {
            const int16_t cx = x + kPlusTileW / 2;
            const int16_t cy = kBottomBarY + kBottomBarH / 2;
            const bool hot2 = (hit == HitZone::TimelinePlus);
            if (hot2) {
                d.setDrawColor(1);
                d.drawBox(x, kBottomBarY + 1, kPlusTileW, kBottomBarH - 1);
                d.setDrawColor(0);
            }
            d.drawHLine(cx - 2, cy, 5);
            d.drawVLine(cx, cy - 2, 5);
            d.setDrawColor(1);
        } else if (idx == total + 1) {
            const int16_t cx = x + kPlusTileW / 2;
            const int16_t cy = kBottomBarY + kBottomBarH / 2;
            const bool hot2 = (hit == HitZone::TimelineDel);
            if (hot2) {
                d.setDrawColor(1);
                d.drawBox(x, kBottomBarY + 1, kPlusTileW, kBottomBarH - 1);
                d.setDrawColor(0);
            }
            d.drawLine(cx - 2, cy - 2, cx + 2, cy + 2);
            d.drawLine(cx + 2, cy - 2, cx - 2, cy + 2);
            d.setDrawColor(1);
        }
    }
}

void DrawScreen::renderToolStrip(oled& d) {
    // Opaque background so canvas pixels underneath are obscured.
    d.setDrawColor(0);
    d.drawBox(kToolStripX, kToolStripY, kToolStripW, kToolStripH);
    d.setDrawColor(1);
    d.drawVLine(kToolStripW, kToolStripY, kToolStripH);

    d.setFont(UIFonts::kText);

    // ToolMenu owns its own pagination via toolMenuCursor_ + toolMenuScrollMs_;
    // in editing mode the strip just shows the first window of slots. We
    // ensure the cursor's slot is visible by adjusting toolStripScroll_ when
    // the on-canvas hit zone reaches the bottom row.
    const uint8_t maxVisible =
        kToolSlotCount > kToolStripVisibleRows ? kToolStripVisibleRows : kToolSlotCount;
    uint8_t scroll = (mode_ == Mode::ToolMenu)
        ? (uint8_t)(toolMenuCursor_ >= maxVisible
                        ? toolMenuCursor_ + 1 - maxVisible : 0)
        : toolStripScroll_;
    if (scroll + maxVisible > kToolSlotCount) {
        scroll = (kToolSlotCount > maxVisible)
                     ? (uint8_t)(kToolSlotCount - maxVisible) : 0;
    }

    for (uint8_t row = 0; row < maxVisible; row++) {
        const uint8_t i = (uint8_t)(scroll + row);
        if (i >= kToolSlotCount) break;
        const ToolSlot& s = kToolSlots[i];
        const int16_t y = kToolStripY + (int16_t)row * kToolRowH;
        const bool hot = (mode_ == Mode::ToolMenu)
            ? (toolMenuCursor_ == i)
            : (hitZoneAtCursor(nullptr) == s.zone);
        bool selected = false;
        if (mode_ != Mode::ToolMenu) {
            if (s.zone == HitZone::ToolDraw && currentTool_ == Tool::Draw) selected = true;
            if (s.zone == HitZone::ToolHand && currentTool_ == Tool::Hand) selected = true;
            if (s.zone == HitZone::ToolEdit && currentTool_ == Tool::Edit) selected = true;
        }

        if (hot || selected) {
            d.setDrawColor(1);
            d.drawBox(kToolStripX, y, kToolStripW - 1, kToolRowH);
            d.setDrawColor(0);
            d.drawXBM(kToolStripX + 1, y, kToolIconW, kToolIconH, s.icon);
            d.drawStr(kToolStripX + kToolIconW + 2, y + 7, s.label);
            d.setDrawColor(1);
        } else {
            d.drawXBM(kToolStripX + 1, y, kToolIconW, kToolIconH, s.icon);
            d.drawStr(kToolStripX + kToolIconW + 2, y + 7, s.label);
        }
    }

    // Up/down arrow chips to indicate hidden tool slots (only in ToolMenu so
    // the on-canvas strip stays visually quiet during normal editing).
    if (mode_ == Mode::ToolMenu) {
        const int16_t arrowX = (int16_t)(kToolStripW - 4);
        if (scroll > 0) {
            d.fillTriangle(arrowX, kToolStripY + 1,
                           arrowX - 2, kToolStripY + 4,
                           arrowX + 2, kToolStripY + 4);
        }
        if (scroll + maxVisible < kToolSlotCount) {
            const int16_t ay = (int16_t)(kToolStripY + kToolStripH - 1);
            d.fillTriangle(arrowX, ay,
                           arrowX - 2, ay - 3,
                           arrowX + 2, ay - 3);
        }
    }
}

void DrawScreen::renderRightStrip(oled& d) {
    d.setDrawColor(0);
    d.drawBox(kRightStripX, kRightStripY, kRightStripW, kRightStripH);
    d.setDrawColor(1);
    d.drawVLine(kRightStripX, kRightStripY, kRightStripH);

    d.setFont(UIFonts::kText);

    if (mode_ == Mode::ContextMenu) {
        renderContextMenu(d);
    }
}

void DrawScreen::renderContextMenu(oled& d) {
    static const char* kLabels[kContextRowCount] = {
        "move", "edit", "size", "Z layer", "del"};
    const uint8_t cellH = kRightStripH / kContextRowCount;  // 9 px, with footer slop.
    d.setFont(UIFonts::kText);
    for (uint8_t i = 0; i < kContextRowCount; i++) {
        const int16_t y = kRightStripY + i * cellH;
        const bool selected = (ctxMenu_.cursor == i);
        if (selected) {
            d.setDrawColor(1);
            d.drawBox(kRightStripX + 1, y, kRightStripW - 1, cellH);
            d.setDrawColor(0);
        }
        char line[20];
        std::snprintf(line, sizeof(line), "%s", kLabels[i]);
        OLEDLayout::fitText(d, line, sizeof(line), kRightStripW - 8);
        d.drawStr(kRightStripX + 4, y + 7, line);
        d.setDrawColor(1);
    }
}

void DrawScreen::renderDurationPopup(oled& d) {
    const uint8_t boxW = 84, boxH = 28;
    const uint8_t boxX = (128 - boxW) / 2;
    const uint8_t boxY = (64 - boxH) / 2;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);

    d.setFont(UIFonts::kText);
    d.drawStr(boxX + 6, boxY + 10, "Frame ms");
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%u", (unsigned)popupDurationMs_);
    int w = d.getStrWidth(buf);
    d.drawStr(boxX + (boxW - w) / 2, boxY + 22, buf);

    renderFooterActions(d, nullptr, "\x1b\x1a", nullptr, "ok");
}

void DrawScreen::renderSettingsPopup(oled& d) {
    constexpr uint8_t rowCount = 4;
    constexpr uint8_t rowH = 11;
    constexpr uint8_t boxW = 96;
    constexpr uint8_t boxH = 6 + rowCount * rowH;
    const uint8_t boxX = (128 - boxW) / 2;
    const uint8_t boxY = (64 - boxH) / 2;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);
    d.setFont(UIFonts::kText);

    static const char* kSpeedLabels[] = {"slow", "med", "fast"};

    auto drawRow = [&](uint8_t idx, const char* label, const char* value) {
        const uint8_t ry = boxY + 3 + idx * rowH;
        if (settingsSel_ == idx) {
            d.setDrawColor(1);
            d.drawBox(boxX + 2, ry, boxW - 4, rowH);
            d.setDrawColor(0);
        }
        d.drawStr(boxX + 5, ry + 8, label);
        d.drawStr(boxX + boxW - 5 - d.getStrWidth(value), ry + 8, value);
        d.setDrawColor(1);
    };

    drawRow(0, "Cursor", kSpeedLabels[cursorSpeed_ < 3 ? cursorSpeed_ : 1]);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%u", (unsigned)popupDurationMs_);
    drawRow(1, "Frame ms", buf);
    drawRow(2, "Tilt",
            imuParallaxEnabled_ ? "on" : "off");
    drawRow(3, "Help", ">");

    renderFooterActions(d, nullptr, nullptr, nullptr, "ok");
}

void DrawScreen::renderTutorial(oled& d) {
    if (tutorialPage_ >= kTutorialPageCount) tutorialPage_ = 0;
    const TutorialPage& page = kTutorialPages[tutorialPage_];

    d.setDrawColor(0);
    d.drawBox(0, 0, 128, 64);
    d.setDrawColor(1);
    d.setFont(UIFonts::kText);

    d.drawStr(0, 6, page.title);
    char count[8];
    std::snprintf(count, sizeof(count), "%u/%u",
                  (unsigned)(tutorialPage_ + 1),
                  (unsigned)kTutorialPageCount);
    d.drawStr(128 - d.getStrWidth(count), 6, count);
    OLEDLayout::drawHeaderRule(d);

    for (uint8_t i = 0; i < 4; i++) {
        const TutorialLine& line = page.lines[i];
        if (!line.text || !line.text[0]) break;
        const int16_t baseline = (int16_t)(20 + i * 9);
        drawHelpTextLine(d, 3, baseline, line.text, line.glyphs, 122);
    }

    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);
    OLEDLayout::drawFooterActions(
        d, nullptr, tutorialPage_ > 0 ? "prev" : nullptr, "skip",
        tutorialPage_ + 1 >= kTutorialPageCount ? "start" : "next");
}

void DrawScreen::renderHelpScroll(oled& d) {
    constexpr uint8_t kContentY = kHelpHeaderH + 1;

    d.setDrawColor(0);
    d.drawBox(0, 0, 128, 64);
    d.setDrawColor(1);
    d.drawHLine(0, kHelpHeaderH, 128);
    d.setFont(UIFonts::kText);
    d.drawStr(4, kHelpHeaderH - 1, "HELP");

    if (helpScroll_ > 0) {
        d.fillTriangle(120, kContentY + 1, 116, kContentY + 4, 124, kContentY + 4);
    }

    for (uint8_t i = 0; i < kHelpVisible; i++) {
        const uint8_t idx = helpScroll_ + i;
        if (idx >= kHelpLineCount) break;
        const int16_t ly = (int16_t)(kContentY + i * kHelpLineH + kHelpLineH - 1);
        if (kHelpLines[idx].header) {
            d.setDrawColor(1);
            d.drawBox(0, (int16_t)(kContentY + i * kHelpLineH), 128, kHelpLineH);
            d.setDrawColor(0);
            d.drawStr(4, ly, kHelpLines[idx].text);
            d.setDrawColor(1);
        } else {
            drawHelpTextLine(d, 2, ly, kHelpLines[idx].text,
                             kHelpLines[idx].glyphs, 124);
        }
    }

    if (helpScroll_ + kHelpVisible < kHelpLineCount) {
        const int16_t ay = (int16_t)(64 - kHelpFooterH - 3);
        d.fillTriangle(120, ay, 116, ay - 3, 124, ay - 3);
    }

    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", nullptr);
}

void DrawScreen::renderZAdjustPopup(oled& d) {
    const uint8_t boxW = 76, boxH = 30;
    const uint8_t boxX = (128 - boxW) / 2;
    const uint8_t boxY = (64 - boxH) / 2;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);
    d.setFont(UIFonts::kText);
    d.drawStr(boxX + 6, boxY + 10, "Layer Z");
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%+d", (int)zAdjust_.pendingZ);
    const int w = d.getStrWidth(buf);
    d.drawStr(boxX + (boxW - w) / 2, boxY + 22, buf);
    renderFooterActions(d, nullptr, "\x1b\x1a", nullptr, "ok");
}

void DrawScreen::renderTextAppearancePopup(oled& d) {
    // Render the placed text using the *pending* settings so the user sees
    // the live result on the canvas as they cycle through values. We swap
    // the def's settings, draw, then put them back so commit/cancel
    // semantics still go through the existing finish() path.
    draw::ObjectDef* def = findObjectDef(textAppear_.targetObjId);
    const draw::ObjectPlacement* placement = nullptr;
    int16_t pw = 0, ph = 0;
    int16_t px = 0, py = 0;
    if (def && def->type == draw::ObjectType::Text &&
        currentFrame_ < doc_.frames.size()) {
        placement = findPlacement(currentFrame_, textAppear_.targetObjId);
        if (placement) {
            const uint8_t prevFamily = def->textFontFamily;
            const uint8_t prevSlot = def->textFontSlot;
            const draw::TextStackMode prevStack = def->textStackMode;
            def->textFontFamily = textAppear_.fontFamily;
            def->textFontSlot = textAppear_.fontSlot;
            def->textStackMode = textAppear_.stack;

            int16_t ox, oy;
            canvasOriginScreen(&ox, &oy);
            int16_t dx = 0, dy = 0;
            parallaxOffset(placement->z, &dx, &dy);
            const draw::ObjectPlacement parallaxP = [&]() {
                draw::ObjectPlacement out = *placement;
                out.x = (int16_t)(out.x + dx);
                out.y = (int16_t)(out.y + dy);
                return out;
            }();
            d.setBitmapTransparent(true);
            renderObjectInstance(d, ox, oy, parallaxP, *def, millis());
            d.setBitmapTransparent(false);
            d.setFontMode(0);
            d.setDrawColor(1);

            objectDimensions(*placement, *def, &pw, &ph);
            px = (int16_t)(ox + parallaxP.x);
            py = (int16_t)(oy + parallaxP.y);

            def->textFontFamily = prevFamily;
            def->textFontSlot = prevSlot;
            def->textStackMode = prevStack;
        }
    }

    constexpr uint8_t kBoxW = 78;
    constexpr uint8_t kBoxH = 30;
    // Pick a popup position that doesn't cover the placement and stays
    // within the visible 0..127 / 0..(footer-edge) area. Try four positions
    // around the placement bbox; fall back to the corner farthest from the
    // bbox if none fit.
    constexpr int16_t kFooterTop = 53;  // leave the footer row clear
    auto fits = [](int16_t x, int16_t y) {
        return x >= 0 && y >= 0 &&
               x + (int16_t)kBoxW <= 128 &&
               y + (int16_t)kBoxH <= kFooterTop;
    };
    auto overlaps = [&](int16_t x, int16_t y) {
        if (!placement || pw <= 0 || ph <= 0) return false;
        const int16_t bx2 = x + (int16_t)kBoxW;
        const int16_t by2 = y + (int16_t)kBoxH;
        const int16_t tx2 = px + pw;
        const int16_t ty2 = py + ph;
        return !(bx2 <= px || tx2 <= x || by2 <= py || ty2 <= y);
    };
    int16_t boxX = (128 - kBoxW) / 2;
    int16_t boxY = 1;
    if (placement) {
        const struct { int16_t x; int16_t y; } cand[] = {
            { (int16_t)((128 - kBoxW) / 2), (int16_t)(py + ph + 2) },           // below
            { (int16_t)((128 - kBoxW) / 2), (int16_t)(py - kBoxH - 2) },        // above
            { (int16_t)(px + pw + 2),      (int16_t)((kFooterTop - kBoxH) / 2) },// right
            { (int16_t)(px - kBoxW - 2),   (int16_t)((kFooterTop - kBoxH) / 2) },// left
        };
        bool chose = false;
        for (const auto& c : cand) {
            if (fits(c.x, c.y) && !overlaps(c.x, c.y)) {
                boxX = c.x; boxY = c.y; chose = true; break;
            }
        }
        if (!chose) {
            // Stick to whichever screen corner is farthest from the bbox center.
            const int16_t bcX = px + pw / 2;
            const int16_t bcY = py + ph / 2;
            boxX = (bcX < 64) ? (int16_t)(128 - kBoxW - 1) : 1;
            boxY = (bcY < (kFooterTop / 2)) ? (int16_t)(kFooterTop - kBoxH - 1) : 1;
        }
        if (boxX < 1) boxX = 1;
        if (boxX + kBoxW > 127) boxX = (int16_t)(127 - kBoxW);
        if (boxY < 1) boxY = 1;
        if (boxY + kBoxH > kFooterTop) boxY = (int16_t)(kFooterTop - kBoxH);
    }

    d.setDrawColor(0);
    d.drawBox(boxX, boxY, kBoxW, kBoxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, kBoxW, kBoxH, 2);
    d.setFont(UIFonts::kText);

    auto drawMarkedRow = [&](int16_t y, bool sel, const char* text) {
        if (sel) {
            d.setDrawColor(1);
            d.drawBox(boxX + 4, y - 6, kBoxW - 8, 9);
            d.setDrawColor(0);
            d.drawStr(boxX + 6, y, text);
            d.setDrawColor(1);
        } else {
            d.drawStr(boxX + 6, y, text);
        }
    };

    char line[38];
    const uint8_t fam = textAppear_.fontFamily;
    const char* famName =
        (fam < kFontFamilyCount) ? kFontFamilyNames[fam] : "Default";
    std::snprintf(line, sizeof(line), "Font %s", famName);
    drawMarkedRow((int16_t)(boxY + 9), textAppear_.fieldSel == 0, line);

    std::snprintf(line, sizeof(line), "Size %s", kSizeLabels[textAppear_.fontSlot]);
    drawMarkedRow((int16_t)(boxY + 18), textAppear_.fieldSel == 1, line);

    const char* stack =
        textAppear_.stack == draw::TextStackMode::XOR ? "XOR" :
        textAppear_.stack == draw::TextStackMode::RoundedBox ? "BOX" : "OR";
    std::snprintf(line, sizeof(line), "stack %s", stack);
    drawMarkedRow((int16_t)(boxY + 27), textAppear_.fieldSel == 2, line);

    d.setFont(UIFonts::kText);
    renderFooterActions(d, nullptr, nullptr, nullptr, "ok");
}

void DrawScreen::renderSaveExitPrompt(oled& d) {
    const uint8_t boxW = 106, boxH = 43;
    const uint8_t boxX = (128 - boxW) / 2;
    const uint8_t boxY = (64 - boxH) / 2;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);
    d.setFont(UIFonts::kText);
    d.drawStr(boxX + 8, boxY + 12,
              saveFailed_ ? "Save failed" : "Save changes?");
    static const char* kRows[3] = {"save+exit", "discard", "stay"};
    for (uint8_t i = 0; i < 3; i++) {
        const int16_t ry = boxY + 21 + (int)i * 8;
        if (savePromptSel_ == i) {
            d.setDrawColor(1);
            d.drawBox(boxX + 6, ry - 6, boxW - 12, 9);
            d.setDrawColor(0);
            d.drawStr(boxX + 10, ry, kRows[i]);
            d.setDrawColor(1);
        } else {
            d.drawStr(boxX + 10, ry, kRows[i]);
        }
    }
    renderFooterActions(d, nullptr, nullptr, nullptr, "ok");
}

void DrawScreen::renderFooterActions(oled& d, const char* xLabel,
                                     const char* yLabel, const char* bLabel,
                                     const char* aLabel) {
    d.setFont(UIFonts::kText);
    OLEDLayout::clearFooter(d);
    OLEDLayout::drawFooterActions(d, xLabel, yLabel, bLabel, aLabel);
}

const char* DrawScreen::helpForZone(HitZone z, uint8_t /*extra*/) const {
    switch (z) {
        case HitZone::ToolDraw:
            return "Draw  -  CONFIRM paints, CANCEL erases  -  bottom strip = brush size";
        case HitZone::ToolStickerAdd:
            return "Add zigmoji  -  pick from catalog or your saved drawings";
        case HitZone::ToolTextAdd:
            return "Add text object  -  type, place, then size/font popup";
        case HitZone::ToolFieldAdd:
            return "Insert a Badge Info field as text (name, title, company, ...)";
        case HitZone::ToolHand:
            return "Hand  -  CONFIRM grabs the topmost object under cursor";
        case HitZone::ToolEdit:
            return "Edit  -  paint into the layer the cursor is hovering";
        case HitZone::ToolSettings:
            return "Settings  -  cursor speed / frame ms / tilt / help";
        case HitZone::ToolDuration:
            return "Frame duration in ms";
        case HitZone::ToolDelFrame:
            return "Delete the current frame";
        case HitZone::ToolUndo:
            return "Undo last stroke or placement change";
        case HitZone::ToolRedo:
            return "Redo last undone change";
        case HitZone::ToolPlay:
            return "Play / stop preview animation";
        case HitZone::ToolSave:
            return "Save changes to flash (no exit)";
        case HitZone::ToolExit:
            return "Exit  -  prompts to save first if dirty";
        case HitZone::TimelineFrame:
            return "Select frame  -  UP on a placed object opens its menu";
        case HitZone::TimelinePlus:
            return "Add a new frame (duplicates the current one)";
        case HitZone::TimelineDel:
            return "Delete the current frame";
        case HitZone::BrushSize:
            return "Brush size  -  CONFIRM picks this size for future strokes";
        case HitZone::Canvas:
            return "Canvas  -  hold CONFIRM to paint, CANCEL to erase, UP for object menu";
        default:
            return "";
    }
}

void DrawScreen::renderHelpMarquee(oled& d) {
    if (mode_ != Mode::Editing && mode_ != Mode::ToolMenu) return;
    if (painting_) return;
    if (doc_.w == draw::kCanvasZigW) return;
    uint8_t extra = 0;
    HitZone z = hitZoneAtCursor(&extra);
    // Only show the marquee for chrome icons; canvas / timeline frames /
    // brush size already self-describe and the marquee would overwrite the
    // timeline thumbs the user is trying to read.
    bool showMarquee = false;
    switch (z) {
        case HitZone::ToolDraw:
        case HitZone::ToolStickerAdd:
        case HitZone::ToolTextAdd:
        case HitZone::ToolFieldAdd:
        case HitZone::ToolHand:
        case HitZone::ToolEdit:
        case HitZone::ToolSettings:
        case HitZone::ToolDuration:
        case HitZone::ToolDelFrame:
        case HitZone::ToolUndo:
        case HitZone::ToolRedo:
        case HitZone::ToolPlay:
        case HitZone::ToolSave:
        case HitZone::ToolExit:
            showMarquee = true;
            break;
        default:
            break;
    }
    // ToolMenu: always show help for the selected tool slot.
    if (mode_ == Mode::ToolMenu) {
        if (toolMenuCursor_ < kToolSlotCount) {
            z = kToolSlots[toolMenuCursor_].zone;
            showMarquee = true;
        }
    }
    if (!showMarquee) return;
    const char* msg = helpForZone(z, extra);
    if (!msg || !msg[0]) return;

    // Replace the bottom 8 px band so the text reads cleanly. The timeline
    // can still be made visible by moving the cursor off the chrome strip.
    d.setDrawColor(0);
    d.drawBox(0, 56, 128, 8);
    d.setDrawColor(1);
    d.drawHLine(0, 56, 128);

    // Right-edge button hint chip ("ok" for the active slot, plus "back" to
    // close the tool menu) so the user always sees what the buttons do.
    const char* aLabel = (z == HitZone::ToolPlay) ? "play" :
                         (z == HitZone::ToolSave) ? "save" :
                         (z == HitZone::ToolExit) ? "exit" :
                         (z == HitZone::ToolUndo) ? "undo" :
                         (z == HitZone::ToolRedo) ? "redo" :
                         (z == HitZone::ToolDelFrame) ? "del" :
                         "select";
    const char* bLabel = (mode_ == Mode::ToolMenu) ? "back" : nullptr;
    d.setFont(UIFonts::kText);
    const int chipsW = OLEDLayout::drawFooterActions(d, nullptr, nullptr,
                                                     bLabel, aLabel);
    const int16_t bandRight = (chipsW > 0) ? (int16_t)(128 - chipsW - 4) : 127;

    constexpr int16_t kBandLeft = 1;
    const int16_t kBandW = (int16_t)(bandRight - kBandLeft);
    if (kBandW <= 4) return;
    constexpr int16_t kBaseY = 63;
    const int textW = d.getStrWidth(msg);
    if (textW <= kBandW - 2) {
        d.drawStr(kBandLeft + (kBandW - textW) / 2, kBaseY, msg);
        return;
    }
    constexpr int kGap = 16;
    const int loop = textW + kGap;
    const int offset = (loop > 0) ? (int)((millis() / 35) % loop) : 0;
    d.setClipWindow(kBandLeft, 57, bandRight, kBaseY + 1);
    d.drawStr(kBandLeft - offset, kBaseY, msg);
    d.drawStr(kBandLeft - offset + loop, kBaseY, msg);
    d.setMaxClipWindow();
}

void DrawScreen::renderFieldPickerPopup(oled& d) {
    constexpr uint8_t boxW = 110;
    constexpr uint8_t rowH = 9;
    constexpr uint8_t boxH = (uint8_t)(6 + kFieldPickerVisibleRows * rowH);
    constexpr uint8_t boxX = (128 - boxW) / 2;
    constexpr uint8_t boxY = 5;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);
    d.setFont(UIFonts::kText);
    d.drawStr(boxX + 4, boxY + 7, "Insert field");

    BadgeInfo::Fields fields{};
    BadgeInfo::getCurrent(fields);
    auto fieldText = [&](uint8_t idx) -> const char* {
        const FieldDef& fd = kFieldDefs[idx];
        const char* base = reinterpret_cast<const char*>(&fields) + fd.offset;
        return base[0] ? base : "(empty)";
    };

    // Adjust scroll so the cursor is visible.
    if (fieldPickerCursor_ < fieldPickerScroll_) fieldPickerScroll_ = fieldPickerCursor_;
    if (fieldPickerCursor_ >= fieldPickerScroll_ + kFieldPickerVisibleRows) {
        fieldPickerScroll_ = (uint8_t)(fieldPickerCursor_ + 1 - kFieldPickerVisibleRows);
    }

    for (uint8_t r = 0; r < kFieldPickerVisibleRows; r++) {
        const uint8_t i = (uint8_t)(fieldPickerScroll_ + r);
        if (i >= kFieldDefCount) break;
        const int16_t y = (int16_t)(boxY + 9 + r * rowH);
        const bool selected = (i == fieldPickerCursor_);
        if (selected) {
            d.setDrawColor(1);
            d.drawBox(boxX + 2, y, boxW - 4, rowH);
            d.setDrawColor(0);
        }
        char line[40];
        std::snprintf(line, sizeof(line), "%s: %s",
                      kFieldDefs[i].label, fieldText(i));
        OLEDLayout::fitText(d, line, sizeof(line), boxW - 8);
        d.drawStr(boxX + 4, y + rowH - 2, line);
        d.setDrawColor(1);
    }

    if (fieldPickerScroll_ > 0) {
        d.fillTriangle(boxX + boxW - 4, boxY + 10,
                       boxX + boxW - 7, boxY + 13,
                       boxX + boxW - 1, boxY + 13);
    }
    if (fieldPickerScroll_ + kFieldPickerVisibleRows < kFieldDefCount) {
        const int16_t ay = (int16_t)(boxY + boxH - 3);
        d.fillTriangle(boxX + boxW - 4, ay,
                       boxX + boxW - 7, ay - 3,
                       boxX + boxW - 1, ay - 3);
    }

    renderFooterActions(d, nullptr, nullptr, "back", "ok");
}

void DrawScreen::renderExitNametagPrompt(oled& d) {
    constexpr uint8_t boxW = 100;
    constexpr uint8_t boxH = 36;
    constexpr uint8_t boxX = (128 - boxW) / 2;
    constexpr uint8_t boxY = (64 - boxH) / 2;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);
    d.setFont(UIFonts::kText);
    d.drawStr(boxX + 8, boxY + 11, "Set as nametag?");

    static const char* kRows[2] = {"Yes", "No"};
    for (uint8_t i = 0; i < 2; i++) {
        const int16_t ry = (int16_t)(boxY + 18 + i * 9);
        if (exitNametagSel_ == i) {
            d.setDrawColor(1);
            d.drawBox(boxX + 6, ry, boxW - 12, 9);
            d.setDrawColor(0);
        }
        d.drawStr(boxX + 12, ry + 7, kRows[i]);
        d.setDrawColor(1);
    }
    renderFooterActions(d, nullptr, nullptr, nullptr, "ok");
}

// ── Hit zones ──────────────────────────────────────────────────────────────

DrawScreen::HitZone DrawScreen::hitZoneAtCursor(uint8_t* outExtra) const {
    if (outExtra) *outExtra = 0;
    const int16_t cx = (int16_t)cursorXf_;
    const int16_t cy = (int16_t)cursorYf_;

    // Top bar zone — top-right icon row (play, save, exit).
    if (topShown_ && cy >= kTopRightIconY && cy < kTopRightIconY + 8) {
        for (uint8_t i = 0; i < kTopRightSlotCount; i++) {
            const TopRightSlot& s = kTopRightSlots[i];
            if (cx >= s.x && cx < s.x + 8) return s.zone;
        }
    }

    // Left tool strip — single column with text labels.
    if (leftShown_ &&
        cx >= kToolStripX && cx < kToolStripX + kToolStripW &&
        cy >= kToolStripY && cy < kToolStripY + kToolStripH) {
        uint8_t row = (uint8_t)((cy - kToolStripY) / kToolRowH);
        const uint8_t maxVisible =
            kToolSlotCount > kToolStripVisibleRows ? kToolStripVisibleRows : kToolSlotCount;
        if (row < maxVisible) {
            uint8_t idx = (uint8_t)(toolStripScroll_ + row);
            if (idx < kToolSlotCount) return kToolSlots[idx].zone;
        }
    }

    // Bottom timeline + brush size area.
    if (bottomShown_ && cy >= kBottomBarY && cy < kBottomBarY + kBottomBarH) {
        constexpr int16_t kBrushAreaW = 48;
        constexpr uint8_t kBrushCount = 8;
        constexpr int16_t kBrushCellW = kBrushAreaW / kBrushCount;
        const int16_t brushAreaX = 128 - kBrushAreaW;
        if (cx >= brushAreaX) {
            uint8_t col = (uint8_t)((cx - brushAreaX) / kBrushCellW);
            if (col < kBrushCount) {
                if (outExtra) *outExtra = col + 1;
                return HitZone::BrushSize;
            }
        }
        const int16_t timelineX0 = 0;
        if (cx < timelineX0) return HitZone::None;
        uint8_t total = (uint8_t)doc_.frames.size();
        uint8_t maxSlots = (uint8_t)((brushAreaX - 2 - timelineX0) / kThumbCellW);
        uint8_t firstVisible = 0;
        if (currentFrame_ + 2 >= maxSlots) {
            firstVisible = (uint8_t)(currentFrame_ + 2 - maxSlots);
            if (firstVisible + maxSlots > total + 2) {
                firstVisible = total + 2 > maxSlots ? (total + 2 - maxSlots) : 0;
            }
        }
        uint8_t slot = (uint8_t)((cx - timelineX0) / kThumbCellW);
        uint8_t idx = firstVisible + slot;
        if (idx < total) {
            if (outExtra) *outExtra = idx;
            return HitZone::TimelineFrame;
        }
        if (idx == total) return HitZone::TimelinePlus;
        if (idx == total + 1) return HitZone::TimelineDel;
    }

    if (cursorInCanvas()) return HitZone::Canvas;
    return HitZone::None;
}

void DrawScreen::onChromeClick(HitZone z, uint8_t extra, GUIManager& gui) {
    switch (z) {
        case HitZone::ToolDraw:
            activeObjId_[0] = '\0';
            currentTool_ = Tool::Draw;
            break;
        case HitZone::ToolHand:       currentTool_ = Tool::Hand; break;
        case HitZone::ToolEdit:       currentTool_ = Tool::Edit; break;
        case HitZone::ToolStickerAdd:
            sStickerPicker.configure(doc_.animId, &onStickerCb, this);
            gui.pushScreen(kScreenDrawStickerPicker);
            break;
        case HitZone::ToolTextAdd:
            textOp_ = TextOp::Add;
            textEditTargetId_[0] = '\0';
            textEditBuf_[0] = '\0';
            sTextInput.configure("Text", textEditBuf_, sizeof(textEditBuf_),
                                 onTextInputCb, this);
            gui.pushScreen(kScreenTextInput);
            break;
        case HitZone::ToolFieldAdd:
            fieldPickerCursor_ = 0;
            fieldPickerScroll_ = 0;
            fieldPickerJoyMs_ = 0;
            mode_ = Mode::FieldPicker;
            break;
        case HitZone::ToolSettings:
            popupDurationMs_ = (currentFrame_ < doc_.frames.size())
                               ? doc_.frames[currentFrame_].durationMs
                               : draw::kDefaultDurationMs;
            settingsSel_ = 0;
            settingsJoyLastMs_ = 0;
            mode_ = Mode::SettingsPopup;
            break;
        case HitZone::ToolDuration:
            popupDurationMs_ = (currentFrame_ < doc_.frames.size())
                               ? doc_.frames[currentFrame_].durationMs
                               : draw::kDefaultDurationMs;
            durationJoyLastMs_ = 0;
            mode_ = Mode::DurationPopup;
            break;
        case HitZone::ToolDelFrame:
            deleteCurrentFrame();
            break;
        case HitZone::ToolPlay:
            if (mode_ == Mode::Playing) {
                mode_ = Mode::Editing;
            } else {
                mode_ = Mode::Playing;
                playFrameStartMs_ = millis();
                // Hide every strip so the animation gets the full screen.
                topShown_ = bottomShown_ = leftShown_ = rightShown_ = false;
            }
            break;
        case HitZone::ToolUndo:
            restoreUndo();
            break;
        case HitZone::ToolRedo:
            restoreRedo();
            break;
        case HitZone::ToolSave:
            if (draw::save(doc_)) {
                saveFailed_ = false;
            } else {
                saveFailed_ = true;
                savePromptSel_ = 0;
                mode_ = Mode::SaveExitPrompt;
            }
            break;
        case HitZone::ToolExit:
            if (doc_.dirty) {
                saveFailed_ = false;
                mode_ = Mode::SaveExitPrompt;
            } else {
                gui.popScreen();
            }
            break;
        case HitZone::TimelineFrame:
            moveToFrame(extra);
            break;
        case HitZone::TimelinePlus:
            // Default: new frame copies current frame's pixels and placements.
            addFrame(true);
            break;
        case HitZone::TimelineDel:
            deleteCurrentFrame();
            break;
        case HitZone::BrushSize:
            if (extra >= 1 && extra <= 8) brushSize_ = extra;
            break;
        default:
            break;
    }
}

void DrawScreen::activateContextAction(GUIManager& gui) {
    switch (kContextRows[ctxMenu_.cursor]) {
        case ContextAction::Move:   doCtxMove(gui); break;
        case ContextAction::Edit:   doCtxEdit(gui); break;
        case ContextAction::Size:   doCtxSize(gui); break;
        case ContextAction::LayerZ: doCtxZ(gui); break;
        case ContextAction::Delete: doCtxDelete(gui); break;
    }
    rightShown_ = false;
    rightSuppressUntilEdge_ = true;
}

void DrawScreen::doCtxMove(GUIManager& /*gui*/) {
    if (currentFrame_ >= doc_.frames.size()) return;
    auto& placements = doc_.frames[currentFrame_].placements;
    if (ctxMenu_.targetPlacementIdx >= placements.size()) return;
    snapshotForUndo(currentFrame_);
    const auto p = placements[ctxMenu_.targetPlacementIdx];
    ghost_ = GhostState{};
    ghost_.active = true;
    ghost_.moveExisting = true;
    ghost_.scale = p.scale;
    ghost_.z = p.z;
    ghost_.phaseMs = p.phaseMs;
    std::strncpy(ghost_.objId, p.objId, sizeof(ghost_.objId) - 1);
    // Capture where the cursor sits within the object so it doesn't
    // jump to (0,0) of the placement when Ghosted mode starts.
    int16_t lx, ly;
    canvasLocalCursor(&lx, &ly);
    ghost_.grabOffsetX = (int16_t)(lx - p.x);
    ghost_.grabOffsetY = (int16_t)(ly - p.y);
    placements.erase(placements.begin() + ctxMenu_.targetPlacementIdx);
    mode_ = Mode::Ghosted;
}

void DrawScreen::doCtxDelete(GUIManager& /*gui*/) {
    if (currentFrame_ >= doc_.frames.size()) return;
    auto& placements = doc_.frames[currentFrame_].placements;
    if (ctxMenu_.targetPlacementIdx >= placements.size()) return;
    snapshotForUndo(currentFrame_);
    placements.erase(placements.begin() + ctxMenu_.targetPlacementIdx);
    doc_.dirty = true;
    currentTool_ = Tool::Hand;
    mode_ = Mode::Editing;
}

void DrawScreen::doCtxEdit(GUIManager& gui) {
    draw::ObjectDef* def = findObjectDef(ctxMenu_.targetObjId);
    if (!def) return;
    if (def->type == draw::ObjectType::DrawnAnim) {
        std::strncpy(activeObjId_, def->id, sizeof(activeObjId_) - 1);
        currentTool_ = Tool::Edit;  // internal draw-into-layer mode; not shown on strip
        mode_ = Mode::Editing;
    } else if (def->type == draw::ObjectType::Text) {
        textOp_ = TextOp::Edit;
        std::strncpy(textEditTargetId_, def->id, sizeof(textEditTargetId_) - 1);
        std::strncpy(textEditBuf_, def->textContent, sizeof(textEditBuf_) - 1);
        textEditBuf_[sizeof(textEditBuf_) - 1] = '\0';
        sTextInput.configure("Text", textEditBuf_, sizeof(textEditBuf_), onTextInputCb, this);
        mode_ = Mode::Editing;
        gui.pushScreen(kScreenTextInput);
    } else {
        doCtxReplaceZigmoji(gui);
    }
}

void DrawScreen::doCtxReplaceZigmoji(GUIManager& gui) {
    std::strncpy(replaceTargetObjId_, ctxMenu_.targetObjId,
                 sizeof(replaceTargetObjId_) - 1);
    mode_ = Mode::Editing;
    sStickerPicker.configure(doc_.animId, &onReplaceStickerCb, this);
    gui.pushScreen(kScreenDrawStickerPicker);
}

void DrawScreen::doCtxSize(GUIManager& gui) {
    draw::ObjectDef* def = findObjectDef(ctxMenu_.targetObjId);
    if (!def) return;
    if (def->type == draw::ObjectType::Text) {
        doCtxTextAppearance(gui);
        return;
    }
    if (def->type == draw::ObjectType::DrawnAnim) {
        mode_ = Mode::Editing;
        return;
    }
    StickerPickerScreen::Result r{};
    if (def->type == draw::ObjectType::Catalog) {
        r.kind = StickerPickerScreen::PickKind::Catalog;
        r.catalogIdx = def->catalogIdx;
    } else {
        r.kind = StickerPickerScreen::PickKind::SavedAnim;
        r.catalogIdx = -1;
        std::strncpy(r.savedAnimId, def->savedAnimId, sizeof(r.savedAnimId) - 1);
    }
    std::strncpy(scaleTargetObjId_, ctxMenu_.targetObjId, sizeof(scaleTargetObjId_) - 1);
    mode_ = Mode::Editing;
    sScalePicker.configure(r, &onCtxScaleCb, this);
    gui.pushScreen(kScreenDrawScalePicker);
}

void DrawScreen::doCtxZ(GUIManager& /*gui*/) {
    if (currentFrame_ >= doc_.frames.size()) return;
    const auto* p = findPlacement(currentFrame_, ctxMenu_.targetObjId);
    if (!p) return;
    snapshotForUndo(currentFrame_);
    std::strncpy(zAdjust_.targetObjId, ctxMenu_.targetObjId,
                 sizeof(zAdjust_.targetObjId) - 1);
    zAdjust_.targetPlacementIdx = ctxMenu_.targetPlacementIdx;
    zAdjust_.originalZ = p->z;
    zAdjust_.pendingZ = p->z;
    zAdjust_.lastJoyMs = 0;
    mode_ = Mode::ZAdjust;
}

void DrawScreen::doCtxTextAppearance(GUIManager& /*gui*/) {
    draw::ObjectDef* def = findObjectDef(ctxMenu_.targetObjId);
    if (!def || def->type != draw::ObjectType::Text) return;
    std::strncpy(textAppear_.targetObjId, def->id, sizeof(textAppear_.targetObjId) - 1);
    textAppear_.fontFamily = def->textFontFamily;
    textAppear_.fontSlot = def->textFontSlot;
    textAppear_.stack = def->textStackMode;
    textAppear_.fieldSel = 0;
    textAppear_.lastJoyMs = 0;
    mode_ = Mode::TextAppearance;
}

// ── Input ──────────────────────────────────────────────────────────────────

void DrawScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                             int16_t /*cy*/, GUIManager& gui) {
    const Inputs::ButtonEdges& e = inputs.edges();
    const Inputs::ButtonStates& b = inputs.buttons();
    const uint32_t now = millis();

    // ── Hold-Cancel-4s = Save+Exit ────────────────────────────────────────
    // Only armed in plain Editing mode (popups eat Cancel as a commit edge,
    // and Draw tool's Cancel is the eraser stroke). Always reset when not
    // eligible so re-entering Editing with the button held doesn't fire pop.
    {
        const bool cancelHoldEligible =
            mode_ == Mode::Editing &&
            currentTool_ != Tool::Draw && !painting_;
        if (cancelHoldEligible && b.cancel) {
            if (cancelHoldStartMs_ == 0) cancelHoldStartMs_ = now;
            if (now - cancelHoldStartMs_ >= kCancelHoldMs) {
                cancelHoldStartMs_ = 0;
                if (draw::save(doc_)) {
                    saveFailed_ = false;
                    exitNametagSel_ = 1;
                    exitNametagJoyMs_ = 0;
                    mode_ = Mode::ExitNametagPrompt;
                    gui.requestRender();
                    return;
                }
                saveFailed_ = true;
                savePromptSel_ = 0;
                mode_ = Mode::SaveExitPrompt;
                gui.requestRender();
                return;
            }
            gui.requestRender();
        } else {
            cancelHoldStartMs_ = 0;
        }
    }

    // ── Modes that absorb input ────────────────────────────────────────────
    if (mode_ == Mode::Tutorial) {
        auto finishTutorial = [&]() {
            tutorialPage_ = 0;
            mode_ = Mode::Editing;
            gui.requestRender();
        };
        if (e.cancelPressed) {
            finishTutorial();
            return;
        }
        if (e.confirmPressed) {
            if (tutorialPage_ + 1 >= kTutorialPageCount) {
                finishTutorial();
            } else {
                tutorialPage_++;
                gui.requestRender();
            }
            return;
        }
        if (e.yPressed && tutorialPage_ > 0) {
            tutorialPage_--;
            gui.requestRender();
            return;
        }
        return;
    }

    if (mode_ == Mode::Playing) {
        if (e.confirmPressed || e.cancelPressed ||
            e.upPressed || e.downPressed ||
            e.leftPressed || e.rightPressed ||
            e.aPressed || e.bPressed || e.xPressed || e.yPressed) {
            mode_ = Mode::Editing;
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::DurationPopup) {
        // Duration popup: LEFT/RIGHT or joystick-X cycles value,
        // DOWN/confirm/cancel all commit and close.
        auto commitAndLeave = [&]() {
            if (currentFrame_ < doc_.frames.size()) {
                doc_.frames[currentFrame_].durationMs = popupDurationMs_;
                doc_.dirty = true;
            }
            mode_ = Mode::Editing;
            gui.requestRender();
        };
        if (e.confirmPressed || e.cancelPressed || e.downPressed) {
            commitAndLeave();
            return;
        }
        static bool durJoyDefl = false;
        const int8_t vs = valueOneShotStep(inputs, e, &durJoyDefl);
        if (vs != 0) {
            constexpr uint16_t kStep = 50;
            if (vs > 0) {
                popupDurationMs_ = (popupDurationMs_ + kStep <= draw::kMaxDurationMs)
                                       ? popupDurationMs_ + kStep
                                       : draw::kMaxDurationMs;
            } else {
                popupDurationMs_ =
                    (popupDurationMs_ > kStep + draw::kMinDurationMs)
                        ? (uint16_t)(popupDurationMs_ - kStep) : draw::kMinDurationMs;
            }
            gui.requestRender();
        }
        // Also accept joystick-Y as value cycle (legacy compat)
        static bool durJoyNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &durJoyNavDefl);
        if (nav != 0) {
            constexpr uint16_t kStep = 50;
            if (nav < 0) {  // up = increase
                popupDurationMs_ = (popupDurationMs_ + kStep <= draw::kMaxDurationMs)
                                       ? popupDurationMs_ + kStep
                                       : draw::kMaxDurationMs;
            } else {
                popupDurationMs_ =
                    (popupDurationMs_ > kStep + draw::kMinDurationMs)
                        ? (uint16_t)(popupDurationMs_ - kStep) : draw::kMinDurationMs;
            }
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::SettingsPopup) {
        constexpr uint8_t rowCount = 4;
        auto commitAndLeave = [&]() {
            if (currentFrame_ < doc_.frames.size()) {
                doc_.frames[currentFrame_].durationMs = popupDurationMs_;
                doc_.dirty = true;
            }
            mode_ = Mode::Editing;
            gui.requestRender();
        };
        // DOWN/confirm/cancel: commit for value rows, or open help for row 3
        if (e.confirmPressed || e.cancelPressed || e.downPressed) {
            if (e.confirmPressed && settingsSel_ == 3) {
                helpScroll_ = 0;
                mode_ = Mode::HelpScroll;
                gui.requestRender();
            } else {
                commitAndLeave();
            }
            return;
        }
        // Joystick-Y / UP for row navigation (one-shot)
        static bool setNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &setNavDefl);
        if (nav != 0) {
            settingsSel_ = (nav > 0)
                ? (uint8_t)((settingsSel_ + 1) % rowCount)
                : (uint8_t)((settingsSel_ + rowCount - 1) % rowCount);
            gui.requestRender();
            return;
        }
        // LEFT/RIGHT / joystick-X for value cycling (one-shot)
        static bool setValDefl = false;
        const int8_t vs = valueOneShotStep(inputs, e, &setValDefl);
        if (vs != 0) {
            if (settingsSel_ == 0) {
                int next = (int)cursorSpeed_ + vs;
                if (next < 0) next = 2;
                if (next > 2) next = 0;
                cursorSpeed_ = (uint8_t)next;
            } else if (settingsSel_ == 1) {
                constexpr uint16_t kStep = 50;
                if (vs > 0) {
                    popupDurationMs_ = (popupDurationMs_ + kStep <= draw::kMaxDurationMs)
                                           ? popupDurationMs_ + kStep
                                           : draw::kMaxDurationMs;
                } else {
                    popupDurationMs_ =
                        (popupDurationMs_ > kStep + draw::kMinDurationMs)
                            ? (uint16_t)(popupDurationMs_ - kStep)
                            : draw::kMinDurationMs;
                }
            } else if (settingsSel_ == 2) {
                imuParallaxEnabled_ = !imuParallaxEnabled_;
            }
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::HelpScroll) {
        if (e.cancelPressed || e.confirmPressed) {
            mode_ = Mode::SettingsPopup;
            gui.requestRender();
            return;
        }
        // Help scroll uses repeating nav (not one-shot) for smooth scrolling.
        static bool helpNavDefl = false;
        static uint32_t helpHoldMs = 0;
        const int8_t nav = menuOneShotNav(inputs, e, &helpNavDefl, &helpHoldMs);
        if (nav != 0) {
            const uint8_t maxScroll = (kHelpLineCount > kHelpVisible)
                                      ? (uint8_t)(kHelpLineCount - kHelpVisible) : 0;
            if (nav > 0 && helpScroll_ < maxScroll) { helpScroll_++; gui.requestRender(); }
            else if (nav < 0 && helpScroll_ > 0)    { helpScroll_--; gui.requestRender(); }
        }
        return;
    }

    if (mode_ == Mode::SaveExitPrompt) {
        // SaveExitPrompt: DOWN/confirm activate the selected row.
        // Cancel also activates (there's no distinction).
        auto activateSel = [&]() {
            if (savePromptSel_ == 0) {
                if (draw::save(doc_)) {
                    saveFailed_ = false;
                    // Hand off to the Set-as-Nametag prompt before popping.
                    exitNametagSel_ = 1;
                    exitNametagJoyMs_ = 0;
                    mode_ = Mode::ExitNametagPrompt;
                    gui.requestRender();
                } else {
                    saveFailed_ = true;
                    gui.requestRender();
                }
                return;
            }
            if (savePromptSel_ == 1) {
                doc_.dirty = false;
                saveFailed_ = false;
                gui.popScreen();
                return;
            }
            saveFailed_ = false;
            mode_ = Mode::Editing;
            gui.requestRender();
        };
        if (e.confirmPressed || e.cancelPressed || e.downPressed) {
            activateSel();
            return;
        }
        // Joystick-Y / UP for row selection (one-shot)
        static bool saveNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &saveNavDefl);
        if (nav != 0) {
            savePromptSel_ = (nav > 0)
                ? (uint8_t)((savePromptSel_ + 1) % 3)
                : (uint8_t)((savePromptSel_ + 2) % 3);
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::ContextMenu) {
        // Cancel closes without activating; confirm/DOWN activate.
        if (e.cancelPressed) {
            mode_ = Mode::Editing;
            rightShown_ = false;
            rightSuppressUntilEdge_ = true;
            gui.requestRender();
            return;
        }
        if (e.confirmPressed || e.downPressed) {
            activateContextAction(gui);
            gui.requestRender();
            return;
        }
        static bool ctxNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &ctxNavDefl);
        if (nav != 0) {
            ctxMenu_.cursor = (nav > 0)
                ? (uint8_t)((ctxMenu_.cursor + 1) % kContextRowCount)
                : (uint8_t)((ctxMenu_.cursor + kContextRowCount - 1) % kContextRowCount);
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::ZAdjust) {
        // Z adjust: DOWN/confirm/cancel all commit.
        auto finish = [&]() {
            if (auto* p = findPlacement(currentFrame_, zAdjust_.targetObjId)) {
                doc_.dirty = true;
            }
            mode_ = Mode::Editing;
            gui.requestRender();
        };
        if (e.confirmPressed || e.cancelPressed || e.downPressed) {
            finish();
            return;
        }
        // LEFT/RIGHT / joystick-X for value cycling
        static bool zValDefl = false;
        const int8_t vs = valueOneShotStep(inputs, e, &zValDefl);
        if (vs != 0) {
            int16_t next = (int16_t)zAdjust_.pendingZ + vs;
            if (next < -8) next = -8;
            if (next > 8) next = 8;
            zAdjust_.pendingZ = (int8_t)next;
            if (auto* p = findPlacement(currentFrame_, zAdjust_.targetObjId)) {
                p->z = zAdjust_.pendingZ;
            }
            gui.requestRender();
        }
        // Also accept joystick-Y as value cycle
        static bool zNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &zNavDefl);
        if (nav != 0) {
            int16_t next = (int16_t)zAdjust_.pendingZ + (nav < 0 ? 1 : -1);
            if (next < -8) next = -8;
            if (next > 8) next = 8;
            zAdjust_.pendingZ = (int8_t)next;
            if (auto* p = findPlacement(currentFrame_, zAdjust_.targetObjId)) {
                p->z = zAdjust_.pendingZ;
            }
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::TextAppearance) {
        // TextAppearance: DOWN/confirm/cancel all commit.
        auto finish = [&]() {
            if (auto* def = findObjectDef(textAppear_.targetObjId)) {
                def->textFontFamily = textAppear_.fontFamily;
                def->textFontSlot = textAppear_.fontSlot;
                def->textStackMode = textAppear_.stack;
                doc_.dirty = true;
            }
            mode_ = Mode::Editing;
            gui.requestRender();
        };
        if (e.confirmPressed || e.cancelPressed || e.downPressed) {
            finish();
            return;
        }
        // Joystick-Y / UP for field navigation (one-shot)
        static bool taNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &taNavDefl);
        if (nav != 0) {
            textAppear_.fieldSel = (nav > 0)
                ? (uint8_t)((textAppear_.fieldSel + 1) % 3)
                : (uint8_t)((textAppear_.fieldSel + 2) % 3);
            gui.requestRender();
            return;
        }
        // LEFT/RIGHT / joystick-X for value cycling (one-shot)
        static bool taValDefl = false;
        const int8_t vs = valueOneShotStep(inputs, e, &taValDefl);
        if (vs != 0) {
            switch (textAppear_.fieldSel) {
                case 0: {
                    int next = (int)textAppear_.fontFamily + vs;
                    if (next < 0) next = kFontGridFamilyCount - 1;
                    if (next >= kFontGridFamilyCount) next = 0;
                    textAppear_.fontFamily = (uint8_t)next;
                    break;
                }
                case 1: {
                    int next = (int)textAppear_.fontSlot + vs;
                    if (next < 0) next = kSizeCount - 1;
                    if (next >= kSizeCount) next = 0;
                    textAppear_.fontSlot = (uint8_t)next;
                    break;
                }
                default: {
                    uint8_t s = static_cast<uint8_t>(textAppear_.stack);
                    s = (uint8_t)((s + (vs > 0 ? 1 : 2)) % 3);
                    textAppear_.stack = static_cast<draw::TextStackMode>(s);
                    break;
                }
            }
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::FieldPicker) {
        if (e.cancelPressed) {
            mode_ = Mode::Editing;
            gui.requestRender();
            return;
        }
        if (e.confirmPressed) {
            // Snapshot the chosen field's current value into the text-add path.
            BadgeInfo::Fields fields{};
            BadgeInfo::getCurrent(fields);
            if (fieldPickerCursor_ < kFieldDefCount) {
                const FieldDef& fd = kFieldDefs[fieldPickerCursor_];
                const char* base = reinterpret_cast<const char*>(&fields) + fd.offset;
                const char* val = base[0] ? base : kFieldDefs[fieldPickerCursor_].label;
                textOp_ = TextOp::Add;
                textEditTargetId_[0] = '\0';
                std::strncpy(textEditBuf_, val, sizeof(textEditBuf_) - 1);
                textEditBuf_[sizeof(textEditBuf_) - 1] = '\0';
                mode_ = Mode::Editing;
                onTextInputDone(textEditBuf_);
            } else {
                mode_ = Mode::Editing;
            }
            gui.requestRender();
            return;
        }
        static bool fpNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &fpNavDefl);
        if (nav < 0 && fieldPickerCursor_ > 0) {
            fieldPickerCursor_--;
            gui.requestRender();
        } else if (nav > 0 && fieldPickerCursor_ + 1 < kFieldDefCount) {
            fieldPickerCursor_++;
            gui.requestRender();
        }
        return;
    }

    if (mode_ == Mode::ExitNametagPrompt) {
        auto activate = [&]() {
            if (exitNametagSel_ == 0) {
                // Yes — adopt this drawing as the live nametag.
                if (doc_.animId[0]) {
                    auto* newDoc = new (std::nothrow) draw::AnimDoc();
                    if (newDoc && draw::load(doc_.animId, *newDoc) &&
                        !newDoc->frames.empty()) {
                        adoptNametagAnimationDoc(doc_.animId, newDoc);
                        badgeConfig.setNametagSetting(doc_.animId);
                        badgeConfig.saveToFile();
                    } else if (newDoc) {
                        draw::freeAll(*newDoc);
                        delete newDoc;
                    }
                }
            }
            gui.popScreen();
        };
        if (e.confirmPressed || e.downPressed || e.cancelPressed) {
            // Cancel = "No" by default (acts as keep-as-is); confirm runs the selection.
            if (e.cancelPressed) {
                gui.popScreen();
                return;
            }
            activate();
            return;
        }
        static bool enNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &enNavDefl);
        if (nav != 0) {
            exitNametagSel_ = (uint8_t)((exitNametagSel_ + 1) & 1);
            gui.requestRender();
        }
        return;
    }

    // ── Cursor + chrome state ──────────────────────────────────────────────
    // ToolMenu / HelpScroll — joystick drives UI, not the cursor.
    if (mode_ != Mode::ToolMenu && mode_ != Mode::HelpScroll) {
        integrateCursor(inputs);
    }
    updateChromeRevealState(inputs, now);

    // LEFT button opens the tool menu (or closes it if already open).
    if (e.leftPressed && !painting_ && mode_ == Mode::Editing) {
        // Pre-select the row matching the active tool.
        toolMenuCursor_ = 0;
        for (uint8_t i = 0; i < kToolSlotCount; i++) {
            if ((kToolSlots[i].zone == HitZone::ToolDraw && currentTool_ == Tool::Draw) ||
                (kToolSlots[i].zone == HitZone::ToolHand && currentTool_ == Tool::Hand) ||
                (kToolSlots[i].zone == HitZone::ToolEdit && currentTool_ == Tool::Edit)) {
                toolMenuCursor_ = i;
                break;
            }
        }
        toolMenuJoyLastMs_ = 0;
        toolMenuScrollMs_ = millis();
        leftShown_ = true;
        mode_ = Mode::ToolMenu;
        gui.requestRender();
        return;  // don't let the ToolMenu handler run this same tick
    }

    if (mode_ == Mode::ToolMenu) {
        if (e.confirmPressed) {
            onChromeClick(kToolSlots[toolMenuCursor_].zone, 0, gui);
            leftShown_ = false;
            // Only reset mode if onChromeClick didn't switch to a different
            // mode itself (e.g. SettingsPopup, DurationPopup, pushScreen…).
            if (mode_ == Mode::ToolMenu) {
                mode_ = Mode::Editing;
            }
            gui.requestRender();
            return;
        }
        if (e.cancelPressed || e.leftPressed) {
            leftShown_ = false;
            mode_ = Mode::Editing;
            gui.requestRender();
            return;
        }
        static bool toolNavDefl = false;
        const int8_t nav = menuOneShotNav(inputs, e, &toolNavDefl);
        if (nav < 0 && toolMenuCursor_ > 0) {
            toolMenuCursor_--;
            gui.requestRender();
        } else if (nav > 0 && toolMenuCursor_ + 1 < kToolSlotCount) {
            toolMenuCursor_++;
            gui.requestRender();
        }
        return;
    }

    // ── Ghost mode (sticker following cursor) ──────────────────────────────
    if (mode_ == Mode::Ghosted) {
        if (e.cancelPressed) {
            // Cancel leaves move state; restore the source placement when this
            // was an existing object grabbed from the frame.
            if (ghost_.moveExisting) {
                restoreUndo();
                if (redo_.pixels) { free(redo_.pixels); redo_.pixels = nullptr; }
                redo_.frameIdx = -1;
                redo_.placements.clear();
                redo_.valid = false;
            }
            ghost_ = GhostState{};
            currentTool_ = Tool::Hand;
            mode_ = Mode::Editing;
            gui.requestRender();
            return;
        }
        if (e.confirmPressed) {
            // Commit ghost: drop at cursor minus the original grab offset so
            // the picked-up pixel stays under the cursor (no teleporting).
            int16_t cxL, cyL;
            canvasLocalCursor(&cxL, &cyL);
            draw::ObjectPlacement p{};
            std::strncpy(p.objId, ghost_.objId, sizeof(p.objId) - 1);
            p.x = (int16_t)(cxL - ghost_.grabOffsetX);
            p.y = (int16_t)(cyL - ghost_.grabOffsetY);
            p.scale = ghost_.scale;
            p.z = ghost_.z;
            p.phaseMs = ghost_.phaseMs;
            if (currentFrame_ < doc_.frames.size()) {
                doc_.frames[currentFrame_].placements.push_back(p);
                doc_.dirty = true;
            }
            // Stash whether the placed object is text and we should auto-
            // open the appearance popup; we have to capture before resetting
            // ghost_ since onTextInputDone for text-add sets autoOpenAppearance_.
            char placedId[draw::kObjIdLen + 1] = {};
            std::strncpy(placedId, ghost_.objId, sizeof(placedId) - 1);
            const bool autoOpen = autoOpenAppearance_;
            ghost_ = GhostState{};
            currentTool_ = Tool::Hand;
            if (autoOpen) {
                autoOpenAppearance_ = false;
                const draw::ObjectDef* placedDef = findObjectDef(placedId);
                if (placedDef && placedDef->type == draw::ObjectType::Text) {
                    std::strncpy(ctxMenu_.targetObjId, placedId,
                                 sizeof(ctxMenu_.targetObjId) - 1);
                    doCtxTextAppearance(gui);
                    gui.requestRender();
                    return;
                }
            }
            mode_ = Mode::Editing;
            gui.requestRender();
        }
        return;
    }

    // ── Editing mode ───────────────────────────────────────────────────────

    // UP — object context menu when hovering; otherwise single-level undo.
    if (e.upPressed) {
        if (cursorInCanvas() && currentFrame_ < doc_.frames.size()) {
            int16_t lx, ly;
            canvasLocalCursor(&lx, &ly);
            const draw::ObjectDef* def = nullptr;
            const int hit = findAnyUnderCursor(currentFrame_, lx, ly, &def);
            if (hit >= 0 && def) {
                std::strncpy(ctxMenu_.targetObjId, def->id,
                             sizeof(ctxMenu_.targetObjId) - 1);
                ctxMenu_.targetPlacementIdx = (uint8_t)hit;
                ctxMenu_.cursor = 0;
                mode_ = Mode::ContextMenu;
                rightShown_ = true;
                gui.requestRender();
                return;
            }
        }
        gui.requestRender();
        return;
    }

    // ── Paint flow (Draw tool only) ────────────────────────────────────────
    //
    // Draw / Edit ink: CONFIRM strokes paint ON. Erase is Left (PlayStation X)
    // and semantic cancel (footer B/back)—same ergonomics across button maps.

    const bool drawPress  = e.confirmPressed;
    const bool erasePress = e.xPressed || e.cancelPressed;
    const bool drawHeld   = b.confirm;
    const bool eraseHeld  = b.x || b.cancel;
    const bool anyPaintHeld = drawHeld || eraseHeld;

    auto isPaintTool = [&]() { return currentTool_ == Tool::Draw; };

    if (isPaintTool() && (drawPress || erasePress) && !painting_) {
        uint8_t extra = 0;
        HitZone z = hitZoneAtCursor(&extra);

        if (z == HitZone::Canvas) {
            // Each Draw stroke targets the active DrawnAnim. Active is set
            // either by the Edit action (clicked an existing layer) or, if
            // empty, gets created fresh here. We deliberately do NOT pick
            // up the layer under the cursor — that path implicitly edited
            // existing drawings and surprised the user. The Edit tool is
            // now the only way to keep painting into an existing sticker.
            snapshotForUndo(currentFrame_);
            findOrCreateActiveDrawn(currentFrame_, /*createIfMissing=*/true);
            // Inflate to full canvas so the user can paint outside the
            // sticker's prior bbox; shrinkActiveDrawnToBBox tightens it
            // back at stroke release.
            expandActiveDrawnToCanvas();
            painting_     = true;
            paintingErase_ = erasePress && !drawPress;
            prevPaintX_   = -1;
            prevPaintY_   = -1;
            // Collapse chrome so the full canvas is visible while painting.
            topShown_ = false; bottomShown_ = false; leftShown_ = false; rightShown_ = false;
            // Steady haptic while painting: weak vibration, higher freq for erase.
            Haptics::setPwmFrequency(paintingErase_ ? kEraseHapticFreqHz : kDrawHapticFreqHz);
            Haptics::setDuty(kDrawHapticDuty);
            gui.requestRender();
            // Fall through so the first stamp lands this tick.
        } else if (drawPress && z != HitZone::None) {
            onChromeClick(z, extra, gui);
            gui.requestRender();
            return;
        }
    } else if (currentTool_ == Tool::Hand && drawPress && !painting_) {
        // Hand tool: clicking a sticker grabs and moves it (existing
        // ghost-mode mechanism). Works for any sticker type.
        uint8_t extra = 0;
        HitZone z = hitZoneAtCursor(&extra);
        if (z == HitZone::Canvas) {
            int16_t lx, ly;
            canvasLocalCursor(&lx, &ly);
            auto& placements = doc_.frames[currentFrame_].placements;
            int hit = -1;
            int8_t hitZ = -127;
            for (uint8_t i = 0; i < placements.size(); i++) {
                const auto& p = placements[i];
                const draw::ObjectDef* def = nullptr;
                for (const auto& o : doc_.objects) {
                    if (std::strcmp(o.id, p.objId) == 0) { def = &o; break; }
                }
                if (!def) continue;
                if (placementHitTest(p, *def, lx, ly) && p.z >= hitZ) {
                    hit = i;
                    hitZ = p.z;
                }
            }
            if (hit >= 0) {
                snapshotForUndo(currentFrame_);
                auto& p = placements[hit];
                ghost_.active = true;
                ghost_.moveExisting = true;
                ghost_.scale = p.scale;
                ghost_.z = p.z;
                ghost_.phaseMs = p.phaseMs;
                std::strncpy(ghost_.objId, p.objId,
                             sizeof(ghost_.objId) - 1);
                // Capture cursor offset within the placement so the grabbed
                // pixel tracks the cursor exactly (no jump on grab/drop).
                ghost_.grabOffsetX = (int16_t)(lx - p.x);
                ghost_.grabOffsetY = (int16_t)(ly - p.y);
                placements.erase(placements.begin() + hit);
                mode_ = Mode::Ghosted;
                gui.requestRender();
                return;
            }
        } else if (z != HitZone::None) {
            onChromeClick(z, extra, gui);
            gui.requestRender();
            return;
        }
    } else if (currentTool_ == Tool::Edit &&
               (drawPress || erasePress) && !painting_) {
        // Edit tool: like Draw, but only paints into an existing DrawnAnim.
        // Clicking on a DrawnAnim sets it as active and starts a stroke into
        // it in the SAME tick (so the click also paints). Cursor on empty
        // canvas does nothing — that's the user's signal that they should
        // switch to Draw to make a new layer.
        uint8_t extra = 0;
        HitZone z = hitZoneAtCursor(&extra);
        if (z == HitZone::Canvas) {
            int16_t lx, ly;
            canvasLocalCursor(&lx, &ly);
            const draw::ObjectDef* def = nullptr;
            int hit = findDrawnUnderCursor(currentFrame_, lx, ly, &def);
            if (hit >= 0 && def) {
                std::strncpy(activeObjId_, def->id, sizeof(activeObjId_) - 1);
                snapshotForUndo(currentFrame_);
                // Inflate so strokes outside the prior bbox land cleanly.
                expandActiveDrawnToCanvas();
                painting_     = true;
                paintingErase_ = erasePress && !drawPress;
                prevPaintX_   = -1;
                prevPaintY_   = -1;
                // Collapse chrome so the full canvas is visible while painting.
                topShown_ = false; bottomShown_ = false; leftShown_ = false; rightShown_ = false;
                // Steady haptic while painting (same as Draw tool).
                Haptics::setPwmFrequency(paintingErase_ ? kEraseHapticFreqHz : kDrawHapticFreqHz);
                Haptics::setDuty(kDrawHapticDuty);
                gui.requestRender();
                // Fall through into the continuation block so the first stamp
                // lands this tick.
            } else {
                return;
            }
        } else if (drawPress && z != HitZone::None) {
            onChromeClick(z, extra, gui);
            gui.requestRender();
            return;
        } else {
            return;
        }
    } else if (drawPress && !painting_) {
        // Other tools / non-canvas clicks: dispatch chrome.
        uint8_t extra = 0;
        HitZone z = hitZoneAtCursor(&extra);
        if (z != HitZone::None && z != HitZone::Canvas) {
            onChromeClick(z, extra, gui);
            gui.requestRender();
        }
        return;
    }

    // ── Paint continuation (Draw tool while a paint button is held) ───────
    if (painting_ && anyPaintHeld) {
        if (cursorInCanvas() && currentFrame_ < doc_.frames.size()) {
            int16_t lx, ly;
            canvasLocalCursor(&lx, &ly);
            draw::ObjectDef* def =
                findOrCreateActiveDrawn(currentFrame_, /*createIfMissing=*/false);
            if (def && def->drawnPixels) {
                // Translate canvas-local coords into the DrawnAnim's local
                // coords (subtract the placement offset).
                int16_t px = -1, py = -1;
                for (const auto& pl : doc_.frames[currentFrame_].placements) {
                    if (std::strcmp(pl.objId, def->id) == 0) {
                        px = pl.x;
                        py = pl.y;
                        break;
                    }
                }
                const int16_t sx = lx - px;
                const int16_t sy = ly - py;
                // Erase vs ink is fixed for the stroke (set at stroke start).
                const bool penColor = !paintingErase_;
                if (prevPaintX_ < 0) {
                    plotStamp(def->drawnPixels, def->drawnW, def->drawnH,
                              sx, sy, penColor);
                } else if (sx != prevPaintX_ || sy != prevPaintY_) {
                    plotLine(def->drawnPixels, def->drawnW, def->drawnH,
                             prevPaintX_, prevPaintY_, sx, sy, penColor);
                }
                def->drawnDirty = true;
                doc_.dirty = true;
                prevPaintX_ = sx;
                prevPaintY_ = sy;
                gui.requestRender();
            }
        }
    }

    // ── Paint release ─────────────────────────────────────────────────────
    if (painting_ && !anyPaintHeld) {
        painting_ = false;
        paintingErase_ = false;
        prevPaintX_ = -1;
        prevPaintY_ = -1;
        // Stop haptic feedback and restore default frequency.
        Haptics::off();
        Haptics::setPwmFrequency(Haptics::kDefaultPwmFreqHz);
        // Tighten the active DrawnAnim's pixel buffer to the bounding box
        // of its actually-drawn pixels (and shift the placement to keep the
        // visual position fixed). Drops the layer entirely if the buffer is
        // empty after an erase stroke.
        shrinkActiveDrawnToBBox();
        rebuildThumb(currentFrame_);
        gui.requestRender();
    }
}

// ── Sticker pick callbacks ─────────────────────────────────────────────────

void DrawScreen::onStickerPicked(const StickerPickerScreen::Result& r) {
    // Stash the source; we can't enter Ghosted yet because ScalePicker still
    // needs a scale value. The scale picker's callback finalizes.
    ghost_ = GhostState{};
    ghost_.src = r;
}

void DrawScreen::onScalePicked(uint8_t scale) {
    // Allocate an ObjectDef in the doc if we don't already have one matching
    // this source, then enter Ghosted mode.
    char objId[draw::kObjIdLen + 1];
    draw::newObjectId(doc_, objId, sizeof(objId));
    draw::ObjectDef def{};
    std::strncpy(def.id, objId, sizeof(def.id) - 1);
    if (ghost_.src.kind == StickerPickerScreen::PickKind::Catalog) {
        def.type = draw::ObjectType::Catalog;
        def.catalogIdx = ghost_.src.catalogIdx;
    } else {
        def.type = draw::ObjectType::SavedAnim;
        std::strncpy(def.savedAnimId, ghost_.src.savedAnimId,
                     sizeof(def.savedAnimId) - 1);
    }
    doc_.objects.push_back(def);
    doc_.dirty = true;

    ghost_.active = true;
    ghost_.moveExisting = false;
    ghost_.scale = scale;
    ghost_.z = nextPlacementZ_;
    // Advance: +1, -1, +2, -2, +3, -3, ...
    if (nextPlacementZ_ > 0)
        nextPlacementZ_ = (int8_t)(-nextPlacementZ_);
    else
        nextPlacementZ_ = (int8_t)(-nextPlacementZ_ + 1);
    ghost_.phaseMs = 0;
    std::strncpy(ghost_.objId, objId, sizeof(ghost_.objId) - 1);
    // Fresh placement: center on cursor (grabOffset = half size).
    ghost_.grabOffsetX = (int16_t)(scale / 2);
    ghost_.grabOffsetY = (int16_t)(scale / 2);
    mode_ = Mode::Ghosted;
}

void DrawScreen::onReplaceStickerPicked(const StickerPickerScreen::Result& r) {
    draw::ObjectDef* def = findObjectDef(replaceTargetObjId_);
    if (!def) return;
    snapshotForUndo(currentFrame_);
    if (r.kind == StickerPickerScreen::PickKind::Catalog) {
        def->type = draw::ObjectType::Catalog;
        def->catalogIdx = r.catalogIdx;
        def->savedAnimId[0] = '\0';
    } else {
        def->type = draw::ObjectType::SavedAnim;
        def->catalogIdx = -1;
        std::strncpy(def->savedAnimId, r.savedAnimId, sizeof(def->savedAnimId) - 1);
        def->savedAnimId[sizeof(def->savedAnimId) - 1] = '\0';
    }
    savedCache_.clear();
    doc_.dirty = true;
}

void DrawScreen::onCtxScalePicked(uint8_t scale) {
    if (auto* p = findPlacement(currentFrame_, scaleTargetObjId_)) {
        snapshotForUndo(currentFrame_);
        p->scale = scale;
        doc_.dirty = true;
    }
}

void DrawScreen::onTextInputDone(const char* text) {
    if (textOp_ == TextOp::Edit) {
        if (auto* def = findObjectDef(textEditTargetId_)) {
            std::strncpy(def->textContent, text ? text : "", draw::kTextContentMax);
            def->textContent[draw::kTextContentMax] = '\0';
            doc_.dirty = true;
        }
    } else if (textOp_ == TextOp::Add && text && text[0]) {
        char objId[draw::kObjIdLen + 1];
        draw::newObjectId(doc_, objId, sizeof(objId));
        draw::ObjectDef def{};
        def.type = draw::ObjectType::Text;
        std::strncpy(def.id, objId, sizeof(def.id) - 1);
        std::strncpy(def.textContent, text, draw::kTextContentMax);
        def.textContent[draw::kTextContentMax] = '\0';
        doc_.objects.push_back(def);
        doc_.dirty = true;

        ghost_ = GhostState{};
        ghost_.active = true;
        ghost_.moveExisting = false;
        ghost_.scale = 0;
        ghost_.z = nextPlacementZ_;
        if (nextPlacementZ_ > 0)
            nextPlacementZ_ = (int8_t)(-nextPlacementZ_);
        else
            nextPlacementZ_ = (int8_t)(-nextPlacementZ_ + 1);
        ghost_.phaseMs = 0;
        std::strncpy(ghost_.objId, objId, sizeof(ghost_.objId) - 1);
        ghost_.grabOffsetX = 0;
        ghost_.grabOffsetY = 0;
        // Auto-open the appearance popup once the user drops the text so they
        // can immediately pick a font/size that fits without backtracking.
        autoOpenAppearance_ = true;
        mode_ = Mode::Ghosted;
    }
    textOp_ = TextOp::None;
    textEditTargetId_[0] = '\0';
}
