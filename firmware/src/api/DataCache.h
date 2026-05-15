// DataCache.h — schedule / speakers / floors content cache.
//
// Resolution chain:
//   1) Read /data.bin from FatFS — verify magic + version + CRC32.
//   2) On corrupt/missing FatFS, optionally accept a caller-supplied bundle
//      fetcher and atomically write /data.bin.
//   3) Last resort: use the bundle linked into firmware via embed_files.
//
// Bundle format (matches data/build-data.py):
//   <magic u32 = 'TBDS'> <version u16> <flags u16> <crc32 u32>
//   <total_sz u32> <sched_len u32> <speak_len u32> <floors_len u32>
//   <schedule msgpack> <speakers msgpack> <floors msgpack>
// CRC32 covers everything from total_sz onward.

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace DataCache {

enum class Source : uint8_t {
    NONE     = 0,
    FATFS    = 1,
    EMBEDDED = 2,
};

// Outcome of the most recent refresh() call this session.
enum class RefreshOutcome : uint8_t {
    NONE        = 0,   // refresh() never called this session
    FRESH       = 1,   // new bytes supplied; /data.bin replaced
    NOT_MODIFIED= 2,   // caller reported no change
    FAILED      = 3,   // download or validation failed
};

struct Span {
    const uint8_t* data;
    size_t         len;
};

// Caller-supplied bundle fetcher. Implementations should:
//   - perform an HTTP(S) GET against the data endpoint
//   - on a fresh body: ps_malloc() a buffer, copy bytes in, set *outBuf and
//     *outLen, and return true. The caller (DataCache) takes ownership.
//   - on "no change" (e.g. server returned 304): return true with
//     *outBuf=nullptr and *outLen=0. The caller leaves current spans intact.
//   - on any error: return false (no allocation).
using DownloadFn = bool (*)(uint8_t** outBuf, size_t* outLen);

// RAII helper: hold one of these for the entire duration of using the
// pointers returned by schedule()/speakers()/floors(). A network refresh on
// another task can free the underlying buffer the moment you release the
// lock. Recursive — same task can re-enter safely.
class ReadLock {
public:
    ReadLock();
    ~ReadLock();
    ReadLock(const ReadLock&)            = delete;
    ReadLock& operator=(const ReadLock&) = delete;
};

// Loads from FatFS → on failure, optionally calls `download` → on failure,
// uses embedded copy. Pass nullptr for `download` to skip the network step
// (e.g. when WiFi isn't connected yet).
//
// Safe to call repeatedly. Returns true if any source succeeded.
bool init(DownloadFn download = nullptr);

// Force a refresh: download → validate → write /data.bin → swap active spans.
// Call after WiFi comes up if source() == EMBEDDED, or for periodic resync.
bool refresh(DownloadFn download);

// Default DownloadFn is offline and always unavailable. Kept so old callsites
// can compile while firmware no longer reaches out to the Replay API.
bool defaultDownloader(uint8_t** outBuf, size_t* outLen);

Source         source();
RefreshOutcome lastRefreshOutcome();

// Short (≤4 chars) debug label combining source + last refresh outcome.
// Useful for an on-screen indicator: "EMB" / "CCH" / "NEW" / "SYN" / "ERR".
const char*    statusLabel();

Span schedule();
Span speakers();
Span floors();

}  // namespace DataCache
