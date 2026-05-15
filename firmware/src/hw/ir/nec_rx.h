
#ifndef NEC_RX_H
#define NEC_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"

#include "hw/ir/nec_mw_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One RMT symbol decodes to one NEC bit (mark + space pair).  A full frame
 * needs 1 leader + 32 * (data_words + 1 CRC) + 1 end symbols. For the
 * 64-word TX ceiling (NEC_MAX_WORDS = 64) we need 1 + 32*65 + 1 = 2082
 * symbols. Round up to a 64-multiple (required by RMT DMA on ESP32-S3) →
 * 2112. This MUST match the TX-side ceiling — was briefly shrunk to 320
 * (good for 8-word frames only) and silently truncated all DATA frames
 * carrying more than one TLV.  See NEC_MAX_WORDS in nec_mw_encoder.h.
 */
#define NEC_RX_SYMBOL_COUNT         (2112U)
#define NEC_RX_TASK_STACK_WORDS     (4096U)
#define NEC_RX_TASK_PRIORITY        ((UBaseType_t)(configMAX_PRIORITIES - 2U))
#define NEC_RX_REARM_RETRIES        (5U)
#define NEC_RX_REARM_BASE_DELAY_MS  (10U)

typedef void (*nec_rx_frame_cb_t)(const nec_mw_result_t *result,
                                   void                  *user_data);

/*
 * Raw symbol callback — invoked on every burst the RMT receiver decodes,
 * BEFORE the multi-word NEC decoder runs. Lets BadgeIR feed the raw
 * symbol stream into MicroPython for consumer NEC / arbitrary-protocol
 * decoding without losing back-compat with the badge's own dialect.
 *
 * Callback runs from decode_task. Symbols pointer is owned by the IDF
 * RMT driver and only valid for the duration of the call.
 */
typedef void (*nec_rx_raw_cb_t)(const rmt_symbol_word_t *symbols,
                                 size_t                   symbol_count,
                                 void                    *user_data);

typedef struct {
    rmt_channel_handle_t  chan;
    QueueHandle_t         queue;
    TaskHandle_t          task;
    nec_rx_frame_cb_t     frame_cb;
    nec_rx_raw_cb_t       raw_cb;     /* optional, may be NULL */
    void                 *user_data;
    rmt_symbol_word_t     symbols[NEC_RX_SYMBOL_COUNT];
} nec_rx_context_t;

esp_err_t nec_rx_init(nec_rx_context_t *ctx,
                       gpio_num_t        gpio,
                       uint32_t          resolution_hz,
                       nec_rx_frame_cb_t frame_cb,
                       void             *user_data);

esp_err_t nec_rx_deinit(nec_rx_context_t *ctx);

/* Optional raw-symbol callback. Pass NULL to disable. Safe to call at any
 * point after nec_rx_init(). The callback fires on every received burst
 * before the multi-word decoder runs. */
void nec_rx_set_raw_cb(nec_rx_context_t *ctx,
                        nec_rx_raw_cb_t   raw_cb);

#ifdef __cplusplus
}
#endif

#endif /* NEC_RX_H */
