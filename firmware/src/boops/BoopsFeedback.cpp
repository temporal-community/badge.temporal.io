// BoopsFeedback.cpp — Marquee event ring + haptic / LED cues on protocol events.
//
// Marquee state is an ~8-slot ring guarded by a portMUX spinlock — pushed
// from Core 0 (irTask, on protocol transitions) and popped from Core 1
// (GUI render). Spinlock is appropriate because both critical sections
// are tiny memcpys with no I/O.

#include "BadgeBoops.h"

#include "../infra/DebugLog.h"
#include "../hardware/Haptics.h"
#include "../ui/Images.h"
#include "../hardware/LEDmatrix.h"

#include <Arduino.h>
#include <cstring>

#include "freertos/FreeRTOS.h"

extern LEDmatrix badgeMatrix;

static const char* TAG_FB = "BoopFeedback";

// ═══════════════════════════════════════════════════════════════════════════
// 7) Feedback dispatch — haptic / LED cues on protocol events
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Fixed-capacity ring of short chips posted by feedback events and
// consumed by the BoopScreen marquee. Protected by a portMUX spinlock
// because pushers can be on either core (today mostly Core 0, but that
// isn't contractual) and the pops happen on Core 1 during render.

static constexpr uint8_t kMarqueeCap = 16;
static constexpr uint8_t kMarqueeChipLen = 12;
static char s_marquee[kMarqueeCap][kMarqueeChipLen] = {};
static volatile uint8_t s_marqueeHead = 0;
static volatile uint8_t s_marqueeTail = 0;
static portMUX_TYPE s_marqueeMux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

namespace BoopFeedback {

void pushMarqueeEvent(const char* chip) {
    if (!chip || !chip[0]) return;
    portENTER_CRITICAL(&s_marqueeMux);
    uint8_t next = (s_marqueeTail + 1) % kMarqueeCap;
    if (next == s_marqueeHead) {
        // Full — drop oldest.
        s_marqueeHead = (s_marqueeHead + 1) % kMarqueeCap;
    }
    strncpy(s_marquee[s_marqueeTail], chip, kMarqueeChipLen - 1);
    s_marquee[s_marqueeTail][kMarqueeChipLen - 1] = '\0';
    s_marqueeTail = next;
    portEXIT_CRITICAL(&s_marqueeMux);
}

bool popMarqueeEvent(char* out, size_t outCap) {
    if (!out || outCap == 0) return false;
    bool got = false;
    portENTER_CRITICAL(&s_marqueeMux);
    if (s_marqueeHead != s_marqueeTail) {
        strncpy(out, s_marquee[s_marqueeHead], outCap - 1);
        out[outCap - 1] = '\0';
        s_marqueeHead = (s_marqueeHead + 1) % kMarqueeCap;
        got = true;
    }
    portEXIT_CRITICAL(&s_marqueeMux);
    return got;
}

void onBeaconLock(BoopType t, const char* installationId) {
    uint32_t now = millis();
    Haptics::pulse(30, now);
    char chip[kMarqueeChipLen];
    if (t == BOOP_PEER) {
        if (BadgeBoops::boopStatus.peerName[0]) {
            snprintf(chip, sizeof(chip), "PEER %.10s",
                     BadgeBoops::boopStatus.peerName);
        } else {
            snprintf(chip, sizeof(chip), "PEER lock");
        }
    } else {
        snprintf(chip, sizeof(chip), "SITE %s",
                 installationId && installationId[0] ? installationId : "-");
    }
    pushMarqueeEvent(chip);
    LOG_BOOP("[%s] beacon lock type=%u\n", TAG_FB, (unsigned)t);
}

void onPeerFieldTx(uint8_t tag) {
    char chip[kMarqueeChipLen];
    snprintf(chip, sizeof(chip), "TX %s", BadgeBoops::fieldShortName(tag));
    pushMarqueeEvent(chip);
    LOG_BOOP("[%s] field TX tag=%u\n", TAG_FB, tag);
}

void onPeerFieldRx(uint8_t tag) {
    Haptics::pulse(15, millis());
    char chip[kMarqueeChipLen];
    snprintf(chip, sizeof(chip), "RX %s", BadgeBoops::fieldShortName(tag));
    pushMarqueeEvent(chip);
    LOG_BOOP("[%s] field RX tag=%u\n", TAG_FB, tag);
}

void onComplete(BoopType t, bool success) {
    if (success) Haptics::pulse(50, millis());
    pushMarqueeEvent(success ? "DONE" : "FAIL");
    LOG_BOOP("[%s] complete type=%u ok=%d\n", TAG_FB, (unsigned)t, success);
}

void onInstallation(BoopType t, const char* installationId) {
    Haptics::pulse(30, millis());
    Serial.printf("[%s] installation boop type=%u id=%s (stub)\n",
                  TAG_FB, (unsigned)t, installationId ? installationId : "?");
}

}  // namespace BoopFeedback
