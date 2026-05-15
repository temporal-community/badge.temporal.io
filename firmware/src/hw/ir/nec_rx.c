
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_common.h"

#include "hw/ir/nec_rx.h"
#include "hw/ir/nec_mw_decoder.h"

static const char *TAG = "nec_rx";

#define RX_SIGNAL_MIN_NS   (3000UL)
/* Bumped from 12 ms while tracking down why long DATA frames were being
 * fragmented across multiple rmt_receive transactions on the peer.  The
 * old 12 ms ceiling caused any mid-frame gap in the TX encoder's output
 * (suspected DMA underrun between 64-symbol refills on long frames) to
 * cut the frame off at RX, producing a stream of structurally-invalid
 * fragments.  The ESP-IDF RMT driver hard-caps signal_range_max_ns at
 * 32767000 ns (16-bit tick register at 1 MHz resolution), so we sit
 * just below that ceiling — at 30 ms RX rides through any gap up to
 * that duration and still reconstructs the whole frame for NEC
 * decoding.  Short frames are unaffected — they don't have inter-frame
 * gaps anywhere close to this, and the end-of-transmission pulse still
 * closes the frame normally. */
#define RX_SIGNAL_MAX_NS   (30000000UL)

/* Per-frame raw symbol dump.  Leave at 0 in production: when the runtime
 * IDF log level is >= INFO, each decoded frame emits ~21 ESP_LOGI lines
 * and the resulting USB-CDC back-pressure stalls decode_task long enough
 * that the 4-slot RMT event queue fills and frames start being dropped
 * silently.  Flip to 1 only for offline debugging with an attached UART. */
#define IR_DEBUG 0U
#define LOG_SYM 20U

static esp_err_t rx_rearm(rmt_channel_handle_t          chan,
                            rmt_symbol_word_t            *symbols,
                            size_t                        symbol_bytes,
                            const rmt_receive_config_t   *cfg)
{
    esp_err_t ret      = ESP_FAIL;
    uint32_t  retry;
    uint32_t  delay_ms = NEC_RX_REARM_BASE_DELAY_MS;

    for (retry = 0U;
         (retry < NEC_RX_REARM_RETRIES) && (ret != ESP_OK);
         retry++)
    {
        ret = rmt_receive(chan, symbols, symbol_bytes, cfg);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "rmt_receive retry %u/%u: %s",
                     (unsigned)(retry + 1U),
                     NEC_RX_REARM_RETRIES,
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            delay_ms *= 2U;
        }
    }

    return ret;
}

static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t            chan,
                                  const rmt_rx_done_event_data_t *ev,
                                  void                           *arg)
{
    BaseType_t woken = pdFALSE;
    (void)chan;
    (void)xQueueSendFromISR((QueueHandle_t)(void *)arg, ev, &woken);
    return (woken == pdTRUE);
}

static void decode_task(void *arg)
{
    nec_rx_context_t *ctx = (nec_rx_context_t *)(void *)arg;

    /*
     * en_partial_rx left OFF intentionally.  NEC_RX_SYMBOL_COUNT (2112)
     * holds a worst-case 64-word NEC frame (2082 symbols) with margin —
     * sized to match NEC_MAX_WORDS in nec_mw_encoder.h so the receiver
     * never truncates a legitimately-sized DATA burst from peer.
     */
    const rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = RX_SIGNAL_MIN_NS,
        .signal_range_max_ns = RX_SIGNAL_MAX_NS,
    };

    rmt_rx_done_event_data_t ev;
    nec_mw_result_t          res;
    esp_err_t                ret;
    uint32_t                 frames_seen = 0U;

    ret = rx_rearm(ctx->chan, ctx->symbols, sizeof(ctx->symbols), &rx_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "initial arm failed — task exiting");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "decode stack_hwm_bytes start=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    for (;;)
    {
        if (xQueueReceive(ctx->queue, &ev, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

#if IR_DEBUG
        ESP_LOGI(TAG, "=== RAW RX DUMP: %u symbols ===", (unsigned)ev.num_symbols);

        uint32_t limit = (ev.num_symbols > LOG_SYM) ? LOG_SYM : ev.num_symbols;
        for (uint32_t i = 0U; i < limit; i++)
        {
            ESP_LOGI(TAG, "[%02u] Burst(L):%5u us  Space(H):%5u us",
                     (unsigned)i,
                     (unsigned)ev.received_symbols[i].duration0,
                     (unsigned)ev.received_symbols[i].duration1);
        }

        if (ev.num_symbols > LOG_SYM)
        {
            ESP_LOGI(TAG, "... (%u more symbols hidden)", (unsigned)(ev.num_symbols - LOG_SYM));
        }
#endif

        /* Always forward the raw symbol stream to BadgeIR's mode router
         * before running the multi-word decoder. The router decides
         * whether to ignore it (badge mode), parse it as consumer NEC,
         * or hand the raw symbols to MicroPython. */
        if (ctx->raw_cb != NULL)
        {
            ctx->raw_cb(ev.received_symbols, ev.num_symbols, ctx->user_data);
        }

        if (nec_mw_decode_frame(ev.received_symbols, ev.num_symbols, &res))
        {
            frames_seen++;
            if ((frames_seen == 1U) || ((frames_seen % 128U) == 0U))
            {
                ESP_LOGI(TAG, "decode stack_hwm_bytes=%u frames=%u symbols=%u",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL),
                         (unsigned)frames_seen,
                         (unsigned)ev.num_symbols);
            }
            if (ctx->frame_cb != NULL)
            {
                ctx->frame_cb(&res, ctx->user_data);
            }
        }
        else
        {
#if IR_DEBUG
            ESP_LOGW(TAG, "Frame decode failed (see raw dump above).");
#else
            ESP_LOGD(TAG, "structurally invalid frame (%u symbols)",
                     (unsigned)ev.num_symbols);
#endif

            if (ctx->frame_cb != NULL)
            {
                ctx->frame_cb(NULL, ctx->user_data);
            }
        }

        ret = rx_rearm(ctx->chan, ctx->symbols, sizeof(ctx->symbols), &rx_cfg);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "re-arm failed — receiver stopped");
            vTaskDelete(NULL);
            return;
        }
    }
}

esp_err_t nec_rx_init(nec_rx_context_t *ctx,
                       gpio_num_t        gpio,
                       uint32_t          resolution_hz,
                       nec_rx_frame_cb_t frame_cb,
                       void             *user_data)
{
    esp_err_t ret = ESP_OK;

    if ((ctx == NULL) || (resolution_hz == 0U) || (frame_cb == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(nec_rx_context_t));

    ctx->frame_cb  = frame_cb;
    ctx->user_data = user_data;

    {
        rmt_rx_channel_config_t cfg = {0};
        cfg.gpio_num          = gpio;
        cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
        cfg.resolution_hz     = resolution_hz;
        cfg.mem_block_symbols = NEC_RX_SYMBOL_COUNT;
        cfg.intr_priority     = 0;
        cfg.flags.invert_in   = 0U;
        cfg.flags.with_dma    = 1U;
        cfg.flags.allow_pd    = 0U;

        ret = rmt_new_rx_channel(&cfg, &ctx->chan);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "rmt_new_rx_channel: %s", esp_err_to_name(ret));
            goto cleanup;
        }
    }

    ctx->queue = xQueueCreate(4U, sizeof(rmt_rx_done_event_data_t));
    if (ctx->queue == NULL)
    {
        ESP_LOGE(TAG, "queue create failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    {
        rmt_rx_event_callbacks_t cbs = {0};
        cbs.on_recv_done = rx_done_cb;

        ret = rmt_rx_register_event_callbacks(ctx->chan, &cbs, ctx->queue);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "register callbacks: %s", esp_err_to_name(ret));
            goto cleanup;
        }
    }

    ret = rmt_enable(ctx->chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_enable: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    {
        BaseType_t created = xTaskCreate(decode_task,
                                          "nec_rx",
                                          NEC_RX_TASK_STACK_WORDS,
                                          ctx,
                                          NEC_RX_TASK_PRIORITY,
                                          &ctx->task);
        if (created != pdPASS)
        {
            ESP_LOGE(TAG, "task create failed");
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    }

    ESP_LOGI(TAG, "RX ready on GPIO %d", (int)gpio);
    return ESP_OK;

cleanup:
    (void)nec_rx_deinit(ctx);
    return ret;
}

void nec_rx_set_raw_cb(nec_rx_context_t *ctx,
                        nec_rx_raw_cb_t   raw_cb)
{
    if (ctx != NULL)
    {
        ctx->raw_cb = raw_cb;
    }
}

esp_err_t nec_rx_deinit(nec_rx_context_t *ctx)
{
    if (ctx == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->task != NULL)
    {
        vTaskDelete(ctx->task);
        ctx->task = NULL;
    }

    if (ctx->chan != NULL)
    {
        (void)rmt_disable(ctx->chan);
        (void)rmt_del_channel(ctx->chan);
        ctx->chan = NULL;
    }

    if (ctx->queue != NULL)
    {
        vQueueDelete(ctx->queue);
        ctx->queue = NULL;
    }

    return ESP_OK;
}
