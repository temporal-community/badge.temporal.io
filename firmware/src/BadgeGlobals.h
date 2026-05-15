#pragma once

#include "screens/draw/AnimDoc.h"
#include "ui/BadgeDisplay.h"

extern BadgeState badgeState;

extern uint8_t* qrBits;
extern int qrByteCount;
extern int qrWidth;
extern int qrHeight;

extern uint8_t* badgeBits;
extern int badgeByteCount;

extern bool gCustomNametagEnabled;
extern char gNametagAnimId[];
extern draw::AnimDoc* gNametagDoc;

extern char badgeName[];
extern char badgeTitle[];
extern char badgeCompany[];
extern char badgeAtType[];

void unloadNametagAnimationDoc();
bool ensureNametagAnimationDocLoaded();
void adoptNametagAnimationDoc(const char* animId, draw::AnimDoc* doc);
