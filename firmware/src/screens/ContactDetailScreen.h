#pragma once
#include "Screen.h"

#include "../boops/BadgeBoops.h"

// ─── Contact detail ────────────────────────────────────────────────────────
// Read-only scrolling list of one peer's exchanged fields, fetched from
// /boops.json by ContactsScreen via lookupContactByUid().

class ContactDetailScreen : public ListMenuScreen {
 public:
  ContactDetailScreen();

  void onEnter(GUIManager& gui) override;

  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  bool navigableItems() const override { return true; }
  const char* hintText() const override { return "Cancel:back"; }
  // Rows are inert read-outs; confirm pops back like cancel.
  void onItemSelect(uint8_t index, GUIManager& gui) override;

 private:
  BadgeBoops::ContactDetail detail_ = {};
  bool loaded_ = false;
};

extern ContactDetailScreen sContactDetail;
