#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "Scheduler.h"

class oled;
class Inputs;
class Config;

// ---------------------------------------------------------------------------
//  Filesystem helpers — central lock + atomic-write API.
//
//  Why this exists:
//    The on-flash FATFS driver (oofatfs) is built with FF_FS_REENTRANT=0
//    via mpconfigport.h, so it has zero internal thread safety.  Multiple
//    journal writers (BadgeBoops, BadgeInfo, BadgeConfig, StartupFiles,
//    FileBrowser) all hit the same
//    FATFS instance from independent tasks/cores.  Without a shared lock
//    a high-frequency writer can race a slower one inside FatFs's
//    directory-cluster bookkeeping — the symptom is JSON body bytes
//    showing up as fake 8.3 directory entries (e.g. `f|/{"entrie.s":|0`)
//    after a heavy write window.
//
//  Public surface:
//    `Filesystem::IOLock` — RAII guard.  Recursive: a task that already
//      holds the lock can construct another guard without deadlocking.
//      Every direct f_* call in the firmware should be wrapped in one.
//
//    Atomic write / read / remove helpers wrap the IOLock for callers
//      that don't need raw access.  Use these for any new code.
// ---------------------------------------------------------------------------

namespace Filesystem {

struct IOLock {
    IOLock();
    ~IOLock();
    IOLock(const IOLock&) = delete;
    IOLock& operator=(const IOLock&) = delete;
};

// Atomic write: serialises `data` into <path>.tmp, fsyncs, closes,
// unlinks the live `path`, then renames .tmp over it.  Internally takes
// the IOLock for the whole sequence so no other task can interleave
// inside FATFS while the directory cluster is being mutated.  Returns
// true iff the file is on disk with the requested contents.
bool writeFileAtomic(const char* path, const void* data, size_t len);

// Read the entire file into a freshly-malloc'd buffer.  Caller must
// free(*outBuf) on success.  Buffer is NUL-terminated for convenience
// (one extra byte beyond the reported `*outLen`).  Internally takes
// the IOLock.  Returns false if the file is missing, oversized
// (`> maxLen`), or the read failed.
bool readFileAlloc(const char* path, char** outBuf, size_t* outLen,
                   size_t maxLen = 64 * 1024);

// Remove a file.  No-op (returns true) if the file is already missing.
// Internally takes the IOLock.
bool removeFile(const char* path);

// File size in bytes, or -1 if missing / fs unavailable.  Internally
// takes the IOLock.
int32_t fileSize(const char* path);

// True iff the path exists.  Internally takes the IOLock.
bool fileExists(const char* path);

}  // namespace Filesystem


// ---------------------------------------------------------------------------
//  FileBrowser — OLED config editor + simple file viewer.
//  Activated by holding A+B for ~1 second.
//  Disables the normal OLED service while active.
//
//  The default view is the config editor. A opens the file list; B exits.
//  The file list reads from /ffat if mounted by MicroPython, otherwise empty.
// ---------------------------------------------------------------------------

class FileBrowser : public IService {
 public:
  void begin(oled* display, const Inputs* inputs, Config* config);

  void service() override;
  const char* name() const override;

  bool isActive() const { return state_ != State::Inactive; }

  void activate();
  void deactivate();

 private:
  enum class State : uint8_t {
    Inactive,
    ConfigEdit,
    FileList,
    FileView,
  };

  void serviceConfigEdit(uint32_t nowMs);
  void serviceFileList();
  void serviceFileView();

  void scanDirectory(const char* path);
  void openSelectedFile();
  void renderConfigEdit();
  void renderFileList();
  void renderFileView();

  oled* display_ = nullptr;
  const Inputs* inputs_ = nullptr;
  Config* config_ = nullptr;

  State state_ = State::Inactive;
  uint32_t activateHoldStartMs_ = 0;
  uint32_t lastJoyAdjustMs_ = 0;
  uint32_t lastJoyNavMs_ = 0;

  static constexpr uint8_t kMaxFiles = 50;
  static constexpr uint8_t kMaxNameLen = 28;
  static constexpr uint8_t kVisibleRows = 6;
  static constexpr uint32_t kActivateHoldMs = 1000;
  static constexpr uint32_t kJoyRepeatMs = 80;
  static constexpr uint16_t kJoyDeadband = 400;

  char fileNames_[kMaxFiles][kMaxNameLen];
  uint32_t fileSizes_[kMaxFiles];
  uint8_t fileCount_ = 0;
  uint8_t cursor_ = 0;
  uint8_t scroll_ = 0;

  static constexpr uint16_t kViewBufSize = 600;
  char viewBuf_[kViewBufSize];
  uint16_t viewLen_ = 0;
  uint8_t viewTotalLines_ = 0;
  char viewTitle_[kMaxNameLen];
};

#endif
