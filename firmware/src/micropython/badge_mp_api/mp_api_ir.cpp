#include <Arduino.h>
#include <cstring>

#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../../ble/BadgeBeaconAdv.h"
#include "../../ble/BleBeaconScanner.h"
#endif
#include "../../ir/BadgeIR.h"

#include "temporalbadge_runtime.h"

// ── IR ──────────────────────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_ir_send(int addr, int cmd)
{
    return irSendRaw((uint8_t)addr, (uint8_t)cmd);
}

extern "C" void temporalbadge_runtime_ir_start(void)
{
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForIr(true);
    BleBeaconScanner::stopScan();
    BleBeaconScanner::clearScanCache();
    for (int i = 0; i < 25 && BadgeBeaconAdv::isBroadcasting(); ++i)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif
    pythonIrListening = true;
    // Wait up to ~500 ms for irTask on Core 0 to bring the RMT hardware up
    // (it polls every 50 ms). Without this, the first ir_send* call after
    // ir_start() would return EPERM if the caller doesn't sleep first.
    for (int i = 0; i < 100; ++i)
    {
        if (irHwIsUp())
            break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    // Drop any frames that lingered from a prior session or the Boop screen
    // so the first ir_read*() after ir_start() never returns stale data.
    irDrainPythonRx();
}

extern "C" void temporalbadge_runtime_ir_stop(void)
{
    pythonIrListening = false;
    irDrainPythonRx();
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForIr(false);
#endif
}

extern "C" int temporalbadge_runtime_ir_available(void)
{
    portENTER_CRITICAL(&irPythonQueueMux);
    int avail = (irPythonQueueHead != irPythonQueueTail) ? 1 : 0;
    portEXIT_CRITICAL(&irPythonQueueMux);
    return avail;
}

extern "C" int temporalbadge_runtime_ir_read(int *addr_out, int *cmd_out)
{
    portENTER_CRITICAL(&irPythonQueueMux);
    if (irPythonQueueHead == irPythonQueueTail)
    {
        portEXIT_CRITICAL(&irPythonQueueMux);
        return -1;
    }
    IrPythonFrame f = irPythonQueue[irPythonQueueHead];
    irPythonQueueHead = (irPythonQueueHead + 1) % IR_PYTHON_QUEUE_SIZE;
    portEXIT_CRITICAL(&irPythonQueueMux);
    if (addr_out)
        *addr_out = f.addr;
    if (cmd_out)
        *cmd_out = f.cmd;
    return 0;
}

// ── IR multi-word ───────────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_ir_send_words(const uint32_t *words,
                                                   size_t count)
{
    return irSendWords(words, count);
}

extern "C" int temporalbadge_runtime_ir_read_words(uint32_t *out,
                                                   size_t max_words,
                                                   size_t *count_out)
{
    return irReadWords(out, max_words, count_out);
}

extern "C" void temporalbadge_runtime_ir_flush(void)
{
    irDrainPythonRx();
}

extern "C" int temporalbadge_runtime_ir_tx_power(int percent)
{
    if (percent < 0)
    {
        return irGetTxPower();
    }
    return (irSetTxPower(percent) == 0) ? irGetTxPower() : -1;
}

// ── IR Playground (consumer NEC + raw symbols) ──────────────────────────────

extern "C" int temporalbadge_runtime_ir_set_mode(const char *name)
{
    if (name == nullptr) return -1;
    int mode;
    if (strcmp(name, "badge") == 0)        mode = IR_MODE_BADGE_MW;
    else if (strcmp(name, "nec") == 0)     mode = IR_MODE_CONSUMER_NEC;
    else if (strcmp(name, "raw") == 0)     mode = IR_MODE_RAW_SYMBOL;
    else return -1;
    return irSetMode(mode);
}

extern "C" const char *temporalbadge_runtime_ir_get_mode(void)
{
    switch (irGetMode())
    {
        case IR_MODE_CONSUMER_NEC: return "nec";
        case IR_MODE_RAW_SYMBOL:   return "raw";
        default:                   return "badge";
    }
}

extern "C" int temporalbadge_runtime_ir_nec_send(int addr,
                                                  int cmd,
                                                  int repeats)
{
    if (repeats < 0) repeats = 0;
    if (repeats > 64) repeats = 64;
    return irNecSend((uint8_t)(addr & 0xFF),
                      (uint8_t)(cmd & 0xFF),
                      (uint8_t)repeats);
}

extern "C" int temporalbadge_runtime_ir_nec_read(int *addr_out,
                                                  int *cmd_out,
                                                  int *repeat_out)
{
    uint8_t a = 0, c = 0, r = 0;
    int rc = irNecRead(&a, &c, &r);
    if (rc != 0) return rc;
    if (addr_out)   *addr_out   = a;
    if (cmd_out)    *cmd_out    = c;
    if (repeat_out) *repeat_out = r;
    return 0;
}

extern "C" int temporalbadge_runtime_ir_raw_capture(uint16_t *out_pairs,
                                                     size_t   max_pairs)
{
    return irRawCapture(out_pairs, max_pairs);
}

extern "C" int temporalbadge_runtime_ir_raw_send(const uint16_t *pairs,
                                                  size_t          pair_count,
                                                  uint32_t        carrier_hz)
{
    return irRawSend(pairs, pair_count, carrier_hz);
}

extern "C" uint32_t temporalbadge_runtime_ir_ms_since_tx(void)
{
    return irMsSinceTx();
}

extern "C" uint32_t temporalbadge_runtime_ir_ms_since_rx(void)
{
    return irMsSinceRx();
}
