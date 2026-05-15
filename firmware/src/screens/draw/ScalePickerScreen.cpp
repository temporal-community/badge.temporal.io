#include "ScalePickerScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "../../hardware/Inputs.h"
#include "../../hardware/oled.h"
#include "../../ui/GUI.h"
#include "../../ui/Images.h"
#include "../../ui/OLEDLayout.h"
#include "../../ui/UIFonts.h"

ScalePickerScreen sScalePicker;

void ScalePickerScreen::configure(const StickerPickerScreen::Result& src,
                                  Callback cb, void* user) {
    src_  = src;
    cb_   = cb;
    user_ = user;
}

void ScalePickerScreen::onEnter(GUIManager& /*gui*/) {
    rebuildOptions();
    cursor_ = 0;
    lastJoyNavMs_ = 0;
}

void ScalePickerScreen::rebuildOptions() {
    optionCount_ = 0;
    std::memset(options_, 0, sizeof(options_));
    std::memset(optionHeights_, 0, sizeof(optionHeights_));

    if (src_.kind == StickerPickerScreen::PickKind::Catalog) {
        if (src_.catalogIdx < 0 || src_.catalogIdx >= (int16_t)kImageCatalogCount) {
            return;
        }
        const ImageInfo& img = kImageCatalog[src_.catalogIdx];
        optionCount_ = ImageScaler::availableScales(img, options_, kMaxOptions);
        for (uint8_t i = 0; i < optionCount_; i++) optionHeights_[i] = options_[i];
        return;
    }

    // Saved anim — load metadata to get native dimensions.
    std::vector<draw::AnimSummary> all;
    draw::listAll(all);
    for (const auto& s : all) {
        if (std::strcmp(s.animId, src_.savedAnimId) == 0) {
            const uint16_t small = std::min<uint16_t>(s.w, s.h);
            // Add native + halved variants while min axis stays >= 4 px.
            // `scale` is the destination width for saved animations; height
            // follows the document aspect ratio.
            for (uint8_t div = 1; div <= 8 && optionCount_ < kMaxOptions; div *= 2) {
                if (small / div < 4) break;
                if (s.w % div != 0 || s.h % div != 0) continue;
                const uint16_t w = s.w / div;
                const uint16_t h = s.h / div;
                if (w > UINT8_MAX || h > UINT8_MAX) continue;
                options_[optionCount_] = (uint8_t)w;
                optionHeights_[optionCount_] = (uint8_t)h;
                optionCount_++;
            }
            break;
        }
    }
}

void ScalePickerScreen::render(oled& d, GUIManager& /*gui*/) {
    OLEDLayout::drawHeader(d, "SIZE", nullptr);
    d.setFont(UIFonts::kText);

    if (optionCount_ == 0) {
        d.drawStr(2, 30, "no sizes");
        OLEDLayout::drawGameFooter(d);
        OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", nullptr);
        return;
    }

    for (uint8_t i = 0; i < optionCount_; i++) {
        uint8_t y = 9 + i * 9;
        if (i == cursor_) {
            OLEDLayout::drawSelectedRow(d, y, 9);
            d.setDrawColor(0);
        } else {
            d.setDrawColor(1);
        }
        char line[16];
        if (src_.kind == StickerPickerScreen::PickKind::SavedAnim) {
            std::snprintf(line, sizeof(line), "%ux%u",
                          (unsigned)options_[i],
                          (unsigned)optionHeights_[i]);
        } else {
            std::snprintf(line, sizeof(line), "%upx", (unsigned)options_[i]);
        }
        d.drawStr(8, y + d.getAscent() + 1, line);
        d.setDrawColor(1);
    }

    OLEDLayout::drawGameFooter(d);
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "ok");
}

void ScalePickerScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                    int16_t /*cy*/, GUIManager& gui) {
    const Inputs::ButtonEdges& e = inputs.edges();

    if (e.cancelPressed) {
        gui.popScreen();
        return;
    }
    if (e.confirmPressed) {
        if (cursor_ < optionCount_ && cb_) {
            cb_(options_[cursor_], gui, user_);
        }
        return;
    }

    const uint32_t now = millis();
    const int16_t dy = (int16_t)inputs.joyY() - 2047;
    if (abs(dy) > (int16_t)kJoyDeadband) {
        if (lastJoyNavMs_ == 0 || now - lastJoyNavMs_ >= 250) {
            lastJoyNavMs_ = now;
            if (dy > 0 && cursor_ + 1 < optionCount_) { cursor_++; gui.requestRender(); }
            else if (dy < 0 && cursor_ > 0) { cursor_--; gui.requestRender(); }
        }
    } else {
        lastJoyNavMs_ = 0;
    }
}
