
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_common.h"

#include "hw/ir/nec_tx.h"
#include "hw/ir/nec_mw_encoder.h"

static const char *TAG = "nec_tx";

esp_err_t nec_tx_wait(nec_tx_context_t *ctx, uint32_t timeout_ms)
{
    if (ctx == NULL) return ESP_ERR_INVALID_ARG;

    if (!ctx->is_transmitting) {
        return ESP_OK;
    }

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (ulTaskNotifyTake(pdTRUE, ticks) == 0) {
        ESP_LOGE(TAG, "Timeout waiting for TX to finish");
        return ESP_ERR_TIMEOUT;
    }

    ctx->is_transmitting = false;

    /* Re-prime the notification so the next nec_tx_send() can immediately
     * consume one and start its transmission.  Without this, back-to-back
     * send+wait cycles leave the counter at 0 and the second send blocks
     * for the full timeout. */
    if (ctx->ask_to_notify != NULL)
    {
        xTaskNotifyGive(ctx->ask_to_notify);
    }

    return ESP_OK;
}

static bool IRAM_ATTR on_trans_done(rmt_channel_handle_t            channel,
                                     const rmt_tx_done_event_data_t *edata,
                                     void                           *user_ctx)
{
    BaseType_t        higher_prio_woken = pdFALSE;
    nec_tx_context_t *ctx;

    (void)channel;
    (void)edata;

    ctx = (nec_tx_context_t *)(void *)user_ctx;

    if (ctx != NULL && ctx->ask_to_notify != NULL)
    {
        (void)vTaskNotifyGiveFromISR(ctx->ask_to_notify, &higher_prio_woken);
    }

    return (higher_prio_woken == pdTRUE);
}

esp_err_t nec_tx_init(nec_tx_context_t *ctx,
                       gpio_num_t        gpio,
                       uint32_t          resolution_hz)
{
    esp_err_t ret = ESP_OK;

    if ((ctx == NULL) || (resolution_hz == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->ask_to_notify = xTaskGetCurrentTaskHandle();
    if (ctx->ask_to_notify == NULL)
    {
        ESP_LOGE(TAG, "task handle create failed");
        return ESP_ERR_NO_MEM;
    }

    /* Prime the notification so the first wait/send does not block
     * until a completion ISR has run. Subsequent completions continue
     * to replenish the count via on_trans_done().
     */
    xTaskNotifyGive(ctx->ask_to_notify);

    rmt_tx_channel_config_t cfg = {0};
    cfg.gpio_num          = gpio;
    cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz     = resolution_hz;
    // mem_block_symbols = DMA buffer size.  Bumped from the original
    // 64-symbol baseline to 256 to give the NEC encoder ~500 ms of
    // on-wire runway per ISR refill cycle.  At 64, long DATA frames
    // (17+ words ≈ 1 s on-wire) consistently truncated at ~450 symbols
    // on the peer RX (a > 30 ms mid-frame gap that cut the receive
    // watchdog off); at 128 the cut still happened but less often.
    // The earlier wedge concern ("trans_done stopped firing") was
    // observed with unknown earlier code; re-testing empirically with
    // current IDF + encoder shows clean operation at 256.  If a wedge
    // ever returns, revert to 128 and look at raising RMT intr_priority
    // instead.
    cfg.mem_block_symbols = 256U;
    // trans_queue_depth = 1 means one transaction in flight at a time.
    // Depth > 1 exposed multi-slot ring races in tx_enqueue_slot under
    // load (DMA reading from a buffer we were simultaneously re-writing).
    // The streaming speedup from pipelining is ~1 ms per frame — not
    // worth the correctness risk.
    cfg.trans_queue_depth = 1U;
    cfg.intr_priority     = 0;
    cfg.flags.with_dma    = 1U;
    cfg.flags.invert_out  = 0U;
    cfg.flags.allow_pd    = 0U;
    cfg.flags.init_level  = 0U;

    ret = rmt_new_tx_channel(&cfg, &ctx->chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_new_tx_channel: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    rmt_carrier_config_t cc = {0};
    cc.frequency_hz              = 38000U;
    cc.duty_cycle                = 0.50f;
    cc.flags.polarity_active_low = 0U;
    cc.flags.always_on           = 0U;

    ret = rmt_apply_carrier(ctx->chan, &cc);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_apply_carrier: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    nec_mw_encoder_config_t enc_cfg = {0};
    enc_cfg.resolution_hz = resolution_hz;

    ret = nec_mw_encoder_create(&enc_cfg, &ctx->encoder);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "nec_mw_encoder_create: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    rmt_tx_event_callbacks_t cbs = {0};
    cbs.on_trans_done = on_trans_done;

    ret = rmt_tx_register_event_callbacks(ctx->chan, &cbs, ctx);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "register callbacks: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = rmt_enable(ctx->chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_enable: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGI(TAG, "TX ready on GPIO %d at %lu Hz", (int)gpio,
             (unsigned long)resolution_hz);
    return ESP_OK;

cleanup:
    (void)nec_tx_deinit(ctx);
    return ret;
}

static const rmt_transmit_config_t s_tx_cfg = {
    .loop_count = 0,
    .flags = {
        .eot_level         = 0U,
        .queue_nonblocking = 1U,
    },
};

/*
 * Stage the words into the next ring slot and hand the transaction to the
 * RMT driver.  Advances ring_idx only on success — a full-queue failure
 * leaves the slot alone so a retry doesn't "skip" it.  NEC_TX_RING_DEPTH is
 * strictly greater than trans_queue_depth (1), so cycling back to a slot is
 * safe: the prior transaction has completed before the blocking send path
 * can enqueue another frame.
 */
static esp_err_t tx_enqueue_slot(nec_tx_context_t *ctx,
                                  const uint32_t   *words,
                                  size_t            count)
{
    const uint8_t slot = ctx->ring_idx;

    memcpy(ctx->ring_buf[slot], words, count * sizeof(uint32_t));
    ctx->ring_payload[slot].words = ctx->ring_buf[slot];
    ctx->ring_payload[slot].count = count;

    esp_err_t ret = rmt_transmit(ctx->chan,
                                  ctx->encoder,
                                  &ctx->ring_payload[slot],
                                  sizeof(ctx->ring_payload[slot]),
                                  &s_tx_cfg);

    if (ret == ESP_OK) {
        ctx->ring_idx = (uint8_t)((slot + 1U) % NEC_TX_RING_DEPTH);
    }
    // On failure, leave ring_idx pointing at this slot.  Caller can retry;
    // the slot's memcpy payload is still valid and will be re-used on
    // the next attempt.  No in-flight corruption because the ring-depth
    // guarantee: slot N is always idle when ring_idx == N.

    return ret;
}

esp_err_t nec_tx_send(nec_tx_context_t *ctx,
                       const uint32_t   *words,
                       size_t            count,
                       int32_t           timeout_ms)
{
    esp_err_t ret = ESP_OK;

    if ((ctx == NULL) || (words == NULL) ||
        (count == 0U) || (count > (size_t)NEC_MAX_WORDS))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->ask_to_notify == NULL)
    {
        ESP_LOGE(TAG, "no task to notify on TX done");
        return ESP_ERR_INVALID_ARG;
    }

    bool wait_forever = (timeout_ms < 0);
    uint32_t timeout_to_use_ms = (timeout_ms > 0) ? (uint32_t)timeout_ms : 7000U;
    TickType_t ticks_to_wait = wait_forever ? portMAX_DELAY : pdMS_TO_TICKS(timeout_to_use_ms);

    if (wait_forever)
    {
        ESP_LOGW(TAG, "Waiting indefinitely for TX buffer");
    }

    if (ulTaskNotifyTake(pdTRUE, ticks_to_wait) != pdTRUE)
    {
        if (wait_forever)
        {
            ESP_LOGE(TAG, "task notification wait failed while waiting indefinitely");
        }
        else
        {
            ESP_LOGE(TAG, "task notification timeout (%lu ms)", timeout_to_use_ms);
        }
        return ESP_ERR_TIMEOUT;
    }

    ctx->is_transmitting = true;

    ret = tx_enqueue_slot(ctx, words, count);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_transmit: %s", esp_err_to_name(ret));
        ctx->is_transmitting = false;
        (void)xTaskNotifyGive(ctx->ask_to_notify);
    }

    return ret;
}

esp_err_t nec_tx_send_nowait(nec_tx_context_t *ctx,
                              const uint32_t   *words,
                              size_t            count)
{
    if ((ctx == NULL) || (words == NULL) ||
        (count == 0U) || (count > (size_t)NEC_MAX_WORDS))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->chan == NULL || ctx->encoder == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Does NOT touch ctx->is_transmitting or the notify counter.  The RMT
     * driver's own queue is the real backpressure mechanism — rmt_transmit
     * will return ESP_ERR_INVALID_STATE if full, caller retries next tick.
     * ISR trans_done still fires per transaction; notify credits accumulate
     * harmlessly and get drained by the next nec_tx_send / nec_tx_wait.
     */
    return tx_enqueue_slot(ctx, words, count);
}


esp_err_t nec_tx_send_symbols(nec_tx_context_t       *ctx,
                               rmt_encoder_handle_t   encoder,
                               const rmt_symbol_word_t *symbols,
                               size_t                  symbol_count,
                               int32_t                 timeout_ms)
{
    if ((ctx == NULL) || (encoder == NULL) ||
        (symbols == NULL) || (symbol_count == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->ask_to_notify == NULL || ctx->chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool wait_forever = (timeout_ms < 0);
    uint32_t timeout_to_use_ms = (timeout_ms > 0) ? (uint32_t)timeout_ms : 7000U;
    TickType_t ticks_to_wait = wait_forever
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_to_use_ms);

    if (ulTaskNotifyTake(pdTRUE, ticks_to_wait) != pdTRUE)
    {
        ESP_LOGE(TAG, "send_symbols: notify timeout");
        return ESP_ERR_TIMEOUT;
    }

    ctx->is_transmitting = true;
    esp_err_t ret = rmt_transmit(ctx->chan,
                                  encoder,
                                  symbols,
                                  symbol_count * sizeof(rmt_symbol_word_t),
                                  &s_tx_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "send_symbols rmt_transmit: %s", esp_err_to_name(ret));
        ctx->is_transmitting = false;
        (void)xTaskNotifyGive(ctx->ask_to_notify);
    }
    return ret;
}

esp_err_t nec_tx_set_carrier_freq(nec_tx_context_t *ctx,
                                   uint32_t          frequency_hz,
                                   float             duty)
{
    if ((ctx == NULL) || (ctx->chan == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ((duty <= 0.0f) || (duty >= 1.0f))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if ((frequency_hz < 1000U) || (frequency_hz > 1000000U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->is_transmitting)
    {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_carrier_config_t cc = {0};
    cc.frequency_hz              = frequency_hz;
    cc.duty_cycle                = duty;
    cc.flags.polarity_active_low = 0U;
    cc.flags.always_on           = 0U;
    return rmt_apply_carrier(ctx->chan, &cc);
}

esp_err_t nec_tx_set_carrier_duty(nec_tx_context_t *ctx, float duty)
{
    if ((ctx == NULL) || (ctx->chan == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ((duty <= 0.0f) || (duty >= 1.0f))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->is_transmitting)
    {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_carrier_config_t cc = {0};
    cc.frequency_hz              = 38000U;
    cc.duty_cycle                = duty;
    cc.flags.polarity_active_low = 0U;
    cc.flags.always_on           = 0U;

    return rmt_apply_carrier(ctx->chan, &cc);
}

esp_err_t nec_tx_deinit(nec_tx_context_t *ctx)
{
    esp_err_t ret = ESP_OK;

    if (ctx == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->is_transmitting = false;

    if (ctx->chan != NULL)
    {
        (void)rmt_disable(ctx->chan);
        ret = rmt_del_channel(ctx->chan);
        ctx->chan = NULL;
    }

    if (ctx->encoder != NULL)
    {
        esp_err_t enc_ret = rmt_del_encoder(ctx->encoder);
        ctx->encoder = NULL;
        if ((ret == ESP_OK) && (enc_ret != ESP_OK))
        {
            ret = enc_ret;
        }
    }

    // Don't delete the task — ask_to_notify is a borrowed handle to the
    // caller (irTask).  Just drop the reference so the ISR stops notifying.
    ctx->ask_to_notify = NULL;

    return ret;
}
