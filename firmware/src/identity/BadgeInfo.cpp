#include "BadgeInfo.h"

#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <cstring>

extern char badgeName[];
extern char badgeTitle[];
extern char badgeCompany[];
extern char badgeAtType[];

namespace {

static const char* TAG = "BadgeInfo";
static constexpr size_t kInfoMaxBytes = 2048;
static constexpr const char* kDefaultNote =
    "Edit this file over USB. See https://badge.temporal.io for update instructions.";

// NVS storage layout: namespace=badge_info, key=json holds a UTF-8
// JSON blob whose schema matches the historical /badgeInfo.json file.
// Migrating BadgeInfo into NVS lets the FATFS partition be reformatted
// (e.g. by a fatfs.bin re-flash from flash_loop_gui2 / Ignition)
// without losing the operator's name/title/contact info — see
// firmware/docs/STORAGE-MODEL.md for the full survival matrix.
static constexpr const char* kNvsNamespace = "badge_info";
static constexpr const char* kNvsKey = "json";

char s_email[64] = "";
char s_website[80] = "";
char s_phone[24] = "";
char s_bio[128] = "";
char s_note[128] = "";
char s_ticketUuid[37] = "";

void safeCopy(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

void ensureNote(BadgeInfo::Fields& f) {
    if (f.note[0] == '\0') safeCopy(f.note, sizeof(f.note), kDefaultNote);
}

bool parseJsonInto(BadgeInfo::Fields& out, const char* buf, size_t len) {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, buf, len);
    if (err) {
        Serial.printf("[%s] JSON parse failed: %s\n", TAG, err.c_str());
        return false;
    }

    safeCopy(out.ticketUuid, sizeof(out.ticketUuid), doc["ticket_uuid"] | "");
    safeCopy(out.name, sizeof(out.name), doc["name"] | "");
    safeCopy(out.title, sizeof(out.title), doc["title"] | "");
    safeCopy(out.company, sizeof(out.company), doc["company"] | "");
    safeCopy(out.attendeeType, sizeof(out.attendeeType),
             doc["attendee_type"] | doc["type"] | "");

    JsonObject contact = doc["contact"];
    safeCopy(out.email, sizeof(out.email),
             contact["email"] | doc["email"] | "");
    safeCopy(out.website, sizeof(out.website),
             contact["website"] | doc["website"] | "");
    safeCopy(out.phone, sizeof(out.phone),
             contact["phone"] | doc["phone"] | "");
    safeCopy(out.bio, sizeof(out.bio),
             contact["bio"] | doc["bio"] | "");
    safeCopy(out.note, sizeof(out.note),
             doc["note"] | doc["instructions"] | "");
    ensureNote(out);
    return true;
}

size_t serializeFields(const BadgeInfo::Fields& f, char* buf, size_t cap) {
    BadgeInfo::Fields copy = f;
    ensureNote(copy);

    StaticJsonDocument<1024> doc;
    doc["name"] = copy.name;
    doc["title"] = copy.title;
    doc["company"] = copy.company;
    doc["attendee_type"] = copy.attendeeType;
    doc["ticket_uuid"] = copy.ticketUuid;
    doc["note"] = copy.note;

    JsonObject contact = doc.createNestedObject("contact");
    contact["email"] = copy.email;
    contact["website"] = copy.website;
    contact["phone"] = copy.phone;
    contact["bio"] = copy.bio;

    return serializeJsonPretty(doc, buf, cap);
}

bool loadFromNvs(BadgeInfo::Fields& out) {
    Preferences p;
    if (!p.begin(kNvsNamespace, true)) return false;
    if (!p.isKey(kNvsKey)) {
        p.end();
        return false;
    }
    size_t len = p.getBytesLength(kNvsKey);
    if (len == 0 || len > kInfoMaxBytes) {
        p.end();
        return false;
    }
    char* buf = static_cast<char*>(BadgeMemory::allocPreferPsram(len + 1));
    if (!buf) {
        p.end();
        return false;
    }
    size_t got = p.getBytes(kNvsKey, buf, len);
    p.end();
    if (got != len) {
        free(buf);
        return false;
    }
    buf[len] = '\0';
    bool ok = parseJsonInto(out, buf, len);
    free(buf);
    if (ok) {
        Serial.printf("[%s] loaded from NVS (%u bytes)\n", TAG,
                      static_cast<unsigned>(len));
    }
    return ok;
}

bool saveToNvs(const BadgeInfo::Fields& f) {
    char buf[kInfoMaxBytes];
    size_t len = serializeFields(f, buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        Serial.printf("[%s] save skipped: JSON buffer too small\n", TAG);
        return false;
    }
    Preferences p;
    if (!p.begin(kNvsNamespace, false)) {
        Serial.printf("[%s] NVS open failed\n", TAG);
        return false;
    }
    size_t wrote = p.putBytes(kNvsKey, buf, len);
    p.end();
    Serial.printf("[%s] %s NVS (%u bytes)\n", TAG,
                  (wrote == len) ? "saved" : "FAILED save",
                  static_cast<unsigned>(len));
    return wrote == len;
}

}  // namespace

namespace BadgeInfo {

void populateDefaults(Fields& out, const uint8_t* /*uidBytes*/) {
    memset(&out, 0, sizeof(out));
    safeCopy(out.name, sizeof(out.name), "Ziggy");
    safeCopy(out.title, sizeof(out.title), "Chief Tartigrade");
    safeCopy(out.company, sizeof(out.company), "Temporal");
    safeCopy(out.attendeeType, sizeof(out.attendeeType), "Dev");
    safeCopy(out.note, sizeof(out.note), kDefaultNote);

    Serial.printf("[%s] default identity: %s / %s / %s\n",
                  TAG, out.name, out.title, out.company);
}

bool loadFromFile(Fields& out) {
    // Despite the name (kept for source-compat with all the old call
    // sites), the canonical store is now NVS. We fall back to the old
    // /badgeInfo.json file for one boot to migrate any existing badge,
    // then nuke both legacy files so a future fatfs.bin reflash can't
    // resurrect stale data.
    memset(&out, 0, sizeof(out));

    if (loadFromNvs(out)) {
        // Drop legacy files defensively in case the user re-uploaded
        // an old image and we just promoted NVS to canonical.
        Filesystem::removeFile(kLegacyInfoPath);
        Filesystem::removeFile(kInfoPath);
        return true;
    }

    // Migration path: read the old file once, mirror it into NVS,
    // then delete both file copies.
    Filesystem::removeFile(kLegacyInfoPath);
    char* buf = nullptr;
    size_t len = 0;
    if (!Filesystem::readFileAlloc(kInfoPath, &buf, &len, kInfoMaxBytes)) {
        return false;
    }
    bool parsed = parseJsonInto(out, buf, len);
    free(buf);
    if (!parsed) {
        Filesystem::removeFile(kInfoPath);
        return false;
    }

    Serial.printf("[%s] migrating %s -> NVS\n", TAG, kInfoPath);
    saveToNvs(out);
    Filesystem::removeFile(kInfoPath);

    const bool matched =
        out.name[0] || out.title[0] || out.company[0] || out.email[0] ||
        out.website[0] || out.phone[0] || out.bio[0];
    return matched;
}

bool saveToFile(const Fields& f) {
    // Source-compat name; goes to NVS. Belt-and-braces remove any
    // straggling legacy file so callers don't see a half-migrated state.
    Filesystem::removeFile(kLegacyInfoPath);
    Filesystem::removeFile(kInfoPath);
    return saveToNvs(f);
}

bool clear() {
    Fields f;
    populateDefaults(f, nullptr);
    safeCopy(f.email, sizeof(f.email), s_email);
    safeCopy(f.website, sizeof(f.website), s_website);
    safeCopy(f.phone, sizeof(f.phone), s_phone);
    safeCopy(f.bio, sizeof(f.bio), s_bio);
    return saveToFile(f);
}

void applyToGlobals(const Fields& f) {
    safeCopy(badgeName, 64, f.name);
    safeCopy(badgeTitle, 64, f.title);
    safeCopy(badgeCompany, 64, f.company);
    safeCopy(badgeAtType, 32, f.attendeeType);

    safeCopy(s_ticketUuid, sizeof(s_ticketUuid), f.ticketUuid);
    safeCopy(s_email, sizeof(s_email), f.email);
    safeCopy(s_website, sizeof(s_website), f.website);
    safeCopy(s_phone, sizeof(s_phone), f.phone);
    safeCopy(s_bio, sizeof(s_bio), f.bio);
    safeCopy(s_note, sizeof(s_note), f.note);
}

void getCurrent(Fields& out) {
    memset(&out, 0, sizeof(out));
    safeCopy(out.ticketUuid, sizeof(out.ticketUuid), s_ticketUuid);
    safeCopy(out.name, sizeof(out.name), badgeName);
    safeCopy(out.title, sizeof(out.title), badgeTitle);
    safeCopy(out.company, sizeof(out.company), badgeCompany);
    safeCopy(out.attendeeType, sizeof(out.attendeeType), badgeAtType);
    safeCopy(out.email, sizeof(out.email), s_email);
    safeCopy(out.website, sizeof(out.website), s_website);
    safeCopy(out.phone, sizeof(out.phone), s_phone);
    safeCopy(out.bio, sizeof(out.bio), s_bio);
    safeCopy(out.note, sizeof(out.note), s_note);
    ensureNote(out);
}

}  // namespace BadgeInfo
