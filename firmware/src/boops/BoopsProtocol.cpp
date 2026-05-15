// BoopsProtocol.cpp — boop state machine + frame codec + v2 exchange protocol.
//
// Owns:
//   - BoopStatus boopStatus, volatile boopEngaged, pairingCancelRequested
//   - All file-static FSM state (s_xchg, s_haveEarlyFrame, …)
//   - Frame encode/decode (manifest, stream req, data, need, fin/finack)
//   - smReset / smTick / smShutdown (entry points called from irTask Core 0)
//
// Calls into BoopsJournal.cpp's public surface (recordBoop, recordBoopEx,
// recordBoop*) when boops complete; calls handlerFor() to dispatch
// per-type behavior (peer/exhibit/queue/kiosk/checkin/unknown).
// Calls BoopFeedback hooks on protocol events.

#include "BadgeBoops.h"

#include "../infra/BadgeConfig.h"
#include "../identity/BadgeInfo.h"
#include "../ir/BadgeIR.h"
#include "../identity/BadgeUID.h"
#include "../infra/DebugLog.h"

#include "Internal.h"

#include <Arduino.h>
#include <cstring>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern Config badgeConfig;

static const char* TAG = "BadgeBoops";

namespace BadgeBoops {
// ═══════════════════════════════════════════════════════════════════════════
// 2) State machine globals + phase helpers
// ═══════════════════════════════════════════════════════════════════════════

BoopStatus     boopStatus = {};
volatile bool  boopEngaged            = false;
volatile bool  pairingCancelRequested = false;

void setPhase(BoopPhase p, unsigned long holdMs) {
    boopStatus.phase = p;
    boopStatus.phaseEnteredMs = millis();
    if (holdMs == 0) {
        // "transition on the next tick" — handleTerminal falls through.
        boopStatus.phaseUntil = 0;
    } else if (holdMs == 0xFFFFFFFFUL) {
        // "wait indefinitely" — handleTerminal stays until the UI (or
        // another caller) clears phaseUntil back to 0.
        boopStatus.phaseUntil = 0xFFFFFFFFUL;
    } else {
        boopStatus.phaseUntil = boopStatus.phaseEnteredMs + holdMs;
    }
}

const char* phaseName(BoopPhase p) {
    switch (p) {
        case BOOP_PHASE_IDLE:       return "IDLE";
        case BOOP_PHASE_BEACONING:  return "BEACON";
        case BOOP_PHASE_EXCHANGE:   return "XCHG";
        case BOOP_PHASE_PAIRED_OK:  return "PAIRED";
        case BOOP_PHASE_FAILED:     return "FAILED";
        case BOOP_PHASE_CANCELLED:  return "CANCEL";
    }
    return "?";
}

const char* statusShort(BoopPhase p) {
    switch (p) {
        case BOOP_PHASE_IDLE:       return "ready";
        case BOOP_PHASE_BEACONING:  return "searching";
        case BOOP_PHASE_EXCHANGE:   return "exchanging";
        case BOOP_PHASE_PAIRED_OK:  return "booped!";
        case BOOP_PHASE_FAILED:     return "no peer";
        case BOOP_PHASE_CANCELLED:  return "cancelled";
    }
    return "";
}

const char* fieldShortName(uint8_t tag) {
    switch (tag) {
        case FIELD_NAME:          return "name";
        case FIELD_TITLE:         return "title";
        case FIELD_COMPANY:       return "company";
        case FIELD_ATTENDEE_TYPE: return "type";
        case FIELD_TICKET_UUID:   return "ticket";
        case FIELD_EMAIL:         return "email";
        case FIELD_WEBSITE:       return "website";
        case FIELD_PHONE:         return "phone";
        case FIELD_BIO:           return "bio";
        default:                  return "?";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 3) Frame codec + beacon helpers
// ═══════════════════════════════════════════════════════════════════════════

static uint32_t packUidLo() {
    return uid[0] | ((uint32_t)uid[1] <<  8) |
           ((uint32_t)uid[2] << 16) | ((uint32_t)uid[3] << 24);
}
static uint32_t packUidHi() {
    return uid[4] | ((uint32_t)uid[5] << 8);
}

// Beacon loop scratch — reset in smReset().
static unsigned long s_next_tx_ms          = 0;
static bool          s_boop_recorded       = false;
static bool          s_boop_failed_final   = false;
static bool          s_peer_done           = false;
static int           s_confirm_tx_remaining = 0;

// Early-MANIFEST stash: if the peer transitions into EXCHANGE a tick
// before we do (their confirm burst finished first), their first
// MANIFEST frame can land while handleBeaconing is still running.
// Capture it once so peer_tickPostConfirm can replay it as its own
// first RX event.
nec_mw_result_t           s_earlyFrame;
bool                      s_haveEarlyFrame = false;

// ═══════════════════════════════════════════════════════════════════════════
// v2 exchange protocol — manifest-driven streaming
// ═══════════════════════════════════════════════════════════════════════════
//
// Retransmit intervals for cached meta frames.  Split into two
// buckets because the response-frame durations are very different:
//
//   kRetxMsSlow — for STREAM_REQ / NEED where the peer answers with a
//   potentially long DATA burst (up to ~1.8 s on the wire at
//   kMaxTlvBytes=120).  kRetxMsSlow MUST exceed that worst-case DATA
//   TX, otherwise the puller retransmits STREAM_REQ / NEED while the
//   server is still mid-burst, the two frames collide half-duplex, and
//   neither side decodes.  STREAM_REQ / NEED retx is further gated by
//   !peerStreamStarted, but that gate only engages after the first
//   DATA header has been decoded — it can't save us if the first DATA
//   itself is longer than kRetxMsSlow.
//
//   kRetxMsFast — for MANIFEST / FIN where the peer answers with a
//   short 3-word frame (MANIFEST, FINACK).  No DATA stream is at risk
//   of collision, so we can retry much more aggressively.  This is the
//   load-bearing fix for the "second badge waits forever before
//   transmitting its fields" behaviour — the FIN/FINACK/STREAM_REQ
//   phase-flip handshake used to eat 2.8 s per lost short frame; now
//   it eats 1 s.
//
// No hard exchange timeout — the user cancels via Left, or pops the
// screen, full stop.
// kRetxMsSlow, kRetxMsFast, XchgPhase moved to boops/Internal.h
// (handlers consume them during the field exchange).

// XchgState defined in boops/Internal.h so handlers can reach it.
XchgState s_xchg = {};

static void sendBeacon() {
    uint32_t words[3];
    uint8_t frameType = s_boop_recorded ? IR_BOOP_DONE : IR_BOOP_BEACON;
    words[0] = frameType
             | ((uint32_t)BOOP_PEER << 8)
             | ((uint32_t)kBoopProtocolVer << 16);
    words[1] = packUidLo();
    words[2] = packUidHi();
    BadgeIR::sendFrame(words, 3);
}

// ─── v2 frame codecs ────────────────────────────────────────────────────────
//
// All v2 exchange frames share a 3-word envelope: word0 = type + type-
// specific bitfields, words[1..2] = sender UID for the self-echo filter.
// DATA additionally carries packed TLV payloads starting at word[3].

// MANIFEST word0:
//   bits  0-7  : type = 0xC0
//   bits  8-16 : tagMask (9 bits)
//   bits 17-19 : bioChunkCount (0..4)
//   bits 20-23 : reserved
//   bits 24-31 : seq
int encodeManifest(uint32_t* words, uint16_t tagMask,
                           uint8_t bioChunks, uint8_t seq) {
    words[0] = IR_BOOP_MANIFEST
             | (((uint32_t)tagMask   & 0x1FFU) << 8)
             | (((uint32_t)bioChunks & 0x07U) << 17)
             | ((uint32_t)seq << 24);
    words[1] = packUidLo();
    words[2] = packUidHi();
    return 3;
}

bool decodeManifest(const uint32_t* words, size_t count,
                            uint16_t* tagMask, uint8_t* bioChunks) {
    if (count < 3) return false;
    if ((words[0] & 0xFF) != IR_BOOP_MANIFEST) return false;
    *tagMask   = (uint16_t)((words[0] >> 8) & 0x1FFU);
    *bioChunks = (uint8_t)((words[0] >> 17) & 0x07U);
    if (*bioChunks > kBoopBioMaxChunks) *bioChunks = kBoopBioMaxChunks;
    return true;
}

// STREAM_REQ word0:
//   bits  0-7  : type = 0xC1
//   bits  8-15 : seq
int encodeStreamReq(uint32_t* words, uint8_t seq) {
    words[0] = IR_BOOP_STREAM_REQ | ((uint32_t)seq << 8);
    words[1] = packUidLo();
    words[2] = packUidHi();
    return 3;
}

// DATA word0 (batched, streaming):
//   bits  0-7  : type = 0xC2
//   bits  8-15 : seq (echo of STREAM_REQ or NEED seq)
//   bits 16-19 : groupCount (1..15)
//   bits 20    : hasMore (1 = more DATA frames in this stream)
//   bits 21-23 : reserved
//   bits 24-31 : reserved
//   words[3..] : packed TLV groups:
//     byte 0 = tag (0..8) | (chunkIdx << 4)
//     byte 1 = payload length (0..255)
//     byte 2.. = payload bytes
//   TLVs pack contiguously; groups straddle word boundaries.
//
// Max payload per frame: NEC_MAX_WORDS - 3 (envelope) = up to 61 words
// = up to 244 bytes of TLV payload.  Live peer boops no longer use this
// profile-exchange path; only legacy kiosk/exhibit frames arrive here.

int encodeDataFrame(uint32_t* words, uint8_t seq,
                            uint8_t groupCount, bool hasMore,
                            const uint8_t* tlvBytes, size_t tlvLen) {
    const uint32_t headerWords = 3;
    const uint32_t payloadWords = (uint32_t)((tlvLen + 3) / 4);
    if (headerWords + payloadWords > NEC_MAX_WORDS) return 0;

    words[0] = IR_BOOP_DATA
             | ((uint32_t)seq << 8)
             | (((uint32_t)groupCount & 0x0FU) << 16)
             | (hasMore ? (1U << 20) : 0U);
    words[1] = packUidLo();
    words[2] = packUidHi();

    // Pack TLV bytes 4-at-a-time into little-endian words.
    for (size_t i = 0; i < payloadWords; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            size_t idx = i * 4 + b;
            if (idx < tlvLen) w |= ((uint32_t)(uint8_t)tlvBytes[idx]) << (b * 8);
        }
        words[headerWords + i] = w;
    }
    return (int)(headerWords + payloadWords);
}

bool parseDataHeader(const uint32_t* words, size_t count,
                             uint8_t* seq, uint8_t* groupCount,
                             bool* hasMore) {
    if (count < 3) return false;
    if ((words[0] & 0xFF) != IR_BOOP_DATA) return false;
    *seq        = (uint8_t)((words[0] >> 8)  & 0xFFU);
    *groupCount = (uint8_t)((words[0] >> 16) & 0x0FU);
    *hasMore    = ((words[0] >> 20) & 0x01U) != 0;
    return true;
}

// NEED word0:
//   bits  0-7  : type = 0xC3
//   bits  8-11 : tag
//   bits 12-15 : chunkIdx
//   bits 16-23 : seq
int encodeNeed(uint32_t* words, uint8_t tag,
                      uint8_t chunkIdx, uint8_t seq) {
    words[0] = IR_BOOP_NEED
             | (((uint32_t)tag      & 0x0FU) << 8)
             | (((uint32_t)chunkIdx & 0x0FU) << 12)
             | ((uint32_t)seq << 16);
    words[1] = packUidLo();
    words[2] = packUidHi();
    return 3;
}

bool decodeNeed(const uint32_t* words, size_t count,
                       uint8_t* tag, uint8_t* chunkIdx) {
    if (count < 3) return false;
    if ((words[0] & 0xFF) != IR_BOOP_NEED) return false;
    *tag      = (uint8_t)((words[0] >> 8)  & 0x0FU);
    *chunkIdx = (uint8_t)((words[0] >> 12) & 0x0FU);
    return true;
}

// FIN / FINACK word0:
//   bits  0-7  : type = 0xC4 / 0xC5
//   bits  8-15 : seq
int encodeFin(uint32_t* words, uint8_t seq) {
    words[0] = IR_BOOP_FIN | ((uint32_t)seq << 8);
    words[1] = packUidLo();
    words[2] = packUidHi();
    return 3;
}
int encodeFinAck(uint32_t* words, uint8_t seq) {
    words[0] = IR_BOOP_FINACK | ((uint32_t)seq << 8);
    words[1] = packUidLo();
    words[2] = packUidHi();
    return 3;
}

// ─── Local field access ────────────────────────────────────────────────────

// Single source of truth for "what fields this badge owns" — driven by
// the FIELD_* tag enum order. The same table feeds storeReceivedField
// (rx side) so a new tag is added in one place. Member-pointer indirection
// keeps the lookup branch-free at the cost of a small const table.
namespace {

struct FieldRow {
    uint8_t tag;
    char (BadgeInfo::Fields::*member)[];  // unused — kept for symmetry
    size_t offset;
    size_t cap;
};

#define FIELD_ROW(TAG, MEMBER) \
    { TAG, nullptr, offsetof(BadgeInfo::Fields, MEMBER), sizeof(((BadgeInfo::Fields*)0)->MEMBER) }

constexpr FieldRow kFieldTable[] = {
    FIELD_ROW(FIELD_NAME,          name),
    FIELD_ROW(FIELD_TITLE,         title),
    FIELD_ROW(FIELD_COMPANY,       company),
    FIELD_ROW(FIELD_ATTENDEE_TYPE, attendeeType),
    FIELD_ROW(FIELD_TICKET_UUID,   ticketUuid),
    FIELD_ROW(FIELD_EMAIL,         email),
    FIELD_ROW(FIELD_WEBSITE,       website),
    FIELD_ROW(FIELD_PHONE,         phone),
    FIELD_ROW(FIELD_BIO,           bio),
};
#undef FIELD_ROW

const FieldRow* findField(uint8_t tag) {
    for (const FieldRow& r : kFieldTable) {
        if (r.tag == tag) return &r;
    }
    return nullptr;
}

}  // anonymous namespace

const char* getLocalField(uint8_t tag) {
    if (!s_xchg.fieldsCached) {
        BadgeInfo::getCurrent(s_xchg.localCache);
        s_xchg.fieldsCached = true;
    }
    const FieldRow* row = findField(tag);
    if (!row) return "";
    return reinterpret_cast<const char*>(&s_xchg.localCache) + row->offset;
}

// Decide whether a tag's value is eligible for TX based on:
//   - non-empty local value
//   - tag != FIELD_ATTENDEE_TYPE (permanently masked — server-set only)
// All other fields always exchange when present locally; the old
// kBoopInfoFields opt-out gate was removed when the badge went fully
// offline, since the contact card is now the only way peers learn
// anything about each other.
static bool tagIsTxEligible(uint8_t tag) {
    if (tag == FIELD_ATTENDEE_TYPE) return false;
    const char* v = getLocalField(tag);
    if (!v || v[0] == '\0') return false;
    return true;
}

// Walk BadgeInfo and compute my outgoing manifest — tagMask bitmap plus
// bio chunk count (ceil(bio_len / 32), capped at 4).  Called once per
// boop during resetFieldExchangeState.
static void buildLocalManifest(uint16_t* tagMask, uint8_t* bioChunks) {
    uint16_t mask = 0;
    for (uint8_t t = 0; t < FIELD_TAG_COUNT; t++) {
        if (tagIsTxEligible(t)) mask |= (1U << t);
    }
    *tagMask = mask;

    uint8_t chunks = 0;
    if (mask & (1U << FIELD_BIO)) {
        const char* bio = getLocalField(FIELD_BIO);
        size_t len = bio ? strlen(bio) : 0;
        chunks = (uint8_t)((len + kBoopBioChunkBytes - 1) / kBoopBioChunkBytes);
        if (chunks > kBoopBioMaxChunks) chunks = kBoopBioMaxChunks;
    }
    *bioChunks = chunks;
}

void storeReceivedField(uint8_t tag, const char* val, uint8_t len) {
    auto copyClamped = [&](char* dst, size_t cap) {
        size_t n = (len < cap - 1) ? len : cap - 1;
        memcpy(dst, val, n);
        dst[n] = '\0';
    };
    switch (tag) {
        case FIELD_NAME:          copyClamped(boopStatus.peerName,         sizeof(boopStatus.peerName));         break;
        case FIELD_TITLE:         copyClamped(boopStatus.peerTitle,        sizeof(boopStatus.peerTitle));        break;
        case FIELD_COMPANY:       copyClamped(boopStatus.peerCompany,      sizeof(boopStatus.peerCompany));      break;
        case FIELD_ATTENDEE_TYPE: copyClamped(boopStatus.peerAttendeeType, sizeof(boopStatus.peerAttendeeType)); break;
        case FIELD_TICKET_UUID:   copyClamped(boopStatus.peerTicketUuid,   sizeof(boopStatus.peerTicketUuid));   break;
        case FIELD_EMAIL:         copyClamped(boopStatus.peerEmail,        sizeof(boopStatus.peerEmail));        break;
        case FIELD_WEBSITE:       copyClamped(boopStatus.peerWebsite,      sizeof(boopStatus.peerWebsite));      break;
        case FIELD_PHONE:         copyClamped(boopStatus.peerPhone,        sizeof(boopStatus.peerPhone));        break;
        case FIELD_BIO:           /* handled separately by bio-chunk path */                                     break;
        default: break;
    }
}

// Store one bio chunk at offset chunkIdx * kBoopBioChunkBytes.  Caller
// has already range-checked chunkIdx < peerBioChunks.
void storeBioChunk(uint8_t chunkIdx, const char* val, uint8_t len) {
    const size_t off = (size_t)chunkIdx * kBoopBioChunkBytes;
    if (off >= sizeof(boopStatus.peerBio)) return;
    size_t room = sizeof(boopStatus.peerBio) - off - 1;
    size_t n = (len < room) ? len : room;
    memcpy(boopStatus.peerBio + off, val, n);
    // Terminate after this chunk for now — if later chunks arrive they'll
    // overwrite the NUL.  Only the final chunk's trailing NUL sticks.
    boopStatus.peerBio[off + n] = '\0';
}

static void unpackPeerUID(const nec_mw_result_t* frame) {
    uint32_t lo = frame->words[1];
    uint32_t hi = frame->words[2];
    boopStatus.peerUidBytes[0] =  lo        & 0xFF;
    boopStatus.peerUidBytes[1] = (lo >>  8) & 0xFF;
    boopStatus.peerUidBytes[2] = (lo >> 16) & 0xFF;
    boopStatus.peerUidBytes[3] = (lo >> 24) & 0xFF;
    boopStatus.peerUidBytes[4] =  hi        & 0xFF;
    boopStatus.peerUidBytes[5] = (hi >>  8) & 0xFF;
    for (int i = 0; i < 6; i++) {
        snprintf(boopStatus.peerUidHex + i * 2, 3, "%02x", boopStatus.peerUidBytes[i]);
    }
    boopStatus.peerUidHex[12] = '\0';
    memcpy(boopStatus.peerUID, boopStatus.peerUidHex, 13);
    boopStatus.peerName[0]         = '\0';
    boopStatus.peerTitle[0]        = '\0';
    boopStatus.peerTicketUuid[0]   = '\0';
    boopStatus.peerCompany[0]      = '\0';
    boopStatus.peerAttendeeType[0] = '\0';
    boopStatus.peerEmail[0]        = '\0';
    boopStatus.peerWebsite[0]      = '\0';
    boopStatus.peerPhone[0]        = '\0';
    boopStatus.peerBio[0]          = '\0';
    boopStatus.boopType = static_cast<BoopType>((frame->words[0] >> 8) & 0xFF);
}

static void resetFieldExchangeState() {
    // boopStatus display masks — kept for chip animation in BoopScreen.
    boopStatus.fieldTxMask        = 0;
    boopStatus.fieldRxMask        = 0;
    boopStatus.peerBioChunkMask   = 0;
    boopStatus.currentFieldTag    = 0;
    boopStatus.lastFieldDirection = 0;
    boopStatus.lastFieldEventMs   = millis();

    // Zero the whole v2 state struct then populate the few fields that
    // persist across the reset (my own manifest + role).
    s_xchg = {};
    s_xchg.isPrimary    = (memcmp(uid, boopStatus.peerUidBytes, 6) < 0);
    s_xchg.phase        = XCHG_MANIFEST_XCHG;
    s_xchg.fieldsCached = false;
    buildLocalManifest(&s_xchg.myTagMask, &s_xchg.myBioChunks);

    // s_haveEarlyFrame is intentionally NOT cleared here — if a peer
    // MANIFEST landed during beacon phase, EXCHANGE's first tick will
    // consume it.  Cleared in smReset on shutdown and after consumption.
}

// Record the boop locally exactly once.
// Does NOT change phase — caller keeps beaconing so the peer can finish.
static void recordBoopOnce() {
    if (s_boop_recorded) return;
    s_boop_recorded = true;
    s_boop_failed_final = false;

    LOG_BOOP("[%s] recording boop — my UID=%s peer UID=%s\n",
                  TAG, uid_hex, boopStatus.peerUID);

    BadgeBoops::recordBoop(boopStatus.peerUID, nullptr, boopStatus.peerTicketUuid);
    boopStatus.offlineBoop = true;
    snprintf(boopStatus.statusMsg, sizeof(boopStatus.statusMsg), "Saved locally");
    Serial.printf("[%s] local boop recorded\n", TAG);
}

static bool serverReadyToFinish() {
    (void)s_boop_failed_final;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 4) State machine
// ═══════════════════════════════════════════════════════════════════════════

static void finishPaired() {
    if (s_boop_recorded) {
        if (boopStatus.fieldRxMask != 0 || boopStatus.fieldTxMask != 0) {
            PartnerInfo pi = {};
            pi.name         = boopStatus.peerName;
            pi.title        = boopStatus.peerTitle;
            pi.company      = boopStatus.peerCompany;
            pi.attendeeType = boopStatus.peerAttendeeType;
            pi.ticketUuid   = boopStatus.peerTicketUuid;
            pi.email        = boopStatus.peerEmail;
            pi.website      = boopStatus.peerWebsite;
            pi.phone        = boopStatus.peerPhone;
            pi.bio          = boopStatus.peerBio;
            recordBoopEx(boopStatus.boopType, boopStatus.peerUID, &pi);
        }
        snprintf(boopStatus.statusMsg, sizeof(boopStatus.statusMsg), "Booped!");
        // Indefinite hold on success — the screen shows the filled-out
        // peer info until the user presses Right/Left (see BoopScreen).
        setPhase(BOOP_PHASE_PAIRED_OK, 0xFFFFFFFFUL);
    } else {
        snprintf(boopStatus.statusMsg, sizeof(boopStatus.statusMsg), "No response");
        setPhase(BOOP_PHASE_FAILED, 3000);
    }
}

static void handleIdle() {
    if (boopEngaged) {
        boopEngaged = false;

        boopStatus.beaconTxCount     = 0;
        boopStatus.beaconRxCount     = 0;
        boopStatus.peerUID[0]        = '\0';
        boopStatus.peerUidHex[0]     = '\0';
        boopStatus.peerName[0]       = '\0';
        boopStatus.peerTitle[0]      = '\0';
        boopStatus.peerTicketUuid[0] = '\0';
        boopStatus.peerCompany[0]    = '\0';
        boopStatus.peerAttendeeType[0] = '\0';
        boopStatus.peerEmail[0]      = '\0';
        boopStatus.peerWebsite[0]    = '\0';
        boopStatus.peerPhone[0]      = '\0';
        boopStatus.peerBio[0]        = '\0';
        boopStatus.installationId[0] = '\0';
        boopStatus.workflowId[0]     = '\0';
        boopStatus.boopType          = BOOP_PEER;
        boopStatus.pairingId         = 0;
        boopStatus.offlineBoop       = false;
        boopStatus.fieldTxMask       = 0;
        boopStatus.fieldRxMask       = 0;
        boopStatus.currentFieldTag   = 0;
        boopStatus.lastFieldDirection = 0;
        boopStatus.lastFieldEventMs  = 0;
        memset(boopStatus.peerUidBytes, 0, sizeof(boopStatus.peerUidBytes));
        s_boop_recorded         = false;
        s_boop_failed_final     = false;
        s_peer_done             = false;
        s_confirm_tx_remaining  = 0;

        // Stagger first TX randomly so two badges entering at the same
        // moment don't collide. The first successful RX locks them into
        // a call-and-response from then on.
        s_next_tx_ms = millis() + (esp_random() % 500);
        snprintf(boopStatus.statusMsg, sizeof(boopStatus.statusMsg), "Searching...");
        setPhase(BOOP_PHASE_BEACONING);
        Serial.printf("[%s] beaconing started\n", TAG);
        return;
    }

    // Drain any frame the Python queue wants while idle.
    nec_mw_result_t frame;
    (void)BadgeIR::recvFrame(&frame, 10);
}

static void handleBeaconing() {
    unsigned long now = millis();

    // Record boop as soon as threshold met — switches us from 0xB0 to 0xB1.
    if (!s_boop_recorded &&
        boopStatus.beaconRxCount >= BOOP_BEACON_RX_THRESHOLD &&
        boopStatus.beaconTxCount >= BOOP_BEACON_TX_MIN) {
        Serial.printf("[%s] threshold met (tx=%u rx=%u) — recording, sending 0xB1\n",
                      TAG, boopStatus.beaconTxCount, boopStatus.beaconRxCount);
        recordBoopOnce();
    }

    // TX if it's time, then fall through to listen.
    if (now >= s_next_tx_ms) {
        sendBeacon();
        boopStatus.beaconTxCount++;
        now = millis();
        s_next_tx_ms = now + BOOP_BEACON_INTERVAL_MS
                           + (esp_random() % BOOP_BEACON_JITTER_MS);
        // Break-lockstep nudge: as long as the handshake hasn't
        // progressed to 'peer also done' (s_peer_done), our TX phase
        // could still be aligned with the peer's — we'd be talking
        // during each other's RX windows and missing beacons.  An
        // extra one-shot random offset each TX after the threshold
        // count forcibly decorrelates the schedules.  Once we see
        // peer's 0xB1 / early MANIFEST the respond-at-now+120 path
        // takes over and this branch is skipped.
        if (!s_peer_done &&
            boopStatus.beaconTxCount >= BOOP_BEACON_LOCKSTEP_NUDGE_AFTER_TX) {
            s_next_tx_ms += 200 + (esp_random() % 500);
        }

        // After mutual confirm, keep sending 0xB1 for a few more frames
        // so the peer definitely sees ours. Then dispatch to the
        // per-type handler (onLock), and either enter local field exchange
        // (peer + config on) or finish straight away.
        if (s_boop_recorded && s_peer_done) {
            s_confirm_tx_remaining--;
            if (s_confirm_tx_remaining <= 0) {
                if (!serverReadyToFinish()) {
                    s_confirm_tx_remaining = 1;
                    s_next_tx_ms = millis() + 750;
                    return;
                }

                Serial.printf("[%s] confirm complete (tx=%u rx=%u)\n",
                              TAG, boopStatus.beaconTxCount, boopStatus.beaconRxCount);

                const BoopHandlerOps* h = handlerFor(boopStatus.boopType);
                BoopFeedback::onBeaconLock(boopStatus.boopType,
                                           boopStatus.installationId);
                h->onLock(boopStatus);

                // Field exchange always runs for handlers that opt in
                // (peer boops). Installation handlers set
                // doFieldExchange=false and skip straight to PAIRED_OK.
                if (h->doFieldExchange) {
                    resetFieldExchangeState();
                    setPhase(BOOP_PHASE_EXCHANGE);
                    snprintf(boopStatus.statusMsg, sizeof(boopStatus.statusMsg), "Handshake...");
                    Serial.printf("[%s] entering field exchange phase (handler=%s)\n",
                                  TAG, h->name);
                    return;
                }

                finishPaired();
                return;
            }
        }
    }

    const char* tag = "";
    tag = s_peer_done ? "OK!" : (s_boop_recorded ? "Saved" : "");
    snprintf(boopStatus.statusMsg, sizeof(boopStatus.statusMsg),
             "%s tx:%u rx:%u", tag,
             boopStatus.beaconTxCount, boopStatus.beaconRxCount);

    uint32_t listen_ms = (s_next_tx_ms > now) ? (uint32_t)(s_next_tx_ms - now) : 0;
    if (listen_ms == 0) return;

    nec_mw_result_t frame;
    if (BadgeIR::recvFrame(&frame, listen_ms)) {
        uint8_t type = (uint8_t)(frame.words[0] & 0xFF);
        if ((type == IR_BOOP_BEACON || type == IR_BOOP_DONE) && frame.count >= 3) {
            if (boopStatus.beaconRxCount == 0) {
                unpackPeerUID(&frame);
                LOG_BOOP("[%s] first peer beacon UID=%s\n",
                              TAG, boopStatus.peerUID);
            }
            boopStatus.beaconRxCount++;

            if (type == IR_BOOP_DONE && !s_peer_done) {
                s_peer_done = true;
                s_confirm_tx_remaining = kBoopConfirmTxCount;
                Serial.printf("[%s] peer done (0xB1) — sending %d more confirms\n",
                              TAG, kBoopConfirmTxCount);
            }

            unsigned long respond_at = millis() + 120;
            if (respond_at < s_next_tx_ms) s_next_tx_ms = respond_at;
        } else if ((type == IR_BOOP_MANIFEST   ||
                    type == IR_BOOP_DATA       ||  // v3 PP FIELD
                    type == IR_BOOP_NEED       ||  // v3 PP ACK
                    type == IR_BOOP_FIN        ||  // v3 PP DONE
                    type == IR_BOOP_STREAM_REQ ||
                    type == IR_BOOP_FINACK) &&
                   frame.count >= 3) {
            // Peer is already in EXCHANGE — they've stopped sending 0xB1
            // and won't send another one we can use to satisfy our
            // s_peer_done check. Without this short-circuit, we'd sit in
            // BEACON forever sending 0xB1 to a peer that's already moved
            // on, the screen pegs at "Searching..." until the user
            // cancels. Force the transition: pretend we got DONE and
            // fast-forward the confirm counter so the next beacon TX
            // triggers handleBeaconing's transition-to-EXCHANGE branch.
            //
            // We don't need to record the boop yet — recordBoopOnce will
            // fire automatically once beaconRxCount hits the threshold.
            // For v2 MANIFEST frames we also stash so peer_tickPostConfirm
            // can replay it; v3 PP frames are best-effort dropped here
            // (they'll retx on their own clock).
            if (type == IR_BOOP_MANIFEST && !s_haveEarlyFrame) {
                s_earlyFrame = frame;
                s_haveEarlyFrame = true;
                Serial.printf("[%s] stashed early MANIFEST from peer\n", TAG);
            } else {
                Serial.printf("[%s] peer already in EXCHANGE (rx 0x%02X) — "
                              "fast-forwarding confirm\n",
                              TAG, type);
            }
            // Capture peer UID if this is our first sight of them.
            if (boopStatus.beaconRxCount == 0) {
                unpackPeerUID(&frame);
            }
            boopStatus.beaconRxCount++;
            if (!s_boop_recorded) recordBoopOnce();
            if (!s_peer_done) s_peer_done = true;
            s_confirm_tx_remaining = 1;
            unsigned long respond_at = millis() + 60;
            if (respond_at < s_next_tx_ms) s_next_tx_ms = respond_at;
        }
    }
}

// Forward decl — defined in the handler-table section below.
static bool peer_tickPostConfirm(BoopStatus& s, uint32_t nowMs);

static void handleExchangeInfo() {
    const BoopHandlerOps* h = handlerFor(boopStatus.boopType);
    if (h->tickPostConfirm(boopStatus, millis())) {
        BoopFeedback::onComplete(boopStatus.boopType, true);
        finishPaired();
    }
}

static void handleCancel() {
    pairingCancelRequested = false;

    BoopPhase phase = boopStatus.phase;
    if (phase == BOOP_PHASE_BEACONING || phase == BOOP_PHASE_EXCHANGE) {
        Serial.printf("[%s] pairing cancelled by user\n", TAG);
        snprintf(boopStatus.statusMsg, sizeof(boopStatus.statusMsg), "Cancelled");
        setPhase(BOOP_PHASE_CANCELLED, 2000);
    }
}

static void handleTerminal() {
    if (boopStatus.phaseUntil != 0 && millis() < boopStatus.phaseUntil) return;

    boopStatus.phase = BOOP_PHASE_IDLE;
    boopStatus.phaseEnteredMs = millis();
    boopStatus.phaseUntil = 0;
    boopStatus.beaconTxCount = 0;
    boopStatus.beaconRxCount = 0;
    boopStatus.pairingId = 0;
    boopStatus.statusMsg[0] = '\0';
    boopStatus.peerUID[0] = '\0';
    boopStatus.peerUidHex[0] = '\0';
    boopStatus.peerName[0] = '\0';
    boopStatus.peerTitle[0] = '\0';
    boopStatus.peerTicketUuid[0] = '\0';
    boopStatus.peerCompany[0] = '\0';
    boopStatus.peerAttendeeType[0] = '\0';
    boopStatus.peerEmail[0] = '\0';
    boopStatus.peerWebsite[0] = '\0';
    boopStatus.peerPhone[0] = '\0';
    boopStatus.peerBio[0] = '\0';
    boopStatus.installationId[0] = '\0';
    boopStatus.workflowId[0] = '\0';
    boopStatus.offlineBoop = false;
    boopStatus.fieldTxMask = 0;
    boopStatus.fieldRxMask = 0;
    boopStatus.peerBioChunkMask = 0;
    memset(boopStatus.peerUidBytes, 0, sizeof(boopStatus.peerUidBytes));
    s_boop_failed_final = false;
    pairingCancelRequested = false;
    boopEngaged = false;
    // No auto-retry — the UI returns to its ready screen after every
    // boop (success, failure, or cancel) and the user presses UP to
    // boop again. Keeping the radio quiet during ready state also
    // avoids confusing a lingering peer that's slow to finish.
}

// ═══════════════════════════════════════════════════════════════════════════
// 5) Public state-machine entry points
// ═══════════════════════════════════════════════════════════════════════════

void smReset() {
    setPhase(BOOP_PHASE_IDLE);
    s_next_tx_ms = 0;
    s_boop_recorded = false;
    s_boop_failed_final = false;
    s_peer_done = false;
    s_confirm_tx_remaining = 0;
    s_haveEarlyFrame = false;
    s_xchg = {};
}

void smTick() {
    if (pairingCancelRequested) handleCancel();

    switch (boopStatus.phase) {
        case BOOP_PHASE_IDLE:       handleIdle();          break;
        case BOOP_PHASE_BEACONING:  handleBeaconing();     break;
        case BOOP_PHASE_EXCHANGE:   handleExchangeInfo();  break;
        case BOOP_PHASE_PAIRED_OK:
        case BOOP_PHASE_FAILED:
        case BOOP_PHASE_CANCELLED:  handleTerminal();      break;
    }
}

void smShutdown() {
    if (boopStatus.phase != BOOP_PHASE_IDLE) setPhase(BOOP_PHASE_IDLE);
    boopEngaged = false;
    pairingCancelRequested = false;
}


}  // namespace BadgeBoops
