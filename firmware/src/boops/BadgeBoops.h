// BadgeBoops.h — Boop protocol state machine, journal, feedback, and
// per-type handler dispatch.
//
// Transport lives in BadgeIR (RMT hardware + raw frame send/recv).
// Screens live in GUI.cpp (BoopScreen / BoopResultScreen).
//
// Threading:
//   - All state machine methods (tick, onLock, etc.) run on Core 0 inside
//     irTask. They call BadgeIR::sendFrame / recvFrame for transport.
//   - BoopStatus fields are read live by Core 1 for rendering. Single-byte
//     and 32-bit fields are atomic on ESP32; strings are read directly
//     without locks (tearing shows as a momentary character flicker which
//     is acceptable for UI).
//   - The journal is protected by its own FreeRTOS mutex (see .cpp).

#pragma once
#include <Arduino.h>

#include "infra/BadgeLimits.h"
#include "ir/BadgeIR.h"          // BoopType, frame type tags, pacing

class oled;

namespace BadgeBoops {

// ─── Sizes ──────────────────────────────────────────────────────────────────

static constexpr uint8_t kMaxContacts = BADGE_BOOPS_MAX_RECORDS;

// ─── Phase ──────────────────────────────────────────────────────────────────

enum BoopPhase : uint8_t {
    BOOP_PHASE_IDLE,
    BOOP_PHASE_BEACONING,
    BOOP_PHASE_EXCHANGE,
    BOOP_PHASE_PAIRED_OK,
    BOOP_PHASE_FAILED,
    BOOP_PHASE_CANCELLED,
};

// ─── Live boop state ────────────────────────────────────────────────────────
// Written by Core 0 (irTask), read by Core 1 (GUI). Per-run scratch only —
// persistent history is /boops.json.

struct BoopStatus {
    volatile BoopPhase phase;
    BoopType           boopType;                       // captured from beacon word0 byte1

    char   peerUID[13];                                // final peer UID (set on completion)
    char   peerUidHex[13];                             // working buffer filled during beaconing
    uint8_t peerUidBytes[6];                           // raw bytes for memcmp ordering

    char   peerName[BADGE_FIELD_NAME_MAX];
    char   peerTitle[BADGE_FIELD_NAME_MAX];
    char   peerTicketUuid[BADGE_UUID_MAX];
    char   peerCompany[BADGE_FIELD_NAME_MAX];
    char   peerAttendeeType[BADGE_FIELD_TYPE_MAX];
    char   peerEmail[64];
    char   peerWebsite[80];
    char   peerPhone[24];
    char   peerBio[128];

    char   installationId[9];                          // for non-peer boops
    char   workflowId[BADGE_WORKFLOW_ID_MAX];          // legacy field, always empty offline
    char   statusMsg[48];                              // live protocol status (short)

    uint8_t  beaconTxCount;
    uint8_t  beaconRxCount;
    int      pairingId;                                // legacy field, always 0 offline

    bool     offlineBoop;                              // last completion was local/offline

    uint16_t fieldTxMask;                              // bit N = tag N ACK'd by peer
    uint16_t fieldRxMask;                              // bit N = tag N received from peer
    uint8_t  peerBioChunkMask;                         // bits 0..3 = received bio chunks
    uint8_t  currentFieldTag;
    uint8_t  lastFieldDirection;                       // 0=idle, 1=tx, 2=rx
    unsigned long lastFieldEventMs;
    unsigned long phaseEnteredMs;                      // millis() when current phase was set
    unsigned long phaseUntil;
};

extern BoopStatus boopStatus;

// Inter-core edge-trigger flags. Core 1 writes, Core 0 reads and clears.
extern volatile bool boopEngaged;            // "start a boop now"
extern volatile bool pairingCancelRequested; // "cancel current boop"

// ─── Phase helpers ──────────────────────────────────────────────────────────

void setPhase(BoopPhase p, unsigned long holdMs = 0);
const char* phaseName(BoopPhase p);

// Short, screen-friendly text for the current phase.
const char* statusShort(BoopPhase p);

// Short display label ("name", "co", "ticket", ...) for a BoopFieldTag value.
// Shared by BoopFeedback marquee chips and BoopScreen TX-chip animation —
// single source of truth so display strings never drift.
const char* fieldShortName(uint8_t tag);

// ─── Journal (/boops.json) ──────────────────────────────────────────────────

struct PartnerInfo {
    const char* name;
    const char* title;
    const char* company;
    const char* attendeeType;
    const char* ticketUuid;
    const char* email;
    const char* website;
    const char* phone;
    const char* bio;
};

void begin();

void recordBoop(const char* peer_badge_uid,
                const char* peer_name = nullptr,
                const char* peer_ticket_uuid = nullptr);

void recordBoopEx(BoopType type,
                  const char* peerIdOrInstallation,
                  const PartnerInfo* partner);

bool updatePartnerNotes(const char* uidLo, const char* uidHi,
                        const char* notes);

// Generic setter: write `value` into the pair's `jsonKey` slot in
// /boops.json. Returns true on success. Call ordering of (uidA, uidB)
// doesn't matter; we canonicalize via sortedPair internally.
bool updatePartnerField(const char* uidA, const char* uidB,
                        const char* jsonKey, const char* value);

char* readJson(size_t* outLen);
int   count();
int   countUniqueActivePairings();
void  clearJournal();

// ─── Peer lookup (spec-010) ─────────────────────────────────────────────────
// Single owner of `/boops.json` parsing. Replaces the duplicated JSON
// walks that used to live in WiFiService::resolveSenderFromBoops and
// the shared peer picker.

// Resolve a peer by ticket UUID. Returns true and fills outPeerUid /
// outPeerName from the matching journal row. outPeerUid receives the
// peer's badge_uuid (12-char hex MAC for IR-recorded rows or 36-char
// UUID for server-synthesized rows). outPeerName is the partner's
// display name; empty if the row carries no partner info.
bool lookupPeerByTicket(const char* ticketUuid,
                        char* outPeerUid,  size_t peerUidCap,
                        char* outPeerName, size_t peerNameCap);

// Fields surfaced to the picker / contact list. All strings null-
// terminated; missing values are empty strings rather than null.
struct PeerEntry {
    char peerUid[BADGE_UUID_MAX];
    char peerTicketUuid[BADGE_UUID_MAX];
    char name[BADGE_FIELD_NAME_MAX];
    char company[BADGE_FIELD_NAME_MAX];
    char lastTs[32];      // ISO8601 of most recent visit; "" for synthetic rows
    int  pairingId;
    int  boopCount;
};

// Walk all active (non-revoked) pairings in /boops.json and invoke the
// callback for each. Stops at the first callback returning false. Uses
// `myTicketUuid` to identify which slot is "me" so the entry's
// peerUid/peerTicketUuid point at the OTHER attendee.
//
// Caller-supplied buffer size matches PeerEntry — no allocation in the
// callback path. Sort order matches journal order (which the server
// already returned recency-DESC at sync time).
using PeerWalkCallback = bool (*)(const PeerEntry& entry, void* user);
void listActivePeers(const char* myTicketUuid,
                     PeerWalkCallback cb, void* user);

// Full per-peer detail used by the Contacts detail screen. Carries
// every partner_* field we journal plus housekeeping. All strings
// are null-terminated; missing values are empty strings.
struct ContactDetail {
    char peerUid[BADGE_UUID_MAX];
    char ticketUuid[BADGE_UUID_MAX];
    char name[BADGE_FIELD_NAME_MAX];
    char title[BADGE_FIELD_NAME_MAX];
    char company[BADGE_FIELD_NAME_MAX];
    char attendeeType[BADGE_FIELD_TYPE_MAX];
    char email[64];
    char website[80];
    char phone[24];
    char bio[128];
    char lastTs[32];
    int  boopCount;
};

// Look up a contact by their badge UID (12-char hex MAC for IR rows).
// Returns true if a row was found and `out` was populated.
bool lookupContactByUid(const char* peerUid, ContactDetail& out);

// ─── State machine entry point (called from irTask on Core 0) ───────────────

// Called once when irTask brings the hardware up. Resets s_state.
void smReset();

// Called once per irTask tick while irHardwareEnabled. Drives the beacon
// loop, cancellation, field exchange, and terminal-phase auto-timeout.
void smTick();

// Clears live state. Called when irTask is shutting hardware down or when
// the Boop screen exits.
void smShutdown();

// ─── Per-type handler dispatch (Phase B) ────────────────────────────────────

// Thin ops struct so exhibit / queue / kiosk / check-in can each define
// their own on-lock action, post-confirm tick, and screen content without
// touching the state-machine switch statement.

struct BoopHandlerOps {
    const char* name;

    // Called once immediately after mutual-confirm, before the handler-
    // specific post-confirm phase begins. Handler can prep per-boop state.
    void (*onLock)(BoopStatus& s);

    // Called each irTask tick while in BOOP_PHASE_EXCHANGE. Return true
    // when the handler is done (state machine will then finishPaired()).
    // Not called if doFieldExchange is false — those handlers complete
    // synchronously in onLock and go straight to PAIRED_OK.
    bool (*tickPostConfirm)(BoopStatus& s, uint32_t nowMs);

    // Paints the handler-specific content area of the BoopScreen (left
    // block next to the 48px ziggy). Coordinate rectangle given in
    // pixels. Handler must not draw outside it.
    void (*drawContent)(oled& d, const BoopStatus& s,
                        int x, int y, int w, int h);

    // Short label for the bottom marquee / status line.
    const char* (*statusLabel)(const BoopStatus& s);

    // True if this type runs the IR field exchange after mutual-confirm.
    // Peer boops: true. Installation boops: false (they jump to PAIRED_OK).
    bool doFieldExchange;
};

const BoopHandlerOps* handlerFor(BoopType t);

}  // namespace BadgeBoops

// ─── Feedback dispatch (haptic / LED cues on boop events) ───────────────────

namespace BoopFeedback {

void onBeaconLock(BoopType t, const char* installationId);
void onPeerFieldTx(uint8_t tag);
void onPeerFieldRx(uint8_t tag);
void onComplete(BoopType t, bool success);
void onInstallation(BoopType t, const char* installationId);

// Append a short (<=11 char) chip to the bottom-marquee event ring.
// Safe to call from either core; ring is guarded by a portMUX spinlock.
void pushMarqueeEvent(const char* chip);

// Pop the oldest pending chip into out (null-terminated). Returns true
// if a chip was copied. out must have >= 12 bytes.
bool popMarqueeEvent(char* out, size_t outCap);

}  // namespace BoopFeedback
