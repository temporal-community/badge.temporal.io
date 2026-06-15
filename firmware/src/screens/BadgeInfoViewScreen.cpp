#include "BadgeInfoViewScreen.h"

#include <cstdio>
#include <cstring>

#include <U8g2lib.h>

#include "../hardware/oled.h"
#include "../identity/BadgeInfo.h"
#include "../ui/GUI.h"
#include "../ui/UIFonts.h"
#include "TextInputScreen.h"

extern TextInputScreen sTextInput;

namespace {

struct FieldSpec {
    const char* label;
    size_t      offset;   // offset into BadgeInfo::Fields
    size_t      cap;      // sizeof(member)
    bool        editable; // ticket_uuid + note are read-only
};

#define FS_ROW(LABEL, MEMBER, EDIT)                                         \
    { LABEL,                                                                \
      offsetof(BadgeInfo::Fields, MEMBER),                                  \
      sizeof(((BadgeInfo::Fields*)0)->MEMBER),                              \
      EDIT }

constexpr FieldSpec kRows[] = {
    FS_ROW("Name",    name,         true),
    FS_ROW("Title",   title,        true),
    FS_ROW("Company", company,      true),
    FS_ROW("Type",    attendeeType, true),
    FS_ROW("Email",   email,        true),
    FS_ROW("Web",     website,      true),
    FS_ROW("Phone",   phone,        true),
    FS_ROW("Bio",     bio,          true),
    FS_ROW("UUID",    ticketUuid,   false),
    FS_ROW("",        note,         false),
};
#undef FS_ROW

constexpr uint8_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

// Column geometry. Tiny 4x6 font puts the longest label ("Company", 7 chars)
// at 28 px wide, so a value column starting at x=32 leaves a 1-px gutter and
// keeps every editable field's value aligned to the same left edge.
// kValueRightX matches the row-content right edge from Screen.cpp's render
// (scrollbar at x=124, with a 1-px gap = 122 usable, less 2 px of breathing
// room so descenders never crowd the highlight box).
constexpr uint8_t kLabelX      = 3;
constexpr uint8_t kValueX      = 32;
constexpr uint8_t kValueRightX = 120;

const char* readField(const BadgeInfo::Fields& f, uint8_t index) {
    if (index >= kRowCount) return "";
    return reinterpret_cast<const char*>(&f) + kRows[index].offset;
}

char* writeField(BadgeInfo::Fields& f, uint8_t index) {
    if (index >= kRowCount) return nullptr;
    return reinterpret_cast<char*>(&f) + kRows[index].offset;
}

void onEditDone(const char* /*text*/, void* user) {
    auto* self = static_cast<BadgeInfoViewScreen*>(user);
    if (self) self->onEditSubmit();
}

}  // namespace

BadgeInfoViewScreen::BadgeInfoViewScreen()
    : ListMenuScreen(kScreenBadgeInfo, "BADGE INFO") {}

uint8_t BadgeInfoViewScreen::itemCount() const { return kRowCount; }

void BadgeInfoViewScreen::formatItem(uint8_t index, char* buf,
                                     uint8_t bufSize) const {
    BadgeInfo::Fields f;
    BadgeInfo::getCurrent(f);
    const char* val = readField(f, index);
    const char* label = (index < kRowCount) ? kRows[index].label : "";
    if (val[0] == '\0') val = "\xAD";
    std::snprintf(buf, bufSize, "%-7s%s", label, val);
}

void BadgeInfoViewScreen::drawItem(oled& d, uint8_t index, uint8_t y,
                                   uint8_t /*rowHeight*/,
                                   bool /*selected*/) const {
    if (index >= kRowCount) return;

    BadgeInfo::Fields f;
    BadgeInfo::getCurrent(f);
    const char* val = readField(f, index);
    if (val[0] == '\0') val = "\xAD";
    const char* label = kRows[index].label;

    // Establish the shared baseline using the regular text font (the
    // base render pre-set this font, but read it explicitly so the
    // baseline math works regardless of caller state).
    d.setFont(UIFonts::kText);
    const int baseline = y + d.getAscent() + 1;

    char tmp[64];
    std::strncpy(tmp, val, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    while (tmp[0] && d.getStrWidth(tmp) > (kValueRightX - kValueX)) {
        tmp[std::strlen(tmp) - 1] = '\0';
    }
    d.drawStr(kValueX, baseline, tmp);

    if (label[0]) {
        d.setFont(u8g2_font_4x6_tr);
        d.drawStr(kLabelX, baseline, label);
        d.setFont(UIFonts::kText);
    }
}

void BadgeInfoViewScreen::onItemSelect(uint8_t index, GUIManager& gui) {
    if (index >= kRowCount) return;
    if (!kRows[index].editable) return;

    BadgeInfo::Fields f;
    BadgeInfo::getCurrent(f);
    const char* current = readField(f, index);

    editIndex_ = index;
    // Cap copy at the smaller of the field cap and our scratch buffer
    // — bio is 128 bytes, all others are smaller.
    const size_t cap = kRows[index].cap < sizeof(editBuf_)
                           ? kRows[index].cap
                           : sizeof(editBuf_);
    std::strncpy(editBuf_, current, cap - 1);
    editBuf_[cap - 1] = '\0';

    sTextInput.configure(kRows[index].label, editBuf_,
                         static_cast<uint16_t>(cap),
                         &onEditDone, this);
    gui.pushScreen(kScreenTextInput);
}

void BadgeInfoViewScreen::onEditSubmit() {
    if (editIndex_ >= kRowCount) return;
    if (!kRows[editIndex_].editable) return;

    BadgeInfo::Fields f;
    BadgeInfo::getCurrent(f);
    char* dst = writeField(f, editIndex_);
    if (!dst) return;

    const size_t cap = kRows[editIndex_].cap;
    std::strncpy(dst, editBuf_, cap - 1);
    dst[cap - 1] = '\0';

    BadgeInfo::saveToFile(f);
    BadgeInfo::applyToGlobals(f);
}
