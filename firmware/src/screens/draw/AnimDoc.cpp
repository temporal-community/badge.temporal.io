#include "AnimDoc.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_random.h"

#include "../../infra/Filesystem.h"
#include "../../infra/PsramAllocator.h"
#include "../../ui/FontCatalog.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

namespace draw {

namespace {

constexpr const char* kRoot = "/composer";

// Single-pass uppercase-hex of a uint32 with `n` chars (pad with zeros).
void formatHex(uint32_t v, char* out, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[n - 1 - i] = kHex[v & 0xF];
        v >>= 4;
    }
    out[n] = '\0';
}

bool ensureRootDir() {
    Filesystem::IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;
    FRESULT r = f_mkdir(fs, kRoot);
    return r == FR_OK || r == FR_EXIST;
}

bool ensureAnimDir(const char* animId) {
    if (!ensureRootDir()) return false;
    char dir[40];
    animDirPath(animId, dir, sizeof(dir));
    Filesystem::IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;
    FRESULT r = f_mkdir(fs, dir);
    return r == FR_OK || r == FR_EXIST;
}

// Atomic write of raw bytes to a per-DrawnAnim pixel file.
bool writeDrawnFile(const char* animId, const char* objId,
                    const uint8_t* data, size_t len) {
    char p[48];
    drawnObjectPath(animId, objId, p, sizeof(p));
    return Filesystem::writeFileAtomic(p, data, len);
}

bool readWhole(const char* path, uint8_t** outBuf, size_t* outLen,
               size_t maxLen) {
    char* buf = nullptr;
    size_t n = 0;
    if (!Filesystem::readFileAlloc(path, &buf, &n, maxLen)) return false;
    *outBuf = reinterpret_cast<uint8_t*>(buf);
    *outLen = n;
    return true;
}

// Recursively enumerate /composer/<id>/ entries; rmdir one anim's folder.
bool removeDir(const char* dir) {
    Filesystem::IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;
    FF_DIR dh;
    FILINFO fno;
    if (f_opendir(fs, &dh, dir) == FR_OK) {
        while (f_readdir(&dh, &fno) == FR_OK && fno.fname[0]) {
            char child[80];
            std::snprintf(child, sizeof(child), "%s/%s", dir, fno.fname);
            f_unlink(fs, child);
        }
        f_closedir(&dh);
    }
    return f_unlink(fs, dir) == FR_OK;
}

}  // namespace

// ── Path helpers ───────────────────────────────────────────────────────────

void animDirPath(const char* animId, char* out, size_t cap) {
    std::snprintf(out, cap, "%s/%s", kRoot, animId ? animId : "");
}

void infoJsonPath(const char* animId, char* out, size_t cap) {
    std::snprintf(out, cap, "%s/%s/info.json", kRoot, animId ? animId : "");
}

void legacyFramePath(const char* animId, uint8_t idx, char* out, size_t cap) {
    std::snprintf(out, cap, "%s/%s/f%02u.fb", kRoot,
                  animId ? animId : "", (unsigned)idx);
}

void drawnObjectPath(const char* animId, const char* objId,
                     char* out, size_t cap) {
    std::snprintf(out, cap, "%s/%s/o%s.fb", kRoot,
                  animId ? animId : "",
                  objId ? objId : "");
}

// ── Identifier helpers ────────────────────────────────────────────────────

void newAnimId(char* out, size_t cap) {
    if (!out || cap < kAnimIdLen + 1) {
        if (out && cap) out[0] = '\0';
        return;
    }
    formatHex(esp_random(), out, kAnimIdLen);
}

void newObjectId(const AnimDoc& doc, char* out, size_t cap) {
    if (!out || cap < kObjIdLen + 1) {
        if (out && cap) out[0] = '\0';
        return;
    }
    // A handful of attempts is plenty given a 65k-id space and at most a few
    // dozen objects per doc.
    for (uint8_t attempt = 0; attempt < 16; attempt++) {
        formatHex(esp_random(), out, kObjIdLen);
        bool clash = false;
        for (const auto& obj : doc.objects) {
            if (std::strcmp(obj.id, out) == 0) { clash = true; break; }
        }
        if (!clash) return;
    }
}

void defaultName(char* out, size_t cap) {
    if (!out || cap == 0) return;
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (now > 0) {
        std::snprintf(out, cap, "Anim %02d%02d-%02d%02d",
                      local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min);
    } else {
        std::snprintf(out, cap, "Untitled");
    }
}

static void trimNametagBuf(char* s) {
    if (!s) return;
    char* start = s;
    while (*start == ' ' || *start == '\t') ++start;
    if (*start == '\0') {
        s[0] = '\0';
        return;
    }
    char* end = start + std::strlen(start) - 1;
    while (end > start &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        --end;
    size_t len = static_cast<size_t>(end - start + 1);
    if (start != s) std::memmove(s, start, len);
    s[len] = '\0';
}

static int asciicasecmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = std::tolower(static_cast<unsigned char>(*a++));
        int cb = std::tolower(static_cast<unsigned char>(*b++));
        if (ca != cb) return ca - cb;
    }
    return std::tolower(static_cast<unsigned char>(*a)) -
           std::tolower(static_cast<unsigned char>(*b));
}

NametagSettingParse parseNametagSetting(const char* raw,
                                        char animIdOut[kAnimIdLen + 1]) {
    animIdOut[0] = '\0';
    if (!raw) return NametagSettingParse::Default;

    char buf[96];
    std::strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trimNametagBuf(buf);
    if (buf[0] == '\0') return NametagSettingParse::Default;
    if (asciicasecmp(buf, "default") == 0) return NametagSettingParse::Default;

    size_t len = std::strlen(buf);
    const char suf[] = "/info.json";
    const size_t slen = sizeof(suf) - 1;
    if (len >= slen) {
        bool match = true;
        for (size_t i = 0; i < slen; ++i) {
            char c = buf[len - slen + i];
            if (std::tolower(static_cast<unsigned char>(c)) !=
                std::tolower(static_cast<unsigned char>(suf[i]))) {
                match = false;
                break;
            }
        }
        if (match) {
            buf[len - slen] = '\0';
            trimNametagBuf(buf);
            len = std::strlen(buf);
        }
    }
    while (len > 0 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
        len--;
    }

    const char* base = buf;
    for (const char* p = buf; *p; ++p) {
        if (*p == '/') base = p + 1;
    }

    char cand[kAnimIdLen + 2];
    std::strncpy(cand, base, sizeof(cand) - 1);
    cand[sizeof(cand) - 1] = '\0';

    len = std::strlen(cand);
    const char jsonExt[] = ".json";
    const size_t jlen = sizeof(jsonExt) - 1;
    if (len > jlen) {
        bool je = true;
        for (size_t i = 0; i < jlen; ++i) {
            char c = cand[len - jlen + i];
            if (std::tolower(static_cast<unsigned char>(c)) !=
                std::tolower(static_cast<unsigned char>(jsonExt[i]))) {
                je = false;
                break;
            }
        }
        if (je) cand[len - jlen] = '\0';
    }

    if (std::strlen(cand) != kAnimIdLen) return NametagSettingParse::Invalid;
    for (uint8_t i = 0; i < kAnimIdLen; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(cand[i])))
            return NametagSettingParse::Invalid;
    }
    std::memcpy(animIdOut, cand, kAnimIdLen);
    animIdOut[kAnimIdLen] = '\0';
    return NametagSettingParse::AnimId;
}

// ── Pixel buffer helpers ──────────────────────────────────────────────────

bool allocDrawnPixels(ObjectDef& def, size_t bytes) {
    if (def.drawnPixels) {
        free(def.drawnPixels);
        def.drawnPixels = nullptr;
    }
    if (bytes == 0) return true;
    def.drawnPixels = static_cast<uint8_t*>(BadgeMemory::allocPreferPsram(bytes));
    if (!def.drawnPixels) return false;
    std::memset(def.drawnPixels, 0, bytes);
    return true;
}

void freeAll(AnimDoc& doc) {
    for (auto& obj : doc.objects) {
        if (obj.drawnPixels) {
            free(obj.drawnPixels);
            obj.drawnPixels = nullptr;
        }
    }
    doc.frames.clear();
    doc.objects.clear();
    doc.animId[0] = '\0';
    doc.name[0]   = '\0';
    doc.w = doc.h = 0;
    doc.dirty = false;
}

// ── JSON helpers ──────────────────────────────────────────────────────────

namespace {

constexpr size_t kInfoMaxBytes = 8 * 1024;

void docToJson(const AnimDoc& doc, JsonDocument& root) {
    root["anim_id"] = doc.animId;
    root["name"]    = doc.name;
    root["w"]       = doc.w;
    root["h"]       = doc.h;
    root["frame_count"] = (uint16_t)doc.frames.size();
    root["created_at"]  = doc.createdAt;
    root["edited_at"]   = doc.editedAt;

    JsonArray durations = root.createNestedArray("frame_durations_ms");
    for (const auto& f : doc.frames) durations.add(f.durationMs);

    JsonArray objs = root.createNestedArray("objects");
    for (const auto& o : doc.objects) {
        JsonObject jo = objs.createNestedObject();
        jo["id"] = o.id;
        switch (o.type) {
            case ObjectType::Catalog:
                jo["type"] = "catalog";
                jo["ref"]  = o.catalogIdx;
                break;
            case ObjectType::SavedAnim:
                jo["type"] = "saved_anim";
                jo["ref"]  = o.savedAnimId;
                break;
            case ObjectType::DrawnAnim:
                jo["type"] = "drawn";
                jo["w"]    = o.drawnW;
                jo["h"]    = o.drawnH;
                break;
            case ObjectType::Text:
                jo["type"] = "text";
                jo["content"] = o.textContent;
                jo["font_family"] = o.textFontFamily;
                jo["font_slot"] = o.textFontSlot;
                jo["stack_mode"] = static_cast<uint8_t>(o.textStackMode);
                break;
        }
    }

    JsonArray frameObjs = root.createNestedArray("frame_objects");
    for (const auto& f : doc.frames) {
        JsonArray frame = frameObjs.createNestedArray();
        for (const auto& p : f.placements) {
            JsonObject jp = frame.createNestedObject();
            jp["id"]       = p.objId;
            jp["x"]        = p.x;
            jp["y"]        = p.y;
            jp["z"]        = p.z;
            jp["scale"]    = p.scale;
            jp["phase_ms"] = p.phaseMs;
        }
    }
}

bool jsonToDoc(JsonDocument& root, AnimDoc& doc) {
    const char* animId = root["anim_id"] | "";
    const char* name   = root["name"]    | "";
    uint16_t w = root["w"] | 0;
    uint16_t h = root["h"] | 0;
    uint8_t  fc = root["frame_count"] | 0;
    if (animId[0] == '\0' || w == 0 || h == 0 || fc == 0 || fc > kMaxFrames) {
        return false;
    }
    if (!((w == kCanvasFullW && h == kCanvasFullH) ||
          (w == kCanvasZigW  && h == kCanvasZigH))) {
        // Unsupported canvas size — skip the doc.
        return false;
    }

    std::strncpy(doc.animId, animId, kAnimIdLen);
    doc.animId[kAnimIdLen] = '\0';
    std::strncpy(doc.name, name, kAnimNameMax);
    doc.name[kAnimNameMax] = '\0';
    doc.w = w;
    doc.h = h;
    doc.createdAt = root["created_at"] | 0u;
    doc.editedAt  = root["edited_at"]  | 0u;

    doc.frames.clear();
    doc.frames.resize(fc);

    JsonArray durations = root["frame_durations_ms"].as<JsonArray>();
    uint8_t idx = 0;
    for (auto v : durations) {
        if (idx >= fc) break;
        uint16_t ms = v.as<uint16_t>();
        if (ms < kMinDurationMs) ms = kMinDurationMs;
        if (ms > kMaxDurationMs) ms = kMaxDurationMs;
        doc.frames[idx].durationMs = ms;
        idx++;
    }
    while (idx < fc) {
        doc.frames[idx].durationMs = kDefaultDurationMs;
        idx++;
    }

    doc.objects.clear();
    JsonArray objs = root["objects"].as<JsonArray>();
    for (JsonObject jo : objs) {
        ObjectDef def{};
        const char* id = jo["id"] | "";
        std::strncpy(def.id, id, kObjIdLen);
        def.id[kObjIdLen] = '\0';
        const char* t = jo["type"] | "";
        if (std::strcmp(t, "catalog") == 0) {
            def.type = ObjectType::Catalog;
            def.catalogIdx = jo["ref"] | -1;
        } else if (std::strcmp(t, "saved_anim") == 0) {
            def.type = ObjectType::SavedAnim;
            const char* ref = jo["ref"] | "";
            std::strncpy(def.savedAnimId, ref, kAnimIdLen);
            def.savedAnimId[kAnimIdLen] = '\0';
        } else if (std::strcmp(t, "drawn") == 0) {
            def.type = ObjectType::DrawnAnim;
            def.drawnW = jo["w"] | doc.w;
            def.drawnH = jo["h"] | doc.h;
        } else if (std::strcmp(t, "text") == 0) {
            def.type = ObjectType::Text;
            const char* content = jo["content"] | "";
            std::strncpy(def.textContent, content, kTextContentMax);
            def.textContent[kTextContentMax] = '\0';
            def.textFontFamily = jo["font_family"] | 0;
            if (def.textFontFamily >= kFontGridFamilyCount) def.textFontFamily = 0;
            def.textFontSlot = jo["font_slot"] | 2;
            if (def.textFontSlot >= kSizeCount) def.textFontSlot = 2;
            uint8_t stack = jo["stack_mode"] | 0;
            if (stack > static_cast<uint8_t>(TextStackMode::RoundedBox)) stack = 0;
            def.textStackMode = static_cast<TextStackMode>(stack);
        } else {
            continue;
        }
        doc.objects.push_back(def);
    }

    JsonArray frameObjs = root["frame_objects"].as<JsonArray>();
    idx = 0;
    for (JsonArray frame : frameObjs) {
        if (idx >= fc) break;
        for (JsonObject jp : frame) {
            ObjectPlacement p{};
            const char* id = jp["id"] | "";
            std::strncpy(p.objId, id, kObjIdLen);
            p.objId[kObjIdLen] = '\0';
            p.x       = jp["x"] | 0;
            p.y       = jp["y"] | 0;
            p.z       = (int8_t)(jp["z"] | 0);
            p.scale   = jp["scale"] | 0;
            p.phaseMs = jp["phase_ms"] | 0u;
            doc.frames[idx].placements.push_back(p);
        }
        idx++;
    }

    return true;
}

}  // namespace

// ── List ──────────────────────────────────────────────────────────────────

bool listAll(std::vector<AnimSummary>& out) {
    out.clear();
    if (!ensureRootDir()) return false;

    Filesystem::IOLock lock;
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;

    FF_DIR root;
    FILINFO fno;
    if (f_opendir(fs, &root, kRoot) != FR_OK) return false;

    while (out.size() < kMaxAnimations) {
        if (f_readdir(&root, &fno) != FR_OK || fno.fname[0] == '\0') break;
        if (!(fno.fattrib & AM_DIR)) continue;
        if (fno.fname[0] == '.') continue;

        char info[48];
        std::snprintf(info, sizeof(info), "%s/%s/info.json", kRoot, fno.fname);

        FIL fil;
        if (f_open(fs, &fil, info, FA_READ) != FR_OK) continue;
        UINT sz = f_size(&fil);
        if (sz == 0 || sz > kInfoMaxBytes) {
            f_close(&fil);
            continue;
        }
        char* buf = static_cast<char*>(BadgeMemory::allocPreferPsram(sz + 1));
        if (!buf) {
            f_close(&fil);
            continue;
        }
        UINT br = 0;
        FRESULT rr = f_read(&fil, buf, sz, &br);
        f_close(&fil);
        if (rr != FR_OK || br != sz) {
            free(buf);
            continue;
        }
        buf[sz] = '\0';

        BadgeMemory::PsramJsonDocument doc(8 * 1024);
        DeserializationError err = deserializeJson(doc, buf, sz);
        free(buf);
        if (err) continue;

        AnimSummary s{};
        const char* aid = doc["anim_id"] | "";
        if (aid[0] == '\0') continue;
        std::strncpy(s.animId, aid, kAnimIdLen);
        s.animId[kAnimIdLen] = '\0';
        const char* name = doc["name"] | "";
        std::strncpy(s.name, name, kAnimNameMax);
        s.name[kAnimNameMax] = '\0';
        s.w = doc["w"] | 0;
        s.h = doc["h"] | 0;
        s.frameCount = doc["frame_count"] | 0;
        s.editedAt   = doc["edited_at"] | 0u;
        JsonArray durations = doc["frame_durations_ms"].as<JsonArray>();
        for (auto v : durations) s.totalMs += v.as<uint16_t>();
        out.push_back(s);
    }
    f_closedir(&root);
    return true;
}

// ── Load ──────────────────────────────────────────────────────────────────

bool load(const char* animId, AnimDoc& doc) {
    freeAll(doc);
    if (!animId || !animId[0]) return false;

    char info[48];
    infoJsonPath(animId, info, sizeof(info));

    char* jsonBuf = nullptr;
    size_t jsonLen = 0;
    if (!Filesystem::readFileAlloc(info, &jsonBuf, &jsonLen, kInfoMaxBytes)) {
        return false;
    }

    BadgeMemory::PsramJsonDocument root(8 * 1024);
    DeserializationError err = deserializeJson(root, jsonBuf, jsonLen);
    free(jsonBuf);
    if (err) return false;

    if (!jsonToDoc(root, doc)) return false;

    const size_t pixelBytes = doc.pixelBytes();

    // Load pixel data for every DrawnAnim ObjectDef.
    for (auto& def : doc.objects) {
        if (def.type != ObjectType::DrawnAnim) continue;
        if (def.drawnW == 0 || def.drawnH == 0) {
            def.drawnW = doc.w;
            def.drawnH = doc.h;
        }
        const size_t bytes = xbmBytes(def.drawnW, def.drawnH);
        if (!allocDrawnPixels(def, bytes)) {
            freeAll(doc);
            return false;
        }
        char p[48];
        drawnObjectPath(animId, def.id, p, sizeof(p));
        uint8_t* buf = nullptr;
        size_t n = 0;
        if (readWhole(p, &buf, &n, bytes + 64)) {
            const size_t copy = n < bytes ? n : bytes;
            std::memcpy(def.drawnPixels, buf, copy);
            free(buf);
        }
        def.drawnDirty = false;
    }

    // ── Legacy migration ──────────────────────────────────────────────────
    // Older docs stored a per-frame pixel layer at <anim>/fNN.fb. Convert
    // each non-empty legacy file into a DrawnAnim object placed at (0,0)
    // on the matching frame. The `.fb` files stay on disk until the next
    // save, which deletes them.
    for (uint8_t i = 0; i < doc.frames.size(); i++) {
        char fp[48];
        legacyFramePath(animId, i, fp, sizeof(fp));
        uint8_t* fbBuf = nullptr;
        size_t fbLen = 0;
        if (!readWhole(fp, &fbBuf, &fbLen, pixelBytes + 64)) continue;

        bool anyBitsSet = false;
        const size_t scan = fbLen < pixelBytes ? fbLen : pixelBytes;
        for (size_t b = 0; b < scan; b++) {
            if (fbBuf[b]) { anyBitsSet = true; break; }
        }
        if (!anyBitsSet) {
            free(fbBuf);
            continue;
        }

        ObjectDef migrated{};
        migrated.type = ObjectType::DrawnAnim;
        newObjectId(doc, migrated.id, sizeof(migrated.id));
        migrated.drawnW = doc.w;
        migrated.drawnH = doc.h;
        if (!allocDrawnPixels(migrated, pixelBytes)) {
            free(fbBuf);
            continue;
        }
        std::memcpy(migrated.drawnPixels, fbBuf, scan);
        migrated.drawnDirty = true;
        free(fbBuf);

        ObjectPlacement pp{};
        std::strncpy(pp.objId, migrated.id, sizeof(pp.objId) - 1);
        pp.x = 0;
        pp.y = 0;
        pp.z = 0;
        pp.scale = 0;
        pp.phaseMs = 0;
        doc.frames[i].placements.push_back(pp);
        doc.objects.push_back(std::move(migrated));
        doc.dirty = true;  // forces a clean rewrite on next save
    }

    return true;
}

// ── Save ──────────────────────────────────────────────────────────────────

bool save(AnimDoc& doc) {
    if (!doc.animId[0] || doc.frames.empty()) return false;
    if (!ensureAnimDir(doc.animId)) return false;

    if (doc.createdAt == 0) doc.createdAt = (uint32_t)time(nullptr);
    doc.editedAt = (uint32_t)time(nullptr);

    BadgeMemory::PsramJsonDocument root(8 * 1024);
    docToJson(doc, root);
    if (root.overflowed()) return false;

    size_t need = measureJson(root) + 1;
    char* out = static_cast<char*>(BadgeMemory::allocPreferPsram(need));
    if (!out) return false;
    size_t wrote = serializeJson(root, out, need);
    out[wrote] = '\0';

    char info[48];
    infoJsonPath(doc.animId, info, sizeof(info));
    bool ok = Filesystem::writeFileAtomic(info, out, wrote);
    free(out);
    if (!ok) return false;

    // Write pixel data for every dirty DrawnAnim.
    for (auto& def : doc.objects) {
        if (def.type != ObjectType::DrawnAnim) continue;
        if (!def.drawnDirty || !def.drawnPixels) continue;
        const size_t bytes = xbmBytes(def.drawnW, def.drawnH);
        if (!writeDrawnFile(doc.animId, def.id, def.drawnPixels, bytes)) {
            return false;
        }
        def.drawnDirty = false;
    }

    // Garbage-collect orphan o<id>.fb files (DrawnAnims that were deleted
    // since the previous save) and any legacy fNN.fb files.
    {
        Filesystem::IOLock lock;
        FATFS* fs = replay_get_fatfs();
        if (fs) {
            char dir[40];
            animDirPath(doc.animId, dir, sizeof(dir));
            FF_DIR dh;
            FILINFO fno;
            if (f_opendir(fs, &dh, dir) == FR_OK) {
                while (f_readdir(&dh, &fno) == FR_OK && fno.fname[0]) {
                    if (fno.fattrib & AM_DIR) continue;
                    const char* nm = fno.fname;
                    // Legacy fNN.fb — always cleanup.
                    if (nm[0] == 'f' && std::strstr(nm, ".fb")) {
                        char p[80];
                        std::snprintf(p, sizeof(p), "%s/%s", dir, nm);
                        f_unlink(fs, p);
                        continue;
                    }
                    // o<id>.fb — keep only if the id is in objects[].
                    if (nm[0] == 'o' && std::strstr(nm, ".fb")) {
                        bool keep = false;
                        for (const auto& def : doc.objects) {
                            if (def.type != ObjectType::DrawnAnim) continue;
                            char want[16];
                            std::snprintf(want, sizeof(want), "o%s.fb", def.id);
                            if (std::strcmp(nm, want) == 0) { keep = true; break; }
                        }
                        if (!keep) {
                            char p[80];
                            std::snprintf(p, sizeof(p), "%s/%s", dir, nm);
                            f_unlink(fs, p);
                        }
                    }
                }
                f_closedir(&dh);
            }
        }
    }

    doc.dirty = false;
    return true;
}

// ── Remove / duplicate ────────────────────────────────────────────────────

bool removeAnim(const char* animId) {
    if (!animId || !animId[0]) return false;
    char dir[40];
    animDirPath(animId, dir, sizeof(dir));
    return removeDir(dir);
}

bool duplicateAnim(const char* animId, char* newIdOut, size_t newIdCap) {
    if (!animId || !animId[0] || !newIdOut || newIdCap < kAnimIdLen + 1) {
        return false;
    }

    AnimDoc src;
    if (!load(animId, src)) return false;

    AnimDoc dup;
    dup.w = src.w;
    dup.h = src.h;
    newAnimId(dup.animId, sizeof(dup.animId));
    std::snprintf(dup.name, sizeof(dup.name), "%.*s copy",
                  (int)kAnimNameMax - 5, src.name[0] ? src.name : "Untitled");
    dup.createdAt = (uint32_t)time(nullptr);
    dup.editedAt  = dup.createdAt;
    // Frame placements copy verbatim (object ids are preserved across the
    // duplication so they stay paired with the cloned ObjectDefs).
    dup.frames = src.frames;
    // Deep-copy ObjectDefs, allocating fresh pixel buffers for DrawnAnims.
    dup.objects.reserve(src.objects.size());
    for (const auto& srcDef : src.objects) {
        ObjectDef def = srcDef;
        def.drawnPixels = nullptr;
        def.drawnDirty = true;
        if (srcDef.type == ObjectType::DrawnAnim && srcDef.drawnPixels) {
            const size_t bytes = xbmBytes(srcDef.drawnW, srcDef.drawnH);
            if (!allocDrawnPixels(def, bytes)) {
                freeAll(src);
                freeAll(dup);
                return false;
            }
            std::memcpy(def.drawnPixels, srcDef.drawnPixels, bytes);
        }
        dup.objects.push_back(std::move(def));
    }
    freeAll(src);

    bool ok = save(dup);
    if (ok) {
        std::strncpy(newIdOut, dup.animId, kAnimIdLen);
        newIdOut[kAnimIdLen] = '\0';
    }
    freeAll(dup);
    return ok;
}

}  // namespace draw
