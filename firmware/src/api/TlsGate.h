// TlsGate.h — Process-wide TLS session serializer for the badge.
//
// The ESP32-S3 has plenty of total RAM (~6 MB PSRAM + ~250 KB internal),
// but mbedTLS handshakes pull *contiguous internal-DRAM* blocks (16-32 KB
// working buffers, 4-16 KB session state, plus assorted small allocs) and
// the WiFiClientSecure / NetworkClientSecure plumbing on arduino-esp32
// adds another ~10 KB on top of that. Two concurrent TLS sessions on a
// badge that already has the GUI + WiFi RX path + MicroPython runtime
// resident easily exceeds the available internal headroom and trips
// mbedTLS with `-32512 SSL - Memory allocation failed`. Empirically the
// post-OTA → community-registry fetch hand-off was the most reliable
// repro, but the same race is latent any time `badge.http_get`,
// the messaging API, the firmware OTA poll, and the registry refresh
// can interleave.
//
// The fix is the simplest one that actually works: a single recursive
// FreeRTOS mutex that gates every WiFiClientSecure open-site in the
// firmware. While one TLS session is active, every other TLS opener
// blocks on the gate (with a caller-supplied timeout) until the active
// owner releases. Plain http:// paths skip the gate entirely — they
// don't pull big contiguous blocks and there's no benefit to
// serialising them.
//
// The mutex is recursive (rather than binary) so a defensive nested
// HTTPS-from-HTTPS-callback pattern in the same task does not deadlock.
// We don't issue nested HTTPS today, but the recursion bound is free
// insurance for future MicroPython callback paths.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace badge {

// RAII guard around the TLS gate. Construct with the calling site's name
// (used for diagnostics + holder identification) and a timeout in
// milliseconds. The constructor blocks until the gate is acquirable or
// the timeout elapses. `acquired()` reports the result. The destructor
// releases the gate iff it was acquired.
//
// Callers MUST check `acquired()` before opening a WiFiClientSecure /
// NetworkClientSecure. On false, surface the failure to the caller as a
// transport error — never proceed with the open without the gate held.
class TlsSession {
 public:
  explicit TlsSession(const char* owner, uint32_t timeoutMs = 15000);
  ~TlsSession();

  TlsSession(const TlsSession&) = delete;
  TlsSession& operator=(const TlsSession&) = delete;
  TlsSession(TlsSession&&) = delete;
  TlsSession& operator=(TlsSession&&) = delete;

  bool acquired() const { return acquired_; }
  const char* owner() const { return owner_ ? owner_ : "?"; }

 private:
  bool acquired_ = false;
  const char* owner_ = nullptr;
};

// Internal-DRAM headroom predicate. True iff the heap currently has
// enough contiguous internal capacity to support one TLS handshake. The
// thresholds (40 KB free + 26 KB largest block) come from the empirical
// peak observed in the H1 instrumentation during a healthy GitHub /
// jsDelivr handshake on an idle badge — bigger than the steady-state
// resident set of mbedTLS, smaller than the number we'd need on a
// fragmented heap. Single source of truth: also used by the retry loop
// inside the TLS open paths.
bool tlsHeapHeadroomOk();

// Reactive predicate-then-acquire helper. Polls `tlsHeapHeadroomOk()`
// AND `!tlsGateBusy()` every 50 ms until both hold OR `timeoutMs`
// elapses. The 50 ms slice is intentionally short — there is no
// FreeRTOS event source for "heap freed" or "another task gave the
// recursive mutex back", so a short polled wait is the only practical
// way to stay reactive on heap state. Returns true when both conditions
// were satisfied within the budget; false on timeout. NOT a fixed
// "wait-as-fix" sleep — every iteration re-evaluates the conditions.
bool waitForTlsReady(uint32_t timeoutMs);

// Non-blocking probe: is the gate currently held by SOMEONE (possibly
// the calling task itself, since the underlying primitive is
// recursive). Use for instrumentation only. Not a substitute for
// actually acquiring a TlsSession before opening a TLS client.
bool tlsGateBusy();

// PARK this task briefly so the per-CPU IDLE task runs. ESP-IDF's
// `task_wdt` monitors IDLE0 / IDLE1; a priority-X task spinning in
// mbedTLS/crypto or a long HTTP GET without blocking can deny IDLE
// its tick quota for tens of seconds. `yield()` alone is NOT enough
// at configUSE_PREEMPTION — other ready tasks at the same priority
// soak the slice ring but IDLE stays starved unless every higher prio
// work blocks too. Blocking here (~15 ms) deliberately parks before
// chunky TLS/socket work so the IDLE hook can satisfy Task WDT.
//
// Caller must NOT hold ISR locks across this call — it's a deliberate
// preemption point identical in spirit to the existing `delay(…)`
// stubs in Arduino/FreeRTOS wrappers.
void kickIdleTaskWatchdog();

}  // namespace badge
