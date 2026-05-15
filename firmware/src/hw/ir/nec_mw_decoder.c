
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "hw/ir/nec_mw_decoder.h"

static const char *TAG = "nec_mw_dec";

#define NEC_LEADING_HI_US  (9000U)
#define NEC_LEADING_LO_US  (4500U)
#define NEC_BIT_HI_US       (562U)
#define NEC_BIT_0_LO_US     (562U)
#define NEC_BIT_1_LO_US    (1688U)
/* NEC per-bit timing tolerance.  Bumped from 25 % to 40 % to cope with
 * the timing wobble we observed when booped badges capture near-full
 * frames (sym≈520-580 of 578 expected) but the per-bit within_tol
 * check still rejects some bits — the IR receiver's demodulation
 * stretches HI durations at close range / low TX power and narrows
 * the window.  Even at 40 % the bit-0 LO (562 µs ±40 % = [337,787])
 * and bit-1 LO (1688 µs ±40 % = [1012, 2363]) ranges remain cleanly
 * separated, so tolerance can't cause misclassification of 0 vs 1. */
#define NEC_TOL_PCT          (40U)

#define NEC_BITS_PER_WORD   (32U)
#define NEC_MIN_GROUPS       (2U)   /* at least 1 data word + 1 CRC word */
#define NEC_MIN_SYMBOLS     (66U)   /* 1 leader + 32 + 32 + 1 end */

static bool within_tol(uint32_t actual, uint32_t nominal)
{
    uint32_t delta = (nominal * NEC_TOL_PCT) / 100U;
    return (actual >= (nominal - delta)) && (actual <= (nominal + delta));
}

static bool decode_word(const rmt_symbol_word_t *syms, uint32_t *out)
{
    uint32_t word = 0U;
    uint32_t b;
    bool     ok   = true;

    for (b = 0U; (b < NEC_BITS_PER_WORD) && ok; b++)
    {
        if (!within_tol(syms[b].duration0, NEC_BIT_HI_US))
        {
            ESP_LOGD(TAG, "bit %" PRIu32 " burst %" PRIu16 " us out of range",
                     b, syms[b].duration0);
            ok = false;
        }
        else if (within_tol(syms[b].duration1, NEC_BIT_1_LO_US))
        {
            word |= (1UL << b);
        }
        else if (within_tol(syms[b].duration1, NEC_BIT_0_LO_US))
        {
            /* bit 0 — already clear */
        }
        else if (syms[b].duration1 == 0U)
        {
            /* stop condition before gap completed — treat as bit 0 */
        }
        else
        {
            ESP_LOGD(TAG, "bit %" PRIu32 " gap %" PRIu16 " us unrecognised",
                     b, syms[b].duration1);
            ok = false;
        }
    }

    if (ok)
    {
        *out = word;
    }

    return ok;
}

bool nec_mw_decode_frame(const rmt_symbol_word_t *symbols,
                          size_t                   symbol_count,
                          nec_mw_result_t         *result)
{
    bool                     ok               = false;
    size_t                   data_symbols;
    size_t                   total_groups;
    size_t                   data_word_count;
    const rmt_symbol_word_t *cursor;
    const rmt_symbol_word_t *last_sym;
    uint32_t                 received_crc     = 0U;
    uint32_t                 computed_crc;
    size_t                   w;

    if ((symbols == NULL) || (result == NULL))
    {
        /* nothing to do */
    }
    else if (symbol_count < NEC_MIN_SYMBOLS)
    {
        ESP_LOGD(TAG, "too few symbols: %u (need >=%u)",
                 (unsigned)symbol_count, NEC_MIN_SYMBOLS);
    }
    else if (!within_tol(symbols[0].duration0, NEC_LEADING_HI_US) ||
             !within_tol(symbols[0].duration1, NEC_LEADING_LO_US))
    {
        ESP_LOGD(TAG, "leader mismatch: hi=%" PRIu16 " lo=%" PRIu16,
                 symbols[0].duration0, symbols[0].duration1);
    }
    else
    {
        data_symbols = symbol_count - 1U;

        last_sym = &symbols[symbol_count - 1U];
        if (within_tol(last_sym->duration0, NEC_BIT_HI_US) &&
            (last_sym->duration1 == 0U))
        {
            data_symbols -= 1U;
        }

        /* Tolerate a handful of extra trailing symbols from RX noise or
         * the signal_range_max_ns timeout marker.  A real TX always
         * emits an exact leader + N*32 data bits + end pulse, but the
         * peer RX occasionally captures 1-3 extra symbols past that
         * (TSOP glitches as the IR carrier dies, or the RMT end-of-
         * transaction symbol accounting).  Round data_symbols down to
         * the nearest multiple of 32 so the decoder proceeds with the
         * whole-word prefix and leaves the trailing noise unparsed.
         * CRC verification at the end catches any frame whose trimming
         * actually hid real data corruption. */
        data_symbols = (data_symbols / NEC_BITS_PER_WORD)
                       * NEC_BITS_PER_WORD;

        total_groups = data_symbols / NEC_BITS_PER_WORD;

        if (total_groups < NEC_MIN_GROUPS)
        {
            ESP_LOGD(TAG, "not enough groups: %u", (unsigned)total_groups);
        }
        else
        {
            data_word_count = total_groups - 1U;

            if (data_word_count > NEC_MAX_WORDS)
            {
                ESP_LOGW(TAG, "frame too long: %u words (max %u)",
                         (unsigned)data_word_count, NEC_MAX_WORDS);
            }
            else
            {
                cursor = &symbols[1U];
                ok     = true;

                for (w = 0U; (w < data_word_count) && ok; w++)
                {
                    if (!decode_word(cursor, &result->words[w]))
                    {
                        ESP_LOGD(TAG, "data word %u failed", (unsigned)w);
                        ok = false;
                    }
                    else
                    {
                        cursor += NEC_BITS_PER_WORD;
                    }
                }

                if (ok && !decode_word(cursor, &received_crc))
                {
                    ESP_LOGD(TAG, "CRC word decode failed");
                    ok = false;
                }

                if (ok)
                {
                    result->count        = data_word_count;
                    result->received_crc = received_crc;

                    computed_crc = esp_rom_crc32_le(
                        0xFFFFFFFFU,
                        (const uint8_t *)result->words,
                        data_word_count * sizeof(uint32_t));

                    result->computed_crc = computed_crc;
                    result->crc_ok       = (computed_crc == received_crc);

                    if (!result->crc_ok)
                    {
                        ESP_LOGW(TAG,
                                 "CRC mismatch: rx=0x%08" PRIX32
                                 " calc=0x%08" PRIX32,
                                 received_crc, computed_crc);
                    }
                }
            }
        }
    }

    return ok;
}
