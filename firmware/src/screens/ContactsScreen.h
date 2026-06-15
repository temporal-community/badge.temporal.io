#pragma once
#include "Screen.h"

#include "../boops/BadgeBoops.h"

// ─── Contacts list ─────────────────────────────────────────────────────────
// Browses peers from /boops.json (populated by IR contact exchange during
// a peer boop). Selecting a row pushes ContactDetailScreen for that peer.

class ContactsScreen : public ListMenuScreen {
 public:
  ContactsScreen();

  void onEnter(GUIManager& gui) override;
  void onResume(GUIManager& gui) override;

  uint8_t itemCount() const override { return entryCount_; }
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  bool navigableItems() const override { return true; }
  const char* hintText() const override { return "View:set Cancel:back"; }
  void onItemSelect(uint8_t index, GUIManager& gui) override;

  // Index of the row most recently confirmed; ContactDetailScreen reads
  // this to know which peer to render.
  uint8_t selectedIndex() const { return lastSelected_; }
  const BadgeBoops::PeerEntry* entryAt(uint8_t index) const;

 private:
  static constexpr uint8_t kMaxContacts = BadgeBoops::kMaxContacts;
  BadgeBoops::PeerEntry* entries_ = nullptr;
  uint8_t entryCount_ = 0;
  uint8_t lastSelected_ = 0;

  void ensureEntries();
  void reload();
};

extern ContactsScreen sContacts;
