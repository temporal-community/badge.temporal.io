// mp_api_kv.cpp — NVS-backed badge.kv_get/put/delete/keys.
//
// Game saves, high scores, user prefs that must survive a `fatfs.bin`
// reflash live here. NVS is invariant across every flash path the
// firmware ships today (see firmware/docs/STORAGE-MODEL.md).
//
// Wire format on NVS: every value is stored as a Preferences "bytes"
// blob whose first byte is an ASCII type tag (s/i/f/b) and the rest
// is the payload. Keeping the tag in-band means we don't need a
// parallel "type" column and `kv_get` can recover the original Python
// type without a second NVS read.

#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nvs.h"

extern "C" {
#include "../../../micropython/usermods/temporalbadge/temporalbadge_runtime.h"
}

namespace {

constexpr const char *kNvsNamespace = "badge_kv";
constexpr size_t kMaxKeyLen = 15;       // NVS hard limit is 15 chars
constexpr size_t kMaxValueLen = 1024;   // 1 KB cap per value
constexpr size_t kMaxKeyCount = 64;     // soft cap; matches docs

bool keyValid(const char *key) {
    if (!key || key[0] == '\0') return false;
    size_t n = strlen(key);
    if (n > kMaxKeyLen) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = key[i];
        // NVS allows any printable ASCII in keys, but we restrict to
        // a sane subset so JSON listings don't need escaping.
        if (c < 0x21 || c > 0x7e) return false;
        if (c == '"' || c == '\\') return false;
    }
    return true;
}

}  // namespace

extern "C" int temporalbadge_runtime_kv_put(const char *key, char type,
                                             const uint8_t *data, size_t len) {
    if (!keyValid(key)) return -1;
    if (type != 's' && type != 'i' && type != 'f' && type != 'b') return -1;
    if (len > kMaxValueLen) return -1;
    if (len > 0 && data == nullptr) return -1;

    // Build tagged blob: <type><payload>.
    uint8_t stack_buf[256];
    uint8_t *blob = stack_buf;
    bool heap = false;
    if (1 + len > sizeof(stack_buf)) {
        blob = static_cast<uint8_t *>(malloc(1 + len));
        if (!blob) return -1;
        heap = true;
    }
    blob[0] = static_cast<uint8_t>(type);
    if (len > 0) memcpy(blob + 1, data, len);

    Preferences p;
    if (!p.begin(kNvsNamespace, false)) {
        if (heap) free(blob);
        return -1;
    }
    size_t wrote = p.putBytes(key, blob, 1 + len);
    p.end();
    if (heap) free(blob);
    return wrote == (1 + len) ? 0 : -1;
}

extern "C" int temporalbadge_runtime_kv_get(const char *key, char *out_type,
                                             uint8_t *buf, size_t buf_cap) {
    if (!keyValid(key)) return -1;
    Preferences p;
    if (!p.begin(kNvsNamespace, true)) return -1;
    if (!p.isKey(key)) {
        p.end();
        return -1;
    }
    size_t total = p.getBytesLength(key);
    if (total == 0 || total > kMaxValueLen + 1) {
        p.end();
        return -1;
    }
    // Read the tag first to size the caller's buffer check.
    uint8_t tmp[kMaxValueLen + 1];
    size_t got = p.getBytes(key, tmp, total);
    p.end();
    if (got != total) return -1;

    char tag = static_cast<char>(tmp[0]);
    if (tag != 's' && tag != 'i' && tag != 'f' && tag != 'b') return -1;
    size_t payload_len = total - 1;
    if (payload_len > buf_cap) return -1;
    if (out_type) *out_type = tag;
    if (payload_len > 0 && buf) memcpy(buf, tmp + 1, payload_len);
    return static_cast<int>(payload_len);
}

extern "C" int temporalbadge_runtime_kv_delete(const char *key) {
    if (!keyValid(key)) return -1;
    Preferences p;
    if (!p.begin(kNvsNamespace, false)) return -1;
    if (!p.isKey(key)) {
        p.end();
        return -1;
    }
    bool ok = p.remove(key);
    p.end();
    return ok ? 0 : -1;
}

extern "C" int temporalbadge_runtime_kv_keys(char *buf, size_t buf_cap) {
    if (!buf || buf_cap < 3) return -1;

    // ESP32 Preferences doesn't expose key iteration directly; we have
    // to drop to nvs_iterator_t for this one call.
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", kNvsNamespace, NVS_TYPE_BLOB, &it);
    if (err != ESP_OK || !it) {
        // Empty namespace.
        if (it) nvs_release_iterator(it);
        strncpy(buf, "[]", buf_cap);
        buf[buf_cap - 1] = '\0';
        return 2;
    }

    size_t pos = 0;
    auto append = [&](const char *s, size_t n) -> bool {
        if (pos + n + 1 > buf_cap) return false;
        memcpy(buf + pos, s, n);
        pos += n;
        return true;
    };

    if (!append("[", 1)) {
        nvs_release_iterator(it);
        return -1;
    }
    bool first = true;
    size_t count = 0;
    while (it && count < kMaxKeyCount) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        const char *k = info.key;
        size_t klen = strlen(k);
        if (!first) {
            if (!append(",", 1)) { nvs_release_iterator(it); return -1; }
        }
        first = false;
        if (!append("\"", 1) || !append(k, klen) || !append("\"", 1)) {
            nvs_release_iterator(it);
            return -1;
        }
        ++count;
        err = nvs_entry_next(&it);
        if (err != ESP_OK) break;
    }
    nvs_release_iterator(it);
    if (!append("]", 1)) return -1;
    buf[pos] = '\0';
    return static_cast<int>(pos);
}
