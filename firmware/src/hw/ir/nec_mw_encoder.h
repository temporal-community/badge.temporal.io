
#ifndef NEC_MW_ENCODER_H
#define NEC_MW_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum 32-bit data words per frame.
 *
 * Sized for the boop v2 streaming exchange: one DATA frame can pack
 * multiple TLV groups (tag+chunkIdx, len, payload bytes) totaling up to
 * ~244 bytes after the 3-word envelope. With WiFi gone the IR contact
 * exchange is the ONLY way peers learn name/title/company/bio, so this
 * ceiling has to fit a worst-case bio chunk (32 B) plus headers.
 *
 * Was briefly tightened to 8 when WiFi handled post-boop sync — that
 * silently dropped any DATA frame whose payload exceeded 20 B (encodeDataFrame
 * returns 0, txOneDataFrame quietly skips), so peer would retransmit
 * STREAM_REQ forever waiting for DATA we'd never send. Restored to 64 to
 * match the original "boops actually work" commit (75db63a8).
 */
#define NEC_MAX_WORDS  (64U)

/*
 * Payload passed to rmt_transmit().  The encoder transmits:
 *   [NEC leader] [words[0]] ... [words[count-1]] [CRC32] [end pulse]
 *
 * words must remain valid until rmt_tx_wait_all_done() returns.
 */
typedef struct {
    const uint32_t *words;
    size_t          count;  /* 1 .. NEC_MAX_WORDS */
} nec_mw_payload_t;

typedef struct {
    uint32_t resolution_hz; /* must match TX channel resolution */
} nec_mw_encoder_config_t;

esp_err_t nec_mw_encoder_create(const nec_mw_encoder_config_t *config,
                                 rmt_encoder_handle_t *ret_encoder);

#ifdef __cplusplus
}
#endif

#endif /* NEC_MW_ENCODER_H */
