#pragma once

#include <Arduino.h>
#include <stdint.h>

struct JoyRamp {
  int8_t dir = 0;
  uint32_t lastMs = 0;
  uint8_t reps = 0;

  void reset() {
    dir = 0;
    lastMs = 0;
    reps = 0;
  }

  bool tick(int8_t newDir, uint32_t nowMs,
            uint32_t startMs = 350,
            uint32_t minMs = 100,
            uint32_t stepMs = 50) {
    if (newDir == 0) {
      reset();
      return false;
    }

    if (newDir != dir) {
      dir = newDir;
      lastMs = nowMs;
      reps = 0;
      return true;
    }

    const int32_t remaining =
        static_cast<int32_t>(startMs) -
        static_cast<int32_t>(reps) * static_cast<int32_t>(stepMs);
    const uint32_t interval =
        remaining > static_cast<int32_t>(minMs)
            ? static_cast<uint32_t>(remaining)
            : minMs;

    if ((nowMs - lastMs) >= interval) {
      lastMs = nowMs;
      if (reps < 255) reps++;
      return true;
    }
    return false;
  }
};
