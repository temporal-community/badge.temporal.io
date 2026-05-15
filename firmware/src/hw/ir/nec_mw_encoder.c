
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_rom_crc.h"
#include "driver/rmt_encoder.h"
#include "nec_mw_encoder.h"

static const char *TAG = "nec_mw_enc";

/* NEC timing constants (us at 1 MHz resolution) */
#define NEC_LEADING_HI_US  (9000U)
#define NEC_LEADING_LO_US  (4500U)
#define NEC_BIT_HI_US       (562U)
#define NEC_BIT_0_LO_US     (562U)
#define NEC_BIT_1_LO_US    (1688U)
#define NEC_END_HI_US       (562U)

static inline uint32_t us2ticks(uint32_t us, uint32_t hz)
{
    return (uint32_t)(((uint64_t)us * (uint64_t)hz) / 1000000UL);
}

typedef enum {
    NEC_MW_STATE_LEADER  = 0,
    NEC_MW_STATE_DATA    = 1,
    NEC_MW_STATE_CRC     = 2,
    NEC_MW_STATE_ENDING  = 3
} nec_mw_state_t;

/* base MUST be the first member — __containerof depends on it. */
typedef struct {
    rmt_encoder_t      base;
    rmt_encoder_t     *copy_encoder;
    rmt_encoder_t     *bytes_encoder;
    rmt_symbol_word_t  leading_symbol;
    rmt_symbol_word_t  ending_symbol;
    nec_mw_state_t     state;
    size_t             word_idx;
    uint32_t           crc_word;
    bool               crc_ready;
} nec_mw_encoder_t;

/*
 * State machine encoder — called from ISR context (IRAM).
 * Picks up where it left off via enc->state and enc->word_idx.
 */
RMT_ENCODER_FUNC_ATTR
static size_t nec_mw_encode(rmt_encoder_t       *encoder,
                             rmt_channel_handle_t tx_channel,
                             const void          *primary_data,
                             size_t               data_size,
                             rmt_encode_state_t  *ret_state)
{
    const nec_mw_payload_t *payload =
        (const nec_mw_payload_t *)(const void *)primary_data;

    nec_mw_encoder_t  *enc     = __containerof(encoder, nec_mw_encoder_t, base);
    rmt_encoder_t     *copy    = enc->copy_encoder;
    rmt_encoder_t     *bytes   = enc->bytes_encoder;
    rmt_encode_state_t session = RMT_ENCODING_RESET;
    rmt_encode_state_t out     = RMT_ENCODING_RESET;
    size_t             encoded = 0U;
    bool               running = true;

    (void)data_size;

    while (running)
    {
        switch (enc->state)
        {
        case NEC_MW_STATE_LEADER:
            encoded += copy->encode(copy, tx_channel,
                                    &enc->leading_symbol,
                                    sizeof(rmt_symbol_word_t), &session);

            if ((session & RMT_ENCODING_COMPLETE) != 0U)
            {
                enc->state    = NEC_MW_STATE_DATA;
                enc->word_idx = 0U;
                (void)bytes->reset(bytes);
            }

            if ((session & RMT_ENCODING_MEM_FULL) != 0U)
            {
                out    |= RMT_ENCODING_MEM_FULL;
                running = false;
            }
            break;

        case NEC_MW_STATE_DATA:
            encoded += bytes->encode(bytes, tx_channel,
                                     &payload->words[enc->word_idx],
                                     sizeof(uint32_t), &session);

            if ((session & RMT_ENCODING_COMPLETE) != 0U)
            {
                enc->word_idx++;

                if (enc->word_idx < payload->count)
                {
                    (void)bytes->reset(bytes);
                }
                else
                {
                    if (!enc->crc_ready)
                    {
                        enc->crc_word = esp_rom_crc32_le(
                            0xFFFFFFFFU,
                            (const uint8_t *)payload->words,
                            payload->count * sizeof(uint32_t));
                        enc->crc_ready = true;
                    }
                    enc->state = NEC_MW_STATE_CRC;
                    (void)bytes->reset(bytes);
                }
            }

            if ((session & RMT_ENCODING_MEM_FULL) != 0U)
            {
                out    |= RMT_ENCODING_MEM_FULL;
                running = false;
            }
            break;

        case NEC_MW_STATE_CRC:
            encoded += bytes->encode(bytes, tx_channel,
                                     &enc->crc_word,
                                     sizeof(uint32_t), &session);

            if ((session & RMT_ENCODING_COMPLETE) != 0U)
            {
                enc->state = NEC_MW_STATE_ENDING;
            }

            if ((session & RMT_ENCODING_MEM_FULL) != 0U)
            {
                out    |= RMT_ENCODING_MEM_FULL;
                running = false;
            }
            break;

        case NEC_MW_STATE_ENDING:
            encoded += copy->encode(copy, tx_channel,
                                    &enc->ending_symbol,
                                    sizeof(rmt_symbol_word_t), &session);

            if ((session & RMT_ENCODING_COMPLETE) != 0U)
            {
                enc->state  = NEC_MW_STATE_LEADER;
                out        |= RMT_ENCODING_COMPLETE;
                running     = false;
            }

            if ((session & RMT_ENCODING_MEM_FULL) != 0U)
            {
                out    |= RMT_ENCODING_MEM_FULL;
                running = false;
            }
            break;

        default:
            enc->state = NEC_MW_STATE_LEADER;
            running    = false;
            break;
        }
    }

    *ret_state = out;
    return encoded;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t nec_mw_reset(rmt_encoder_t *encoder)
{
    nec_mw_encoder_t *enc = __containerof(encoder, nec_mw_encoder_t, base);
    esp_err_t         ret;

    ret = rmt_encoder_reset(enc->copy_encoder);
    if (ret == ESP_OK)
    {
        ret = rmt_encoder_reset(enc->bytes_encoder);
    }

    if (ret == ESP_OK)
    {
        enc->state     = NEC_MW_STATE_LEADER;
        enc->word_idx  = 0U;
        enc->crc_word  = 0U;
        enc->crc_ready = false;
    }

    return ret;
}

static esp_err_t nec_mw_del(rmt_encoder_t *encoder)
{
    nec_mw_encoder_t *enc = __containerof(encoder, nec_mw_encoder_t, base);
    esp_err_t         ret = ESP_OK;

    if (enc->copy_encoder != NULL)
    {
        ret = rmt_del_encoder(enc->copy_encoder);
    }

    if ((ret == ESP_OK) && (enc->bytes_encoder != NULL))
    {
        ret = rmt_del_encoder(enc->bytes_encoder);
    }

    free(enc);

    return ret;
}

esp_err_t nec_mw_encoder_create(const nec_mw_encoder_config_t *config,
                                 rmt_encoder_handle_t          *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    uint32_t  hz;

    ESP_RETURN_ON_FALSE((config != NULL) && (ret_encoder != NULL),
                        ESP_ERR_INVALID_ARG, TAG, "null argument");
    ESP_RETURN_ON_FALSE(config->resolution_hz > 0U,
                        ESP_ERR_INVALID_ARG, TAG, "resolution_hz must be > 0");

    hz = config->resolution_hz;

    nec_mw_encoder_t *enc =
        (nec_mw_encoder_t *)rmt_alloc_encoder_mem(sizeof(nec_mw_encoder_t));

    ESP_RETURN_ON_FALSE(enc != NULL, ESP_ERR_NO_MEM, TAG, "alloc failed");

    enc->base.encode   = nec_mw_encode;
    enc->base.reset    = nec_mw_reset;
    enc->base.del      = nec_mw_del;
    enc->copy_encoder  = NULL;
    enc->bytes_encoder = NULL;
    enc->state         = NEC_MW_STATE_LEADER;
    enc->word_idx      = 0U;
    enc->crc_word      = 0U;
    enc->crc_ready     = false;

    enc->leading_symbol.level0    = 1U;
    enc->leading_symbol.duration0 = us2ticks(NEC_LEADING_HI_US, hz);
    enc->leading_symbol.level1    = 0U;
    enc->leading_symbol.duration1 = us2ticks(NEC_LEADING_LO_US, hz);

    enc->ending_symbol.level0    = 1U;
    enc->ending_symbol.duration0 = us2ticks(NEC_END_HI_US, hz);
    enc->ending_symbol.level1    = 0U;
    enc->ending_symbol.duration1 = 0U;

    {
        rmt_copy_encoder_config_t copy_cfg;
        (void)memset(&copy_cfg, 0, sizeof(copy_cfg));
        ESP_GOTO_ON_ERROR(
            rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder),
            err, TAG, "copy encoder create failed");
    }

    {
        rmt_bytes_encoder_config_t bytes_cfg = {0};
        bytes_cfg.bit0.level0    = 1U;
        bytes_cfg.bit0.duration0 = us2ticks(NEC_BIT_HI_US,   hz);
        bytes_cfg.bit0.level1    = 0U;
        bytes_cfg.bit0.duration1 = us2ticks(NEC_BIT_0_LO_US, hz);
        bytes_cfg.bit1.level0    = 1U;
        bytes_cfg.bit1.duration0 = us2ticks(NEC_BIT_HI_US,   hz);
        bytes_cfg.bit1.level1    = 0U;
        bytes_cfg.bit1.duration1 = us2ticks(NEC_BIT_1_LO_US, hz);
        bytes_cfg.flags.msb_first = 0U;
        ESP_GOTO_ON_ERROR(
            rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder),
            err, TAG, "bytes encoder create failed");
    }

    *ret_encoder = &enc->base;
    return ESP_OK;

err:
    if (enc->copy_encoder != NULL)
    {
        (void)rmt_del_encoder(enc->copy_encoder);
    }
    if (enc->bytes_encoder != NULL)
    {
        (void)rmt_del_encoder(enc->bytes_encoder);
    }
    free(enc);
    return ret;
}
