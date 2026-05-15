
#ifndef NEC_MW_DECODER_H
#define NEC_MW_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/rmt_types.h"
#include "nec_mw_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t words[NEC_MAX_WORDS];
    size_t   count;
    uint32_t received_crc;
    uint32_t computed_crc;
    bool     crc_ok;
} nec_mw_result_t;

/*
 * Decode raw RMT symbols into a multi-word NEC frame.
 * Returns true if structurally valid; check crc_ok before acting on data.
 */
bool nec_mw_decode_frame(const rmt_symbol_word_t *symbols,
                          size_t                   symbol_count,
                          nec_mw_result_t         *result);

#ifdef __cplusplus
}
#endif

#endif /* NEC_MW_DECODER_H */
