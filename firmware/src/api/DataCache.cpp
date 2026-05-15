#include "DataCache.h"

#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

// Compile-time fallback bundle (linked via board_build.embed_files in
// platformio.ini). If linking fails with "undefined reference", run
//   xtensa-esp32s3-elf-nm <build>/firmware.elf | grep bundle
// to find the actual symbol name and update the asm() aliases below.
extern const uint8_t kBundleEmbedStart[] asm("_binary_bundle_bin_start");
extern const uint8_t kBundleEmbedEnd[]   asm("_binary_bundle_bin_end");

namespace DataCache {

namespace {

constexpr const char* kCachePath  = "/data.bin";
constexpr const char* kTmpPath    = "/data.tmp";
constexpr uint32_t    kBundleMagic   = 0x53444254;  // 'TBDS'
constexpr uint16_t    kBundleVersion = 1;
constexpr size_t      kBundleHeaderSz = 28;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t crc32;
    uint32_t total_sz;
    uint32_t sched_len;
    uint32_t speak_len;
    uint32_t floors_len;
};
#pragma pack(pop)
static_assert(sizeof(Header) == kBundleHeaderSz, "Header size mismatch");

// Active state. Spans point into either an owned heap buffer (FATFS) or
// directly into .rodata (EMBEDDED). All access is serialized via s_mutex.
Source         s_source         = Source::NONE;
RefreshOutcome s_lastOutcome    = RefreshOutcome::NONE;
uint8_t*       s_owned          = nullptr;   // null when using embedded
size_t         s_owned_len      = 0;
Span           s_schedule       = {nullptr, 0};
Span           s_speakers       = {nullptr, 0};
Span           s_floors         = {nullptr, 0};

SemaphoreHandle_t s_mutex = nullptr;

void ensureMutex() {
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
}

// Extract CRC32 from the active bundle's header (offset 8..12). Caller must
// hold s_mutex. Returns 0 if no source is active.
uint32_t activeCrc32_locked() {
    const uint8_t* buf = nullptr;
    if (s_source == Source::FATFS && s_owned)        buf = s_owned;
    else if (s_source == Source::EMBEDDED)           buf = kBundleEmbedStart;
    if (!buf) return 0;
    uint32_t crc;
    memcpy(&crc, buf + 8, 4);
    return crc;
}

// Validates a bundle in `buf` of length `len`. On success, populates the
// out-spans (pointing into buf) and returns true.
bool validate(const uint8_t* buf, size_t len,
              Span& sched, Span& speak, Span& flrs) {
    if (len < kBundleHeaderSz) return false;
    Header h;
    memcpy(&h, buf, sizeof(h));
    if (h.magic   != kBundleMagic)   return false;
    if (h.version != kBundleVersion) return false;
    if (h.total_sz != len)           return false;

    const uint8_t* covered = buf + offsetof(Header, total_sz);
    size_t covered_len = len - offsetof(Header, total_sz);
    uint32_t crc = esp_rom_crc32_le(0, covered, covered_len);
    if (crc != h.crc32) return false;

    if ((size_t)h.sched_len + h.speak_len + h.floors_len + kBundleHeaderSz != len) {
        return false;
    }

    const uint8_t* p = buf + kBundleHeaderSz;
    sched = { p,                               h.sched_len };
    speak = { p + h.sched_len,                 h.speak_len };
    flrs  = { p + h.sched_len + h.speak_len,   h.floors_len };
    return true;
}

// Caller must hold s_mutex.
void releaseOwned_locked() {
    if (s_owned) {
        free(s_owned);
        s_owned = nullptr;
        s_owned_len = 0;
    }
}

// Adopt buf as the active backing buffer (FATFS or fresh-from-network).
// Takes ownership; on failure, frees buf.
bool adoptOwned(uint8_t* buf, size_t len) {
    Span sch, spk, flr;
    if (!validate(buf, len, sch, spk, flr)) {
        free(buf);
        return false;
    }
    ReadLock lock;
    releaseOwned_locked();
    s_owned     = buf;
    s_owned_len = len;
    s_schedule  = sch;
    s_speakers  = spk;
    s_floors    = flr;
    s_source    = Source::FATFS;
    return true;
}

bool useEmbedded() {
    size_t len = (size_t)(kBundleEmbedEnd - kBundleEmbedStart);
    Span sch, spk, flr;
    if (!validate(kBundleEmbedStart, len, sch, spk, flr)) {
        Serial.println("[DataCache] embedded bundle invalid (build mismatch?)");
        ReadLock lock;
        s_source = Source::NONE;
        return false;
    }
    ReadLock lock;
    releaseOwned_locked();
    s_schedule = sch;
    s_speakers = spk;
    s_floors   = flr;
    s_source   = Source::EMBEDDED;
    return true;
}

// Reads /data.bin into a PSRAM-backed buffer and adopts it. Returns false on
// any error (file missing, read fail, header invalid, CRC mismatch).
bool loadFromFatFs() {
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;

    FIL fil;
    if (f_open(fs, &fil, kCachePath, FA_READ) != FR_OK) return false;

    DWORD sz = f_size(&fil);
    if (sz < kBundleHeaderSz || sz > 256 * 1024) {  // sanity cap
        f_close(&fil);
        return false;
    }

    uint8_t* buf = (uint8_t*)ps_malloc(sz);
    if (!buf) {
        f_close(&fil);
        return false;
    }

    UINT got = 0;
    FRESULT res = f_read(&fil, buf, sz, &got);
    f_close(&fil);
    if (res != FR_OK || got != sz) {
        free(buf);
        return false;
    }

    if (!adoptOwned(buf, sz)) {
        Serial.println("[DataCache] /data.bin failed validation — discarding");
        // adoptOwned already freed buf on failure
        f_unlink(fs, kCachePath);
        return false;
    }
    Serial.printf("[DataCache] loaded /data.bin (%u B) from FatFS\n", (unsigned)sz);
    return true;
}

// Atomically replace /data.bin with the bytes in `buf`. Caller still owns buf.
bool writeFatFsAtomic(const uint8_t* buf, size_t len) {
    FATFS* fs = replay_get_fatfs();
    if (!fs) return false;

    f_unlink(fs, kTmpPath);

    FIL fil;
    if (f_open(fs, &fil, kTmpPath, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;

    UINT written = 0;
    FRESULT res = f_write(&fil, buf, len, &written);
    f_sync(&fil);
    f_close(&fil);
    if (res != FR_OK || written != len) {
        f_unlink(fs, kTmpPath);
        return false;
    }

    f_unlink(fs, kCachePath);
    if (f_rename(fs, kTmpPath, kCachePath) != FR_OK) {
        f_unlink(fs, kTmpPath);
        return false;
    }
    return true;
}

// Calls download → validate → write → adopt. Returns true if the active
// bundle is current (either freshly downloaded or 304-confirmed-current).
// Returns false on download/validation failure.
bool tryDownload(DownloadFn download) {
    if (!download) {
        // Note: not setting outcome here — caller didn't actually attempt.
        return false;
    }

    uint8_t* buf = nullptr;
    size_t   len = 0;
    if (!download(&buf, &len)) {
        Serial.println("[DataCache] download failed");
        { ReadLock lock; s_lastOutcome = RefreshOutcome::FAILED; }
        return false;
    }

    // Sentinel for "no change" (e.g. server returned 304 Not Modified).
    if (!buf || len == 0) {
        Serial.println("[DataCache] server reports no change; current bundle retained");
        { ReadLock lock; s_lastOutcome = RefreshOutcome::NOT_MODIFIED; }
        return true;
    }

    // Validate before writing to flash.
    Span sch, spk, flr;
    if (!validate(buf, len, sch, spk, flr)) {
        Serial.println("[DataCache] downloaded bundle invalid — discarding");
        free(buf);
        { ReadLock lock; s_lastOutcome = RefreshOutcome::FAILED; }
        return false;
    }

    if (!writeFatFsAtomic(buf, len)) {
        Serial.println("[DataCache] FatFS write failed; using download in-RAM only");
        // Still adopt so the badge has fresh data this session.
    } else {
        Serial.printf("[DataCache] wrote /data.bin (%u B)\n", (unsigned)len);
    }

    bool adopted = adoptOwned(buf, len);   // takes ownership of buf
    { ReadLock lock;
      s_lastOutcome = adopted ? RefreshOutcome::FRESH : RefreshOutcome::FAILED; }
    return adopted;
}

}  // namespace

bool init(DownloadFn download) {
    if (loadFromFatFs())   return true;
    if (tryDownload(download)) return true;
    return useEmbedded();
}

bool refresh(DownloadFn download) {
    return tryDownload(download);
}

bool defaultDownloader(uint8_t** outBuf, size_t* outLen) {
    if (!outBuf || !outLen) return false;
    *outBuf = nullptr;
    *outLen = 0;
    return false;
}

ReadLock::ReadLock()  { ensureMutex(); if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
ReadLock::~ReadLock() { if (s_mutex) xSemaphoreGiveRecursive(s_mutex); }

Source         source()              { ReadLock lock; return s_source;       }
RefreshOutcome lastRefreshOutcome()  { ReadLock lock; return s_lastOutcome;  }
Span           schedule()            { ReadLock lock; return s_schedule;     }
Span           speakers()            { ReadLock lock; return s_speakers;     }
Span           floors()              { ReadLock lock; return s_floors;       }

const char* statusLabel() {
    ReadLock lock;
    if (s_source == Source::NONE)     return "NUL";
    if (s_source == Source::EMBEDDED) {
        return s_lastOutcome == RefreshOutcome::FAILED ? "ERR" : "EMB";
    }
    // Source::FATFS
    switch (s_lastOutcome) {
        case RefreshOutcome::FRESH:        return "NEW";
        case RefreshOutcome::NOT_MODIFIED: return "SYN";
        case RefreshOutcome::FAILED:       return "STL";   // stale: refresh failed
        case RefreshOutcome::NONE:
        default:                            return "CCH";   // cached, no refresh yet
    }
}

}  // namespace DataCache
