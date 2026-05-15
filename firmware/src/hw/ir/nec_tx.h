
#ifndef NEC_TX_H
#define NEC_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"

#include "hw/ir/nec_mw_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Worst-case frame: 8 data words + 1 CRC word = 9 words.
 * 9 x 32 bits x ~1.69 ms/bit ~ 490 ms.  3000 ms keeps generous margin.
 */
#define NEC_TX_SEM_TIMEOUT_MS   (3000U)

/*
 * Ring depth must be STRICTLY GREATER than rmt_tx_channel_config_t::
 * trans_queue_depth (currently 1). Each in-flight RMT transaction needs its own
 * live payload buffer because the encoder reads from it asynchronously
 * from the ISR — see the "You CAN'T modify the `payload` during the
 * transmission" note in the ESP-IDF RMT docs.
 *
 * With queue_depth=1 and BadgeIR::sendFrameNoWait() currently falling back
 * to blocking sendFrame(), two slots preserve the async lifetime guarantee
 * without keeping six idle 64-word payload buffers in internal/DMA memory.
 */
#define NEC_TX_RING_DEPTH       (2U)

/*
 * Tasking constraint:
 * This TX implementation uses FreeRTOS task notifications for TX-complete
 * signaling.  nec_tx_init() captures the calling task and stores it in the
 * context as the task to notify on TX completion.  Therefore, for any given
 * nec_tx_context_t, nec_tx_init(), nec_tx_send(), nec_tx_send_nowait(), and
 * nec_tx_wait() must all be called from the same FreeRTOS task.  Using
 * different tasks with the same context is not supported.
 */
typedef struct {
    rmt_channel_handle_t  chan;
    rmt_encoder_handle_t  encoder;
    TaskHandle_t          ask_to_notify;      /* task to notify on TX done */
    bool                  is_transmitting;
    uint8_t               ring_idx;           /* next slot to use (mod depth) */
    uint32_t              ring_buf[NEC_TX_RING_DEPTH][NEC_MAX_WORDS];
    nec_mw_payload_t      ring_payload[NEC_TX_RING_DEPTH];
} nec_tx_context_t;

/*
 * Initialize a TX context.
 *
 * Must be called from the same FreeRTOS task that will later call
 * nec_tx_send() and nec_tx_wait() for this context.
 */
esp_err_t nec_tx_init(nec_tx_context_t *ctx,
                       gpio_num_t        gpio,
                       uint32_t          resolution_hz);

/*
 * Enqueue a multi-word NEC frame.  Blocks until the TX buffer is free
 * (up to timeout_ms), copies words into the buffer, calls rmt_transmit().
 * Does not wait for the transmission to complete — use nec_tx_wait().
 *
 * Must be called from the same FreeRTOS task that called nec_tx_init() for
 * this context, because completion signaling uses task notifications.
 *
 * timeout_ms > 0: use that value.  0: default NEC_TX_SEM_TIMEOUT_MS.  < 0:
 * wait forever.
 */
esp_err_t nec_tx_send(nec_tx_context_t *ctx,
                       const uint32_t   *words,
                       size_t            count,
                       int32_t           timeout_ms);

/*
 * Enqueue without blocking or touching the trans-done notification counter.
 * Copies into the next ring slot and hands the transaction to the RMT driver.
 * Meant for back-to-back streaming where the caller knows it can accept up
 * to NEC_TX_RING_DEPTH pending transactions; the RMT driver's own queue
 * (trans_queue_depth) is the true gate.  Returns ESP_ERR_INVALID_STATE if
 * the RMT transaction queue is currently full — caller should retry on the
 * next tick.
 *
 * Must be called from the same FreeRTOS task as nec_tx_init().
 */
esp_err_t nec_tx_send_nowait(nec_tx_context_t *ctx,
                              const uint32_t   *words,
                              size_t            count);

/*
 * Block until the current transmission completes.  0 = wait forever.
 *
 * Must be called from the same FreeRTOS task that called nec_tx_init() for
 * this context, because completion signaling uses task notifications.
 */
esp_err_t nec_tx_wait(nec_tx_context_t *ctx, uint32_t timeout_ms);

/* Tear down TX resources.  Safe only when no transmission is in progress. */
esp_err_t nec_tx_deinit(nec_tx_context_t *ctx);

/*
 * Transmit a pre-built array of raw RMT symbols using a caller-supplied
 * encoder (typically a copy encoder from rmt_new_copy_encoder()). Used by
 * the IR Playground for consumer NEC frames and arbitrary captured remote
 * codes that do not fit the badge's multi-word + CRC dialect.
 *
 * Acquires the same task-notify guard as nec_tx_send so it serializes
 * cleanly with badge-mode traffic. Caller is responsible for keeping
 * `symbols` valid until nec_tx_wait() returns.
 *
 * Must be called from the same FreeRTOS task that owns the context.
 */
esp_err_t nec_tx_send_symbols(nec_tx_context_t       *ctx,
                               rmt_encoder_handle_t   encoder,
                               const rmt_symbol_word_t *symbols,
                               size_t                  symbol_count,
                               int32_t                 timeout_ms);

/*
 * Reapply the carrier with a custom frequency. Default boot config is
 * 38 kHz; some captured remotes (Sony/RC5/RC6) want 36–40 kHz, and the
 * raw API exposes a wider 30–60 kHz range. duty stays at the current
 * cached value. Safe only between transmissions.
 */
esp_err_t nec_tx_set_carrier_freq(nec_tx_context_t *ctx,
                                   uint32_t          frequency_hz,
                                   float             duty);

/*
 * Change the 38 kHz carrier duty cycle at runtime.  Lower duty = lower
 * effective LED drive current = shorter IR range.  Useful for testing
 * self-loopback where a full-duty LED overwhelms even a covered sensor.
 *
 * duty must be in (0.0, 1.0).  Nominal default is 0.50.  Values below
 * ~0.05 may not be decodable by typical 38 kHz receivers.
 */
esp_err_t nec_tx_set_carrier_duty(nec_tx_context_t *ctx, float duty);

#ifdef __cplusplus
}
#endif

#endif /* NEC_TX_H */
