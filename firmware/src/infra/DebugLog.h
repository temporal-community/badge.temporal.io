// DebugLog.h — per-category Serial log gates.
//
// High-frequency event prints that spam the serial console (IR
// self-echo drops, boop beacon retx, etc.) are wrapped with the
// macros below so they can be silenced
// from `settings.txt` without recompiling.
//
// Usage:
//   #include "DebugLog.h"
//   LOG_IR("[BadgeIR] self-echo dropped (type=0x%02x w=%u)\n", t, n);
//
// Categories cover the noisy modules:
//
//   LOG_IR     → BadgeIR frame-level:
//                self-echo, TX, ACK, NEED, MANIFEST, DATA chunks)
//   LOG_BOOP   → BadgeBoops + BoopFeedback (pairing protocol:
//                beacon lock, retx meta, field TX/RX, recording)
//   LOG_NOTIFY → reserved legacy notification category
//   LOG_ZIGMOJI → reserved legacy zigmoji category
//   LOG_IMU     → IMU samples/orientation thresholds/flip transitions
//
// Boot banners, init messages, and error / failure paths keep
// using plain `Serial.printf` — they're rare and always useful.
//
// Each macro expands to a `do { if (gate) Serial.printf(...); } while(0)`
// so it's safe to use in `if`/`else` without extra braces, and the
// gate check is one `LDR + CBZ` — effectively free when disabled.

#pragma once

#include <Arduino.h>
#include "BadgeConfig.h"

#define LOG_IR(...)      do { if (badgeConfig.get(kLogIr))     Serial.printf(__VA_ARGS__); } while (0)
#define LOG_BOOP(...)    do { if (badgeConfig.get(kLogBoop))   Serial.printf(__VA_ARGS__); } while (0)
#define LOG_NOTIFY(...)  do { if (badgeConfig.get(kLogNotify)) Serial.printf(__VA_ARGS__); } while (0)
#define LOG_ZIGMOJI(...) do { if (badgeConfig.get(kLogZigmoji)) Serial.printf(__VA_ARGS__); } while (0)
#define LOG_IMU(...)     do { if (badgeConfig.get(kLogImu))    Serial.printf(__VA_ARGS__); } while (0)
