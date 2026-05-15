#include "hw/ir/nec_consumer.h"

#include <string.h>

/* Standard NEC pulse widths in microseconds. */
#define NEC_LEADER_MARK_US   9000U
#define NEC_LEADER_SPACE_US  4500U
#define NEC_REPEAT_SPACE_US  2250U
#define NEC_BIT_MARK_US       560U
#define NEC_ZERO_SPACE_US     560U
#define NEC_ONE_SPACE_US     1690U

/* Acceptance windows. NEC remotes are notoriously sloppy in the real
 * world — TSOP38xx receivers stretch marks ~150 us and shrink spaces
 * by similar amounts. ~30% tolerance on every edge keeps cheap remotes
 * decodable while still rejecting other protocols (Sony / RC5 / RC6). */
static bool within_tol(uint32_t actual_us, uint32_t target_us)
{
    uint32_t lo = (target_us * 7U) / 10U;   /* -30% */
    uint32_t hi = (target_us * 13U) / 10U;  /* +30% */
    return (actual_us >= lo) && (actual_us <= hi);
}

static rmt_symbol_word_t make_sym(uint16_t hi_us, uint16_t lo_us)
{
    rmt_symbol_word_t s = {0};
    s.level0    = 1U;
    s.duration0 = hi_us;
    s.level1    = 0U;
    s.duration1 = lo_us;
    return s;
}

size_t nec_consumer_encode(uint8_t           addr,
                            uint8_t           cmd,
                            rmt_symbol_word_t *out,
                            size_t            max)
{
    if ((out == NULL) || (max < NEC_CONSUMER_FRAME_SYMBOLS))
    {
        return 0U;
    }

    /* Build the 32-bit payload LSB-first as the wire wants:
     *   addr | ~addr | cmd | ~cmd
     * sent low-bit-first across the wire. */
    uint32_t payload = ((uint32_t)addr)
                       | ((uint32_t)((uint8_t)~addr) << 8)
                       | ((uint32_t)cmd            << 16)
                       | ((uint32_t)((uint8_t)~cmd)  << 24);

    size_t i = 0U;
    out[i++] = make_sym(NEC_LEADER_MARK_US, NEC_LEADER_SPACE_US);

    for (size_t b = 0U; b < 32U; b++)
    {
        bool one = ((payload >> b) & 1U) != 0U;
        out[i++] = make_sym(NEC_BIT_MARK_US,
                            one ? NEC_ONE_SPACE_US : NEC_ZERO_SPACE_US);
    }

    /* End pulse — the IDF copy encoder treats duration1=0 as
     * "terminate transmission" so the carrier turns off cleanly. */
    out[i++] = make_sym(NEC_BIT_MARK_US, 0U);

    return i;
}

size_t nec_consumer_encode_repeat(rmt_symbol_word_t *out, size_t max)
{
    if ((out == NULL) || (max < NEC_CONSUMER_REPEAT_SYMBOLS))
    {
        return 0U;
    }
    out[0] = make_sym(NEC_LEADER_MARK_US, NEC_REPEAT_SPACE_US);
    out[1] = make_sym(NEC_BIT_MARK_US, 0U);
    return 2U;
}

bool nec_consumer_decode(const rmt_symbol_word_t *symbols,
                          size_t                   symbol_count,
                          uint8_t                 *addr_out,
                          uint8_t                 *cmd_out,
                          bool                    *is_repeat_out)
{
    if ((symbols == NULL) || (is_repeat_out == NULL))
    {
        return false;
    }

    /* Repeat frame: leader-with-2250us-space, then a short mark. The
     * RX channel often appends a trailing 0 symbol; tolerate up to
     * 4 symbols total. */
    if (symbol_count >= 1U && symbol_count <= 4U)
    {
        if (within_tol(symbols[0].duration0, NEC_LEADER_MARK_US) &&
            within_tol(symbols[0].duration1, NEC_REPEAT_SPACE_US))
        {
            *is_repeat_out = true;
            return true;
        }
    }

    /* Full frame: 33 symbols mandatory (leader + 32 bits). Tolerate up
     * to 4 trailing pad symbols (RX glitches at end-of-burst). */
    if (symbol_count < 33U || symbol_count > (33U + 4U))
    {
        return false;
    }

    if (!within_tol(symbols[0].duration0, NEC_LEADER_MARK_US) ||
        !within_tol(symbols[0].duration1, NEC_LEADER_SPACE_US))
    {
        return false;
    }

    uint32_t payload = 0U;
    for (size_t b = 0U; b < 32U; b++)
    {
        const rmt_symbol_word_t *s = &symbols[1U + b];
        if (!within_tol(s->duration0, NEC_BIT_MARK_US))
        {
            return false;
        }
        if (within_tol(s->duration1, NEC_ONE_SPACE_US))
        {
            payload |= (1UL << b);
        }
        else if (!within_tol(s->duration1, NEC_ZERO_SPACE_US))
        {
            /* Last bit on the wire often has duration1=0 when the
             * RX driver compresses the trailing space — accept. */
            if ((b == 31U) && (s->duration1 == 0U))
            {
                /* leave bit as 0 */
            }
            else
            {
                return false;
            }
        }
    }

    uint8_t a    = (uint8_t)(payload         & 0xFFU);
    uint8_t a_in = (uint8_t)((payload >> 8)  & 0xFFU);
    uint8_t c    = (uint8_t)((payload >> 16) & 0xFFU);
    uint8_t c_in = (uint8_t)((payload >> 24) & 0xFFU);

    /* NEC strict: addr and cmd are always followed by their bitwise
     * inverse. "Extended NEC" relaxes the addr inverse but keeps the
     * cmd inverse — accept that too. */
    if ((uint8_t)(c ^ c_in) != 0xFFU)
    {
        return false;
    }
    (void)a_in;  /* relaxed for extended NEC */

    if (addr_out) *addr_out = a;
    if (cmd_out)  *cmd_out  = c;
    *is_repeat_out = false;
    return true;
}
