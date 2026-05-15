// BadgeIR.h — Pure RMT NEC multi-word transport layer.
//
// The boop protocol state machine (phases, journal, per-type handlers)
// lives in BadgeBoops. This module owns only:
//   - RMT TX/RX hardware lifecycle (irHwInit / irHwDeinit)
//   - Frame send/recv with self-echo filtering (BadgeIR::sendFrame /
//     BadgeIR::recvFrame)
//   - MicroPython-facing raw send/recv queues
//   - TX carrier duty (power) control
//   - The irTask() FreeRTOS loop that pumps all of the above and dispatches
//     BadgeBoops::tick() when the Boop screen is up
//
// Wire protocol constants (BoopType, frame type tags, field tags, pacing)
// stay here because the transport touches them when framing beacons.
//
// Inter-core contract:
//   irHardwareEnabled, pythonIrListening — volatile bool, set by Core 1,
//   consumed by irTask on Core 0. Single-byte atomic writes on ESP32.
//   irPythonQueue head/tail protected by irPythonQueueMux (portMUX spinlock).

#pragma once
#include <Arduino.h>

#include "hw/ir/nec_mw_encoder.h"
#include "hw/ir/nec_mw_decoder.h"

// ─── Role number constants (attendee type, NOT IR protocol values) ──────────

#define ROLE_NUM_ATTENDEE  1
#define ROLE_NUM_STAFF     2
#define ROLE_NUM_VENDOR    3
#define ROLE_NUM_SPEAKER   4
#define ROLE_NUM_DEITY     5

// ─── Boop types ─────────────────────────────────────────────────────────────
// The beacon carries a BoopType in word0 byte1 so the same protocol serves
// peer-to-peer exchanges, exhibits, queue joins, kiosks, and future
// installation types.

enum BoopType : uint8_t {
    BOOP_PEER        = 0x00,
    BOOP_EXHIBIT     = 0x01,
    BOOP_QUEUE_JOIN  = 0x02,
    BOOP_KIOSK_INFO  = 0x03,
    BOOP_CHECKIN     = 0x04,
    BOOP_RESERVED_05 = 0x05,
    BOOP_RESERVED_06 = 0x06,
    BOOP_RESERVED_07 = 0x07,
    BOOP_TYPE_COUNT  = 0x08,
};

// ─── Frame type tags (protocol v2) ────────────────────────────────────────── //#TODO: we should pack this frame bitwise into minimal bytes and then put UID in here
// All frames share this word0 shape:
//   byte0: frame type (0xB0/0xB1/0xC0..0xC5)
//   byte1: type-specific (BoopType for beacons, tag/role for others)
//   byte2: protocol version (must be kBoopProtocolVer to exchange)
//   byte3: reserved / flags
//
// words[1..2] always carry the sender UID for self-echo filtering.

// Beacon phase — unchanged from v1 in layout (back-compat wire semantics):
#define IR_BOOP_BEACON     0xB0  // "I'm here, UID X, ready to boop"
#define IR_BOOP_DONE       0xB1  // "I've recorded the boop, you can stop"

// Exchange phase — v2 manifest-driven streaming protocol:
#define IR_BOOP_MANIFEST   0xC0  // "Here are the tags I'll send, bio is K chunks"
#define IR_BOOP_STREAM_REQ 0xC1  // "Send me everything in your manifest now"
#define IR_BOOP_DATA       0xC2  // 1+ batched (tag,chunkIdx,len,bytes) TLVs + hasMore
#define IR_BOOP_NEED       0xC3  // "I'm missing tag N chunk M, resend it"
#define IR_BOOP_FIN        0xC4  // "I'm done pulling"
#define IR_BOOP_FINACK     0xC5  // "I saw your FIN, I'm done too"

// Notification frame — target-UID-addressed, retry-with-ACK.
//   word0 : byte0=0xD0  byte1=kind  byte2=kNotifyProtocolVer  byte3=flags
//   word1/word2 : TARGET UID bytes (receiver filter).  Opposite of
//                 beacon frames which carry SENDER UID there for self-
//                 echo suppression.
//   word3..   : TLV payload (tag, len, bytes...)
//                 0x01 = title  (UTF-8 bytes)
//                 0x02 = body   (UTF-8 bytes)
//                 0x03 = sender UID (6 bytes, for reply routing)
//                 0x04 = nonce  (2 bytes, little-endian, echoed in ACK)
//                 0x05 = msg token (4 bytes, little-endian, cross-transport dedup)
#define IR_NOTIFY          0xD0

// Acknowledgement for IR_NOTIFY and its multi-frame streaming variant.
//   word0 : byte0=0xD1  byte1=kind (echo)  byte2=kNotifyProtocolVer  byte3=0
//   word1/word2 : TARGET UID (original sender of the NOTIFY / MANIFEST)
//   word3       : single-frame ACK: low 16 bits = nonce echoed from NOTIFY.
//                 multi-frame  ACK: full 32-bit msgToken echoed from MANIFEST.
//                 The sender's pending-slot state tells it which to
//                 match against — nonce for single-frame, token for
//                 streaming — so the two semantics share one wire word.
#define IR_NOTIFY_ACK      0xD1

// Multi-frame notification protocol (plan 02 Part B).  Lifts the
// single-frame ~90 B body cap by splitting the TLV payload across
// fixed-size DATA chunks with receiver-driven NEED repair.  Same
// shape as the boop v2 streaming protocol (MANIFEST / DATA / NEED);
// carries SENDER UID in words[1..2] so `isSelfEcho` can drop our own
// reflections using the boop-style filter.
//
// IR_NOTIFY_MANIFEST (0xD2) — "I'm about to send msgToken in N chunks":
//   word0       : byte0=0xD2  byte1=kind  byte2=kNotifyProtocolVer  byte3=0
//   words[1..2] : SENDER UID (for self-echo suppression)
//   words[3..4] : TARGET UID (receiver filter — dispatch drops others)
//   word5       : 32-bit msgToken (LE)
//   word6       : low8  = totalChunks
//                 mid8  = chunkBytes (declared chunk body size)
//                 high16= totalBytes (across every DATA chunk)
#define IR_NOTIFY_MANIFEST 0xD2

// IR_NOTIFY_DATA (0xD3) — one chunk of the flattened TLV payload:
//   word0       : byte0=0xD3  byte1=chunkIdx  byte2=kNotifyProtocolVer
//                 byte3 bit0 = hasMore (1 while more chunks still to come)
//   words[1..2] : SENDER UID (self-echo filter)
//   word3       : 32-bit msgToken (LE, same key the MANIFEST used)
//   word4       : low8  = chunkLen (this frame's payload byte count)
//                 mid8  = totalChunks (redundant with MANIFEST; handy
//                          for receivers who missed MANIFEST)
//                 high16= reserved / 0
//   words[5..]  : chunkLen bytes packed 4/word (LE), zero-padded
#define IR_NOTIFY_DATA     0xD3

// IR_NOTIFY_NEED (0xD4) — receiver asks for specific missing chunks:
//   word0       : byte0=0xD4  byte1=kind  byte2=kNotifyProtocolVer  byte3=0
//   words[1..2] : SENDER UID of this NEED — i.e. the ORIGINAL TARGET of
//                 the MANIFEST.  Self-echo filter drops our own NEED
//                 echoes via the boop-style "matches my uid" rule.
//   word3       : 32-bit msgToken (LE) — correlates with sender's slot
//   word4       : low 16 bits = missingChunkBitmap (bit i = chunk i is
//                                  still missing on the receiver side)
#define IR_NOTIFY_NEED     0xD4

static constexpr uint8_t kBoopProtocolVer     = 0x02;
// Protocol v2 introduces MANIFEST / DATA / NEED for long messages.  The
// wire has not shipped — no legacy-receiver fallback is attempted.
static constexpr uint8_t kNotifyProtocolVer   = 0x02;

// Field tags for peer info exchange (priority/transmission order)
enum BoopFieldTag : uint8_t {
    FIELD_NAME          = 0,
    FIELD_TITLE         = 1,
    FIELD_COMPANY       = 2,
    FIELD_ATTENDEE_TYPE = 3,   // RX-accepted for back-compat; never TX'd
    FIELD_TICKET_UUID   = 4,
    FIELD_EMAIL         = 5,
    FIELD_WEBSITE       = 6,
    FIELD_PHONE         = 7,
    FIELD_BIO           = 8,
    FIELD_TAG_COUNT     = 9,
};

// Bio segmentation: up to 4 chunks of up to 32 bytes each reassembles into
// the full 128-byte peerBio[] buffer.  Smaller chunks drop wire time per
// frame (~660ms vs ~1944ms for one giant bio), and a single lost chunk is
// cheap to repair via IR_BOOP_NEED instead of retransmitting the whole bio.
static constexpr uint8_t kBoopBioChunkBytes = 32;
static constexpr uint8_t kBoopBioMaxChunks  = 4;

// ─── Beacon phase pacing ────────────────────────────────────────────────────
// Beacon TX takes ~230 ms (3 data + CRC word at NEC timing).
// TSOP AGC recovery after our own TX: ~80-100 ms.
// The interval is silence BETWEEN TXes (not including TX time).
// A peer beacon (also ~230 ms) must start AND finish inside our
// listen window = interval + jitter - recovery.  So interval must
// comfortably exceed 230 + 100 = 330 ms.
//
// Minimum handshake: one BEACON TX + one peer BEACON RX establishes the
// pair, then we swap one DONE each before entering EXCHANGE.  ~4 frames
// of handshake traffic total.

// Base TX cadence.  One beacon is ~230 ms on the wire; interval must
// exceed that by enough to leave a usable RX window per cycle.  At
// 400 + 0..300 ms (range 400-700 ms) the listen window is 170-470 ms
// and averages ~320 ms — deliberately longer than the 250-ms variant
// we tried first, because too-fast TX makes the TX duty cycle high
// enough that whichever beacon a peer TXes mostly lands in our own
// TX blocked-RX window.  Fewer-but-cleaner TXes beats more-frequent-
// but-colliding ones for this handshake.  The jitter range stays
// generous so two badges can't lock into in-phase lockstep for long,
// and BOOP_BEACON_LOCKSTEP_NUDGE below forcibly decorrelates after
// a few blank cycles just in case.
#define BOOP_BEACON_INTERVAL_MS   400
#define BOOP_BEACON_JITTER_MS     300
#define BOOP_BEACON_RX_THRESHOLD    1
#define BOOP_BEACON_TX_MIN          1
// After this many beacons without ever seeing peer, the next TX gets
// an extra big random shift to break out of any accidental TX-phase
// lockstep with the peer.  Kicks in during the "both talking over
// each other" scenario that made the initial UID handshake feel slow.
#define BOOP_BEACON_LOCKSTEP_NUDGE_AFTER_TX  3

// How many additional DONE (0xB1) frames to TX after we've seen peer's
// first DONE.  1 gives peer a confirmation frame before both sides
// transition to EXCHANGE; higher values just pad the handshake.
static constexpr int kBoopConfirmTxCount = 1;

// ─── IR hardware lifecycle flags (Core 1 writes, Core 0 reads) ──────────────

extern volatile bool irHardwareEnabled;   // true while Boop screen is active

// ─── Python IR receive queue ────────────────────────────────────────────────
// Core 0 (irTask) writes; Core 1 (MicroPython bridge) reads.

#define IR_PYTHON_QUEUE_SIZE 8

struct IrPythonFrame {
    uint8_t addr;
    uint8_t cmd;
};

extern volatile bool      pythonIrListening;
extern IrPythonFrame      irPythonQueue[IR_PYTHON_QUEUE_SIZE];
extern volatile int       irPythonQueueHead;
extern volatile int       irPythonQueueTail;
extern portMUX_TYPE       irPythonQueueMux;

// ─── Multi-word IR TX request (Python → Core 0 queue) ───────────────────────

struct ir_tx_request_t {
    uint32_t words[NEC_MAX_WORDS];
    size_t   count;
};

// ─── Transport API used by the BadgeBoops state machine ─────────────────────
//
// Both functions must be called from irTask (Core 0). nec_tx requires that
// init/send/wait all run on the same FreeRTOS task because completion
// signaling uses task notifications.

namespace BadgeIR {

// Blocking send of a multi-word NEC frame. Returns true on success.
// Blocks for the full wire time of the frame.  Use this outside of
// streaming bursts where serialized semantics are desired (beacon phase,
// single-shot meta frames).
bool sendFrame(const uint32_t* words, size_t count);

// Non-blocking enqueue of a multi-word NEC frame.  Hands the transaction
// to the RMT driver's trans_queue (depth 4 — see nec_tx.h) and returns
// immediately.  The hardware will clock it out back-to-back with any
// other queued transactions with zero inter-frame software gap.  Meant
// for streaming DATA bursts; use sendFrame() for anything else.
// Returns true on successful enqueue, false if the queue is currently
// full (caller should retry on the next tick).
bool sendFrameNoWait(const uint32_t* words, size_t count);

// Block up to timeout_ms for a non-self-echo frame. Returns true if one
// arrived. Self-echoes are silently consumed inside this function.
bool recvFrame(nec_mw_result_t* out, uint32_t timeout_ms);

}  // namespace BadgeIR

// ─── Python / legacy flat-C API ─────────────────────────────────────────────

// True once the RMT hardware has been initialized (either by the Boop
// screen or a Python ir_start()). False between hardware up requests.
bool irHwIsUp();

// Send a raw single-word NEC frame for MicroPython (legacy API).
// Returns 0 on success, -1 if IR not init'd.
int irSendRaw(uint8_t addr, uint8_t cmd);

// Send a multi-word NEC frame from MicroPython.
// Returns 0 on success, -1 if IR not init'd or count out of range.
int irSendWords(const uint32_t* words, size_t count);

// IR_NOTIFY send/receive functions removed — notifications delivered via WiFi.
// Post-pairing messaging is WiFi API-only; legacy IR notify code removed.

// Read a multi-word NEC frame received while pythonIrListening is set.
// Copies up to max_words into out[], sets *count_out. Returns 0 if a frame
// was available, -1 if the queue is empty.
int irReadWords(uint32_t* out, size_t max_words, size_t* count_out);

// Drop every pending frame in the Python-facing RX paths.
// Safe to call from any task.  No-op if the IR hardware is down.
void irDrainPythonRx();

// Set the IR TX carrier duty cycle (effectively LED drive strength).
// percent must be in [1, 50].  Returns 0 on success, -1 if the hardware
// is down or the value is out of range.
int irSetTxPower(int percent);

// Read back the currently configured TX carrier duty cycle as a percent
// (1..50).  Returns 0 if the hardware is down.
int irGetTxPower();

// ─── IR Playground mode router ──────────────────────────────────────────────
// Three mutually exclusive interpretations of the same RMT hardware:
//   - BADGE_MW       : the legacy multi-word + CRC32 dialect used by Boop
//                      and badge↔badge apps (ir_send_words / ir_read_words).
//   - CONSUMER_NEC   : standard 32-bit NEC (TVs, audio, AC remotes).
//   - RAW_SYMBOL     : arbitrary RMT mark/space pairs — Sony, RC5, RC6,
//                      anything captureable as raw timings.
//
// Mode is a *queue routing* flag; the RMT hardware stays up the whole time.
// Switching is instant and does not tear down the channel. While
// CONSUMER_NEC or RAW_SYMBOL is active the badge multi-word RX path keeps
// pumping its own queue (Boop is gated separately by phase=IDLE), but
// MicroPython sees the alternate stream.
//
// Setting mode != BADGE_MW while a Boop is in flight is rejected.

enum IrMode : uint8_t {
    IR_MODE_BADGE_MW     = 0,
    IR_MODE_CONSUMER_NEC = 1,
    IR_MODE_RAW_SYMBOL   = 2,
};

// Returns 0 on success, -1 if the requested mode is invalid or the boop
// state machine is not idle. Always succeeds for IR_MODE_BADGE_MW.
int irSetMode(int mode);
int irGetMode();

// Consumer NEC path. Both calls fall through (return -1) unless the
// active mode is IR_MODE_CONSUMER_NEC. ir_nec_send queues a frame for
// transmission; repeats > 0 schedules that many leader-only "button-held"
// repeat frames ~110 ms apart.
int irNecSend(uint8_t addr, uint8_t cmd, uint8_t repeats);

// Returns 0 and writes (*addr_out, *cmd_out, *repeat_out) on success.
// repeat_out=1 means the frame was a NEC repeat code; addr/cmd are
// passed through from the most recent full frame the badge has seen.
int irNecRead(uint8_t* addr_out, uint8_t* cmd_out, uint8_t* repeat_out);

// Raw symbol path. mark_us / space_us pairs, packed little-endian as a
// `uint16_t[2*n]` byte buffer for MicroPython.
//
// irRawCapture: returns the number of mark/space *pairs* copied into
// out_pairs, or 0 if no frame is queued. max_pairs is the buffer capacity
// (2 bytes * 2 = 4 bytes per pair).
int irRawCapture(uint16_t* out_pairs, size_t max_pairs);

// irRawSend: emits the supplied mark/space pairs at the requested
// carrier frequency (3000–60000 Hz). carrier_hz=0 keeps the cached
// 38 kHz default.
int irRawSend(const uint16_t* pairs, size_t pair_count, uint32_t carrier_hz);

// Drop every frame queued in the active alternate-mode RX path.
void irDrainAltRx();

// IR Playground top-bar activity. The badge has dedicated TX (D2) and RX
// (D3) lines, so "is something flashing" is literal hardware state. We
// expose two short-lived counters that snapshots how recently the carrier
// was on (TX) or a frame was decoded (RX), both in milliseconds since the
// event. UINT32_MAX means "no event yet this session".
//
// Used by the IR Playground's status bar to drive a 2-segment pixel
// indicator that flashes in lock-step with the actual hardware. Other
// apps don't need to call this.
uint32_t irMsSinceTx();
uint32_t irMsSinceRx();

// ─── FreeRTOS task (Core 0) ─────────────────────────────────────────────────

// Launch via: xTaskCreatePinnedToCore(irTask, "IR", 8192, NULL, 1, NULL, 0);
void irTask(void* pvParameters);
