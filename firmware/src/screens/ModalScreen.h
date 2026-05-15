#pragma once

// ============================================================
//  ModalScreen — shared base for full-screen + inset modal
//  "apps". Both the section modal (full-screen, no frame) and
//  the schedule detail modal (inset, rounded-frame, scrolling
//  title + time-range subhead) build on this base so push/pop,
//  fade transitions, chrome rendering, footer chip layout, and
//  input dispatch live in one place.
//
//  Subclass contract:
//    • Provide title() (required) + subhead() (optional).
//    • Override drawBody() to paint inside chrome.body region.
//    • Override chip labels (xChip/yChip/bChip/aChip) — bChip /
//      aChip are passed straight to drawFooterActions, which is
//      swap-aware (kSwapConfirmCancel) so labels follow the
//      physical button glyph regardless of config.
//    • Override onConfirm()/onY() for primary/tertiary actions.
//      Cancel always pops; subclass doesn't need to handle it.
//    • Override layout (boxX/boxY/boxW/boxH/useFrame/actionStripH)
//      to opt into the inset framed variant. Defaults to full-
//      screen unframed.
// ============================================================

#include "Screen.h"
#include "../ui/OLEDLayout.h"

class ModalScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  bool showCursor() const override { return false; }

 protected:
  // ── Layout ────────────────────────────────────────────────
  // Default: full-screen rounded-rectangle frame. All modals share
  // this look — title strip flush to the top, action chips flush to
  // the bottom, rounded outline along the screen edges. Subclasses
  // override only when the visual contract differs.
  virtual int  boxX() const { return 0; }
  virtual int  boxY() const { return 0; }
  virtual int  boxW() const { return OLEDLayout::kScreenW; }
  virtual int  boxH() const { return OLEDLayout::kScreenH; }
  virtual int  actionStripH() const { return 10; }
  virtual bool useFrame() const { return true; }

  // ── Chrome content ────────────────────────────────────────
  virtual const char* title()   const = 0;
  virtual const char* subhead() const { return nullptr; }

  // ── Body content (caller draws inside chrome.body region) ─
  virtual void drawBody(oled& d, const OLEDLayout::ModalChrome& chrome) = 0;

  // ── Action chips ──────────────────────────────────────────
  virtual const char* xChip() const { return nullptr; }
  virtual const char* yChip() const { return nullptr; }
  virtual const char* bChip() const { return "back"; }
  virtual const char* aChip() const { return nullptr; }

  // ── Input dispatch (cancel always pops) ───────────────────
  virtual void onConfirm(GUIManager& gui) { (void)gui; }
  virtual void onY(GUIManager& gui)        { (void)gui; }

  // millis() at the moment the modal was pushed onto the stack —
  // used as the scroll-start timestamp for the title strip.
  uint32_t openedMs_ = 0;
};
