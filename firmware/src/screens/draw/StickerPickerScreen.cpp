#include "StickerPickerScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../hardware/Inputs.h"
#include "../../hardware/oled.h"
#include "../../infra/Filesystem.h"
#include "../../infra/PsramAllocator.h"
#include "../../ui/GUI.h"
#include "../../ui/Images.h"
#include "../../ui/OLEDLayout.h"
#include "../../ui/UIFonts.h"
#include "AnimDoc.h"

StickerPickerScreen sStickerPicker;

namespace {

// Grid layout: 4 cols × 2 rows visible. Cells are 32 px wide × 24 px tall
// to fit 24×24 thumbnails. We drop the standard footer rule (which would
// land at y=54 and clip the bottom row) and put the action hints directly
// below the grid; this keeps the entire grid inside y=10..57 with the
// hints at y=63 baseline.
constexpr uint8_t kCols       = 4;
constexpr uint8_t kVisRows    = 2;
constexpr uint8_t kCellW      = 32;
constexpr uint8_t kCellH      = 24;
constexpr uint8_t kGridTopY   = 10;
constexpr uint8_t kThumbDim   = 24;     // 24 still divides 48 cleanly; 64-only
                                         // catalog sources fall back to a 32→24
                                         // crop (acceptable for the small
                                         // number of catalog images that lack
                                         // a data48 variant).
constexpr uint16_t kJoyDeadband = 400;

// Read all .fb frames of a saved animation into a freshly-malloc'd PSRAM
// buffer concatenated frame-after-frame. Returns the buffer + count + bytes-
// per-frame; caller is responsible for free()-ing the buffer. Used for the
// grid preview's middle frame; we don't keep a full editor doc loaded.
struct SavedPreview {
    uint8_t* frames    = nullptr;       // count * (w*h/8) bytes
    uint8_t  count     = 0;
    uint16_t w         = 0;
    uint16_t h         = 0;
};

void freePreview(SavedPreview& p) {
    if (p.frames) free(p.frames);
    p = {};
}

// Load the saved animation's first DrawnAnim layer as a single-frame
// preview. This avoids compositing every layer for the picker grid; the
// trade-off is that previews of multi-layer animations only show one layer
// (good enough for "did I draw this one?" recognition).
bool loadSavedPreview(const draw::AnimSummary& s, SavedPreview& out) {
    out = {};
    draw::AnimDoc doc;
    if (!draw::load(s.animId, doc)) return false;

    const draw::ObjectDef* first = nullptr;
    for (const auto& obj : doc.objects) {
        if (obj.type == draw::ObjectType::DrawnAnim && obj.drawnPixels) {
            first = &obj;
            break;
        }
    }
    if (!first) {
        draw::freeAll(doc);
        return false;
    }

    const size_t bytes = draw::xbmBytes(first->drawnW, first->drawnH);
    out.frames = (uint8_t*)BadgeMemory::allocPreferPsram(bytes);
    if (!out.frames) {
        draw::freeAll(doc);
        return false;
    }
    std::memcpy(out.frames, first->drawnPixels, bytes);
    out.count = 1;
    out.w = first->drawnW;
    out.h = first->drawnH;
    draw::freeAll(doc);
    return true;
}

}  // namespace

void StickerPickerScreen::configure(const char* excludeAnimId, Callback cb,
                                    void* user) {
    cb_ = cb;
    user_ = user;
    if (excludeAnimId) {
        std::strncpy(excludeId_, excludeAnimId, sizeof(excludeId_) - 1);
        excludeId_[sizeof(excludeId_) - 1] = '\0';
    } else {
        excludeId_[0] = '\0';
    }
}

void StickerPickerScreen::onEnter(GUIManager& /*gui*/) {
    rebuildEntries();
    cursor_ = 0;
    scroll_ = 0;
    lastJoyNavMs_ = 0;
}

void StickerPickerScreen::rebuildEntries() {
    catalogCount_ = kImageCatalogCount;
    savedEntries_.clear();
    draw::listAll(savedEntries_);
    if (excludeId_[0]) {
        savedEntries_.erase(
            std::remove_if(savedEntries_.begin(), savedEntries_.end(),
                           [this](const draw::AnimSummary& s) {
                               return std::strcmp(s.animId, excludeId_) == 0;
                           }),
            savedEntries_.end());
    }
    std::sort(savedEntries_.begin(), savedEntries_.end(),
              [](const draw::AnimSummary& a, const draw::AnimSummary& b) {
                  return a.editedAt > b.editedAt;
              });
}

uint8_t StickerPickerScreen::total() const {
    return catalogCount_ + (uint8_t)savedEntries_.size();
}

void StickerPickerScreen::moveCursor(int8_t delta) {
    const uint8_t t = total();
    if (t == 0) return;
    int16_t next = (int16_t)cursor_ + delta;
    if (next < 0) next = 0;
    if (next >= t) next = t - 1;
    cursor_ = (uint8_t)next;

    // Scroll-by-row (one row of `kCols` entries).
    const uint8_t firstVisible = scroll_ * kCols;
    const uint8_t lastVisible  = firstVisible + kVisRows * kCols - 1;
    if (cursor_ < firstVisible) {
        scroll_ = cursor_ / kCols;
    } else if (cursor_ > lastVisible) {
        const uint8_t lastRow = cursor_ / kCols;
        scroll_ = (lastRow >= kVisRows) ? lastRow - kVisRows + 1 : 0;
    }
}

// Draw a single cell at (x, y), kCellW × kCellH. Catalog entries draw their
// middle frame (or frame 0 when single-frame); saved-anim entries do the same.
namespace {

void drawCatalogPreview(oled& d, int16_t cellX, int16_t cellY,
                        const ImageInfo& img) {
    const uint8_t frameIdx = (img.frameCount > 0) ? (img.frameCount / 2) : 0;
    // `getFrame` chooses data48 only when the target dim is 48/24/12;
    // for any other size it falls back to data64. We must mirror that
    // logic here, otherwise srcDim disagrees with the buffer `getFrame`
    // returned and ImageScaler::scale reads with the wrong row stride —
    // which surfaces as lines wrapping around in the rendered preview.
    const bool use48 =
        img.data48 && (kThumbDim == 48 || kThumbDim == 24 || kThumbDim == 12);
    const uint8_t srcDim = use48 ? 48 : 64;
    const uint8_t* frame = ImageScaler::getFrame(img, frameIdx, kThumbDim);
    const int16_t x = cellX + (kCellW - kThumbDim) / 2;
    const int16_t y = cellY;
    d.drawXBM(x, y, srcDim, srcDim, frame, kThumbDim, kThumbDim);
}

void drawSavedPreview(oled& d, int16_t cellX, int16_t cellY,
                      const draw::AnimSummary& s) {
    SavedPreview p;
    if (!loadSavedPreview(s, p)) return;
    const uint8_t frameIdx = (p.count > 0) ? (p.count / 2) : 0;
    const size_t bytesPerFrame = (size_t)p.w * p.h / 8u;
    const uint8_t* bits = p.frames + frameIdx * bytesPerFrame;

    // Pick the smallest power-of-2 divisor that fits the cell on both axes
    // (cell is 32×24). ImageScaler::scale needs srcW%dstW==0 / srcH%dstH==0,
    // and our doc sizes (128, 64, 48) all divide cleanly by powers of 2.
    uint8_t div = 1;
    while ((p.w / div > kCellW || p.h / div > kCellH) && div < 64) {
        div *= 2;
    }
    const uint16_t dstW = (uint16_t)(p.w / div);
    const uint16_t dstH = (uint16_t)(p.h / div);

    const int16_t x = cellX + (kCellW - dstW) / 2;
    const int16_t y = cellY + (kCellH - dstH) / 2;

    if (div == 1) {
        d.drawXBM(x, y, p.w, p.h, bits);
    } else if (p.w % dstW == 0 && p.h % dstH == 0 &&
               p.w <= 255 && p.h <= 255) {
        const size_t scaledBytes = ((dstW + 7) / 8) * dstH;
        uint8_t* scaled = (uint8_t*)BadgeMemory::allocPreferPsram(scaledBytes);
        if (scaled) {
            ImageScaler::scale(bits, (uint8_t)p.w, (uint8_t)p.h,
                               scaled, (uint8_t)dstW, (uint8_t)dstH);
            d.drawXBM(x, y, dstW, dstH, scaled);
            free(scaled);
        }
    }
    freePreview(p);
}

}  // namespace

void StickerPickerScreen::render(oled& d, GUIManager& /*gui*/) {
    OLEDLayout::drawHeader(d, "STICKER", nullptr);
    d.setFont(UIFonts::kText);

    const uint8_t t = total();
    if (t == 0) {
        d.drawStr(2, 30, "no items");
        OLEDLayout::drawGameFooter(d);
        OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", nullptr);
        return;
    }

    // Header: show selected entry's name on the right of the title.
    char nameBuf[28] = {};
    if (cursor_ < catalogCount_) {
        std::snprintf(nameBuf, sizeof(nameBuf), "%s",
                      kImageCatalog[cursor_].name);
    } else {
        const auto& s = savedEntries_[cursor_ - catalogCount_];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s",
                      s.name[0] ? s.name : "Untitled");
    }
    OLEDLayout::fitText(d, nameBuf, sizeof(nameBuf), 80);
    d.drawStr(56, 6, nameBuf);

    // Grid.
    for (uint8_t row = 0; row < kVisRows; row++) {
        for (uint8_t col = 0; col < kCols; col++) {
            uint8_t idx = (scroll_ + row) * kCols + col;
            if (idx >= t) break;
            const int16_t cellX = col * kCellW;
            const int16_t cellY = kGridTopY + row * kCellH;

            if (idx < catalogCount_) {
                drawCatalogPreview(d, cellX, cellY, kImageCatalog[idx]);
            } else {
                drawSavedPreview(d, cellX, cellY,
                                 savedEntries_[idx - catalogCount_]);
            }

            if (idx == cursor_) {
                // 1px XOR selection border.
                d.setDrawColor(2);
                d.drawHLine(cellX,            cellY,            kCellW);
                d.drawHLine(cellX,            cellY + kCellH-1, kCellW);
                d.drawVLine(cellX,            cellY,            kCellH);
                d.drawVLine(cellX + kCellW-1, cellY,            kCellH);
                d.setDrawColor(1);
            }
        }
    }

    // No drawFooterRule here — the 24-tall grid extends below y=54, and the
    // rule would carve a horizontal line through the bottom row. The action
    // hints render straight at y=63 instead.
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "pick");
}

void StickerPickerScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                      int16_t /*cy*/, GUIManager& gui) {
    const Inputs::ButtonEdges& e = inputs.edges();

    if (e.cancelPressed) {
        gui.popScreen();
        return;
    }

    if (e.confirmPressed) {
        if (cursor_ < catalogCount_) {
            Result r{};
            r.kind = PickKind::Catalog;
            r.catalogIdx = (int16_t)cursor_;
            r.savedAnimId[0] = '\0';
            if (cb_) cb_(r, gui, user_);
        } else {
            const auto& s = savedEntries_[cursor_ - catalogCount_];
            Result r{};
            r.kind = PickKind::SavedAnim;
            r.catalogIdx = -1;
            std::strncpy(r.savedAnimId, s.animId, sizeof(r.savedAnimId) - 1);
            r.savedAnimId[sizeof(r.savedAnimId) - 1] = '\0';
            if (cb_) cb_(r, gui, user_);
        }
        return;
    }

    const uint32_t now = millis();
    const int16_t dy = (int16_t)inputs.joyY() - 2047;
    const int16_t dx = (int16_t)inputs.joyX() - 2047;

    auto repeat = [](int16_t v) -> uint32_t {
        return abs(v) > 1500 ? 80 : (abs(v) > 900 ? 160 : 300);
    };

    if (abs(dy) > (int16_t)kJoyDeadband) {
        if (lastJoyNavMs_ == 0 || now - lastJoyNavMs_ >= repeat(dy)) {
            lastJoyNavMs_ = now;
            moveCursor(dy > 0 ? kCols : -kCols);
            gui.requestRender();
        }
    } else if (abs(dx) > (int16_t)kJoyDeadband) {
        if (lastJoyNavMs_ == 0 || now - lastJoyNavMs_ >= repeat(dx)) {
            lastJoyNavMs_ = now;
            moveCursor(dx > 0 ? 1 : -1);
            gui.requestRender();
        }
    } else {
        lastJoyNavMs_ = 0;
    }
}
