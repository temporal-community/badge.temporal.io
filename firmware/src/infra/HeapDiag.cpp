#include "HeapDiag.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <multi_heap.h>

namespace HeapDiag {

namespace {

constexpr uint32_t kIntCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

}  // namespace

#if BADGE_HEAP_DIAG_VERBOSE

void printSummary(const char* tag) {
  const size_t intFree = heap_caps_get_free_size(kIntCaps);
  const size_t intLgst = heap_caps_get_largest_free_block(kIntCaps);
  const size_t intMin = heap_caps_get_minimum_free_size(kIntCaps);
  const size_t psrFree = heap_caps_get_free_size(kPsramCaps);
  const size_t psrLgst = heap_caps_get_largest_free_block(kPsramCaps);

  Serial.printf("[heap] %s — int: %u free, %u lgst, %u min | "
                "psram: %u free, %u lgst\n",
                tag ? tag : "?",
                (unsigned)intFree, (unsigned)intLgst, (unsigned)intMin,
                (unsigned)psrFree, (unsigned)psrLgst);
}

void printRegionMap(const char* tag) {
  Serial.printf("\n[heap] === %s — region map ===\n", tag ? tag : "?");

  Serial.println("[heap] --- INTERNAL (MALLOC_CAP_INTERNAL | 8BIT) ---");
  heap_caps_print_heap_info(kIntCaps);

  Serial.println("[heap] --- PSRAM (MALLOC_CAP_SPIRAM | 8BIT) ---");
  heap_caps_print_heap_info(kPsramCaps);

  Serial.printf("[heap] === end %s ===\n\n", tag ? tag : "?");
}

void printSnapshot(const char* tag) {
  multi_heap_info_t intInfo, psrInfo;
  heap_caps_get_info(&intInfo, kIntCaps);
  heap_caps_get_info(&psrInfo, kPsramCaps);

  const size_t intMin = heap_caps_get_minimum_free_size(kIntCaps);

  // TLS readiness mirrors TlsGate thresholds (40 KB free, 26 KB largest)
  const bool tlsOk = (intInfo.total_free_bytes >= 40 * 1024) &&
                     (intInfo.largest_free_block >= 26 * 1024);

  Serial.printf("[heap] ┌─ %s ────────────────────────────\n",
                tag ? tag : "?");
  Serial.printf("[heap] │ INTERNAL  free=%6u  largest=%6u  min_ever=%6u\n",
                (unsigned)intInfo.total_free_bytes,
                (unsigned)intInfo.largest_free_block,
                (unsigned)intMin);
  Serial.printf("[heap] │           alloc=%6u  blocks=%u\n",
                (unsigned)intInfo.total_allocated_bytes,
                (unsigned)intInfo.allocated_blocks);
  Serial.printf("[heap] │ PSRAM     free=%7u  largest=%7u\n",
                (unsigned)psrInfo.total_free_bytes,
                (unsigned)psrInfo.largest_free_block);
  Serial.printf("[heap] │           alloc=%7u  blocks=%u\n",
                (unsigned)psrInfo.total_allocated_bytes,
                (unsigned)psrInfo.allocated_blocks);
  Serial.printf("[heap] │ TLS-ready=%s  (need ≥40K free + ≥26K block)\n",
                tlsOk ? "YES" : "NO");
  Serial.printf("[heap] └──────────────────────────────────\n");
}

#endif

void printAllocFailure(const char* context, size_t requested_bytes) {
  const size_t intFree =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t intLgst = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t intMin =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t psrFree = heap_caps_get_free_size(kPsramCaps);
  const size_t psrLgst = heap_caps_get_largest_free_block(kPsramCaps);

  Serial.printf(
      "[heap] ALLOC FAIL \"%s\" want=%u | int: free=%u lgst=%u min_ever=%u | "
      "psram: free=%u lgst=%u\n",
      context ? context : "?",
      (unsigned)requested_bytes, (unsigned)intFree, (unsigned)intLgst,
      (unsigned)intMin, (unsigned)psrFree, (unsigned)psrLgst);

#if BADGE_HEAP_DIAG_VERBOSE
  Serial.println("[heap] --- ALLOC FAIL — region map (INTERNAL) ---");
  heap_caps_print_heap_info(kIntCaps);
  Serial.println("[heap] --- ALLOC FAIL — region map (PSRAM) ---");
  heap_caps_print_heap_info(kPsramCaps);
#endif
}

}  // namespace HeapDiag
