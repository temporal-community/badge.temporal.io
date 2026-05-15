// BadgeGlobals.cpp — Global state variables shared across badge modules.

#include "BadgeGlobals.h"

#include "infra/BadgeConfig.h"

#include <Arduino.h>
#include <cstring>
#include <new>

BadgeState badgeState = BADGE_UNPAIRED;

const char* badgeStateName(BadgeState s) {
  switch (s) {
    case BADGE_UNPAIRED: return "unpaired";
    case BADGE_PAIRED:   return "paired";
    case BADGE_OFFLINE:  return "offline";
  }
  return "?";
}

// Online/offline transitions are kept for legacy callers; the offline
// firmware boots directly into the activated local UI.
void setBadgeOnline(bool online) {
  const BadgeState prev = badgeState;
  if (prev == BADGE_UNPAIRED) return;
  const BadgeState next = online ? BADGE_PAIRED : BADGE_OFFLINE;
  if (next == prev) return;
  badgeState = next;
  Serial.printf("[BadgeState] %s -> %s\n",
                badgeStateName(prev), badgeStateName(next));
}
volatile int assignedRole = ROLE_NUM_DEITY;

uint8_t* qrBits = nullptr;
int qrByteCount = 0;
int qrWidth = 0;
int qrHeight = 0;

uint8_t* badgeBits = nullptr;
int badgeByteCount = 0;
bool gCustomNametagEnabled = false;

// Live nametag: animId of the chosen drawing + its loaded doc.
// gNametagDoc is heap-allocated; call draw::freeAll + delete before reassigning.
char gNametagAnimId[draw::kAnimIdLen + 1] = {};
draw::AnimDoc* gNametagDoc = nullptr;

char badgeName[64] = "YOUR_NAME";
char badgeTitle[64] = "YOUR_TITLE";
char badgeCompany[64] = "YOUR_COMPANY_NAME";
char badgeAtType[32] = "YOUR_ATTENDEE_TYPE"; // Attendee, Staff, Vendor, Speaker, Deity

volatile bool pythonMenuRequested = false;

void unloadNametagAnimationDoc() {
  if (!gNametagDoc) return;
  draw::freeAll(*gNametagDoc);
  delete gNametagDoc;
  gNametagDoc = nullptr;
  Serial.println("[Nametag] unloaded custom animation doc");
}

void adoptNametagAnimationDoc(const char* animId, draw::AnimDoc* doc) {
  unloadNametagAnimationDoc();

  if (!doc || !animId || std::strlen(animId) != draw::kAnimIdLen) {
    if (doc) {
      draw::freeAll(*doc);
      delete doc;
    }
    gCustomNametagEnabled = false;
    gNametagAnimId[0] = '\0';
    return;
  }

  gNametagDoc = doc;
  std::memcpy(gNametagAnimId, animId, draw::kAnimIdLen);
  gNametagAnimId[draw::kAnimIdLen] = '\0';
  gCustomNametagEnabled = true;
}

bool ensureNametagAnimationDocLoaded() {
  if (gNametagDoc) return true;

  char animId[draw::kAnimIdLen + 1] = {};
  if (std::strlen(gNametagAnimId) == draw::kAnimIdLen) {
    std::memcpy(animId, gNametagAnimId, draw::kAnimIdLen);
    animId[draw::kAnimIdLen] = '\0';
  } else {
    const draw::NametagSettingParse parse =
        draw::parseNametagSetting(badgeConfig.nametagSetting(), animId);
    if (parse != draw::NametagSettingParse::AnimId) return false;
  }

  auto* doc = new (std::nothrow) draw::AnimDoc();
  if (!doc) {
    Serial.println("[Nametag] failed to allocate custom animation doc");
    return false;
  }

  if (draw::load(animId, *doc) && !doc->frames.empty()) {
    adoptNametagAnimationDoc(animId, doc);
    return true;
  }

  draw::freeAll(*doc);
  delete doc;
  Serial.printf("[Nametag] settings anim '%s' missing — disabling custom\n",
                animId);
  gCustomNametagEnabled = false;
  gNametagAnimId[0] = '\0';
  return false;
}
