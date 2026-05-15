#include "StartupFiles.h"

#include <Arduino.h>
#include <cstring>

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

#include "../infra/Filesystem.h"
#include "StartupFilesData.h"

// ── Dev force-refresh helpers ─────────────────────────────────────────────
//
// Source 1: compile-time `-DBADGE_DEV_FORCE_REFRESH="..."` (string literal).
// Source 2: runtime marker file `/dev_force_refresh.txt`. Read once per
//           provisioning pass, cached in s_runtime_force_refresh, deleted
//           after the pass so it's a one-shot.
//
// Both sources concatenate. Entries are comma- or newline-separated;
// trailing `/` or `*` makes the entry a prefix match.

static constexpr const char* kForceRefreshMarker = "/dev_force_refresh.txt";
static char s_runtime_force_refresh[1024] = {};
static bool s_runtime_marker_loaded     = false;
static bool s_runtime_marker_existed    = false;

static void loadForceRefreshMarker(FATFS* fs) {
    if (s_runtime_marker_loaded) return;
    s_runtime_marker_loaded = true;

    FIL fil;
    if (f_open(fs, &fil, kForceRefreshMarker, FA_READ) != FR_OK) {
        return;
    }
    s_runtime_marker_existed = true;
    UINT n = 0;
    FRESULT res = f_read(&fil, s_runtime_force_refresh,
                         sizeof(s_runtime_force_refresh) - 1, &n);
    f_close(&fil);
    if (res != FR_OK) {
        s_runtime_force_refresh[0] = '\0';
        return;
    }
    s_runtime_force_refresh[n] = '\0';
}

static void consumeForceRefreshMarker(FATFS* fs) {
    if (!s_runtime_marker_existed) return;
    f_unlink(fs, kForceRefreshMarker);
    Serial.printf("[startup] consumed %s (one-shot dev marker)\n",
                  kForceRefreshMarker);
}

// Match a single path against a comma/newline-separated list of entries.
// Each entry is trimmed; `#` starts a line comment. A trailing `/` or
// `*` makes the entry a prefix match; otherwise it's an exact match.
static bool matchInList(const char* path, const char* list) {
    if (!path || !list || !*list) return false;

    const char* p = list;
    while (*p) {
        // Skip separators.
        while (*p == ',' || *p == '\n' || *p == '\r' ||
               *p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) break;

        // Read one entry into a small stack buffer.
        char entry[80];
        size_t ei = 0;
        while (*p && *p != ',' && *p != '\n' && *p != '\r' && *p != '#' &&
               ei + 1 < sizeof(entry)) {
            entry[ei++] = *p++;
        }
        // Skip past the rest of the line if we hit `#` or ran out of buf.
        while (*p && *p != ',' && *p != '\n') p++;

        // Right-trim spaces.
        while (ei > 0 && (entry[ei - 1] == ' ' || entry[ei - 1] == '\t')) {
            ei--;
        }
        entry[ei] = '\0';
        if (ei == 0) continue;

        // Treat trailing `*` or `/` as a prefix marker.
        bool prefix = false;
        if (entry[ei - 1] == '*') {
            entry[--ei] = '\0';
            prefix = true;
        }
        if (ei > 0 && entry[ei - 1] == '/') {
            // Keep the slash in the comparison so `/apps/foo/` doesn't
            // accidentally match `/apps/foobar.py`.
            prefix = true;
        }

        if (prefix) {
            if (strncmp(path, entry, ei) == 0) return true;
        } else if (strcmp(path, entry) == 0) {
            return true;
        }
    }
    return false;
}

static bool shouldForceRefresh(const char* path) {
#ifdef BADGE_DEV_FORCE_REFRESH
    if (matchInList(path, BADGE_DEV_FORCE_REFRESH)) return true;
#endif
    if (s_runtime_force_refresh[0] != '\0' &&
        matchInList(path, s_runtime_force_refresh)) {
        return true;
    }
    return false;
}

// ── FNV-1a hash (matches the Python generator) ─────────────────────────────

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193;
    }
    return h;
}

static bool fnv1a32_file(FATFS* fs, const char* path, uint32_t& outHash) {
    FIL fil;
    if (f_open(fs, &fil, path, FA_READ) != FR_OK) return false;

    uint32_t h = 0x811C9DC5;
    uint8_t buf[256];
    UINT bytesRead;

    for (;;) {
        if (f_read(&fil, buf, sizeof(buf), &bytesRead) != FR_OK) {
            f_close(&fil);
            return false;
        }
        if (bytesRead == 0) break;
        for (UINT i = 0; i < bytesRead; i++) {
            h ^= buf[i];
            h *= 0x01000193;
        }
    }

    f_close(&fil);
    outHash = h;
    return true;
}

// ── File helpers ────────────────────────────────────────────────────────────

static bool writeFile(FATFS* fs, const char* path, const char* data, uint32_t len) {
    f_unlink(fs, path);

    FIL fil;
    if (f_open(fs, &fil, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        Serial.printf("[startup] FAILED to create %s\n", path);
        return false;
    }

    UINT written = 0;
    FRESULT res = f_write(&fil, data, len, &written);
    f_sync(&fil);
    f_close(&fil);

    // FATFS returns FR_OK with written < len when the volume is full.
    // Treat any short write as a failure AND clean up the dirent so we
    // don't leave a 0-byte ghost behind that later boots will see as
    // "user-modified" via the historical-hash check. Without this, a
    // single out-of-space write at first boot poisons every subsequent
    // boot's view of the file ("file exists, hash unknown → preserve
    // user edits → never overwrite") and Python apps fail forever with
    // `can't import name run_app`.
    if (res != FR_OK || written != len) {
        Serial.printf("[startup] FAILED to write %s (%u/%u, fr=%d)%s\n",
                      path, written, len, (int)res,
                      (res == FR_OK && written != len) ? " (NO SPACE)" : "");
        f_unlink(fs, path);
        return false;
    }
    return true;
}

static bool fileExists(FATFS* fs, const char* path) {
    FILINFO fno;
    return f_stat(fs, path, &fno) == FR_OK;
}

// Returns true and fills outSize when the file exists. False on stat
// failure (treated as "unknown"). Used by the empty-file recovery
// path in provisionStartupFiles.
static bool fileSize(FATFS* fs, const char* path, uint32_t& outSize) {
    FILINFO fno;
    if (f_stat(fs, path, &fno) != FR_OK) return false;
    outSize = static_cast<uint32_t>(fno.fsize);
    return true;
}

static void ensureDir(FATFS* fs, const char* path) {
    if (!fileExists(fs, path)) {
        f_mkdir(fs, path);
    }
}

// ── Force-sync: remove files not in the startup set ─────────────────────────

static bool isInStartupSet(const char* fullPath) {
    for (int i = 0; i < kStartupFileCount; i++) {
        if (strcmp(fullPath, kStartupFiles[i].path) == 0) return true;
    }
    return false;
}

static void cleanManagedDir(FATFS* fs, const char* dirPath) {
    FF_DIR dir;
    FILINFO fno;

    if (f_opendir(fs, &dir, dirPath) != FR_OK) return;

    char fullPath[128];
    char toDelete[20][128];
    int deleteCount = 0;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fattrib & AM_DIR) continue;

        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, fno.fname);

        if (!isInStartupSet(fullPath) && deleteCount < 20) {
            strncpy(toDelete[deleteCount], fullPath, sizeof(toDelete[0]) - 1);
            toDelete[deleteCount][sizeof(toDelete[0]) - 1] = '\0';
            deleteCount++;
        }
    }
    f_closedir(&dir);

    for (int i = 0; i < deleteCount; i++) {
        Serial.printf("[startup] Removing extra file: %s\n", toDelete[i]);
        f_unlink(fs, toDelete[i]);
    }
}

// ── Main provisioning logic ─────────────────────────────────────────────────

void provisionStartupFiles(bool forceSync) {
    if (kStartupFileCount == 0) return;

    // Pre-flight: detect a wedged FAT (free space tiny relative to
    // the partition). Common cause: partitions.csv resized but the
    // on-disk FAT still references the old smaller layout, so writes
    // silently truncate to 0 bytes. Reformat once and re-enter via
    // formatAndReprovisionFFat → provisionStartupFiles(true). Skip in
    // forceSync so the explicit-action path stays predictable. Take
    // the lock in a sub-scope so it's released before we recurse into
    // formatAndReprovisionFFat() (which takes its own lock).
    bool needsReformat = false;
    if (!forceSync) {
        Filesystem::IOLock probeLock;
        FATFS* probeFs = replay_get_fatfs();
        if (probeFs != nullptr) {
            DWORD freeClusters = 0;
            if (f_getfree(probeFs, &freeClusters) == FR_OK) {
                const uint32_t bytesPerCluster =
                    static_cast<uint32_t>(probeFs->csize) * FF_MAX_SS;
                const uint64_t totalBytes =
                    static_cast<uint64_t>(probeFs->n_fatent - 2) *
                    bytesPerCluster;
                const uint64_t freeBytes =
                    static_cast<uint64_t>(freeClusters) * bytesPerCluster;
                if (totalBytes >= (1u << 20) && freeBytes < (16u << 10)) {
                    Serial.printf(
                        "[startup] FAT wedged (free=%u B / total=%u B); "
                        "auto-reformatting\n",
                        static_cast<unsigned>(freeBytes),
                        static_cast<unsigned>(totalBytes));
                    needsReformat = true;
                }
            }
        }
    }
    if (needsReformat) {
        formatAndReprovisionFFat();
        return;
    }

    // Wrap the entire provisioning pass in one IOLock — many f_open /
    // f_write / f_unlink calls in sequence, and we don't want any other
    // journal task interleaving inside FATFS while we're churning the
    // root directory.  The lock is recursive so the static helpers
    // (writeFile/fileExists/etc.) which never re-lock are fine.
    Filesystem::IOLock fsLock;

    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) {
        Serial.println("[startup] FAT not mounted, skipping file provisioning");
        return;
    }

    // Ensure managed directories exist.
    for (int i = 0; i < kStartupDirCount; i++) {
        ensureDir(fs, kStartupDirs[i]);
    }

    // Pull in the dev force-refresh marker (if any). Cached for the
    // remainder of this pass and consumed at the end so it's one-shot.
    loadForceRefreshMarker(fs);
#ifdef BADGE_DEV_FORCE_REFRESH
    Serial.printf("[startup] dev force-refresh (build): %s\n",
                  BADGE_DEV_FORCE_REFRESH);
#endif
    if (s_runtime_marker_existed && s_runtime_force_refresh[0] != '\0') {
        Serial.printf("[startup] dev force-refresh (marker %s): %s\n",
                      kForceRefreshMarker, s_runtime_force_refresh);
    }

    int created = 0, updated = 0, skipped = 0, devForced = 0;

    for (int i = 0; i < kStartupFileCount; i++) {
        const StartupFileInfo& f = kStartupFiles[i];
        bool isProtected = (f.flags & STARTUP_FILE_PROTECTED);
        bool exists = fileExists(fs, f.path);

        if (isProtected && exists) {
            skipped++;
            continue;
        }

        if (!exists) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Created %s\n", f.path);
                created++;
            }
            continue;
        }

        // File exists. Decide whether to overwrite.
        if (forceSync) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Force-synced %s\n", f.path);
                updated++;
            }
            continue;
        }

        // Dev force-refresh — opt-in per file/prefix via build flag or
        // /dev_force_refresh.txt marker. Bypasses the user-edit
        // preservation entirely so iterating on a Python app doesn't
        // require nuking the whole filesystem from Settings.
        if (shouldForceRefresh(f.path)) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Dev force-refreshed %s\n", f.path);
                devForced++;
            }
            continue;
        }

        // Empty-file recovery: nobody intentionally edits a startup
        // file down to zero bytes when the upstream has real content.
        // Without this branch, a corrupted (0-byte) on-FAT file's
        // FNV-1a hash equals the seed (0x811C9DC5), which isn't in
        // any known-historical-default list, so the "user-modified"
        // path stamps the upstream marker and skips the overwrite —
        // leaving the badge stuck with empty libs (`from badge_app
        // import run_app` then fails on every Python app launch).
        // Detect that case here and treat it as missing instead.
        uint32_t diskSize = 0;
        if (f.contentLen > 0 && fileSize(fs, f.path, diskSize) &&
            diskSize == 0) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Recovered empty %s\n", f.path);
                updated++;
            }
            continue;
        }

        // Normal mode: 3-way hash check (same logic as JumperlOS).
        uint32_t currentHash = (f.hashCount > 0) ? f.knownHashes[0] : 0;
        uint32_t diskHash = 0;
        bool hashed = fnv1a32_file(fs, f.path, diskHash);

        if (hashed && diskHash == currentHash) {
            skipped++;
            continue;
        }

        bool isOldFirmwareDefault = false;
        if (hashed) {
            for (int h = 1; h < f.hashCount; h++) {
                if (diskHash == f.knownHashes[h]) {
                    isOldFirmwareDefault = true;
                    break;
                }
            }
        }

        if (isOldFirmwareDefault) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Updated %s (old firmware default)\n", f.path);
                updated++;
            }
        } else {
            // User-modified file with upstream changes. Preserve the
            // user's edits silently — no marker stamping (the previous
            // marker scheme corrupted any binary file whose extension
            // wasn't on the eligibility allow-list).
            skipped++;
        }
    }

    if (forceSync) {
        for (int i = 0; i < kStartupDirCount; i++) {
            cleanManagedDir(fs, kStartupDirs[i]);
        }
    }

    // Consume the runtime marker so the next boot starts clean. The
    // build-flag list (if any) keeps applying — that's the design,
    // it's the persistent dev-build behavior.
    consumeForceRefreshMarker(fs);

    // Most user-facing files (apps, docs, images, doom1.wad) are
    // expected-missing on a freshly-flashed badge that hasn't run
    // `pio run -t uploadfs` / Community Apps install / JumperIDE sync
    // yet. Suppressing per-file warnings here keeps the boot log
    // useful for actual problems. Operators can run
    //     python3 firmware/scripts/badge_sync.py diff $PORT
    // for a full diff against firmware/data/manifest.json.
    if (created > 0 || updated > 0 || devForced > 0) {
        Serial.printf(
            "[startup] Provisioned: %d created, %d updated, %d dev-forced, %d unchanged\n",
            created, updated, devForced, skipped);
    }
}

// ── Emergency FAT reformat ─────────────────────────────────────────────────
//
// In-place `f_mkfs` against the same FATFS object that
// replay_vfs_mount_fat() registered with MicroPython. We don't unmount
// from MicroPython's VFS — we just ask the FATFS driver to lay down a
// fresh super-floppy filesystem on top of the wear-levelled block
// device, then re-call f_mount on the same FATFS struct. After the
// reformat we synchronously reprovision the embedded startup files so
// the badge has a usable /lib + /apps tree before returning.
//
// Only the ffat partition is touched; NVS-backed state (badge UID,
// HMAC secret, contacts, menu order) is preserved.
bool formatAndReprovisionFFat() {
    // Hold the I/O lock for the entire operation — the boops/journal
    // tasks must not interleave a write between our unmount and the
    // mkfs.
    Filesystem::IOLock fsLock;

    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) {
        Serial.println("[startup] formatAndReprovisionFFat: FAT not mounted");
        return false;
    }

    Serial.println("[startup] reformatting ffat partition…");

    // Working buffer for f_mkfs. 4 KiB matches the original mount path
    // in replay_bdev.c; smaller buffers fail on FAT32-eligible volumes.
    constexpr size_t kWorkBufSize = 4096;
    uint8_t* working_buf = static_cast<uint8_t*>(malloc(kWorkBufSize));
    if (!working_buf) {
        Serial.println("[startup] formatAndReprovisionFFat: malloc 4 KiB failed");
        return false;
    }

    // Unmount the FATFS struct so f_mkfs can take it. f_mount(NULL,
    // path, …) would be the upstream incantation; here we just clear
    // the volume's mount flag by remounting onto the same struct after
    // mkfs. f_mkfs operates on the FATFS struct directly via its
    // ->drv pointer (which our bdev layer set up).
    FRESULT res = f_mkfs(fs, FM_ANY | FM_SFD, 0, working_buf, kWorkBufSize);
    free(working_buf);
    if (res != FR_OK) {
        Serial.printf("[startup] f_mkfs failed: %d\n", (int)res);
        return false;
    }

    // Re-attach the freshly-formatted FATFS to its driver so subsequent
    // f_open/f_write calls hit the new on-disk structure.
    res = f_mount(fs);
    if (res != FR_OK) {
        Serial.printf("[startup] post-mkfs f_mount failed: %d\n", (int)res);
        return false;
    }

    Serial.println("[startup] ffat reformatted; reprovisioning embedded files…");

    // Force-sync because every embedded file is now "missing" — but
    // forceSync also rewalks the dir tree to remove stale entries
    // (none after reformat) and is the one path that doesn't depend on
    // the historical-hash logic we're trying to escape from.
    provisionStartupFiles(/*forceSync=*/true);
    Serial.println("[startup] formatAndReprovisionFFat: done");
    return true;
}
