#include "ContactsScreen.h"

#include <cstdio>
#include <cstring>

#include "../identity/BadgeInfo.h"
#include "../infra/PsramAllocator.h"
#include "../ui/GUI.h"

ContactsScreen sContacts;

namespace {

// listActivePeers walks /boops.json and invokes our callback for each
// non-revoked pairing. We accumulate up to kMaxContacts entries; once
// full the callback returns false to stop the walk.
struct CollectCtx {
    BadgeBoops::PeerEntry* slots;
    uint8_t cap;
    uint8_t count;
};

bool collectCb(const BadgeBoops::PeerEntry& entry, void* user) {
    auto* ctx = static_cast<CollectCtx*>(user);
    if (ctx->count >= ctx->cap) return false;
    ctx->slots[ctx->count++] = entry;
    return true;
}

}  // namespace

ContactsScreen::ContactsScreen()
    : ListMenuScreen(kScreenContacts, "CONTACTS") {}

void ContactsScreen::onEnter(GUIManager& gui) {
    ListMenuScreen::onEnter(gui);
    reload();
}

void ContactsScreen::onResume(GUIManager& /*gui*/) {
    // Re-read /boops.json after the detail screen pops in case anything
    // mutated it (future delete affordance, etc.).
    reload();
}

void ContactsScreen::ensureEntries() {
    if (entries_) return;
    entries_ = static_cast<BadgeBoops::PeerEntry*>(
        BadgeMemory::allocPreferPsram(kMaxContacts * sizeof(BadgeBoops::PeerEntry)));
    if (entries_) std::memset(entries_, 0, kMaxContacts * sizeof(BadgeBoops::PeerEntry));
}

void ContactsScreen::reload() {
    entryCount_ = 0;
    ensureEntries();
    if (!entries_) return;
    BadgeInfo::Fields me;
    BadgeInfo::getCurrent(me);

    CollectCtx ctx{entries_, kMaxContacts, 0};
    BadgeBoops::listActivePeers(me.ticketUuid, &collectCb, &ctx);
    entryCount_ = ctx.count;
}

void ContactsScreen::formatItem(uint8_t index, char* buf,
                                uint8_t bufSize) const {
    if (index >= entryCount_) {
        if (bufSize > 0) buf[0] = '\0';
        return;
    }
    const BadgeBoops::PeerEntry& e = entries_[index];
    const char* name = e.name[0] ? e.name : e.peerUid;
    if (e.company[0]) {
        std::snprintf(buf, bufSize, "%s \xb7 %s", name, e.company);
    } else {
        std::snprintf(buf, bufSize, "%s", name);
    }
}

const BadgeBoops::PeerEntry* ContactsScreen::entryAt(uint8_t index) const {
    if (!entries_ || index >= entryCount_) return nullptr;
    return &entries_[index];
}

void ContactsScreen::onItemSelect(uint8_t index, GUIManager& gui) {
    if (index >= entryCount_) return;
    lastSelected_ = index;
    gui.pushScreen(kScreenContactDetail);
}
