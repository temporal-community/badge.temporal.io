#pragma once

#include <Arduino.h>

#include "../infra/Scheduler.h"

class LEDmatrix;

class LEDAppRuntime : public IService {
 public:
  enum class Mode : uint8_t {
    Temporal = 0,
    Replay,
    Sparkle,
    Rain,
    Wave,
    Life,
    LifeRandom,
    Custom,
    Off,
    PythonApp,
  };

  static constexpr uint8_t kPythonSlugCap = 32;

  static constexpr uint8_t kFrameRows = 8;
  static constexpr uint8_t kStaleGrace = 8;
  static constexpr uint16_t kDefaultDelay = 120;
  static constexpr uint8_t kDefaultBrightness = 20;
  using Frame = uint8_t[kFrameRows];

  void begin(LEDmatrix* matrix);
  void service() override;
  const char* name() const override { return "LEDAppRuntime"; }

  void restoreAmbient();
  void beginPreview(Mode mode, const uint8_t* draft = nullptr);
  void updatePreview(Mode mode, const uint8_t* draft = nullptr);
  void endPreview();

  void commitMode(Mode mode);
  void commitMode(Mode mode, uint16_t delay, uint8_t brightness);
  void commitLife(const uint8_t* seed);
  void commitCustom(const uint8_t* pattern);

  // Persist a Python-driven ambient matrix app. The slug names a folder
  // under /apps/ whose matrix.py registers a callback via
  // badge.matrix_app_start(...). After commit, MicroPythonMatrixService
  // drains the pending-exec request and sources the registration script.
  void commitMatrixApp(const char* slug);
  const char* pythonAppSlug() const { return state_.pythonAppSlug; }
  // Pop-and-clear the pending exec slug. Returns true and copies the
  // slug into `out` (NUL-terminated) when there is work to do.
  bool consumePendingExec(char* out, size_t cap);

  void beginOverride();
  void endOverride();

  Mode mode() const { return state_.mode; }
  uint16_t delay() const { return state_.delay; }
  uint8_t brightness() const { return state_.brightness; }
  void setDelay(uint16_t d) { state_.delay = d < 5 ? 5 : (d > 10000 ? 10000 : d); }
  void setBrightness(uint8_t b) { state_.brightness = b; }
  const uint8_t* lifeSeed() const { return state_.lifeSeed; }
  const uint8_t* customPattern() const { return state_.custom; }

  static const char* modeId(Mode mode);
  static const char* modeName(Mode mode);
  static Mode modeAt(uint8_t index);
  static uint8_t modeIndex(Mode mode);
  static uint8_t modeCount();
  static void posterFrame(Mode mode, const uint8_t* lifeSeed,
                          const uint8_t* custom, uint8_t out[kFrameRows]);

 private:
  struct State {
    Mode mode = Mode::Temporal;
    uint16_t delay = kDefaultDelay;
    uint8_t brightness = kDefaultBrightness;
    uint8_t lifeSeed[kFrameRows] = {
        0x00, 0x00, 0x20, 0x10, 0x70, 0x00, 0x00, 0x00,
    };
    uint8_t custom[kFrameRows] = {
        0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00,
    };
    bool lifeRandomize = false;
    char pythonAppSlug[kPythonSlugCap] = {};
  };

  static bool isLifeMode(Mode m) {
    return m == Mode::Life || m == Mode::LifeRandom;
  }

  void loadState();
  void saveState();
  void restartActiveMode();
  void stopNativeAnimation();
  void drawFrame(const uint8_t frame[kFrameRows]);
  void copyFrame(uint8_t dst[kFrameRows], const uint8_t* src,
                 const uint8_t fallback[kFrameRows]);
  const uint8_t* activeSeed() const;
  Mode activeMode() const;
  uint16_t activeInterval() const;
  void buildFrame(Mode mode, uint8_t out[kFrameRows]);
  void buildLifeFrame(uint8_t out[kFrameRows]);
  uint8_t randomByte();

  LEDmatrix* matrix_ = nullptr;
  State state_;
  bool loaded_ = false;

  bool previewActive_ = false;
  Mode previewMode_ = Mode::Temporal;
  uint8_t previewDraft_[kFrameRows] = {};
  bool previewHasDraft_ = false;

  Mode runningMode_ = Mode::Off;
  bool runningValid_ = false;
  uint32_t lastTickMs_ = 0;
  uint32_t frameIndex_ = 0;
  uint8_t life_[kFrameRows] = {};
  uint8_t lifeNext_[kFrameRows] = {};
  uint8_t lifePrev_[kFrameRows] = {};
  uint8_t lifePrev2_[kFrameRows] = {};
  uint8_t staleCount_ = 0;
  bool lifeRestart_ = false;
  uint8_t rain_[kFrameRows] = {};
  uint32_t rng_ = 0x43C9A1D7;
  uint8_t overrideDepth_ = 0;

  bool pendingPyExec_ = false;
  char pendingPySlug_[kPythonSlugCap] = {};
};

extern LEDAppRuntime ledAppRuntime;

extern "C" void led_app_runtime_restore_ambient(void);
extern "C" void led_app_runtime_begin_override(void);
extern "C" void led_app_runtime_end_override(void);
