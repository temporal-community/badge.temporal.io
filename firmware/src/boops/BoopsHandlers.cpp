// BoopsHandlers.cpp — Per-boop-type behavior tables.
//
// Five handlers: peer / exhibit / queue / kiosk / checkin (+ unknown
// fallback). Each defines the on-lock action, post-confirm field
// exchange policy, and screen content drawing. The state machine in
// BoopsProtocol.cpp routes dispatch through handlerFor(BoopType).

#include "BadgeBoops.h"

#include "../identity/BadgeInfo.h"
#include "../infra/DebugLog.h"
#include "../hardware/Haptics.h"
#include "../ui/Images.h"
#include "../hardware/LEDmatrix.h"
#include "../hardware/oled.h"

#include "Internal.h"

#include <Arduino.h>
#include <cstring>

#include "esp_random.h"

extern LEDmatrix badgeMatrix;

static const char* TAG = "BadgeBoops";

namespace BadgeBoops {
// ═══════════════════════════════════════════════════════════════════════════
// 6) Per-type handler dispatch
// ═══════════════════════════════════════════════════════════════════════════
//
// Five handlers: peer / exhibit / queue / kiosk / checkin. Each owns the
// post-confirm behavior and the BoopScreen left-content drawing for its
// BoopType. The state machine above calls handlerFor(t)->onLock() and
// ->tickPostConfirm() — switch statements stay out of the hot paths.

// Helper — truncate-fit a string into max width using FONT_SMALL / LARGE /
// TINY by ellipsizing. Returns the final string pointer (may be out or src).
static const char* fitStr(oled& d, const char* src, int maxW,
                          char* out, size_t outCap) {
    if (!src || !src[0]) return "";
    int w = d.getStrWidth(src);
    if (w <= maxW) return src;
    // Trim until it fits, replacing last two chars with ".."
    size_t n = strlen(src);
    size_t copyN = (n < outCap - 1) ? n : outCap - 1;
    strncpy(out, src, copyN);
    out[copyN] = '\0';
    while (copyN > 2) {
        out[copyN - 1] = '\0';
        out[copyN - 2] = '.';
        out[copyN - 3] = '.';
        if (d.getStrWidth(out) <= maxW) return out;
        copyN--;
    }
    return out;
}

// ─── Peer handler ────────────────────────────────────────────────────────────

static void peer_onLock(BoopStatus& /*s*/) {
    // No extra prep — the manifest exchange that fills out partner
    // name/title/company/email/etc kicks off in BOOP_PHASE_EXCHANGE
    // immediately after this returns.
}

// ─── v2 exchange — manifest-driven streaming with NEED repair ──────────────
//
// STRICT PRIMARY/SECONDARY ASYMMETRY.  To avoid the half-duplex standoff
// where both sides transmit at the same time and neither hears the other,
// the protocol is coordinated so primary always speaks first in every
// round and secondary is purely reactive — secondary only transmits in
// direct response to an incoming primary frame.
//
// At any point each side is one of:
//   XCHG_MANIFEST_XCHG   — primary TX'd MANIFEST, waiting for secondary's reply
//   XCHG_PRIMARY_PULL    — primary STREAM_REQ'd, secondary streams DATA back
//   XCHG_PRIMARY_REPAIR  — primary NEEDs missing tags; secondary answers
//   XCHG_SECONDARY_PULL  — roles flip: secondary STREAM_REQs, primary streams
//   XCHG_SECONDARY_REPAIR
//   XCHG_FIN_XCHG        — FIN + FINACK 3-way close
//   XCHG_DONE
//
// Retransmit: every "meta" frame (MANIFEST, STREAM_REQ, NEED, FIN) is
// cached in s_xchg.lastMetaWords/Count with lastMetaTxMs.  Only the
// INITIATING side retransmits — the replying side re-responds whenever a
// duplicate initiator frame arrives (see processXchgFrame).  This way the
// two sides never retransmit simultaneously and the link self-heals.

// (Legacy v2 manifest/streaming helpers — kept defined below for now,
//  unused since v3 pingpong replaced peer_tickPostConfirm.)
static bool processXchgFrame(const nec_mw_result_t& f, uint32_t nowMs);
static void txMeta(const uint32_t* words, size_t count, uint32_t nowMs);
static void retxMetaIfStale(uint32_t nowMs);
static void serveStreamReq(uint32_t nowMs);
static void serveNeed(uint8_t tag, uint8_t chunkIdx, uint32_t nowMs);
static bool peerPullSatisfied();
static void fireNextNeed(uint32_t nowMs);
static void sendMyManifest(uint32_t nowMs);
static void sendMyStreamReq(uint32_t nowMs);
static void sendMyFin(uint32_t nowMs);
static void sendMyFinAck(uint8_t peerSeq, uint32_t nowMs);
static void primaryTick(uint32_t nowMs);
static void secondaryTick(uint32_t nowMs);

// ─── v3 pingpong protocol ──────────────────────────────────────────────────
//
// Replaces the v2 MANIFEST / STREAM_REQ / DATA / NEED / FIN / FINACK
// dance with a simple symmetric per-field swap. Both sides walk
// kPpTagOrder in lockstep: send my FIELD(tag), wait for peer's
// FIELD(tag), advance cursor. Each round handles exactly one field, so
// a lost frame just means one round is retried (not the entire
// stream). Per-tag arrival fires BoopFeedback chips so the user sees
// "name… title… company…" appear progressively on the boop screen.
constexpr uint8_t kPpTagOrder[] = {
    FIELD_NAME, FIELD_TITLE, FIELD_COMPANY,
    FIELD_EMAIL, FIELD_WEBSITE, FIELD_PHONE,
    FIELD_BIO,
};
constexpr uint8_t kPpTagOrderCount =
    sizeof(kPpTagOrder) / sizeof(kPpTagOrder[0]);

// Field retx interval — only fires when peer hasn't ACK'd yet. Long
// enough that the channel has clear silence between our retransmits so
// peer's own TX (FIELD or ACK) can land instead of colliding with us.
inline constexpr uint32_t kPpFieldRetxMs       = 1500;
inline constexpr uint32_t kPpFieldRetxJitterMs = 400;
inline constexpr uint32_t kPpDoneRetxMs        = 800;
inline constexpr uint32_t kPpSecondaryHoldMs   = 150;

// Send a single FIELD frame (one TLV) on the wire. Reuses IR_BOOP_DATA
// opcode and the existing TLV format (tag, chunkIdx, len, payload[]).
static void ppSendField(uint8_t tag, uint32_t nowMs);
// Send a FIELD ACK frame. 3-word frame, ~150 ms wire time. Reuses the
// IR_BOOP_NEED opcode (decoder already extracts the tag from byte 1).
// Receiving side stops retxing the FIELD once the matching ACK arrives.
static void ppSendAck(uint8_t tag, uint32_t nowMs);
// Send the END marker (reuses IR_BOOP_FIN) — "I've sent every field".
static void ppSendDone(uint32_t nowMs);
// Process one inbound frame in pingpong context. Stores received
// FIELD values into boopStatus.peer*, sends ACK back, marks
// peerFinSeen on END, sets ppPeerAckMask on incoming ACK.
static void processPpFrame(const nec_mw_result_t& f, uint32_t nowMs);

// Forward decl — definition lives further down (with the v2 helpers).
static size_t packOneTlv(uint8_t* out, size_t pos, size_t maxBytes,
                          uint8_t tag, uint8_t chunkIdx,
                          const char* payload, uint8_t len);

// Peer-deaf watchdog. Without this, an exchange against a peer running the
// pre-offline firmware (doFieldExchange=false → never sends MANIFEST) sits
// in XCHG_MANIFEST_XCHG forever and the screen pegs at "Handshake...".
//
// kPeerSilenceTimeoutMs — no peer MANIFEST seen at all. We finish with just
//   the UID (same outcome as a non-exchange handler), so the user isn't
//   trapped on the boop screen.
// kExchangeMaxMs — peer manifest arrived but the full handshake (PULL +
//   FIN + FINACK both ways) didn't complete in time. Save what we have and
//   exit; the protocol's own retx logic should finish the happy path well
//   under this bound (~3-5 s typical).
inline constexpr uint32_t kPeerSilenceTimeoutMs = 60000;
inline constexpr uint32_t kExchangeMaxMs        = 120000;

static bool peer_tickPostConfirm(BoopStatus& s, uint32_t nowMs) {
    // -------- 0. Hard timeout. --------
    const uint32_t elapsed = nowMs - s.phaseEnteredMs;
    if (elapsed > kExchangeMaxMs) {
        Serial.printf("[%s] EXCHANGE max %ums hit (cursor=%u rx=0x%03X) — finishing\n",
                      TAG, (unsigned)elapsed,
                      (unsigned)s_xchg.ppCursor,
                      (unsigned)s_xchg.localReceivedMask);
        s_xchg.phase = XCHG_DONE;
        return true;
    }

    // -------- 1. Drain inbound frames. --------
    if (s_haveEarlyFrame) {
        // The beacon-phase early-MANIFEST stash predates v3. If a peer
        // running an older firmware sent a MANIFEST during our DONE
        // burst, just drop it — we'll exchange data via FIELDs now.
        s_haveEarlyFrame = false;
    }
    {
        nec_mw_result_t frame;
        int drainBudget = 16;
        while (drainBudget-- > 0 && BadgeIR::recvFrame(&frame, 0)) {
            processPpFrame(frame, nowMs);
        }
    }

    // -------- 2. Pingpong (ASYMMETRIC request/response). --------
    //
    // Primary drives every round: it sends FIELD(T), waits silently for
    // peer's reply, advances on RX. Secondary stays silent until peer's
    // FIELD(T) arrives — at which point processPpFrame fires its own
    // FIELD(T) right back AS the synchronous reply. Half-duplex
    // collisions are impossible because the two sides never transmit
    // unprovoked at the same moment. No explicit ACK frame needed —
    // each side's FIELD(T) IS the other side's ACK.
    if (s_xchg.ppCursor < kPpTagOrderCount) {
        const uint8_t T = kPpTagOrder[s_xchg.ppCursor];
        const bool peerSent = (s_xchg.localReceivedMask & (1U << T)) != 0;
        const bool mySent = s_xchg.ppMySent;

        if (peerSent && mySent) {
            Serial.printf("[%s] PP advance: cursor %u→%u (tag %u %s done)\n",
                          TAG,
                          (unsigned)s_xchg.ppCursor,
                          (unsigned)(s_xchg.ppCursor + 1),
                          (unsigned)T,
                          BadgeBoops::fieldShortName(T));
            s_xchg.ppCursor++;
            s_xchg.ppMySent = false;
            s_xchg.ppLastFieldTxMs = 0;
        } else if (s_xchg.isPrimary) {
            // Primary leads. Always TX FIELD(T) — even when our value
            // is empty — so secondary, which is purely reactive, gets
            // a frame to reply to and the cursor can advance in
            // lockstep. The previous "skip silently if myEmpty &&
            // !peerSent" optimization stranded boops where the two
            // badges had different sparse field sets: secondary would
            // sit at cursor T forever waiting for a primary FIELD that
            // primary had decided not to send, and the boop only
            // unwedged at the 120 s exchange timeout.  Empty FIELD
            // frames are 3-word TLV-headers (~150 ms wire) — at most 7
            // wasted per all-empty boop, far cheaper than the timeout
            // they replace.
            if (!mySent) {
                ppSendField(T, nowMs);
                s_xchg.ppMySent = true;
                s_xchg.ppLastFieldTxMs = nowMs;
            } else if (!peerSent) {
                const uint32_t jitter = esp_random() % kPpFieldRetxJitterMs;
                if ((nowMs - s_xchg.ppLastFieldTxMs) > kPpFieldRetxMs + jitter) {
                    ppSendField(T, nowMs);
                    s_xchg.ppLastFieldTxMs = nowMs;
                    BoopFeedback::pushMarqueeEvent("retry");
                }
            }
        } else {
            // Secondary: stay silent unless peer pings us. Apply the
            // same "skip if both empty" optimization once enough time
            // has passed — if primary hasn't sent FIELD(T) within
            // kPpSecondaryEmptyWaitMs of cursor entry, primary
            // probably skipped this slot. Advance to keep cursors in
            // sync. Catch-up in processPpFrame handles any crossings.
            constexpr uint32_t kPpSecondaryEmptyWaitMs = 2000;
            const char* myVal = (T < FIELD_TAG_COUNT && T != FIELD_ATTENDEE_TYPE)
                                ? getLocalField(T) : "";
            const bool myEmpty = (!myVal || myVal[0] == '\0');
            if (myEmpty && !peerSent) {
                if (s_xchg.ppLastFieldTxMs == 0) {
                    // Use ppLastFieldTxMs to track our wait start.
                    s_xchg.ppLastFieldTxMs = nowMs;
                } else if ((nowMs - s_xchg.ppLastFieldTxMs) > kPpSecondaryEmptyWaitMs) {
                    Serial.printf("[%s] PP skip empty (sec): cursor %u→%u (tag %u %s)\n",
                                  TAG,
                                  (unsigned)s_xchg.ppCursor,
                                  (unsigned)(s_xchg.ppCursor + 1),
                                  (unsigned)T,
                                  BadgeBoops::fieldShortName(T));
                    s_xchg.ppCursor++;
                    s_xchg.ppMySent = false;
                    s_xchg.ppLastFieldTxMs = 0;
                }
            }
            // else: silent. processPpFrame() handles incoming FIELDs.
        }
        // Secondary: silent. processPpFrame() sends our reply
        // synchronously when peer's FIELD arrives.
    } else if (s_xchg.isPrimary) {
        // Primary leads the DONE handshake. Send once on cursor end,
        // retx until secondary echoes back. Symmetric send-on-tick
        // collides every cycle on half-duplex (both badges hit cursor
        // end at the same moment and the 3-word DONE frames mask each
        // other), so secondary stays silent until processPpFrame()
        // sees primary's FIN and sends its own as a synchronous reply.
        if (!s_xchg.ppDoneSent) {
            ppSendDone(nowMs);
            s_xchg.ppDoneSent = true;
            s_xchg.ppLastDoneTxMs = nowMs;
        } else if (!s_xchg.peerFinSeen &&
                   (nowMs - s_xchg.ppLastDoneTxMs) > kPpDoneRetxMs) {
            ppSendDone(nowMs);
            s_xchg.ppLastDoneTxMs = nowMs;
            BoopFeedback::pushMarqueeEvent("retry done");
        }
    } else if (s_xchg.peerFinSeen && !s_xchg.ppDoneSent) {
        // Secondary at end-of-cursor AND already saw primary's DONE
        // (it arrived via processPpFrame before our cursor reached the
        // end). Fire our DONE now — without this, processPpFrame's
        // "send DONE on FIN rx" branch only runs ONCE on FIN arrival;
        // if our cursor hadn't reached end at that moment we'd never
        // send DONE at all.
        ppSendDone(nowMs);
        s_xchg.ppDoneSent = true;
        s_xchg.ppLastDoneTxMs = nowMs;
    }
    // Secondary with !peerFinSeen: stays silent. Its DONE TX fires
    // synchronously in processPpFrame() when primary's FIN arrives.

    // -------- 3. Status message + completion. --------
    if (s_xchg.ppCursor < kPpTagOrderCount) {
        snprintf(s.statusMsg, sizeof(s.statusMsg), "share %s",
                 BadgeBoops::fieldShortName(kPpTagOrder[s_xchg.ppCursor]));
    } else if (!s_xchg.peerFinSeen) {
        snprintf(s.statusMsg, sizeof(s.statusMsg), "closing");
    } else {
        snprintf(s.statusMsg, sizeof(s.statusMsg), "done");
    }

    const bool done = (s_xchg.ppCursor >= kPpTagOrderCount) &&
                       s_xchg.ppDoneSent && s_xchg.peerFinSeen;
    if (done) s_xchg.phase = XCHG_DONE;
    return done;
}

// ─── v3 pingpong helpers ───────────────────────────────────────────────────

static void ppSendField(uint8_t tag, uint32_t nowMs) {
    // Resolve my value for this tag. Empty-but-present is fine: peer
    // still gets a FIELD frame, advances their cursor, and learns I
    // didn't share that field.
    const char* v = "";
    uint8_t len = 0;
    // Skip server-only fields and tags peer can't use.  Mirrors
    // BoopsProtocol.cpp's tagIsTxEligible() but inlined here so we
    // don't depend on a static helper from another TU.
    if (tag < FIELD_TAG_COUNT && tag != FIELD_ATTENDEE_TYPE) {
        v = getLocalField(tag);
        if (!v) v = "";
        const size_t slen = strlen(v);
        // Cap len at what one frame can carry: NEC_MAX_WORDS=64 → 244 B
        // payload area; minus 2 B TLV header = 242 B max value bytes.
        // Bio chunks are already <=32 B; non-bio fields top out at 128 B.
        len = (uint8_t)(slen > 240 ? 240 : slen);
    }

    uint8_t buf[NEC_MAX_WORDS * 4];
    size_t wrote = packOneTlv(buf, 0, sizeof(buf),
                               tag, /*chunkIdx=*/0, v, len);
    if (wrote == 0) {
        // packOneTlv returned 0 — write the bare 2-byte header so peer
        // still sees a FIELD frame for this tag (advances their cursor).
        buf[0] = (uint8_t)(tag & 0x0F);
        buf[1] = 0;
        wrote = 2;
    }

    s_xchg.mySeq++;
    uint32_t words[NEC_MAX_WORDS];
    const int n = encodeDataFrame(words, s_xchg.mySeq,
                                   /*groupCount=*/1, /*hasMore=*/false,
                                   buf, wrote);
    if (n <= 0) return;
    Serial.printf("[%s] PP FIELD tx tag=%u (%s) len=%u w=%d\n",
                  TAG, (unsigned)tag,
                  BadgeBoops::fieldShortName(tag),
                  (unsigned)len, n);
    BadgeIR::sendFrame(words, (size_t)n);
    boopStatus.fieldTxMask |= (1U << tag);
    BoopFeedback::onPeerFieldTx(tag);
    boopStatus.lastFieldDirection = 1;
    boopStatus.lastFieldEventMs = nowMs;
}

static void ppSendDone(uint32_t nowMs) {
    uint32_t words[3];
    s_xchg.mySeq++;
    const int n = encodeFin(words, s_xchg.mySeq);
    Serial.printf("[%s] PP DONE tx seq=%u\n", TAG, (unsigned)s_xchg.mySeq);
    BadgeIR::sendFrame(words, (size_t)n);
    s_xchg.myFinSent = true;
    (void)nowMs;
}

static void ppSendAck(uint8_t tag, uint32_t nowMs) {
    // Reuse encodeNeed — same word[0] shape (type | tag<<8) and same
    // 3-word UID-bearing envelope. chunkIdx=0, seq=0 — we don't use
    // either field for ACK semantics.
    uint32_t words[3];
    const int n = encodeNeed(words, tag, /*chunkIdx=*/0, /*seq=*/0);
    Serial.printf("[%s] PP ACK tx tag=%u (%s)\n", TAG, (unsigned)tag,
                  BadgeBoops::fieldShortName(tag));
    BadgeIR::sendFrame(words, (size_t)n);
    (void)nowMs;
}

static void processPpFrame(const nec_mw_result_t& f, uint32_t nowMs) {
    if (f.count < 3) return;
    const uint8_t type = (uint8_t)(f.words[0] & 0xFF);

    if (type == IR_BOOP_FIN) {
        if (!s_xchg.peerFinSeen) {
            Serial.printf("[%s] PP DONE rx\n", TAG);
        }
        s_xchg.peerFinSeen = true;
        // Secondary's synchronous DONE reply. Idempotent: re-send even
        // if we've already sent once, because primary retxing means
        // primary hasn't seen our reply.
        if (!s_xchg.isPrimary &&
            s_xchg.ppCursor >= kPpTagOrderCount) {
            ppSendDone(nowMs);
            s_xchg.ppDoneSent = true;
            s_xchg.ppLastDoneTxMs = nowMs;
        }
        return;
    }

    if (type != IR_BOOP_DATA) return;  // ignore stray legacy frames
                                        // (and any pre-asymmetric ACK)

    uint8_t seq, groupCount;
    bool hasMore;
    if (!parseDataHeader(f.words, f.count, &seq, &groupCount, &hasMore)) return;
    if (groupCount == 0) return;

    // Unpack TLV bytes (we expect 1 TLV per FIELD frame, but loop in case
    // a peer is sending packed TLVs).
    const size_t bytesAvail = (f.count - 3) * 4;
    uint8_t tlvs[NEC_MAX_WORDS * 4];
    size_t tlvLen = (bytesAvail < sizeof(tlvs)) ? bytesAvail : sizeof(tlvs);
    for (size_t i = 0; i < tlvLen; i++) {
        tlvs[i] = (uint8_t)((f.words[3 + i / 4] >> ((i % 4) * 8)) & 0xFFU);
    }

    size_t pos = 0;
    for (uint8_t g = 0; g < groupCount && pos + 2 <= tlvLen; g++) {
        const uint8_t hdr = tlvs[pos++];
        const uint8_t tag = (uint8_t)(hdr & 0x0F);
        const uint8_t chunkIdx = (uint8_t)((hdr >> 4) & 0x0F);
        const uint8_t len = tlvs[pos++];
        if (pos + len > tlvLen) break;
        const char* payload = (const char*)(tlvs + pos);
        pos += len;

        if (tag >= FIELD_TAG_COUNT) continue;

        if (tag == FIELD_BIO) {
            if (len > 0) {
                storeBioChunk(chunkIdx, payload, len);
                s_xchg.peerBioChunkMask |= (1U << chunkIdx);
                boopStatus.peerBioChunkMask = s_xchg.peerBioChunkMask;
            }
        } else if (len > 0) {
            storeReceivedField(tag, payload, len);
        }

        // Always mark as seen so the cursor can advance, even if the
        // value was empty (peer signaled "I have nothing for this tag").
        const bool wasNew = !(s_xchg.localReceivedMask & (1U << tag));
        s_xchg.localReceivedMask |= (1U << tag);
        boopStatus.fieldRxMask |= (1U << tag);
        if (wasNew) {
            BoopFeedback::onPeerFieldRx(tag);
            boopStatus.lastFieldDirection = 2;
            boopStatus.lastFieldEventMs = nowMs;
            Serial.printf("[%s] PP FIELD rx tag=%u (%s) len=%u\n",
                          TAG, (unsigned)tag,
                          BadgeBoops::fieldShortName(tag),
                          (unsigned)len);
        }

        // ── Cursor catch-up: if peer is sending a tag PAST our current
        //    cursor, jump forward. Peer wouldn't have advanced unless
        //    they already had our earlier values.
        int tagIdx = -1;
        for (uint8_t i = 0; i < kPpTagOrderCount; i++) {
            if (kPpTagOrder[i] == tag) { tagIdx = (int)i; break; }
        }
        if (tagIdx > (int)s_xchg.ppCursor) {
            Serial.printf("[%s] PP cursor catch-up: %u → %d (peer ahead "
                          "on tag %u %s)\n",
                          TAG,
                          (unsigned)s_xchg.ppCursor, tagIdx,
                          (unsigned)tag,
                          BadgeBoops::fieldShortName(tag));
            s_xchg.ppCursor = (uint8_t)tagIdx;
            s_xchg.ppMySent = false;
            s_xchg.ppLastFieldTxMs = 0;
        }

        // ── Secondary's synchronous reply: we just got primary's
        //    FIELD(T), so fire OUR FIELD(T) right back. This IS the ACK
        //    primary is waiting for AND delivers our value in one
        //    go — half the wire traffic of an ACK+FIELD scheme, and
        //    no chance of TX collision because primary won't send
        //    again until they hear from us.
        //
        //    ALWAYS re-respond, even if we've moved past this cursor
        //    locally: a primary retx of FIELD(T) means primary still
        //    hasn't seen our reply (frame was lost on the air). If we
        //    don't re-reply, primary retransmits forever and the
        //    handshake wedges right at the boundary between fields and
        //    DONE. Idempotent: if primary already had our reply it
        //    just gets a duplicate and ignores it.
        if (!s_xchg.isPrimary && tagIdx >= 0 &&
            tagIdx < (int)kPpTagOrderCount) {
            ppSendField(tag, nowMs);
            // Only update mySent state if this IS our current cursor;
            // re-replies for past cursors are courtesy unblockers, not
            // forward progress.
            if (tagIdx == (int)s_xchg.ppCursor) {
                s_xchg.ppMySent = true;
                s_xchg.ppLastFieldTxMs = nowMs;
            }
        }
    }
}

// ─── Role-specific tick logic ──────────────────────────────────────────────

// Primary drives every phase — sends the initiating frame, retransmits on
// kRetxMs if no reply.  Secondary-reactive phases (serving STREAM_REQ /
// NEED) are entirely handled inside processXchgFrame.
static void primaryTick(uint32_t nowMs) {
    switch (s_xchg.phase) {
    case XCHG_MANIFEST_XCHG:
        if (!s_xchg.sentMyManifest) {
            sendMyManifest(nowMs);
        } else if (!s_xchg.gotPeerManifest) {
            retxMetaIfStale(nowMs);
        }
        break;

    case XCHG_PRIMARY_PULL:
        if (s_xchg.peerStreamDone) {
            if (peerPullSatisfied()) {
                sendMyFin(nowMs);
                s_xchg.phase = XCHG_SECONDARY_PULL;
                // Reset for secondary's upcoming stream into us.  Primary
                // sentMyStreamReq stays true (historical); we track
                // secondary's separately via peerFinSeen and the stream flags.
                s_xchg.peerStreamStarted = false;
                s_xchg.peerStreamDone    = false;
            } else {
                s_xchg.phase = XCHG_PRIMARY_REPAIR;
                fireNextNeed(nowMs);
            }
        } else if (!s_xchg.peerStreamStarted) {
            // Retx STREAM_REQ only if peer hasn't started streaming.  Once
            // DATA begins to arrive we know peer got the request; don't
            // interrupt a multi-second bio burst with a collision.
            retxMetaIfStale(nowMs);
        }
        break;

    case XCHG_PRIMARY_REPAIR:
        if (peerPullSatisfied()) {
            sendMyFin(nowMs);
            s_xchg.phase = XCHG_SECONDARY_PULL;
            s_xchg.peerStreamStarted = false;
            s_xchg.peerStreamDone    = false;
        } else {
            retxMetaIfStale(nowMs);
        }
        break;

    case XCHG_SECONDARY_PULL:
    case XCHG_SECONDARY_REPAIR:
        // Primary is serving secondary's STREAM_REQ / NEED frames via
        // processXchgFrame.  We retransmit our FIN (sent when our own
        // pull finished) until we see secondary's FINACK — that's proof
        // secondary started pulling.
        if (!s_xchg.peerFinAckSeen) {
            retxMetaIfStale(nowMs);
        }
        break;

    case XCHG_FIN_XCHG:
        if (s_xchg.myFinSent && !s_xchg.peerFinAckSeen) {
            retxMetaIfStale(nowMs);
        }
        if (s_xchg.peerFinSeen && s_xchg.peerFinAckSeen &&
            s_xchg.myFinSent && s_xchg.myFinAckSent) {
            s_xchg.phase = XCHG_DONE;
        }
        break;

    case XCHG_DONE:
    default:
        break;
    }
}

// Secondary is mostly reactive in MANIFEST / PRIMARY_PULL / PRIMARY_REPAIR —
// it only transmits from within processXchgFrame when an incoming primary
// frame asks it to.  In SECONDARY_PULL onwards, secondary becomes the
// initiator and takes over the retx responsibility.
//
// MANIFEST_XCHG: secondary holds back ~kSecondaryManifestHoldMs before its
// initial MANIFEST send. The hope is that primary's MANIFEST lands first
// and secondary's processXchgFrame reply path sends ours back to primary
// — that two-step asymmetric handshake avoids the simultaneous-TX collision
// that wedged the symmetric "both fire on entry" version (two new badges
// would synchronize their retx and never hear each other).
//
// If primary's MANIFEST never arrives (e.g., primary still in BEACONING)
// secondary sends anyway after the hold expires; primary's handleBeaconing
// has an early-MANIFEST branch that fast-forwards its confirm and joins us.
inline constexpr uint32_t kSecondaryManifestHoldMs = 350;

static void secondaryTick(uint32_t nowMs) {
    switch (s_xchg.phase) {
    case XCHG_MANIFEST_XCHG:
        if (!s_xchg.sentMyManifest) {
            const uint32_t held = nowMs - boopStatus.phaseEnteredMs;
            if (held >= kSecondaryManifestHoldMs) sendMyManifest(nowMs);
        } else if (!s_xchg.gotPeerManifest) {
            // Retx if primary hasn't answered — covers the case where our
            // initial MANIFEST was lost in a collision AND primary is
            // still in BEACONING (so primary wouldn't retx on its own).
            retxMetaIfStale(nowMs);
        }
        break;
    case XCHG_PRIMARY_PULL:
    case XCHG_PRIMARY_REPAIR:
        // Nothing to do — reactive handlers in processXchgFrame.
        break;

    case XCHG_SECONDARY_PULL:
        // After primary FINs, secondary starts its own pull.  The phase
        // transition to SECONDARY_PULL happened in the IR_BOOP_FIN handler
        // in processXchgFrame.
        if (!s_xchg.sentMyStreamReq) {
            sendMyStreamReq(nowMs);
        } else if (s_xchg.peerStreamDone) {
            if (peerPullSatisfied()) {
                sendMyFin(nowMs);
                s_xchg.phase = XCHG_FIN_XCHG;
            } else {
                s_xchg.phase = XCHG_SECONDARY_REPAIR;
                fireNextNeed(nowMs);
            }
        } else if (!s_xchg.peerStreamStarted) {
            retxMetaIfStale(nowMs);
        }
        break;

    case XCHG_SECONDARY_REPAIR:
        if (peerPullSatisfied()) {
            sendMyFin(nowMs);
            s_xchg.phase = XCHG_FIN_XCHG;
        } else {
            retxMetaIfStale(nowMs);
        }
        break;

    case XCHG_FIN_XCHG:
        if (s_xchg.myFinSent && !s_xchg.peerFinAckSeen) {
            retxMetaIfStale(nowMs);
        }
        if (s_xchg.peerFinSeen && s_xchg.peerFinAckSeen &&
            s_xchg.myFinSent && s_xchg.myFinAckSent) {
            s_xchg.phase = XCHG_DONE;
        }
        break;

    case XCHG_DONE:
    default:
        break;
    }
}

// ─── processXchgFrame — dispatch by type, update state, serve REQs ─────────

static bool processXchgFrame(const nec_mw_result_t& f, uint32_t nowMs) {
    if (f.count < 3) return false;
    const uint8_t type = (uint8_t)(f.words[0] & 0xFF);

    switch (type) {

    case IR_BOOP_MANIFEST: {
        uint16_t mask;
        uint8_t  chunks;
        if (decodeManifest(f.words, f.count, &mask, &chunks)) {
            s_xchg.peerTagMask     = mask;
            s_xchg.peerBioChunks   = chunks;
            s_xchg.gotPeerManifest = true;
            // Unconditional — single per-boop event, not spammy. Lets us
            // confirm "did we ever actually hear peer's manifest" in the
            // serial log without flipping log_boop=1 first.
            Serial.printf("[%s] peer MANIFEST mask=0x%03X bioChunks=%u\n",
                          TAG, (unsigned)mask, (unsigned)chunks);
            // Secondary auto-responds with its own MANIFEST.  We respond
            // to EVERY incoming MANIFEST — including ones that arrive
            // after we've locally advanced out of MANIFEST_XCHG — because
            // primary's retransmit means our prior reply was lost and
            // primary is still stuck waiting on our manifest.  Without
            // this, primary retxes forever while secondary thinks the
            // handshake is complete (the bug that wedged v2.0 in the
            // field).  Idempotent on the happy path: once primary
            // receives one of our replies it advances and stops retxing.
            if (!s_xchg.isPrimary) {
                sendMyManifest(nowMs);
            }
        }
        return true;
    }

    case IR_BOOP_STREAM_REQ: {
        // Receipt of STREAM_REQ implicitly confirms peer received our
        // MANIFEST (they wouldn't know what to ask for otherwise).
        // Advance phase on the secondary side so status msg reflects
        // reality even though the serveStreamReq call is the only
        // real work here.
        if (!s_xchg.isPrimary && s_xchg.phase == XCHG_MANIFEST_XCHG) {
            s_xchg.phase = XCHG_PRIMARY_PULL;
        }
        // Throttle: peer retransmits STREAM_REQ every ~2.1-3.5 s while
        // waiting for our DATA. If we re-serve on every receipt we'll
        // saturate the channel and TX over peer's own retx, never letting
        // the burst land cleanly. 2.5 s lets peer see at least one of our
        // DATA bursts before we attempt another. (DATA bursts are 12-word
        // frames ≈ 700 ms each; 3 such frames take ~2.1 s.)
        constexpr uint32_t kServeMinIntervalMs = 2500;
        if (s_xchg.lastServedStreamReqMs != 0 &&
            (nowMs - s_xchg.lastServedStreamReqMs) < kServeMinIntervalMs) {
            return true;
        }
        s_xchg.lastServedStreamReqMs = nowMs;
        // Serve: reply with 1+ DATA frames covering my entire manifest.
        serveStreamReq(nowMs);
        return true;
    }

    case IR_BOOP_DATA: {
        uint8_t seq, groupCount;
        bool hasMore;
        if (!parseDataHeader(f.words, f.count, &seq, &groupCount, &hasMore)) {
            return false;
        }
        Serial.printf("[%s] DATA rx seq=%u gc=%u hasMore=%d w=%u\n",
                      TAG, (unsigned)seq, (unsigned)groupCount,
                      hasMore ? 1 : 0, (unsigned)f.count);
        // First DATA tells the puller that peer heard the STREAM_REQ —
        // stops further STREAM_REQ retransmits even if the full burst
        // takes seconds (bio chunks).
        s_xchg.peerStreamStarted = true;

        // Walk TLV bytes: word[3..] holds groupCount TLVs packed 4 B per word.
        const size_t bytesAvail = (f.count - 3) * 4;
        // Scratch reassembly buffer for the unpack.  Fixed upper bound
        // based on our max frame size.
        uint8_t tlvs[NEC_MAX_WORDS * 4];
        size_t tlvLen = (bytesAvail < sizeof(tlvs)) ? bytesAvail : sizeof(tlvs);
        for (size_t i = 0; i < tlvLen; i++) {
            tlvs[i] = (uint8_t)((f.words[3 + i / 4] >> ((i % 4) * 8)) & 0xFFU);
        }

        size_t pos = 0;
        for (uint8_t g = 0; g < groupCount && pos + 2 <= tlvLen; g++) {
            const uint8_t hdr      = tlvs[pos++];
            const uint8_t tag      = (uint8_t)(hdr & 0x0FU);
            const uint8_t chunkIdx = (uint8_t)((hdr >> 4) & 0x0FU);
            const uint8_t len      = tlvs[pos++];
            if (pos + len > tlvLen) break;  // truncated
            const char* payload    = (const char*)(tlvs + pos);
            pos += len;

            if (tag >= FIELD_TAG_COUNT) continue;

            if (tag == FIELD_BIO) {
                if (chunkIdx < s_xchg.peerBioChunks &&
                    !(s_xchg.peerBioChunkMask & (1U << chunkIdx))) {
                    storeBioChunk(chunkIdx, payload, len);
                    s_xchg.peerBioChunkMask |= (1U << chunkIdx);
                    boopStatus.peerBioChunkMask = s_xchg.peerBioChunkMask;
                    BoopFeedback::onPeerFieldRx(tag);
                    boopStatus.lastFieldDirection = 2;
                    boopStatus.lastFieldEventMs   = nowMs;
                    // All bio chunks received → mark the tag itself as received.
                    const uint8_t full = (uint8_t)((1U << s_xchg.peerBioChunks) - 1U);
                    if ((s_xchg.peerBioChunkMask & full) == full) {
                        s_xchg.localReceivedMask |= (1U << FIELD_BIO);
                        boopStatus.fieldRxMask   |= (1U << FIELD_BIO);
                    }
                }
            } else {
                if (!(s_xchg.localReceivedMask & (1U << tag))) {
                    storeReceivedField(tag, payload, len);
                    s_xchg.localReceivedMask |= (1U << tag);
                    boopStatus.fieldRxMask   |= (1U << tag);
                    BoopFeedback::onPeerFieldRx(tag);
                    boopStatus.lastFieldDirection = 2;
                    boopStatus.lastFieldEventMs   = nowMs;
                }
            }
        }

        if (!hasMore) s_xchg.peerStreamDone = true;
        return true;
    }

    case IR_BOOP_NEED: {
        uint8_t tag, chunkIdx;
        if (decodeNeed(f.words, f.count, &tag, &chunkIdx)) {
            serveNeed(tag, chunkIdx, nowMs);
        }
        return true;
    }

    case IR_BOOP_FIN: {
        s_xchg.peerFinSeen = true;
        // Ack peer's FIN (idempotent — if peer retransmits FIN, we
        // re-ACK so they eventually see a FINACK).
        const uint8_t peerSeq = (uint8_t)((f.words[0] >> 8) & 0xFFU);
        sendMyFinAck(peerSeq, nowMs);
        // Secondary receiving primary's FIN → roles flip, secondary now pulls.
        if (!s_xchg.isPrimary &&
            (s_xchg.phase == XCHG_PRIMARY_PULL ||
             s_xchg.phase == XCHG_PRIMARY_REPAIR)) {
            s_xchg.phase = XCHG_SECONDARY_PULL;
            // Reset stream trackers for the upcoming primary-streams-to-me
            // direction.  sentMyStreamReq gets toggled false so
            // secondaryTick's "send my STREAM_REQ now" branch fires on
            // the next tick.
            s_xchg.peerStreamStarted = false;
            s_xchg.peerStreamDone    = false;
            s_xchg.sentMyStreamReq   = false;
        }
        // Primary receiving secondary's FIN → both sides are done pulling,
        // enter FIN_XCHG.
        else if (s_xchg.isPrimary &&
                 (s_xchg.phase == XCHG_SECONDARY_PULL ||
                  s_xchg.phase == XCHG_SECONDARY_REPAIR)) {
            s_xchg.phase = XCHG_FIN_XCHG;
        }
        return true;
    }

    case IR_BOOP_FINACK: {
        s_xchg.peerFinAckSeen = true;
        return true;
    }

    default:
        return false;
    }
}

// ─── meta-frame TX + retx ────────────────────────────────────────────────

static void txMeta(const uint32_t* words, size_t count, uint32_t nowMs) {
    BadgeIR::sendFrame(words, count);
    memcpy(s_xchg.lastMetaWords, words, count * sizeof(uint32_t));
    s_xchg.lastMetaCount = count;
    s_xchg.lastMetaTxMs  = nowMs;
}

static void retxMetaIfStale(uint32_t nowMs) {
    if (s_xchg.lastMetaCount == 0) return;
    // Pick the interval based on what kind of response we're waiting for.
    // STREAM_REQ / NEED expect a DATA burst back — slow retx so our
    // retransmit doesn't collide with the mid-burst wire.  MANIFEST /
    // FIN expect a short 3-word reply — fast retx so a single dropped
    // handshake frame doesn't stall the phase flip for ~3 s.  (FINACK
    // isn't cached for retx; the FIN sender's retx triggers another
    // FINACK from the receiver's IR_BOOP_FIN handler.)
    const uint8_t lastType = (uint8_t)(s_xchg.lastMetaWords[0] & 0xFFU);
    const bool waitingForData =
        (lastType == IR_BOOP_STREAM_REQ) || (lastType == IR_BOOP_NEED);
    const uint32_t base = waitingForData ? kRetxMsSlow : kRetxMsFast;
    // Jitter ± up to 25 % of the base so two badges that ended up
    // synchronised (both initial MANIFESTs collided, both started their
    // 1 s timer at the same instant) decorrelate within a few retries
    // instead of colliding forever in lockstep. esp_random() is cheap.
    const uint32_t jitter = esp_random() % (base / 2);  // 0 .. base/2
    const uint32_t interval = (base * 3 / 4) + jitter;  // 0.75x .. 1.25x
    if ((nowMs - s_xchg.lastMetaTxMs) < interval) return;
    Serial.printf("[%s] retx meta type=0x%02X (%s, %ums)\n",
                  TAG, lastType, waitingForData ? "slow" : "fast",
                  (unsigned)interval);
    BadgeIR::sendFrame(s_xchg.lastMetaWords, s_xchg.lastMetaCount);
    s_xchg.lastMetaTxMs = nowMs;

    // Surface the retry as a marquee chip so the user sees the protocol
    // is still alive when the channel is being re-tried. Short label so
    // it fits in the 11-char chip ring slot. The frame type tells us
    // what we're retrying (manifest/req/need/fin); show that so a
    // wedge in a specific phase is visible at a glance.
    const char* retryLabel = "retry";
    switch (lastType) {
        case IR_BOOP_MANIFEST:   retryLabel = "rtx mf";  break;
        case IR_BOOP_STREAM_REQ: retryLabel = "rtx req"; break;
        case IR_BOOP_NEED:       retryLabel = "rtx nd";  break;
        case IR_BOOP_FIN:        retryLabel = "rtx fin"; break;
        default: break;
    }
    BoopFeedback::pushMarqueeEvent(retryLabel);
}

static void sendMyManifest(uint32_t nowMs) {
    uint32_t words[3];
    s_xchg.mySeq++;
    const int n = encodeManifest(words, s_xchg.myTagMask,
                                  s_xchg.myBioChunks, s_xchg.mySeq);
    txMeta(words, (size_t)n, nowMs);
    s_xchg.sentMyManifest = true;
    // Unconditional — first protocol event of the exchange phase. Pairs
    // with the "peer MANIFEST mask=…" log so the serial trace shows both
    // sides talking even with log_boop off.
    Serial.printf("[%s] MANIFEST tx mask=0x%03X bio=%u\n",
                  TAG, (unsigned)s_xchg.myTagMask,
                  (unsigned)s_xchg.myBioChunks);
}

static void sendMyStreamReq(uint32_t nowMs) {
    uint32_t words[3];
    s_xchg.mySeq++;
    const int n = encodeStreamReq(words, s_xchg.mySeq);
    txMeta(words, (size_t)n, nowMs);
    s_xchg.sentMyStreamReq    = true;
    s_xchg.peerStreamStarted  = false;  // fresh request → retx path armed
    s_xchg.peerStreamDone     = false;
    Serial.printf("[%s] STREAM_REQ tx seq=%u\n", TAG, (unsigned)s_xchg.mySeq);
}

static void sendMyFin(uint32_t nowMs) {
    uint32_t words[3];
    s_xchg.mySeq++;
    const int n = encodeFin(words, s_xchg.mySeq);
    txMeta(words, (size_t)n, nowMs);
    s_xchg.myFinSent = true;
    Serial.printf("[%s] FIN tx seq=%u\n", TAG, (unsigned)s_xchg.mySeq);
}

static void sendMyFinAck(uint8_t peerSeq, uint32_t nowMs) {
    uint32_t words[3];
    const int n = encodeFinAck(words, peerSeq);
    // FINACK is NOT cached for retx — we re-send it every time we see a
    // duplicate FIN from peer.
    BadgeIR::sendFrame(words, (size_t)n);
    s_xchg.myFinAckSent = true;
    (void)nowMs;
    Serial.printf("[%s] FINACK tx echoSeq=%u\n", TAG, (unsigned)peerSeq);
}

// ─── pull-side NEED emission ─────────────────────────────────────────────

// Returns true if every tag in peerTagMask has been received (including all
// bio chunks, if bio was advertised).
static bool peerPullSatisfied() {
    // All non-bio tags in manifest received?
    const uint16_t nonBioTarget = (uint16_t)(s_xchg.peerTagMask & ~(1U << FIELD_BIO));
    if ((s_xchg.localReceivedMask & nonBioTarget) != nonBioTarget) return false;
    // Bio fully reassembled?
    if (s_xchg.peerTagMask & (1U << FIELD_BIO)) {
        const uint8_t full = (uint8_t)((1U << s_xchg.peerBioChunks) - 1U);
        if ((s_xchg.peerBioChunkMask & full) != full) return false;
    }
    return true;
}

static void fireNextNeed(uint32_t nowMs) {
    // Prioritize non-bio gaps first, then missing bio chunks.
    const uint16_t nonBioGaps = (uint16_t)((s_xchg.peerTagMask & ~(1U << FIELD_BIO))
                                            & ~s_xchg.localReceivedMask);
    uint8_t tag      = 0xFF;
    uint8_t chunkIdx = 0;
    if (nonBioGaps != 0) {
        tag = (uint8_t)__builtin_ctz(nonBioGaps);
    } else if (s_xchg.peerTagMask & (1U << FIELD_BIO)) {
        const uint8_t full = (uint8_t)((1U << s_xchg.peerBioChunks) - 1U);
        const uint8_t missing = (uint8_t)(full & ~s_xchg.peerBioChunkMask);
        if (missing) {
            tag      = FIELD_BIO;
            chunkIdx = (uint8_t)__builtin_ctz(missing);
        }
    }
    if (tag == 0xFF) return;  // nothing to need
    uint32_t words[3];
    s_xchg.mySeq++;
    const int n = encodeNeed(words, tag, chunkIdx, s_xchg.mySeq);
    txMeta(words, (size_t)n, nowMs);
    LOG_BOOP("[%s] NEED tx tag=%u chunk=%u\n", TAG,
                  (unsigned)tag, (unsigned)chunkIdx);
}

// ─── serve-side DATA encoding ────────────────────────────────────────────

// Packs one TLV into `out` at offset `pos`.  Returns number of bytes
// written, or 0 if the TLV doesn't fit in (maxBytes - pos).
static size_t packOneTlv(uint8_t* out, size_t pos, size_t maxBytes,
                          uint8_t tag, uint8_t chunkIdx,
                          const char* payload, uint8_t len) {
    if (pos + 2 + len > maxBytes) return 0;
    out[pos]     = (uint8_t)((tag & 0x0FU) | ((chunkIdx & 0x0FU) << 4));
    out[pos + 1] = len;
    memcpy(out + pos + 2, payload, len);
    return 2 + (size_t)len;
}

// Frame up and send one DATA frame carrying the TLV groups in `buf`.
// Updates boopStatus.fieldTxMask bits for the tags sent.
static void txOneDataFrame(uint8_t seq, uint8_t groupCount, bool hasMore,
                            const uint8_t* buf, size_t bufLen,
                            uint16_t tagsJustSent,
                            uint32_t nowMs) {
    uint32_t words[NEC_MAX_WORDS];
    const int n = encodeDataFrame(words, seq, groupCount, hasMore,
                                   buf, bufLen);
    if (n <= 0) {
        // Defensive log — encodeDataFrame returns 0 when the encoded
        // payload would exceed NEC_MAX_WORDS. With NEC_MAX_WORDS bumped
        // back to 64 this should never fire in practice, but keep it
        // visible so a future cap regression doesn't silently strand the
        // exchange like the 8-word cap did.
        Serial.printf("[%s] DATA encode failed: tlvLen=%u groups=%u (cap=%u words)\n",
                      TAG, (unsigned)bufLen, (unsigned)groupCount,
                      (unsigned)NEC_MAX_WORDS);
        return;
    }
    Serial.printf("[%s] DATA tx seq=%u gc=%u hasMore=%d words=%d tags=0x%03X\n",
                  TAG, (unsigned)seq, (unsigned)groupCount, hasMore ? 1 : 0,
                  n, (unsigned)tagsJustSent);
    // Use nowait during the streaming burst so frame N+1 can queue while
    // frame N is still draining the DMA buffer.  RMT trans_queue_depth=4
    // absorbs up to 4 pending transactions.
    if (!BadgeIR::sendFrameNoWait(words, (size_t)n)) {
        // Queue full — fall back to blocking send to make sure this frame
        // lands.  Rare in practice because the state machine paces itself.
        BadgeIR::sendFrame(words, (size_t)n);
    }
    // Update TX mask + feedback hooks.
    for (uint8_t t = 0; t < FIELD_TAG_COUNT; t++) {
        if (tagsJustSent & (1U << t)) {
            boopStatus.fieldTxMask |= (1U << t);
            BoopFeedback::onPeerFieldTx(t);
        }
    }
    boopStatus.lastFieldDirection = 1;
    boopStatus.lastFieldEventMs   = nowMs;
}

// Pack the entire manifest into ONE atomic DATA frame.
//
// "One big frame" beats "N small frames" on this radio because peer's
// primary side keeps retxing STREAM_REQ until it sees DATA arrive.
// With N small frames, any one collision means peer never sees
// hasMore=0 → it stays in PRIMARY_PULL forever even though most of
// the payload landed.  With one frame, it's atomic: either peer got
// everything, or peer's NEED-repair path picks up exactly what was
// missing on the next round.  At NEC_MAX_WORDS=64 the payload area
// holds ~244 B, plenty for name + title + company + email + website
// + phone + bio chunks.
static void serveStreamReq(uint32_t nowMs) {
    Serial.printf("[%s] serveStreamReq: tagMask=0x%03X bioChunks=%u\n",
                  TAG, (unsigned)s_xchg.myTagMask,
                  (unsigned)s_xchg.myBioChunks);
    constexpr size_t kMaxTlvBytes = (NEC_MAX_WORDS - 3U) * 4U;
    uint8_t  buf[NEC_MAX_WORDS * 4];
    size_t   pos = 0;
    uint8_t  groupCount = 0;
    uint16_t tagsInFrame = 0;
    s_xchg.mySeq++;
    const uint8_t streamSeq = s_xchg.mySeq;

    // 1. Pack all non-bio tags that are in my manifest.
    for (uint8_t t = 0; t < FIELD_TAG_COUNT; t++) {
        if (t == FIELD_BIO) continue;
        if (!(s_xchg.myTagMask & (1U << t))) continue;
        const char* v = getLocalField(t);
        const uint8_t len = (uint8_t)strlen(v);
        if (pos + 2 + len > kMaxTlvBytes) {
            txOneDataFrame(streamSeq, groupCount, /*hasMore=*/true,
                           buf, pos, tagsInFrame, nowMs);
            pos = 0;
            groupCount = 0;
            tagsInFrame = 0;
        }
        size_t wrote = packOneTlv(buf, pos, kMaxTlvBytes, t, 0, v, len);
        if (wrote == 0) continue;
        pos += wrote;
        groupCount++;
        tagsInFrame |= (1U << t);
    }

    // 2. Pack bio chunks one at a time (each chunk is its own TLV).
    if (s_xchg.myTagMask & (1U << FIELD_BIO)) {
        const char* bio = getLocalField(FIELD_BIO);
        const size_t bioLen = bio ? strlen(bio) : 0;
        for (uint8_t c = 0; c < s_xchg.myBioChunks; c++) {
            const size_t off = (size_t)c * kBoopBioChunkBytes;
            if (off >= bioLen) break;
            size_t chunkLen = bioLen - off;
            if (chunkLen > kBoopBioChunkBytes) chunkLen = kBoopBioChunkBytes;
            if (pos + 2 + chunkLen > kMaxTlvBytes) {
                txOneDataFrame(streamSeq, groupCount, /*hasMore=*/true,
                               buf, pos, tagsInFrame, nowMs);
                pos = 0;
                groupCount = 0;
                tagsInFrame = 0;
            }
            size_t wrote = packOneTlv(buf, pos, kMaxTlvBytes,
                                       FIELD_BIO, c, bio + off, (uint8_t)chunkLen);
            if (wrote == 0) continue;
            pos += wrote;
            groupCount++;
            tagsInFrame |= (1U << FIELD_BIO);
        }
    }

    // 3. Flush the final frame with hasMore=0.
    if (groupCount > 0) {
        txOneDataFrame(streamSeq, groupCount, /*hasMore=*/false,
                       buf, pos, tagsInFrame, nowMs);
    } else {
        txOneDataFrame(streamSeq, 0, /*hasMore=*/false, buf, 0, 0, nowMs);
    }
    s_xchg.myStreamDone = true;
}

// Serve a single NEED — encode exactly one TLV for the requested
// (tag, chunkIdx) and send it in a one-off DATA frame.
static void serveNeed(uint8_t tag, uint8_t chunkIdx, uint32_t nowMs) {
    if (tag >= FIELD_TAG_COUNT) return;
    if (!(s_xchg.myTagMask & (1U << tag))) return;
    const char* v = getLocalField(tag);
    if (!v) return;

    uint8_t buf[NEC_MAX_WORDS * 4];
    size_t wrote = 0;
    if (tag == FIELD_BIO) {
        if (chunkIdx >= s_xchg.myBioChunks) return;
        const size_t bioLen = strlen(v);
        const size_t off = (size_t)chunkIdx * kBoopBioChunkBytes;
        if (off >= bioLen) return;
        size_t chunkLen = bioLen - off;
        if (chunkLen > kBoopBioChunkBytes) chunkLen = kBoopBioChunkBytes;
        wrote = packOneTlv(buf, 0, sizeof(buf), tag, chunkIdx,
                            v + off, (uint8_t)chunkLen);
    } else {
        wrote = packOneTlv(buf, 0, sizeof(buf), tag, 0, v,
                            (uint8_t)strlen(v));
    }
    if (wrote == 0) return;

    uint32_t words[NEC_MAX_WORDS];
    s_xchg.mySeq++;
    const int n = encodeDataFrame(words, s_xchg.mySeq,
                                   /*groupCount=*/1, /*hasMore=*/false,
                                   buf, wrote);
    if (n <= 0) return;
    BadgeIR::sendFrame(words, (size_t)n);
    boopStatus.fieldTxMask |= (1U << tag);
    BoopFeedback::onPeerFieldTx(tag);
    boopStatus.lastFieldDirection = 1;
    boopStatus.lastFieldEventMs   = nowMs;
    LOG_BOOP("[%s] serve NEED tag=%u chunk=%u len=%u\n",
                  TAG, (unsigned)tag, (unsigned)chunkIdx,
                  (unsigned)(wrote - 2));
}

// Split src into up to two lines that each fit in maxW using the current
// font. Breaks at the last space before the width boundary; falls back
// to hard-cut + "..." if a single word overruns. Returns the number of
// lines produced (0/1/2). Caller-provided buffers must be >= 24 chars.
static uint8_t wrapTwoLines(oled& d, const char* src, int maxW,
                            char* line1, size_t cap1,
                            char* line2, size_t cap2) {
    line1[0] = '\0';
    line2[0] = '\0';
    if (!src || !src[0]) return 0;

    int w = d.getStrWidth(src);
    if (w <= maxW) {
        strncpy(line1, src, cap1 - 1);
        line1[cap1 - 1] = '\0';
        return 1;
    }

    // Find the rightmost space that still fits on line 1.
    size_t n = strlen(src);
    int breakAt = -1;
    char buf[64];
    for (size_t i = 0; i < n && i < sizeof(buf) - 1; i++) {
        buf[i]     = src[i];
        buf[i + 1] = '\0';
        if (src[i] == ' ' && d.getStrWidth(buf) <= maxW) {
            breakAt = (int)i;
        } else if (d.getStrWidth(buf) > maxW) {
            break;
        }
    }

    if (breakAt > 0) {
        size_t copyN = (breakAt < (int)cap1 - 1) ? (size_t)breakAt : cap1 - 1;
        memcpy(line1, src, copyN);
        line1[copyN] = '\0';
        // Line 2 = the rest, possibly still too long — ellipsize.
        const char* rest = src + breakAt + 1;
        strncpy(line2, rest, cap2 - 1);
        line2[cap2 - 1] = '\0';
        while (strlen(line2) > 2 && d.getStrWidth(line2) > maxW) {
            size_t L = strlen(line2);
            line2[L - 1] = '\0';
            line2[L - 2] = '.';
            line2[L - 3] = '.';
        }
        return 2;
    }

    // No space break possible — ellipsize line 1 only.
    strncpy(line1, src, cap1 - 1);
    line1[cap1 - 1] = '\0';
    while (strlen(line1) > 2 && d.getStrWidth(line1) > maxW) {
        size_t L = strlen(line1);
        line1[L - 1] = '\0';
        line1[L - 2] = '.';
        line1[L - 3] = '.';
    }
    return 1;
}

static void peer_drawContent(oled& d, const BoopStatus& s,
                             int x, int y, int w, int /*h*/) {
    char tmp[64];
    char l1[64], l2[64];
    const bool confirmed = (s.phase == BOOP_PHASE_PAIRED_OK);
    // peerName arrives via the IR manifest exchange; it can briefly be empty
    // between phase=PAIRED_OK and the
    // partner-info backfill landing. Fall back to peerUID (always set at
    // boop time) so the user sees the person they connected with — same
    // pattern BadgeDisplay::renderBoopResult uses.
    const char* name = s.peerName[0] ? s.peerName
                     : s.peerUID[0]  ? s.peerUID
                     : "...";

    if (confirmed) {
        // Confirmed contact card. Hard-cap the bottom at y=46 so we
        // never bleed into the footer/help text strip (footer divider
        // sits at y=47 and chips/labels live below that). Auto-fit the
        // name with FONT_LARGE → FONT_SMALL → wrap-2-lines fallback so
        // long names display cleanly in any width.
        constexpr int kBottomGuard = 46;
        const int yTop = y;
        int cursorY = yTop;

        // ── Name: try LARGE on one line, then SMALL on one line, then
        //    SMALL wrapped to two lines. Whichever fits first wins.
        d.setFontPreset(FONT_LARGE);
        if (d.getStrWidth(name) <= w) {
            d.drawStr(x, yTop + 12, name);
            cursorY = yTop + 22;
        } else {
            d.setFontPreset(FONT_SMALL);
            if (d.getStrWidth(name) <= w) {
                d.drawStr(x, yTop + 10, name);
                cursorY = yTop + 18;
            } else {
                uint8_t nameLines = wrapTwoLines(d, name, w,
                                                 l1, sizeof(l1),
                                                 l2, sizeof(l2));
                d.drawStr(x, yTop + 10, l1);
                cursorY = yTop + 18;
                if (nameLines >= 2) {
                    d.drawStr(x, cursorY, l2);
                    cursorY += 8;
                }
            }
        }

        // ── Subordinate fields (title/company/email) below the name in
        //    FONT_TINY so we have headroom for 2-3 rows before the
        //    footer guard. Each row ellipsizes to width if too long.
        d.setFontPreset(FONT_TINY);
        constexpr int kSubRowH = 7;
        auto drawRow = [&](const char* val) {
            if (!val || !val[0]) return;
            if (cursorY + kSubRowH > kBottomGuard) return;
            d.drawStr(x, cursorY + 6, fitStr(d, val, w, tmp, sizeof(tmp)));
            cursorY += kSubRowH;
        };
        drawRow(s.peerTitle);
        drawRow(s.peerCompany);
        drawRow(s.peerEmail);
        drawRow(s.peerWebsite);
        drawRow(s.peerPhone);
        return;
    }

    // Name — FONT_SMALL, up to 2 lines, uses the full 80 px block width.
    d.setFontPreset(FONT_SMALL);
    uint8_t nameLines = wrapTwoLines(d, name, w, l1, sizeof(l1), l2, sizeof(l2));
    int cursorY = y + 10;
    d.drawStr(x, cursorY, l1);
    cursorY += 10;
    if (nameLines >= 2) {
        d.drawStr(x, cursorY, l2);
        cursorY += 10;
    }

    // Company, title, email — FONT_TINY. Each on its own line, stacked
    // in whatever room is left after the name. Budget is roughly rows
    // 12..46 = 34 px, with the vertical-log divider at 47.
    d.setFontPreset(FONT_TINY);
    if (s.peerCompany[0] && cursorY <= 40) {
        d.drawStr(x, cursorY, fitStr(d, s.peerCompany, w, tmp, sizeof(tmp)));
        cursorY += 7;
    }
    if (s.peerTitle[0] && cursorY <= 40) {
        d.drawStr(x, cursorY, fitStr(d, s.peerTitle, w, tmp, sizeof(tmp)));
        cursorY += 7;
    }
    if (s.peerEmail[0] && cursorY <= 46) {
        d.drawStr(x, cursorY, fitStr(d, s.peerEmail, w, tmp, sizeof(tmp)));
    }
}

static const char* peer_statusLabel(const BoopStatus& s) {
    return s.peerName[0] ? s.peerName
         : "peer";
}

// ─── Shared helper: copy peerUID into installationId for non-peer types ─────
// Beacon word0 byte1 is BoopType, words[1..2] are the installation ID
// (reusing the same slot as peer UID). For non-peer handlers we surface
// it in boopStatus.installationId so drawContent can display it.

static void installation_populateId(BoopStatus& s) {
    strncpy(s.installationId, s.peerUidHex, sizeof(s.installationId) - 1);
    s.installationId[sizeof(s.installationId) - 1] = '\0';
}

// ─── Exhibit handler ────────────────────────────────────────────────────────

static void exhibit_onLock(BoopStatus& s) {
    installation_populateId(s);
    BoopFeedback::onInstallation(BOOP_EXHIBIT, s.installationId);
    // Journal entry is written by finishPaired via recordBoopEx when the
    // state machine transitions to PAIRED_OK.
}

static bool exhibit_tickPostConfirm(BoopStatus& /*s*/, uint32_t /*n*/) {
    return true;  // installation flow completes synchronously
}

static void exhibit_drawContent(oled& d, const BoopStatus& s,
                                int x, int y, int w, int h) {
    (void)h;
    char tmp[40];
    d.setFontPreset(FONT_LARGE);
    d.drawStr(x, y + 12, "EXHIBIT");
    d.setFontPreset(FONT_SMALL);
    d.drawStr(x, y + 22, "Visit logged!");
    if (s.installationId[0]) {
        d.setFontPreset(FONT_TINY);
        snprintf(tmp, sizeof(tmp), "id:%s", s.installationId);
        d.drawStr(x, y + 30, fitStr(d, tmp, w, tmp, sizeof(tmp)));
    }
}

static const char* exhibit_statusLabel(const BoopStatus& s) {
    return s.installationId[0] ? s.installationId : "exhibit";
}

// ─── Queue handler ──────────────────────────────────────────────────────────

static void queue_onLock(BoopStatus& s) {
    installation_populateId(s);
    BoopFeedback::onInstallation(BOOP_QUEUE_JOIN, s.installationId);
}

static bool queue_tickPostConfirm(BoopStatus& /*s*/, uint32_t /*n*/) {
    return true;
}

static void queue_drawContent(oled& d, const BoopStatus& s,
                              int x, int y, int w, int h) {
    (void)h;
    char tmp[40];
    d.setFontPreset(FONT_LARGE);
    d.drawStr(x, y + 12, "QUEUE");
    d.setFontPreset(FONT_SMALL);
    d.drawStr(x, y + 22, "Queued!");
    d.setFontPreset(FONT_TINY);
    d.drawStr(x, y + 30, fitStr(d, "we'll buzz when", w, tmp, sizeof(tmp)));
    d.drawStr(x, y + 38, fitStr(d, "it's your turn.", w, tmp, sizeof(tmp)));
}

static const char* queue_statusLabel(const BoopStatus& s) {
    return s.installationId[0] ? s.installationId : "queue";
}

// ─── Kiosk handler ──────────────────────────────────────────────────────────

static void kiosk_onLock(BoopStatus& s) {
    installation_populateId(s);
    BoopFeedback::onInstallation(BOOP_KIOSK_INFO, s.installationId);
}

static bool kiosk_tickPostConfirm(BoopStatus& /*s*/, uint32_t /*n*/) {
    return true;
}

static void kiosk_drawContent(oled& d, const BoopStatus& s,
                              int x, int y, int w, int h) {
    (void)h;
    char tmp[40];
    d.setFontPreset(FONT_LARGE);
    d.drawStr(x, y + 12, "KIOSK");
    d.setFontPreset(FONT_SMALL);
    d.drawStr(x, y + 22, "Info received.");
    if (s.installationId[0]) {
        d.setFontPreset(FONT_TINY);
        snprintf(tmp, sizeof(tmp), "id:%s", s.installationId);
        d.drawStr(x, y + 30, fitStr(d, tmp, w, tmp, sizeof(tmp)));
    }
}

static const char* kiosk_statusLabel(const BoopStatus& s) {
    return s.installationId[0] ? s.installationId : "kiosk";
}

// ─── Check-in handler ───────────────────────────────────────────────────────

static void checkin_onLock(BoopStatus& s) {
    installation_populateId(s);
    BoopFeedback::onInstallation(BOOP_CHECKIN, s.installationId);
}

static bool checkin_tickPostConfirm(BoopStatus& /*s*/, uint32_t /*n*/) {
    return true;
}

static void checkin_drawContent(oled& d, const BoopStatus& s,
                                int x, int y, int w, int h) {
    (void)h;
    char tmp[40];
    d.setFontPreset(FONT_LARGE);
    d.drawStr(x, y + 12, "CHECK-IN");
    d.setFontPreset(FONT_SMALL);
    d.drawStr(x, y + 22, "Checked in!");
    if (s.installationId[0]) {
        d.setFontPreset(FONT_TINY);
        snprintf(tmp, sizeof(tmp), "id:%s", s.installationId);
        d.drawStr(x, y + 30, fitStr(d, tmp, w, tmp, sizeof(tmp)));
    }
}

static const char* checkin_statusLabel(const BoopStatus& s) {
    return s.installationId[0] ? s.installationId : "checkin";
}

// ─── Unknown handler (fallback for reserved BoopType values) ────────────────

static void unknown_onLock(BoopStatus& s) {
    installation_populateId(s);
    Serial.printf("[%s] unknown boop type=%u — recording with stub metadata\n",
                  TAG, (unsigned)s.boopType);
}
static bool unknown_tickPostConfirm(BoopStatus& /*s*/, uint32_t /*n*/) { return true; }
static void unknown_drawContent(oled& d, const BoopStatus& s,
                                int x, int y, int w, int /*h*/) {
    char tmp[40];
    d.setFontPreset(FONT_SMALL);
    d.drawStr(x, y + 12, "UNKNOWN");
    if (s.installationId[0]) {
        d.setFontPreset(FONT_TINY);
        snprintf(tmp, sizeof(tmp), "id:%s", s.installationId);
        d.drawStr(x, y + 22, fitStr(d, tmp, w, tmp, sizeof(tmp)));
    }
}
static const char* unknown_statusLabel(const BoopStatus& /*s*/) { return "?"; }

// ─── Handler table ──────────────────────────────────────────────────────────

// All six handlers follow the same shape — the three callbacks (onLock,
// tickPostConfirm, drawContent, statusLabel) are named <type>_<callback>
// and the only meaningful per-row variant is doFieldExchange. The macro
// keeps that contract local: adding a new handler is now one BOOP_HANDLER
// invocation plus the four function definitions, instead of restating
// the table layout each time.
#define BOOP_HANDLER(Cap, lower, label, doExchange)                            \
    static const BoopHandlerOps k##Cap##Handler = {                            \
        label,                                                                 \
        lower##_onLock,                                                        \
        lower##_tickPostConfirm,                                               \
        lower##_drawContent,                                                   \
        lower##_statusLabel,                                                   \
        doExchange,                                                            \
    }
BOOP_HANDLER(Peer,    peer,    "peer",    /*doFieldExchange=*/true);
BOOP_HANDLER(Exhibit, exhibit, "exhibit", false);
BOOP_HANDLER(Queue,   queue,   "queue",   false);
BOOP_HANDLER(Kiosk,   kiosk,   "kiosk",   false);
BOOP_HANDLER(Checkin, checkin, "checkin", false);
BOOP_HANDLER(Unknown, unknown, "unknown", false);
#undef BOOP_HANDLER

const BoopHandlerOps* handlerFor(BoopType t) {
    switch (t) {
        case BOOP_PEER:       return &kPeerHandler;
        case BOOP_EXHIBIT:    return &kExhibitHandler;
        case BOOP_QUEUE_JOIN: return &kQueueHandler;
        case BOOP_KIOSK_INFO: return &kKioskHandler;
        case BOOP_CHECKIN:    return &kCheckinHandler;
        default:              return &kUnknownHandler;
    }
}

}  // namespace BadgeBoops
