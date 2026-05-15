#include "ContactDetailScreen.h"

#include <cstdio>
#include <cstring>

#include "../ui/GUI.h"
#include "ContactsScreen.h"

ContactDetailScreen sContactDetail;

namespace {

// Mirror of the BadgeInfo edit-screen row table, but pointed at
// ContactDetail and with last_seen / boop_count appended.
struct DetailRow {
    const char* label;
    enum Source : uint8_t {
        kName, kTitle, kCompany, kType, kEmail, kWeb, kPhone, kBio,
        kUid, kLast, kCount,
    } source;
};

constexpr DetailRow kRows[] = {
    {"Name    ",    DetailRow::kName},
    {"Title   ",   DetailRow::kTitle},
    {"Company ", DetailRow::kCompany},
    {"Type    ",    DetailRow::kType},
    {"Email   ",   DetailRow::kEmail},
    {"Web     ",     DetailRow::kWeb},
    {"Phone   ",   DetailRow::kPhone},
    {"Bio     ",     DetailRow::kBio},
    {"UID     ",     DetailRow::kUid},
    {"Seen    ",    DetailRow::kLast},
    {"Boops  ",   DetailRow::kCount},
};

constexpr uint8_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

const char* readSource(const BadgeBoops::ContactDetail& d, DetailRow::Source s,
                       char* numScratch, size_t scratchCap) {
    switch (s) {
        case DetailRow::kName:    return d.name;
        case DetailRow::kTitle:   return d.title;
        case DetailRow::kCompany: return d.company;
        case DetailRow::kType:    return d.attendeeType;
        case DetailRow::kEmail:   return d.email;
        case DetailRow::kWeb:     return d.website;
        case DetailRow::kPhone:   return d.phone;
        case DetailRow::kBio:     return d.bio;
        case DetailRow::kUid:     return d.peerUid;
        case DetailRow::kLast:    return d.lastTs;
        case DetailRow::kCount:
            std::snprintf(numScratch, scratchCap, "%d", d.boopCount);
            return numScratch;
    }
    return "";
}

}  // namespace

ContactDetailScreen::ContactDetailScreen()
    : ListMenuScreen(kScreenContactDetail, "CONTACT") {}

void ContactDetailScreen::onEnter(GUIManager& gui) {
    ListMenuScreen::onEnter(gui);
    loaded_ = false;
    std::memset(&detail_, 0, sizeof(detail_));

    const BadgeBoops::PeerEntry* selected =
        sContacts.entryAt(sContacts.selectedIndex());
    if (!selected) return;

    loaded_ = BadgeBoops::lookupContactByUid(selected->peerUid, detail_);
    // Fall back to the picker entry if the journal lookup somehow fails
    // — at least the name + UID render so the screen isn't blank.
    if (!loaded_) {
        std::strncpy(detail_.peerUid, selected->peerUid,
                     sizeof(detail_.peerUid) - 1);
        std::strncpy(detail_.name, selected->name,
                     sizeof(detail_.name) - 1);
        std::strncpy(detail_.company, selected->company,
                     sizeof(detail_.company) - 1);
        std::strncpy(detail_.lastTs, selected->lastTs,
                     sizeof(detail_.lastTs) - 1);
        detail_.boopCount = selected->boopCount;
    }
}

uint8_t ContactDetailScreen::itemCount() const { return kRowCount; }

void ContactDetailScreen::onItemSelect(uint8_t /*index*/, GUIManager& gui) {
    gui.popScreen();
}

void ContactDetailScreen::formatItem(uint8_t index, char* buf,
                                     uint8_t bufSize) const {
    if (index >= kRowCount) {
        if (bufSize > 0) buf[0] = '\0';
        return;
    }
    char numScratch[12] = {};
    const char* val = readSource(detail_, kRows[index].source,
                                 numScratch, sizeof(numScratch));
    if (!val || val[0] == '\0') val = "\xAD";
    std::snprintf(buf, bufSize, "%-7s%s", kRows[index].label, val);
}
