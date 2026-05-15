// Internal.h — cross-TU symbols inside boops/.
//
// Keeps the boops/ subsystem's internal contracts (file-static state,
// codec helpers) reachable across BoopsProtocol.cpp and BoopsHandlers.cpp
// without exposing them via the public BadgeBoops.h facade.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../identity/BadgeInfo.h"
#include "../ir/BadgeIR.h"  // NEC_MAX_WORDS

namespace BadgeBoops {

// Exchange-phase enum used by the v2 manifest-streaming FSM. Defined here
// (rather than file-private) because handlers' tickPostConfirm routines
// drive phase transitions during field exchange.
enum XchgPhase : uint8_t {
    XCHG_MANIFEST_XCHG = 0,
    XCHG_PRIMARY_PULL  = 1,
    XCHG_PRIMARY_REPAIR= 2,
    XCHG_SECONDARY_PULL= 3,
    XCHG_SECONDARY_REPAIR=4,
    XCHG_FIN_XCHG      = 5,
    XCHG_DONE          = 6,
};

inline constexpr unsigned long kRetxMsSlow = 2800;
inline constexpr unsigned long kRetxMsFast = 1000;

// v2 exchange-protocol scratch state. Defined in BoopsProtocol.cpp;
// read by BoopsHandlers.cpp's per-type tickPostConfirm routines that
// drive the field exchange.
struct XchgState {
    bool     isPrimary;
    uint8_t  phase;
    uint16_t myTagMask;
    uint8_t  myBioChunks;
    bool     sentMyManifest;
    uint16_t peerTagMask;
    uint8_t  peerBioChunks;
    bool     gotPeerManifest;
    uint16_t localReceivedMask;
    uint8_t  peerBioChunkMask;
    bool     peerStreamStarted;
    bool     peerStreamDone;
    bool     myStreamDone;
    bool     sentMyStreamReq;
    bool     myFinSent;
    bool     peerFinSeen;
    bool     myFinAckSent;
    bool     peerFinAckSeen;
    uint32_t lastMetaTxMs;
    uint32_t lastMetaWords[NEC_MAX_WORDS];
    size_t   lastMetaCount;
    uint8_t  mySeq;
    bool     fieldsCached;
    BadgeInfo::Fields localCache;
    // (legacy v2 throttle — unused since v3 pingpong took over.)
    uint32_t lastServedStreamReqMs;

    // v3 pingpong protocol state. Both sides walk a fixed tag order
    // (NAME → TITLE → COMPANY → EMAIL → WEB → PHONE → BIO), send
    // exactly ONE FIELD frame per cursor position, and advance when
    // peer has reciprocated. No manifest handshake, no STREAM_REQ
    // dance — every round is a single-field swap that self-recovers
    // via simple retransmit.
    uint8_t  ppCursor;            // index into kPpTagOrder
    bool     ppMySent;            // sent FIELD for current cursor
    uint32_t ppLastFieldTxMs;     // throttle for current-field retx
    bool     ppDoneSent;          // sent the END marker
    uint32_t ppLastDoneTxMs;      // throttle for END retx
    // Bits of FIELD tags peer has explicitly ACK'd (received). Without
    // ACKs, both sides retx FIELD constantly and crowd the half-duplex
    // channel so badly that nothing lands. Stop retxing as soon as peer
    // confirms receipt.
    uint16_t ppPeerAckMask;
};

extern XchgState s_xchg;

// Codec — encoders push frames (handlers compose), decoders parse incoming
// frames (handlers also rx-handle in tickPostConfirm).
int encodeManifest(uint32_t* words, uint16_t tagMask,
                   uint8_t bioChunks, uint8_t seq);
int encodeStreamReq(uint32_t* words, uint8_t seq);
int encodeDataFrame(uint32_t* words, uint8_t seq,
                    uint8_t groupCount, bool hasMore,
                    const uint8_t* tlvBytes, size_t tlvLen);
int encodeNeed(uint32_t* words, uint8_t tag,
               uint8_t chunkIdx, uint8_t seq);
int encodeFin(uint32_t* words, uint8_t seq);
int encodeFinAck(uint32_t* words, uint8_t seq);

bool decodeManifest(const uint32_t* words, size_t count,
                    uint16_t* tagMask, uint8_t* bioChunks);
bool decodeNeed(const uint32_t* words, size_t count,
                uint8_t* tag, uint8_t* chunkIdx);
bool parseDataHeader(const uint32_t* words, size_t count,
                     uint8_t* seq, uint8_t* groupCount,
                     bool* hasMore);

// Local-field accessor + receive-side stores. Used by handlers when
// serving NEED requests and when assembling DATA frames from peer.
const char* getLocalField(uint8_t tag);
void storeReceivedField(uint8_t tag, const char* val, uint8_t len);
void storeBioChunk(uint8_t chunkIdx, const char* val, uint8_t len);

// Early-frame stash — race workaround in beacon→exchange transition.
// Defined in BoopsProtocol.cpp; handlers consume it on the first tick.
extern bool s_haveEarlyFrame;
extern nec_mw_result_t s_earlyFrame;

}  // namespace BadgeBoops
