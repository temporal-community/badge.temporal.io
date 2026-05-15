#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace BadgeInfo {

struct Fields {
    char ticketUuid[37];
    char name[64];
    char title[64];
    char company[64];
    char attendeeType[32];

    char email[64];
    char website[80];
    char phone[24];
    char bio[128];
    char note[128];
};

static constexpr const char* kInfoPath = "/badgeInfo.json";
static constexpr const char* kLegacyInfoPath = "/badgeInfo.txt";

bool loadFromFile(Fields& out);
bool saveToFile(const Fields& f);
bool clear();
void populateDefaults(Fields& out, const uint8_t* uidBytes);

void applyToGlobals(const Fields& f);
void getCurrent(Fields& out);

}  // namespace BadgeInfo
