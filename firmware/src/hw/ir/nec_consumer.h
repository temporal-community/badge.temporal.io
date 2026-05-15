/*
 * nec_consumer.h — Consumer (Flipper-style) NEC IR codec.
 *
 * Pure functions that translate between high-level (addr, cmd) +
 * an explicit "is repeat" flag and arrays of rmt_symbol_word_t suitable
 * for the ESP-IDF copy encoder. Used by the IR Playground sub-apps to
 * record / replay arbitrary consumer NEC remotes (TVs, audio gear, etc.).
 *
 * Wire format: leader (9000us mark + 4500us space), 32 LSB-first data
 * bits encoded as 560us mark + (560us | 1690us) space, then a 560us end
 * mark. Data layout follows the canonical NEC convention:
 *     bits[0..7]   = addr
 *     bits[8..15]  = ~addr   (validated on decode)
 *     bits[16..23] = cmd
 *     bits[24..31] = ~cmd    (validated on decode)
 *
 * Repeat frame is a single short burst: 9000us mark + 2250us space + 560us
 * end mark, emitted ~110 ms after the previous frame while a button on a
 * real remote is held.
 *
 * This codec deliberately does NOT touch RMT hardware. BadgeIR owns the
 * channel + copy encoder and just feeds these symbol arrays through it.
 */

#ifndef NEC_CONSUMER_H
#define NEC_CONSUMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* leader + 32 data bits + end pulse = 34 symbols. */
#define NEC_CONSUMER_FRAME_SYMBOLS  (34U)

/* Repeat = leader + end pulse = 2 symbols. */
#define NEC_CONSUMER_REPEAT_SYMBOLS (2U)

/*
 * Build a full NEC frame for (addr, cmd) into out[]. Caller-provided
 * buffer must hold at least NEC_CONSUMER_FRAME_SYMBOLS entries.
 *
 * Returns the number of symbols written (always NEC_CONSUMER_FRAME_SYMBOLS),
 * or 0 if out is NULL or max is too small.
 */
size_t nec_consumer_encode(uint8_t           addr,
                            uint8_t           cmd,
                            rmt_symbol_word_t *out,
                            size_t            max);

/*
 * Build a NEC repeat ("button-held") frame into out[]. Caller-provided
 * buffer must hold at least NEC_CONSUMER_REPEAT_SYMBOLS entries.
 *
 * Returns the number of symbols written, or 0 on bad args.
 */
size_t nec_consumer_encode_repeat(rmt_symbol_word_t *out, size_t max);

/*
 * Parse an array of received RMT symbols as a consumer NEC frame.
 * On success returns true and writes addr, cmd, and the is_repeat flag.
 * On a NEC repeat frame, addr and cmd are left untouched (callers should
 * carry them forward from the previous full frame) and is_repeat is set.
 *
 * Returns false for any structurally-invalid frame, including the badge's
 * own multi-word NEC dialect (which has many more symbols).
 */
bool nec_consumer_decode(const rmt_symbol_word_t *symbols,
                          size_t                   symbol_count,
                          uint8_t                 *addr_out,
                          uint8_t                 *cmd_out,
                          bool                    *is_repeat_out);

#ifdef __cplusplus
}
#endif

#endif /* NEC_CONSUMER_H */
