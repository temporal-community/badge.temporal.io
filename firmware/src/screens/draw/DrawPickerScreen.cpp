#include "DrawPickerScreen.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "../../BadgeGlobals.h"
#include "../../hardware/Inputs.h"
#include "../../hardware/oled.h"
#include "../../infra/BadgeConfig.h"
#include "../../infra/PsramAllocator.h"
#include "../../ui/GUI.h"
#include "../../ui/OLEDLayout.h"
#include "../../ui/UIFonts.h"
#include "../TextInputScreen.h"
#include "DrawScreen.h"

extern TextInputScreen sTextInput;

DrawPickerScreen sDrawPicker;

namespace {

void onRenameDone(const char* /*text*/, void* user) {
    auto* self = static_cast<DrawPickerScreen*>(user);
    if (self) self->onRenameSubmit();
}

}  // namespace

void DrawPickerScreen::onEnter(GUIManager& /*gui*/) {
    reload();
    cursor_ = 0;
    scroll_ = 0;
    lastJoyNavMs_ = 0;
    mode_ = Mode::List;
    pendingActionId_[0] = '\0';
    cancelHoldStartMs_ = 0;
}

void DrawPickerScreen::onResume(GUIManager& /*gui*/) {
    // Returning from the editor / TextInput sub-screen: refresh the listing
    // so renames, new files, deletions are visible without a re-enter.
    reload();
    if (cursor_ >= totalRows()) cursor_ = totalRows() ? totalRows() - 1 : 0;
    if (scroll_ + kVisibleRows > totalRows()) {
        scroll_ = totalRows() > kVisibleRows ? totalRows() - kVisibleRows : 0;
    }
    cancelHoldStartMs_ = 0;
    mode_ = Mode::List;
}

void DrawPickerScreen::reload() {
    entries_.clear();
    draw::listAll(entries_);
    std::sort(entries_.begin(), entries_.end(),
              [](const draw::AnimSummary& a, const draw::AnimSummary& b) {
                  return a.editedAt > b.editedAt;
              });
}

uint8_t DrawPickerScreen::totalRows() const {
    return 2 + (uint8_t)entries_.size();
}

void DrawPickerScreen::moveCursor(int8_t delta) {
    const uint8_t total = totalRows();
    if (total == 0) return;
    int16_t next = (int16_t)cursor_ + delta;
    if (next < 0) next = 0;
    if (next >= total) next = total - 1;
    cursor_ = (uint8_t)next;
    if (cursor_ < scroll_) scroll_ = cursor_;
    if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1;
}

// ── Render ────────────────────────────────────────────────────────────────

void DrawPickerScreen::drawHelpMarquee(oled& d, const char* msg, uint32_t nowMs,
                                        int16_t rightEdge) {
    if (!msg || !msg[0]) return;
    d.setFont(UIFonts::kText);
    constexpr int16_t kBandLeft = 1;
    const int16_t bandRight = rightEdge;
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
    const int offset = (loop > 0) ? (int)((nowMs / 35) % loop) : 0;
    d.setClipWindow(kBandLeft, kBaseY - 7, bandRight - 1, kBaseY + 1);
    d.drawStr(kBandLeft - offset, kBaseY, msg);
    d.drawStr(kBandLeft - offset + loop, kBaseY, msg);
    d.setMaxClipWindow();
}

void DrawPickerScreen::render(oled& d, GUIManager& /*gui*/) {
    if (mode_ == Mode::List) renderList(d);
    else if (mode_ == Mode::Context) renderContext(d);
    else renderConfirm(d);
}

void DrawPickerScreen::renderList(oled& d) {
    OLEDLayout::drawHeader(d, "DRAW", nullptr);
    d.setFont(UIFonts::kText);

    const uint8_t total = totalRows();
    if (total == 0) {
        d.drawStr(2, 30, "no items");
        OLEDLayout::drawGameFooter(d);
        OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "ok");
        return;
    }

    for (uint8_t i = 0; i < kVisibleRows; i++) {
        uint8_t idx = scroll_ + i;
        if (idx >= total) break;
        uint8_t y = kTopY + i * kRowHeight;
        const bool selected = (idx == cursor_);
        if (selected) {
            OLEDLayout::drawSelectedRow(d, y, kRowHeight);
            d.setDrawColor(0);
        } else {
            d.setDrawColor(1);
        }

        char line[40];
        if (idx == 0) {
            std::snprintf(line, sizeof(line), "+ New 128x64");
        } else if (idx == 1) {
            std::snprintf(line, sizeof(line), "+ New 48x48");
        } else {
            const auto& s = entries_[savedIndex(idx)];
            const char* nm = s.name[0] ? s.name : "Untitled";
            std::snprintf(line, sizeof(line), "%s  %ux%u f%u",
                          nm, (unsigned)s.w, (unsigned)s.h,
                          (unsigned)s.frameCount);
        }
        OLEDLayout::fitText(d, line, sizeof(line), 124);
        d.drawStr(3, y + d.getAscent() + 1, line);
        d.setDrawColor(1);
    }

    if (scroll_ > 0) {
        d.fillTriangle(124, kTopY + 3, 121, kTopY + 6, 127, kTopY + 6);
    }
    if (scroll_ + kVisibleRows < total) {
        uint8_t arrowY = kTopY + kVisibleRows * kRowHeight;
        d.fillTriangle(124, arrowY, 121, arrowY - 3, 127, arrowY - 3);
    }

    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);

    // Right-edge button-hint chips (drawn first so we know how much room
    // they take, then the marquee fills the remaining left band).
    int chipsW = 0;
    if (isSavedRow(cursor_)) {
        chipsW = OLEDLayout::drawFooterActions(d, nullptr, "menu", "back", "open");
    } else {
        chipsW = OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "new");
    }

    // Scrolling description of what the highlighted row does — a 4 px gap
    // keeps it from kissing the chip glyphs.
    char helpBuf[160];
    if (cursor_ == 0) {
        std::snprintf(helpBuf, sizeof(helpBuf),
                      "New 128x64 drawing - CONFIRM to start - hold CANCEL to leave");
    } else if (cursor_ == 1) {
        std::snprintf(helpBuf, sizeof(helpBuf),
                      "New 48x48 zigmoji - CONFIRM to start - hold CANCEL to leave");
    } else if (isSavedRow(cursor_)) {
        const auto& s = entries_[savedIndex(cursor_)];
        const char* nm = s.name[0] ? s.name : "Untitled";
        const bool isNametag =
            std::strcmp(s.animId, badgeConfig.nametagSetting()) == 0;
        std::snprintf(helpBuf, sizeof(helpBuf),
                      "%s - UP for menu%s",
                      nm, isNametag ? " - current nametag" : "");
    } else {
        helpBuf[0] = '\0';
    }
    const int16_t marqueeRight = (chipsW > 0) ? (int16_t)(128 - chipsW - 4) : 128;
    drawHelpMarquee(d, helpBuf, millis(), marqueeRight);
}

void DrawPickerScreen::renderContext(oled& d) {
    OLEDLayout::drawHeader(d, "DRAW", nullptr);

    constexpr uint8_t kCount = (uint8_t)CtxRow::Count;
    const char* labels[kCount] = {"Nametag", "Rename", "Duplicate", "Delete"};

    constexpr uint8_t boxW = 96;
    constexpr uint8_t rowH = 9;
    constexpr uint8_t boxH = (uint8_t)(6 + kCount * rowH);
    constexpr uint8_t boxX = (128 - boxW) / 2;
    constexpr uint8_t boxY = 11;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);

    const bool nametagOn = isCurrentNametag();
    for (uint8_t i = 0; i < kCount; i++) {
        uint8_t y = (uint8_t)(boxY + 3 + i * rowH);
        const bool selected = (i == ctxCursor_);
        if (selected) {
            d.setDrawColor(1);
            d.drawBox(boxX + 2, y, boxW - 4, rowH);
            d.setDrawColor(0);
        }
        // Nametag row: leading checkbox (filled vs outline 7x7).
        if (i == (uint8_t)CtxRow::Nametag) {
            const int16_t cbX = boxX + 5;
            const int16_t cbY = y + 1;
            d.drawHLine(cbX, cbY, 7);
            d.drawHLine(cbX, cbY + 6, 7);
            d.drawVLine(cbX, cbY, 7);
            d.drawVLine(cbX + 6, cbY, 7);
            if (nametagOn) {
                d.drawBox(cbX + 2, cbY + 2, 3, 3);
            }
            d.drawStr(boxX + 16, y + rowH - 1, labels[i]);
        } else {
            d.drawStr(boxX + 6, y + rowH - 1, labels[i]);
        }
        d.setDrawColor(1);
    }

    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);
    const int chipsW =
        OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "ok");

    char helpBuf[160];
    switch ((CtxRow)ctxCursor_) {
        case CtxRow::Nametag:
            std::snprintf(helpBuf, sizeof(helpBuf),
                          "%s nametag - CONFIRM toggles",
                          nametagOn ? "Currently set as" : "Set this drawing as");
            break;
        case CtxRow::Rename:
            std::snprintf(helpBuf, sizeof(helpBuf),
                          "Edit the drawing name - CONFIRM opens keyboard");
            break;
        case CtxRow::Duplicate:
            std::snprintf(helpBuf, sizeof(helpBuf),
                          "Make a copy of this drawing");
            break;
        case CtxRow::Delete:
            std::snprintf(helpBuf, sizeof(helpBuf),
                          "Permanently remove this drawing");
            break;
        default:
            helpBuf[0] = '\0';
            break;
    }
    const int16_t marqueeRight = (chipsW > 0) ? (int16_t)(128 - chipsW - 4) : 128;
    drawHelpMarquee(d, helpBuf, millis(), marqueeRight);
}

void DrawPickerScreen::renderConfirm(oled& d) {
    OLEDLayout::drawHeader(d, "DELETE?", nullptr);
    d.setFont(UIFonts::kText);
    d.drawStr(8, 24, "Delete this anim?");
    d.drawStr(8, 36, "This cannot be undone.");
    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "no", "yes");
}

// ── Input ─────────────────────────────────────────────────────────────────

void DrawPickerScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                   int16_t /*cy*/, GUIManager& gui) {
    const Inputs::ButtonEdges& e = inputs.edges();
    const Inputs::ButtonStates& b = inputs.buttons();
    const uint32_t now = millis();

    // Hold-Cancel-to-leave (4s). Only armed in the list view; popups handle
    // Cancel via their own edge press. Reset the hold timer whenever we
    // leave the list so re-entering with the button held doesn't fire pop.
    if (mode_ == Mode::List && b.cancel) {
        if (cancelHoldStartMs_ == 0) cancelHoldStartMs_ = now;
        if (now - cancelHoldStartMs_ >= kCancelHoldMs) {
            cancelHoldStartMs_ = 0;
            gui.popScreen();
            return;
        }
        gui.requestRender();
    } else {
        cancelHoldStartMs_ = 0;
    }

    if (mode_ == Mode::Confirm) {
        if (e.confirmPressed) {
            doDelete(gui);
            return;
        }
        if (e.cancelPressed) {
            mode_ = Mode::List;
            gui.requestRender();
            return;
        }
        return;
    }

    if (mode_ == Mode::Context) {
        if (e.cancelPressed) {
            mode_ = Mode::List;
            gui.requestRender();
            return;
        }
        if (e.confirmPressed) {
            switch ((CtxRow)ctxCursor_) {
                case CtxRow::Nametag:   doToggleNametag(gui); return;
                case CtxRow::Rename:    doRename(gui); return;
                case CtxRow::Duplicate: doDuplicate(gui); return;
                case CtxRow::Delete:
                    mode_ = Mode::Confirm;
                    gui.requestRender();
                    return;
                default: return;
            }
            return;
        }
        const uint8_t ctxCount = (uint8_t)CtxRow::Count;
        const int16_t dy = (int16_t)inputs.joyY() - 2047;
        if (abs(dy) > (int16_t)kJoyDeadband) {
            if (lastJoyNavMs_ == 0 || now - lastJoyNavMs_ >= 250) {
                lastJoyNavMs_ = now;
                if (dy > 0 && ctxCursor_ + 1 < ctxCount) {
                    ctxCursor_++;
                    gui.requestRender();
                } else if (dy < 0 && ctxCursor_ > 0) {
                    ctxCursor_--;
                    gui.requestRender();
                }
            }
        } else {
            lastJoyNavMs_ = 0;
        }
        return;
    }

    // Mode::List
    if (e.confirmPressed) {
        if (cursor_ == 0) enterEditorNew(gui, draw::kCanvasFullW, draw::kCanvasFullH);
        else if (cursor_ == 1) enterEditorNew(gui, draw::kCanvasZigW, draw::kCanvasZigH);
        else enterEditorForCurrent(gui);
        return;
    }
    if (e.cancelReleased) {
        // Quick tap-and-release: leave the picker. Long holds were already
        // handled above and would have popped before this edge fired.
        gui.popScreen();
        return;
    }
    if (e.upPressed && isSavedRow(cursor_)) {
        openContextMenu();
        gui.requestRender();
        return;
    }

    const int16_t dy = (int16_t)inputs.joyY() - 2047;
    if (abs(dy) > (int16_t)kJoyDeadband) {
        const uint32_t repeatMs = abs(dy) > 1500 ? 80 : (abs(dy) > 900 ? 160 : 300);
        if (lastJoyNavMs_ == 0 || now - lastJoyNavMs_ >= repeatMs) {
            lastJoyNavMs_ = now;
            moveCursor(dy > 0 ? 1 : -1);
            gui.requestRender();
        }
    } else {
        lastJoyNavMs_ = 0;
    }
}

void DrawPickerScreen::openContextMenu() {
    if (!isSavedRow(cursor_)) return;
    const auto& s = entries_[savedIndex(cursor_)];
    std::strncpy(pendingActionId_, s.animId, sizeof(pendingActionId_) - 1);
    pendingActionId_[sizeof(pendingActionId_) - 1] = '\0';
    ctxCursor_ = 0;
    mode_ = Mode::Context;
}

// ── Actions ───────────────────────────────────────────────────────────────

void DrawPickerScreen::enterEditorForCurrent(GUIManager& gui) {
    if (!isSavedRow(cursor_)) return;
    const auto& s = entries_[savedIndex(cursor_)];
    sDrawScreen.openExisting(s.animId);
    gui.pushScreen(kScreenDraw);
}

void DrawPickerScreen::enterEditorNew(GUIManager& gui, uint16_t w, uint16_t h) {
    sDrawScreen.openNew(w, h);
    gui.pushScreen(kScreenDraw);
}

void DrawPickerScreen::doRename(GUIManager& gui) {
    if (!pendingActionId_[0]) return;
    // Seed buffer with current name.
    for (const auto& s : entries_) {
        if (std::strcmp(s.animId, pendingActionId_) == 0) {
            std::strncpy(renameBuf_, s.name, sizeof(renameBuf_) - 1);
            renameBuf_[sizeof(renameBuf_) - 1] = '\0';
            break;
        }
    }
    sTextInput.configure("Rename", renameBuf_, sizeof(renameBuf_),
                         &onRenameDone, this);
    mode_ = Mode::List;
    gui.pushScreen(kScreenTextInput);
}

void DrawPickerScreen::onRenameSubmit() {
    if (!pendingActionId_[0] || !renameBuf_[0]) return;
    draw::AnimDoc doc;
    if (draw::load(pendingActionId_, doc)) {
        std::strncpy(doc.name, renameBuf_, sizeof(doc.name) - 1);
        doc.name[sizeof(doc.name) - 1] = '\0';
        // Mark every frame dirty so save writes ALL .fb files? No — only need
        // info.json to change. dirtyOnDisk for frames is already false; save()
        // skips them. Just update editedAt and save.
        draw::save(doc);
    }
    draw::freeAll(doc);
    pendingActionId_[0] = '\0';
    reload();
}

void DrawPickerScreen::doDuplicate(GUIManager& gui) {
    if (!pendingActionId_[0]) return;
    char newId[draw::kAnimIdLen + 1] = {};
    draw::duplicateAnim(pendingActionId_, newId, sizeof(newId));
    pendingActionId_[0] = '\0';
    reload();
    mode_ = Mode::List;
    gui.requestRender();
}

void DrawPickerScreen::doDelete(GUIManager& gui) {
    if (!pendingActionId_[0]) {
        mode_ = Mode::List;
        return;
    }
    draw::removeAnim(pendingActionId_);
    pendingActionId_[0] = '\0';
    reload();
    if (cursor_ >= totalRows()) cursor_ = totalRows() - 1;
    mode_ = Mode::List;
    gui.requestRender();
}

bool DrawPickerScreen::isCurrentNametag() const {
    if (!pendingActionId_[0]) return false;
    return std::strcmp(pendingActionId_, badgeConfig.nametagSetting()) == 0;
}

void DrawPickerScreen::setNametag(const char* animId) {
    if (!animId || !animId[0]) return;
    auto* newDoc = new (std::nothrow) draw::AnimDoc();
    if (!newDoc) return;
    if (!draw::load(animId, *newDoc) || newDoc->frames.empty()) {
        draw::freeAll(*newDoc);
        delete newDoc;
        return;
    }
    adoptNametagAnimationDoc(animId, newDoc);
    badgeConfig.setNametagSetting(animId);
    badgeConfig.saveToFile();
}

void DrawPickerScreen::clearNametagToDefault() {
    // Drop any custom nametag doc; the renderer falls back to the built-in
    // default text nametag whenever gCustomNametagEnabled is false.
    unloadNametagAnimationDoc();
    gCustomNametagEnabled = false;
    badgeConfig.setNametagSetting("default");
    badgeConfig.saveToFile();
}

void DrawPickerScreen::doToggleNametag(GUIManager& gui) {
    if (!pendingActionId_[0]) { mode_ = Mode::List; return; }
    if (isCurrentNametag()) {
        clearNametagToDefault();
    } else {
        setNametag(pendingActionId_);
    }
    // Stay in the context popup so the user sees the checkbox flip.
    gui.requestRender();
}
