// BoopsJournal.cpp — /boops.json persistence for local IR boops.
//
// Owns s_doc, s_mutex, BoopsLock. Public surface (recordBoop, recordBoopEx,
// updatePartner*, readJson, count, begin) is declared in BadgeBoops.h and
// called by Core 1 UI and Core 0 irTask state machine. All public
// entrypoints acquire BoopsLock; the lock is a FreeRTOS mutex (not a
// portMUX spinlock) because the FAT writes inside take tens-to-hundreds
// of ms and would otherwise trip the interrupt watchdog.

#include "BadgeBoops.h"

#include "../identity/BadgeUID.h"
#include "../infra/DebugLog.h"
#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

static const char* TAG = "BadgeBoops";

// ═══════════════════════════════════════════════════════════════════════════
// 1) Journal persistence (/boops.json)
// ═══════════════════════════════════════════════════════════════════════════

static const char* kBoopsPath = "/boops.json";

// Journal sizing — each record serializes to roughly 300-450 bytes
// depending on which partner fields (name/company/bio/…) the peer
// populated. No per-visit history is kept; subsequent boops with the
// same badge UID pair overwrite `last_seen` and bump `boop_count`.
// 16 KB is plenty for kMaxBoopRecords=30 records at worst case, and
// the serialized output stays well under the 32 KB loadFromDisk cap.
static constexpr int    kMaxBoopRecords    = 30;
static constexpr size_t kDocCapacity       = 16 * 1024;
static constexpr size_t kLoadMax           = 32 * 1024;
static constexpr size_t kSaveMax           = 32000;

static BadgeMemory::PsramJsonDocument* s_doc = nullptr;

// FreeRTOS mutex — *not* a portMUX spinlock.  We call this while holding
// writes to /boops.json (FAT + SPI flash, tens-to-hundreds of ms), which
// a portMUX would pin against the current core with interrupts disabled
// and trip the interrupt watchdog.  A real mutex just suspends the other
// task until the lock is released, with interrupts fully alive.
static SemaphoreHandle_t s_mutex = nullptr;

namespace {
struct BoopsLock {
    bool held;
    BoopsLock() : held(false) {
        if (s_mutex) {
            held = (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE);
        }
    }
    ~BoopsLock() { if (held) xSemaphoreGive(s_mutex); }
    BoopsLock(const BoopsLock&) = delete;
    BoopsLock& operator=(const BoopsLock&) = delete;
};
}  // anonymous namespace

static void isoTimestamp(char* buf, size_t len) {
    time_t now = time(nullptr);
    if (now < 1700000000) {
        buf[0] = '\0';
        return;
    }
    struct tm t;
    gmtime_r(&now, &t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &t);
}

static void sortedPair(const char* a, const char* b,
                       const char** lo, const char** hi) {
    if (strcmp(a, b) <= 0) { *lo = a; *hi = b; }
    else                   { *lo = b; *hi = a; }
}

// ─── ArduinoJson string-copy helpers ───────────────────────────────────────
// ArduinoJson v6's `const char*` overload links by pointer (no copy);
// the `char*` overload duplicates strlen() bytes into the doc.  Almost
// every value we store comes from memory that won't outlive the next
// serialize (stack-local BoopResult, volatile boopStatus.peer* slots,
// etc.) so we ALWAYS route through these helpers.  Touching the doc
// directly with a const char* will reintroduce the dangling-pointer
// corruption documented in the journal-recovery code.
template <typename T>
static inline void jsonSetCopy(T target, const char* s) {
    target = const_cast<char*>(s ? s : "");
}

static inline void jsonAddCopy(JsonArray a, const char* s) {
    a.add(const_cast<char*>(s ? s : ""));
}

static void* allocPreferPsram(size_t bytes) {
    return BadgeMemory::allocPreferPsram(bytes);
}

static bool compactDoc(const char* reason) {
    if (!s_doc) return false;
    const size_t before = s_doc->memoryUsage();
    if (!s_doc->garbageCollect()) {
        Serial.printf("[%s] json gc failed during %s (used=%u cap=%u)\n",
                      TAG, reason ? reason : "save", (unsigned)before,
                      (unsigned)s_doc->capacity());
        return false;
    }
    const size_t after = s_doc->memoryUsage();
    if (before > after + 128) {
        Serial.printf("[%s] json gc %s reclaimed %u B\n",
                      TAG, reason ? reason : "save",
                      (unsigned)(before - after));
    }
    return true;
}

static bool loadFromDisk() {
    char* buf = nullptr;
    size_t blen = 0;
    if (!Filesystem::readFileAlloc(kBoopsPath, &buf, &blen, kLoadMax)) return false;
    if (blen == 0) { free(buf); return false; }

    DeserializationError err = deserializeJson(*s_doc, (const char*)buf, blen);
    free(buf);

    if (err) {
        // File is corrupt — unlink so stale clusters don't stick around
        // and choke the next save with FAT-full.
        Serial.printf("[%s] parse error: %s, unlinking /boops.json\n",
                      TAG, err.c_str());
        s_doc->clear();
        Filesystem::removeFile(kBoopsPath);
        return false;
    }
    return true;
}

static bool saveToDisk() {
    compactDoc("save");

    if (s_doc->overflowed()) {
        Serial.printf("[%s] save skipped: doc overflowed (cap=%u used=%u)\n",
                      TAG, (unsigned)kDocCapacity,
                      (unsigned)s_doc->memoryUsage());
        return false;
    }

    const size_t jsonLen = measureJson(*s_doc);
    if (jsonLen > kSaveMax) {
        Serial.printf("[%s] save skipped: json too large (%u B)\n",
                      TAG, (unsigned)jsonLen);
        return false;
    }

    char* buf = (char*)allocPreferPsram(jsonLen + 1);
    if (!buf) {
        Serial.printf("[%s] save: alloc %u failed\n", TAG, (unsigned)(jsonLen + 1));
        return false;
    }
    serializeJson(*s_doc, buf, jsonLen + 1);

    const bool ok = Filesystem::writeFileAtomic(kBoopsPath, buf, jsonLen);
    free(buf);
    if (!ok) {
        Serial.printf("[%s] writeFileAtomic failed (len=%u)\n",
                      TAG, (unsigned)jsonLen);
    }
    return ok;
}

// Find the existing /boops.json row whose `badge_uuids` is exactly
// (lo, hi) (already sorted by caller via sortedPair).  Returns a null
// JsonObject when no match exists.  Caller holds BoopsLock.
static JsonObject findPairing(JsonArray pairings,
                              const char* lo, const char* hi) {
    for (JsonObject p : pairings) {
        JsonArray buids = p["badge_uuids"];
        if (buids.size() != 2) continue;
        const char* a = buids[0] | "";
        const char* b = buids[1] | "";
        if (strcmp(a, lo) == 0 && strcmp(b, hi) == 0) return p;
    }
    return JsonObject();
}

static bool nonEmpty(const char* s) {
    return s && s[0] != '\0';
}

static bool looksCanonicalUuid(const char* s) {
    return s && strlen(s) == BADGE_UUID_LEN &&
           s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
}

static bool hasFullTicketSlots(JsonObject p) {
    JsonArray tids = p["ticket_uuids"];
    if (tids.isNull() || tids.size() < 2) return false;
    return nonEmpty(tids[0] | "") && nonEmpty(tids[1] | "");
}

static bool hasCanonicalBadgeSlots(JsonObject p) {
    JsonArray buids = p["badge_uuids"];
    if (buids.isNull() || buids.size() < 2) return false;
    return looksCanonicalUuid(buids[0] | "") && looksCanonicalUuid(buids[1] | "");
}

static int serverTruthScore(JsonObject p) {
    int score = 0;
    if (hasFullTicketSlots(p)) score += 1000;
    if (hasCanonicalBadgeSlots(p)) score += 500;
    const char* status = p["status"] | "";
    if (strcmp(status, "remote") == 0) score += 50;
    if (strcmp(status, "confirmed") == 0) score += 25;
    if (nonEmpty(p["partner_name"] | "")) score += 10;
    if (nonEmpty(p["partner_company"] | "")) score += 5;
    return score;
}

static void copyStringIfBlank(JsonObject dst, JsonObject src, const char* key) {
    const char* dstVal = dst[key] | "";
    const char* srcVal = src[key] | "";
    if (!nonEmpty(dstVal) && nonEmpty(srcVal)) {
        jsonSetCopy(dst[key], srcVal);
    }
}

static void copyArrayBlanks(JsonObject dst, JsonObject src, const char* key) {
    JsonArray srcArr = src[key];
    if (srcArr.isNull() || srcArr.size() == 0) return;

    JsonArray dstArr = dst[key];
    if (dstArr.isNull() || dstArr.size() < srcArr.size()) {
        dst.remove(key);
        dstArr = dst.createNestedArray(key);
        for (JsonVariant v : srcArr) {
            jsonAddCopy(dstArr, v.as<const char*>());
        }
        return;
    }

    for (uint8_t i = 0; i < srcArr.size() && i < dstArr.size(); i++) {
        const char* dstVal = dstArr[i] | "";
        const char* srcVal = srcArr[i] | "";
        if (!nonEmpty(dstVal) && nonEmpty(srcVal)) {
            jsonSetCopy(dstArr[i], srcVal);
        }
    }
}

static void mergePairingRows(JsonObject keeper, JsonObject duplicate) {
    if (keeper.isNull() || duplicate.isNull()) return;

    const int dupCount = duplicate["boop_count"] | 0;
    if (dupCount > (keeper["boop_count"] | 0)) keeper["boop_count"] = dupCount;

    const char* dupLast = duplicate["last_seen"] | "";
    const char* keepLast = keeper["last_seen"] | "";
    if (nonEmpty(dupLast) && (!nonEmpty(keepLast) || strcmp(dupLast, keepLast) > 0)) {
        jsonSetCopy(keeper["last_seen"], dupLast);
    }

    copyStringIfBlank(keeper, duplicate, "first_seen");
    copyStringIfBlank(keeper, duplicate, "partner_name");
    copyStringIfBlank(keeper, duplicate, "partner_company");
    copyStringIfBlank(keeper, duplicate, "partner_attendee_type");
    copyArrayBlanks(keeper, duplicate, "ticket_uuids");
    copyArrayBlanks(keeper, duplicate, "badge_uuids");

    const char* status = keeper["status"] | "";
    const char* dupStatus = duplicate["status"] | "";
    if (strcmp(status, "confirmed") != 0 && strcmp(dupStatus, "confirmed") == 0) {
        keeper["status"] = "confirmed";
    }
}

static bool dedupePairingIdLocked(JsonArray pairings, int pairingId,
                                  const char* reason) {
    if (pairingId <= 0) return false;

    int keepIndex = -1;
    int keepScore = -1;
    int matches = 0;
    for (int i = 0; i < (int)pairings.size(); i++) {
        JsonObject p = pairings[i];
        if ((p["pairing_id"] | 0) != pairingId) continue;
        matches++;
        const int score = serverTruthScore(p);
        if (score > keepScore) {
            keepScore = score;
            keepIndex = i;
        }
    }
    if (matches < 2 || keepIndex < 0) return false;

    JsonObject keeper = pairings[keepIndex];
    bool dirty = false;
    for (int i = (int)pairings.size() - 1; i >= 0; i--) {
        if (i == keepIndex) continue;
        JsonObject p = pairings[i];
        if ((p["pairing_id"] | 0) != pairingId) continue;
        mergePairingRows(keeper, p);
        pairings.remove((size_t)i);
        dirty = true;
        DBG("[%s] deduped pairing %d during %s\n",
            TAG, pairingId, reason ? reason : "sync");
    }
    return dirty;
}

static bool dedupeConfirmedPairingsLocked(JsonArray pairings,
                                          const char* reason) {
    if (pairings.isNull()) return false;
    bool dirty = false;
    for (int i = 0; i < (int)pairings.size(); i++) {
        JsonObject p = pairings[i];
        const int pairingId = p["pairing_id"] | 0;
        if (pairingId <= 0) continue;
        if (dedupePairingIdLocked(pairings, pairingId, reason)) {
            dirty = true;
        }
    }
    return dirty;
}

static void mergeTicketSlots(JsonObject pairing, bool iAmLo,
                             const char* peerTicketUuid) {
    if (pairing.isNull()) return;

    char slots[2][BADGE_UUID_MAX] = {};
    JsonArray tids = pairing["ticket_uuids"];
    if (!tids.isNull()) {
        for (uint8_t i = 0; i < 2 && i < tids.size(); i++) {
            const char* cur = tids[i] | "";
            strncpy(slots[i], cur, sizeof(slots[i]) - 1);
        }
    }

    const int mySlot = iAmLo ? 0 : 1;
    const int peerSlot = iAmLo ? 1 : 0;
    (void)mySlot;
    if (peerTicketUuid && peerTicketUuid[0]) {
        strncpy(slots[peerSlot], peerTicketUuid, sizeof(slots[peerSlot]) - 1);
    }

    if (tids.isNull() || tids.size() < 2) {
        pairing.remove("ticket_uuids");
        tids = pairing.createNestedArray("ticket_uuids");
        jsonAddCopy(tids, slots[0]);
        jsonAddCopy(tids, slots[1]);
    } else {
        jsonSetCopy(tids[0], slots[0]);
        jsonSetCopy(tids[1], slots[1]);
    }
}

static bool pairingExistsForPeer(const char* peer_badge_uid) {
    if (!s_doc || !peer_badge_uid || !peer_badge_uid[0]) return false;
    const char* lo;
    const char* hi;
    sortedPair(uid_hex, peer_badge_uid, &lo, &hi);
    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    return !pairings.isNull() && !findPairing(pairings, lo, hi).isNull();
}

namespace BadgeBoops {

void begin() {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_doc) s_doc = new BadgeMemory::PsramJsonDocument(kDocCapacity);
    s_doc->clear();

    // Boot-time filesystem hygiene. Older firmware could leave the ffat
    // partition in a ragged state — leaked tmp files from aborted saves,
    // half-written journals whose binary garbage fails to parse later,
    // or the partition itself wedged at 0 bytes free. This block logs
    // what's on the partition and recovers aggressively.
    Filesystem::removeFile("/boops.json.tmp");
    {
        Filesystem::IOLock fsLock;  // serialise the boot-hygiene scan
        if (FATFS* fs = replay_get_fatfs()) {
            FF_DIR dir;
            FILINFO info;
            if (f_opendir(fs, &dir, "/") == FR_OK) {
                // DBG("[%s] ffat contents:\n", TAG);
                unsigned long total_used = 0;
                while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
                    // DBG("[%s]   %-20s %10lu B\n", TAG,
                        // info.fname, (unsigned long)info.fsize);
                    total_used += (unsigned long)info.fsize;
                }
                f_closedir(&dir);
                // DBG("[%s] ffat total used (files): %lu B\n", TAG, total_used);
            }

            DWORD free_clusters = 0;
            if (f_getfree(fs, &free_clusters) == FR_OK) {
                // ssize is the actual sector size in bytes (4096 on
                // ESP32 wear-levelled FATFS, not 512); the old "* 512"
                // form underreported free space by 8x.
                const unsigned long free_bytes =
                    (unsigned long)free_clusters * fs->csize * fs->ssize;
                // DBG("[%s] ffat free: %lu B\n", TAG, free_bytes);

                // If the partition is critically low, the journal can't
                // be saved.  Delete /boops.json so the next save has
                // room to land — losing history is a worse outcome than
                // a permanently-broken journal.  Threshold of 8 KB ≈ 2×
                // the max serialized size we write today.
                if (free_bytes < 8192) {
                    FRESULT ur = f_unlink(fs, kBoopsPath);
                    // Serial.printf("[%s] ffat low — unlinked /boops.json (fr=%d)\n",
                                  // TAG, (int)ur);
                }
            }
        }
    }

    bool loaded;
    { BoopsLock lock; loaded = loadFromDisk(); }

    if (loaded) {
        // -------- One-shot: strip garbage top-level keys. --------
        // The old zero-copy-deserialize bug left junk keys at the root of
        // /boops.json (e.g. "�P=<���?xV��h": [...]). Now that loadFromDisk
        // copies strings properly those junk keys survive as "valid" UTF-8
        // noise and the write path would dutifully preserve them forever.
        // `pairings` is the only legitimate top-level key, so anything else
        // is guaranteed stale corruption — strip it and flag a rewrite.
        //
        // Two-pass because mutating during ArduinoJson iteration is not
        // supported. kMaxJunkKeys=16 is enough slack for the handful of
        // corrupted roots we've seen in the field without dragging another
        // container dependency into this TU.
        bool stripped = false;
        {
            JsonObject root = s_doc->as<JsonObject>();
            constexpr int  kMaxJunkKeys  = 16;
            constexpr size_t kKeyBufLen  = 64;
            char junkKeys[kMaxJunkKeys][kKeyBufLen] = {};
            int junkCount = 0;
            int junkOver  = 0;  // count of keys we had to drop for lack of slots
            for (JsonPair kv : root) {
                const char* k = kv.key().c_str();
                if (!k) continue;
                if (strcmp(k, "pairings") == 0) continue;
                if (junkCount < kMaxJunkKeys) {
                    strncpy(junkKeys[junkCount], k, kKeyBufLen - 1);
                    junkCount++;
                } else {
                    junkOver++;
                }
            }
            for (int i = 0; i < junkCount; i++) {
                root.remove(junkKeys[i]);
                stripped = true;
            }
            if (stripped) {
                DBG("[%s] stripped %d garbage top-level key(s)%s\n",
                    TAG, junkCount,
                    junkOver ? " (+more, ran out of slots)" : "");
                // If there were more than the scratch array fit, loop the
                // removal a couple more times. Each pass unblocks another
                // batch of up to kMaxJunkKeys. Capped so a pathological
                // file can't wedge the boot path.
                for (int pass = 0; junkOver > 0 && pass < 4; pass++) {
                    junkCount = 0;
                    junkOver  = 0;
                    for (JsonPair kv : root) {
                        const char* k = kv.key().c_str();
                        if (!k || strcmp(k, "pairings") == 0) continue;
                        if (junkCount < kMaxJunkKeys) {
                            strncpy(junkKeys[junkCount], k, kKeyBufLen - 1);
                            junkCount++;
                        } else {
                            junkOver++;
                        }
                    }
                    for (int i = 0; i < junkCount; i++) root.remove(junkKeys[i]);
                    DBG("[%s]   pass %d: stripped %d more\n",
                        TAG, pass + 2, junkCount);
                }
            }
            // Guarantee the pairings array exists even if the loaded file
            // somehow had only garbage at the root (it'd parse to a doc
            // with no "pairings" key).
            if (!(*s_doc).containsKey("pairings")) {
                (*s_doc)["pairings"] = s_doc->createNestedArray("pairings");
                stripped = true;  // force a save so disk reflects the fixup
            }
        }

        JsonArray pairings = (*s_doc)["pairings"];

        // Migrate legacy records: early versions kept a `connected_at`
        // array with every visit timestamp. We now only track the most
        // recent visit via `last_seen`. Strip the array on load, pulling
        // the tail timestamp into `last_seen` if it's not already set.
        // If anything got migrated, flush a clean copy to disk so the
        // next boot is already slim.
        bool migrated = false;
        for (JsonObject p : pairings) {
            if (!p.containsKey("connected_at")) continue;
            JsonArray ca = p["connected_at"];
            const char* lastTs = "";
            if (!ca.isNull() && ca.size() > 0) {
                lastTs = ca[ca.size() - 1] | "";
            }
            if (lastTs[0] != '\0') {
                const char* cur = p["last_seen"] | "";
                // lastTs points into the about-to-be-removed connected_at
                // array. In ArduinoJson v6's linear pool the bytes would
                // survive until the doc is cleared, but once the sibling
                // key is unlinked we can't reason about that safely —
                // force a copy so the new last_seen value stands on its
                // own in the arena.
                if (cur[0] == '\0') jsonSetCopy(p["last_seen"], lastTs);
            }
            p.remove("connected_at");
            migrated = true;
        }

        const bool deduped = dedupeConfirmedPairingsLocked(pairings, "load");

        DBG("[%s] loaded %d records from disk%s%s%s\n", TAG,
            (int)pairings.size(),
            migrated ? " (migrated connected_at → last_seen)" : "",
            stripped ? " (stripped garbage roots)" : "",
            deduped ? " (deduped pairing_id)" : "");

        if (migrated || stripped || deduped) {
            BoopsLock lock;
            saveToDisk();
        }
    } else {
        (*s_doc)["pairings"] = s_doc->createNestedArray("pairings");
        // DBG("[%s] starting with empty boops.json\n", TAG);
    }
}

void recordBoop(const char* peer_badge_uid,
                const char* peer_name, const char* peer_ticket_uuid) {
    if (!s_doc || !peer_badge_uid || peer_badge_uid[0] == '\0') return;

    // Defense in depth — a self-loopback boop has no meaning and pollutes
    // the journal. Reject silently here so callers don't have to check.
    if (strcmp(uid_hex, peer_badge_uid) == 0) {
        // DBG("[%s] refusing self-boop (UID=%s)\n", TAG, peer_badge_uid);
        return;
    }

    const char* lo;
    const char* hi;
    sortedPair(uid_hex, peer_badge_uid, &lo, &hi);

    char ts[32];
    isoTimestamp(ts, sizeof(ts));

    BoopsLock lock;

    JsonArray pairings = (*s_doc)["pairings"];
    if (pairings.isNull()) pairings = s_doc->createNestedArray("pairings");

    JsonObject existing = findPairing(pairings, lo, hi);
    const bool iAmLo = (strcmp(uid_hex, lo) == 0);

    if (existing.isNull()) {
        if (pairings.size() >= kMaxBoopRecords) {
            DBG("[%s] max records reached, dropping boop\n", TAG);
            return;
        }
        existing = pairings.createNestedObject();
        JsonArray buids = existing.createNestedArray("badge_uuids");
        // lo/hi may point to boopStatus.peerUID (volatile — cleared between
        // boops); force a copy into the doc's arena via jsonAddCopy.
        jsonAddCopy(buids, lo);
        jsonAddCopy(buids, hi);
        existing["boop_count"] = 0;
        existing["first_seen"] = ts;  // char[32] → char* overload → already copied
        existing["last_seen"]  = ts;
        existing["status"] = "local";        // string literal — safe as linked
        existing["pairing_id"] = 0;
        mergeTicketSlots(existing, iAmLo, peer_ticket_uuid);

        jsonSetCopy(existing["partner_name"], peer_name);
        existing["partner_company"] = "";          // literal, safe
        existing["partner_attendee_type"] = "";    // literal, safe
    } else {
        // Merge policy for repeat boops with the same peer: if the
        // incoming value is non-blank, OVERWRITE the stored value
        // (the peer's latest broadcast wins). Blank incoming values
        // do NOT wipe an existing value — that way a peer who's only
        // exposing a subset of fields this time won't erase context
        // they shared on a previous boop.
        mergeTicketSlots(existing, iAmLo, peer_ticket_uuid);
        if (peer_name && peer_name[0]) {
            jsonSetCopy(existing["partner_name"], peer_name);
        }
    }

    // A fresh local/IR boop with the same peer resurrects a row that a
    // previous API sync had marked revoked. Without this, repeat boops
    // update the row but the shared contact picker keeps hiding it.
    existing.remove("revoked");
    existing.remove("revoked_at");

    existing["boop_count"] = (existing["boop_count"] | 0) + 1;
    if (ts[0] != '\0') existing["last_seen"] = ts;  // char[] → copied, safe
    // Legacy: older on-disk journals carried a "connected_at" array of
    // every visit timestamp which was the main source of doc bloat.
    // Strip it on the first write after an upgrade so it doesn't linger.
    if (existing.containsKey("connected_at")) {
        existing.remove("connected_at");
    }

    const int logCount = existing["boop_count"] | 0;
    char logStatus[16] = {};
    strncpy(logStatus, existing["status"] | "?", sizeof(logStatus) - 1);

    saveToDisk();

    LOG_BOOP("[%s] recorded boop with %s (count=%d, status=%s)\n",
                  TAG, peer_badge_uid,
                  logCount,
                  logStatus);
}

char* readJson(size_t* outLen) {
    if (!s_doc) { if (outLen) *outLen = 0; return nullptr; }
    BoopsLock lock;
    size_t jsonLen = measureJson(*s_doc);
    char* buf = (char*)allocPreferPsram(jsonLen + 1);
    if (buf) serializeJson(*s_doc, buf, jsonLen + 1);
    if (outLen) *outLen = buf ? jsonLen : 0;
    return buf;
}

int count() {
    if (!s_doc) return 0;
    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    return pairings.isNull() ? 0 : (int)pairings.size();
}

int countUniqueActivePairings() {
    if (!s_doc) return 0;
    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    if (pairings.isNull()) return 0;
    int n = 0;
    for (JsonObject p : pairings) {
        if ((p["revoked"] | false) == true) continue;
        n++;
    }
    return n;
}

void clearJournal() {
    if (!s_doc) return;
    BoopsLock lock;
    s_doc->clear();
    (*s_doc)["pairings"] = s_doc->createNestedArray("pairings");
    Filesystem::removeFile("/boops.json.tmp");
    saveToDisk();
    DBG("[BadgeBoops] cleared local boop journal\n");
}

// ─── Peer lookup (spec-010) ─────────────────────────────────────────────────

bool lookupPeerByTicket(const char* ticketUuid,
                        char* outPeerUid,  size_t peerUidCap,
                        char* outPeerName, size_t peerNameCap) {
    if (outPeerUid  && peerUidCap  > 0) outPeerUid[0]  = '\0';
    if (outPeerName && peerNameCap > 0) outPeerName[0] = '\0';
    if (!s_doc || !ticketUuid || !ticketUuid[0]) return false;

    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    if (pairings.isNull()) return false;

    for (JsonObject p : pairings) {
        if ((p["revoked"] | false) == true) continue;
        JsonArray tids = p["ticket_uuids"];
        if (tids.isNull() || tids.size() < 2) continue;

        const char* t0 = tids[0] | "";
        const char* t1 = tids[1] | "";
        const int slot = (strcmp(t0, ticketUuid) == 0) ? 0
                       : (strcmp(t1, ticketUuid) == 0) ? 1
                       : -1;
        if (slot < 0) continue;

        JsonArray buids = p["badge_uuids"];
        if (outPeerUid && peerUidCap > 0 && !buids.isNull() && buids.size() > (size_t)slot) {
            strncpy(outPeerUid, buids[slot] | "", peerUidCap - 1);
            outPeerUid[peerUidCap - 1] = '\0';
        }
        if (outPeerName && peerNameCap > 0) {
            const char* pname = p["partner_name"] | "";
            strncpy(outPeerName, pname, peerNameCap - 1);
            outPeerName[peerNameCap - 1] = '\0';
        }
        return true;
    }
    return false;
}

void listActivePeers(const char* myTicketUuid,
                     PeerWalkCallback cb, void* user) {
    if (!cb || !s_doc) return;
    if (!myTicketUuid) myTicketUuid = "";

    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    if (pairings.isNull()) {
        DBG("[BadgeBoops] listActivePeers: no pairings array\n");
        return;
    }
    DBG("[BadgeBoops] listActivePeers: rows=%u myTicket=%s uid=%s badge=%s\n",
        (unsigned)pairings.size(), myTicketUuid, uid_hex, badge_uuid);

    for (JsonObject p : pairings) {
        const char* status = p["status"] | "";
        const bool revoked = (p["revoked"] | false) == true;
        JsonArray tids = p["ticket_uuids"];
        JsonArray buids = p["badge_uuids"];
        const char* t0 = (!tids.isNull() && tids.size() > 0) ? (tids[0] | "") : "";
        const char* t1 = (!tids.isNull() && tids.size() > 1) ? (tids[1] | "") : "";
        const char* b0 = (!buids.isNull() && buids.size() > 0) ? (buids[0] | "") : "";
        const char* b1 = (!buids.isNull() && buids.size() > 1) ? (buids[1] | "") : "";
        DBG("[BadgeBoops] row status=%s revoked=%d pid=%d t0=%s t1=%s b0=%s b1=%s name=%s\n",
            status, revoked ? 1 : 0, p["pairing_id"] | 0,
            t0, t1, b0, b1, p["partner_name"] | "");
        if (revoked) continue;

        // Identify the peer slot. Prefer ticket UUIDs because server rows
        // are canonical there, then fall back to badge UUIDs so local/legacy
        // IR rows with missing tickets still show up in contact pickers.
        int peerSlot = -1;
        if (myTicketUuid[0]) {
            if (strcmp(t0, myTicketUuid) == 0)      peerSlot = 1;
            else if (strcmp(t1, myTicketUuid) == 0) peerSlot = 0;
        }
        if (peerSlot < 0) {
            if (b0[0] && (strcmp(b0, uid_hex) == 0 || strcmp(b0, badge_uuid) == 0)) {
                peerSlot = 1;
            } else if (b1[0] && (strcmp(b1, uid_hex) == 0 || strcmp(b1, badge_uuid) == 0)) {
                peerSlot = 0;
            }
        }
        // Fallback: first non-empty ticket, then first non-self badge. Used
        // when the row pre-dates my-ticket sync or has sparse ticket slots.
        if (peerSlot < 0) {
            if (t0[0]) peerSlot = 0;
            else if (t1[0]) peerSlot = 1;
        }
        if (peerSlot < 0 && !buids.isNull()) {
            for (uint8_t i = 0; i < 2 && i < buids.size(); i++) {
                const char* bu = buids[i] | "";
                if (!bu[0]) continue;
                if (strcmp(bu, uid_hex) == 0 || strcmp(bu, badge_uuid) == 0) continue;
                peerSlot = i;
                break;
            }
        }
        if (peerSlot < 0) {
            DBG("[BadgeBoops] row skipped: could not identify peer slot\n");
            continue;
        }

        PeerEntry e = {};
        e.pairingId  = p["pairing_id"] | 0;
        e.boopCount  = p["boop_count"] | 0;

        const char* tk = (!tids.isNull() && tids.size() > (size_t)peerSlot)
            ? (tids[peerSlot] | "")
            : "";
        strncpy(e.peerTicketUuid, tk, sizeof(e.peerTicketUuid) - 1);

        if (!buids.isNull() && buids.size() > (size_t)peerSlot) {
            const char* bu = buids[peerSlot] | "";
            strncpy(e.peerUid, bu, sizeof(e.peerUid) - 1);
        }

        const char* nm = p["partner_name"]    | "";
        const char* co = p["partner_company"] | "";
        const char* ts = p["last_seen"]       | "";
        strncpy(e.name,    nm, sizeof(e.name)    - 1);
        strncpy(e.company, co, sizeof(e.company) - 1);
        strncpy(e.lastTs,  ts, sizeof(e.lastTs)  - 1);

        if (!cb(e, user)) return;
    }
}

bool lookupContactByUid(const char* peerUid, ContactDetail& out) {
    memset(&out, 0, sizeof(out));
    if (!s_doc || !peerUid || !peerUid[0]) return false;

    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    if (pairings.isNull()) return false;

    auto copyStr = [](char* dst, size_t cap, const char* src) {
        if (!dst || cap == 0) return;
        if (!src) src = "";
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
    };

    for (JsonObject p : pairings) {
        if ((p["revoked"] | false) == true) continue;
        JsonArray buids = p["badge_uuids"];
        if (buids.isNull() || buids.size() < 2) continue;
        const char* b0 = buids[0] | "";
        const char* b1 = buids[1] | "";
        int peerSlot = -1;
        if (strcmp(b0, peerUid) == 0) peerSlot = 0;
        else if (strcmp(b1, peerUid) == 0) peerSlot = 1;
        if (peerSlot < 0) continue;

        copyStr(out.peerUid, sizeof(out.peerUid), peerUid);

        JsonArray tids = p["ticket_uuids"];
        if (!tids.isNull() && tids.size() > (size_t)peerSlot) {
            copyStr(out.ticketUuid, sizeof(out.ticketUuid),
                    tids[peerSlot] | "");
        }

        copyStr(out.name,         sizeof(out.name),         p["partner_name"]          | "");
        copyStr(out.title,        sizeof(out.title),        p["partner_title"]         | "");
        copyStr(out.company,      sizeof(out.company),      p["partner_company"]       | "");
        copyStr(out.attendeeType, sizeof(out.attendeeType), p["partner_attendee_type"] | "");
        copyStr(out.email,        sizeof(out.email),        p["partner_email"]         | "");
        copyStr(out.website,      sizeof(out.website),      p["partner_website"]       | "");
        copyStr(out.phone,        sizeof(out.phone),        p["partner_phone"]         | "");
        copyStr(out.bio,          sizeof(out.bio),          p["partner_bio"]           | "");
        copyStr(out.lastTs,       sizeof(out.lastTs),       p["last_seen"]             | "");
        out.boopCount = p["boop_count"] | 0;
        return true;
    }
    return false;
}

void recordBoopEx(BoopType type, const char* peerIdOrInstallation,
                  const PartnerInfo* partner) {
    const char* peerName = partner ? partner->name : nullptr;
    const char* peerTicket = partner ? partner->ticketUuid : nullptr;

    if (!pairingExistsForPeer(peerIdOrInstallation)) {
        recordBoop(peerIdOrInstallation, peerName, peerTicket);
    }

    if (!s_doc || !peerIdOrInstallation || peerIdOrInstallation[0] == '\0') return;

    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    if (pairings.isNull()) return;

    const char* lo;
    const char* hi;
    sortedPair(uid_hex, peerIdOrInstallation, &lo, &hi);

    JsonObject existing = findPairing(pairings, lo, hi);
    if (existing.isNull()) return;

    existing.remove("revoked");
    existing.remove("revoked_at");
    existing["boop_type"] = static_cast<int>(type);

    if (partner) {
        // Repeat-boop merge policy: if the incoming value is non-blank,
        // OVERWRITE the stored value so the latest peer broadcast wins
        // (e.g. they fixed a typo in their title). Blank incoming
        // values are silently ignored rather than clobbering what we
        // already have — a peer who's temporarily transmitting a
        // subset of fields shouldn't erase data they shared before.
        //
        // PartnerInfo members are `const char*` pointing into the
        // global boopStatus.peer* buffers, which get cleared between
        // boops — so jsonSetCopy is required to strdup into the doc's
        // arena before the source bytes go away.
        if (partner->ticketUuid && partner->ticketUuid[0]) {
            mergeTicketSlots(existing, strcmp(uid_hex, lo) == 0,
                             partner->ticketUuid);
        }

        auto updateIfPresent = [&](const char* key, const char* val) {
            if (val && val[0]) {
                jsonSetCopy(existing[key], val);
            }
        };
        updateIfPresent("partner_name",          partner->name);
        updateIfPresent("partner_title",         partner->title);
        updateIfPresent("partner_company",       partner->company);
        updateIfPresent("partner_attendee_type", partner->attendeeType);
        updateIfPresent("partner_email",         partner->email);
        updateIfPresent("partner_website",       partner->website);
        updateIfPresent("partner_phone",         partner->phone);
        updateIfPresent("partner_bio",           partner->bio);
    }

    if (type != BOOP_PEER) {
        // peerIdOrInstallation usually points at boopStatus.peerUID (volatile).
        jsonSetCopy(existing["installation_id"], peerIdOrInstallation);
        const char* kindName = "unknown";
        switch (type) {
            case BOOP_EXHIBIT:    kindName = "exhibit";  break;
            case BOOP_QUEUE_JOIN: kindName = "queue";    break;
            case BOOP_KIOSK_INFO: kindName = "kiosk";    break;
            case BOOP_CHECKIN:    kindName = "checkin";  break;
            default: break;
        }
        existing["installation_kind"] = kindName;  // all branches = string literal, safe
    }

    saveToDisk();
}

bool updatePartnerNotes(const char* uidA, const char* uidB, const char* notes) {
    return updatePartnerField(uidA, uidB, "user_notes", notes);
}

bool updatePartnerField(const char* uidA, const char* uidB,
                        const char* jsonKey, const char* value) {
    if (!s_doc || !uidA || !jsonKey || !value) return false;

    BoopsLock lock;
    JsonArray pairings = (*s_doc)["pairings"];
    if (pairings.isNull()) return false;

    const char* lo;
    const char* hi;
    sortedPair(uidA, uidB ? uidB : "", &lo, &hi);

    JsonObject existing = findPairing(pairings, lo, hi);
    if (existing.isNull()) return false;

    // `value` comes from a caller-owned buffer; jsonSetCopy duplicates
    // into the doc arena so we don't depend on its lifetime.
    jsonSetCopy(existing[jsonKey], value);
    saveToDisk();
    return true;
}

}  // namespace BadgeBoops
