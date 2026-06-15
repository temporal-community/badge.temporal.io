#include "OTAHttp.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp32-hal-cpu.h"

#include "../api/TlsGate.h"
#include "../api/WiFiService.h"
#include "../infra/HeapDiag.h"
#include "../infra/PsramBodySink.h"

namespace ota {

ThroughputBoost::ThroughputBoost() {
  prevSleep_ = WiFi.getSleep();
  prevCpuMhz_ = getCpuFrequencyMhz();
  WiFi.setSleep(false);
  if (prevCpuMhz_ < 240) setCpuFrequencyMhz(240);
}

ThroughputBoost::~ThroughputBoost() {
  if (getCpuFrequencyMhz() != prevCpuMhz_ && prevCpuMhz_ != 0) {
    setCpuFrequencyMhz(prevCpuMhz_);
  }
  WiFi.setSleep(prevSleep_);
}

namespace {

bool isHttps(const char* url) {
  return url && std::strncmp(url, "https://", 8) == 0;
}

bool isHttp(const char* url) {
  return url && (std::strncmp(url, "http://", 7) == 0 || isHttps(url));
}

// Configure common HTTPClient options. Caller must already have called
// http.begin().
void applyCommonOptions(HTTPClient& http, uint32_t timeoutMs) {
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", kUserAgent);
  http.addHeader("Accept", "*/*");
  // The ESP32 HTTPClient does not transparently decompress responses,
  // so explicitly ask for raw bytes. Cloudflare-fronted hosts (incl.
  // jsDelivr) otherwise return gzip/brotli to clients that don't pin
  // identity, which then fails JSON parsing with "InvalidInput".
  http.addHeader("Accept-Encoding", "identity");
}

void applyCommonOptionsNoRedirect(HTTPClient& http, uint32_t timeoutMs) {
  applyCommonOptions(http, timeoutMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
}
}  // namespace

HttpResult getJson(const char* url, char** outBuf, size_t* outLen,
                   size_t maxBytes, uint32_t timeoutMs) {
  static char sError[80];
  sError[0] = '\0';
  HttpResult result{0, 0, false, sError};
  if (outBuf) *outBuf = nullptr;
  if (outLen) *outLen = 0;

  if (!isHttp(url)) {
    std::snprintf(sError, sizeof(sError), "url must start with http(s)://");
    return result;
  }
  if (!wifiService.connect()) {
    std::snprintf(sError, sizeof(sError), "wifi unavailable");
    return result;
  }

  const bool https = isHttps(url);
  String urlStr(url);

  // Hold the process-wide TLS gate for the whole getJson() call when
  // talking to an HTTPS endpoint, including across any internal retry
  // attempts so mbedTLS can't interleave with another module's
  // handshake mid-flight. Plain http:// skips the gate entirely — no
  // big contiguous blocks, nothing to serialise.
  badge::TlsSession tls("OTAHttp::getJson", https ? timeoutMs : 0);
  if (https && !tls.acquired()) {
    std::snprintf(sError, sizeof(sError), "tls slot busy");
    wifiService.noteRequestFailed();
    return result;
  }

  // Bounded retry on alloc-class failures inside the same TlsSession.
  // The inner loop is heap-driven, not time-driven: we tear down the
  // HTTPClient + WiFiClientSecure (returning their internal mbedTLS /
  // lwIP allocations to the heap), then poll `tlsHeapHeadroomOk()` in
  // 50 ms slices for up to 1500 ms before reopening. The retry budget
  // (`kMaxAttempts`) is a hard upper bound; we also bail early on a
  // "stalled" heap (no growth in `largest free block` between attempts)
  // so a permanently fragmented heap doesn't burn the full budget.
  constexpr int kMaxAttempts = 3;
  constexpr uint32_t kHeapRecoveryWindowMs = 1500;
  size_t prevLargestInternal = 0;

  // Per-attempt handshake budget. mbedTLS' arduino-esp32 default is
  // 30 s, which is longer than the IDLE-task watchdog (5 s) AND longer
  // than our own getJson retry budget — a stuck handshake on a slow /
  // half-broken AP would otherwise starve IDLE0 on the worker's core
  // and trip the TASK_WDT before we'd ever see a clean failure to
  // retry on. Clamp to 4 s so any one handshake stays well under WDT.
  constexpr uint32_t kHandshakeTimeoutSec = 4;

  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    if (https && attempt > 1) {
      badge::kickIdleTaskWatchdog();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (https && attempt == 1) {
      HeapDiag::printSummary("getJson pre-TLS");
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    bool began = false;
    if (https) {
      secure.setInsecure();
      secure.setHandshakeTimeout(kHandshakeTimeoutSec);
      began = http.begin(secure, urlStr);
    } else {
      began = http.begin(plain, urlStr);
    }
    if (!began) {
      std::snprintf(sError, sizeof(sError), "http begin failed");
      wifiService.noteRequestFailed();
      return result;
    }
    applyCommonOptions(http, timeoutMs);

    if (https) {
      badge::kickIdleTaskWatchdog();
    }

    const int code = http.GET();
    result.httpCode = code;
    if (code <= 0) {
      // Plain HTTP and final-attempt failures are real transport
      // errors — surface immediately. Only HTTPS gets the alloc-class
      // retry: the empirical failure mode is mbedTLS -32512 (memory
      // allocation failed), which `HTTPClient::GET()` reports as a
      // generic `code <= 0`. Retrying inside the same gate, after
      // mbedTLS has released its allocations and the heap has had a
      // chance to recover, is exactly what fixes Bug A.
      if (!https || attempt == kMaxAttempts) {
        std::snprintf(sError, sizeof(sError),
                      "http error %d (%s)", code,
                      HTTPClient::errorToString(code).c_str());
        http.end();
        wifiService.noteRequestFailed();
        return result;
      }

      // Tear down the HTTPClient before waiting so mbedTLS releases its
      // internal allocations back to the heap. The local secure /
      // plain are stack-scoped to this iteration and will destruct at
      // the loop body's bottom.
      http.end();

      const size_t largestNow = heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

      // Stall detector: if the largest internal block hasn't grown
      // between attempts, the heap is permanently fragmented under
      // some long-resident allocation. No amount of waiting will help;
      // surface the original error rather than burn the rest of the
      // retry budget.
      if (attempt > 1 && largestNow <= prevLargestInternal) {
        std::snprintf(sError, sizeof(sError),
                      "tls retry stall (heap=%u)",
                      static_cast<unsigned>(largestNow));
        wifiService.noteRequestFailed();
        return result;
      }
      prevLargestInternal = largestNow;

      // Reactive wait on heap state — every iteration re-evaluates
      // `tlsHeapHeadroomOk()`. Bail as soon as the predicate flips true
      // OR the deadline expires; we then immediately retry the GET.
      const uint32_t deadline = millis() + kHeapRecoveryWindowMs;
      while (!badge::tlsHeapHeadroomOk()) {
        if ((int32_t)(millis() - deadline) >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      // Even when headroom is already OK we must yield — back-to-back
      // mbedTLS handshakes on the same idle-TWDT core otherwise starve
      // IDLE0 for longer than one TWDT quantum (same bug class as stacked
      // registry + GitHub Releases checks).
      if (https) {
        badge::kickIdleTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;  // next attempt within the same TlsSession
    }

    if (code != 200) {
      // Drain a bounded amount of error body without String/getString().
      constexpr size_t kErrBodyCap = 512;
      BadgeMemory::PsramBodySink errSink(kErrBodyCap);
      if (errSink.ok()) {
        (void)http.writeToStream(&errSink);
      }
      Serial.printf("[ota-http] %d body[%u]: %.*s\n", code,
                    (unsigned)errSink.length(),
                    errSink.length() > 200 ? 200 : (int)errSink.length(),
                    errSink.c_str());
      std::snprintf(sError, sizeof(sError), "http status %d", code);
      http.end();
      wifiService.noteRequestFailed();
      return result;
    }

    const int contentLen = http.getSize();
    if (contentLen > 0 && static_cast<size_t>(contentLen) > maxBytes) {
      std::snprintf(sError, sizeof(sError),
                    "response too large (%d > %u)",
                    contentLen, (unsigned)maxBytes);
      http.end();
      wifiService.noteRequestFailed();
      return result;
    }

    // Stream via HTTPClient::writeToStream so chunked encoding and
    // Content-Length=-1 are decoded by the library (same as getString),
    // but the body buffer is one PSRAM-backed allocation — no
    // StreamString reserve on internal heap.
    BadgeMemory::PsramBodySink sink(maxBytes, https ? 4096u : 0u);
    if (!sink.ok()) {
      std::snprintf(sError, sizeof(sError), "out of memory");
      http.end();
      wifiService.noteRequestFailed();
      return result;
    }
    const int wr = http.writeToStream(&sink);
    if (wr < 0) {
      std::snprintf(sError, sizeof(sError), "http read %d (%s)", wr,
                    HTTPClient::errorToString(wr).c_str());
      http.end();
      wifiService.noteRequestFailed();
      return result;
    }
    const size_t pos = sink.length();
    if (https) {
      badge::kickIdleTaskWatchdog();
    }

    // Short-body detection. When the server declared Content-Length, the
    // sink length must equal it; anything else is a silent transport
    // truncation that ArduinoJson would otherwise happily report as a
    // successful parse on the truncated bytes (Bug B class-1).
    if (contentLen > 0 && pos != static_cast<size_t>(contentLen)) {
      std::snprintf(sError, sizeof(sError),
                    "short body %u/%d",
                    static_cast<unsigned>(pos), contentLen);
      http.end();
      wifiService.noteRequestFailed();
      return result;
    }

    char* buf = sink.release();
    http.end();
    wifiService.noteRequestOk();

    if (outBuf) *outBuf = buf;
    else std::free(buf);
    if (outLen) *outLen = pos;
    result.bytesRead = pos;
    result.ok = true;
    result.error = "";
    return result;
  }

  // Defensive: the loop returns on every path, but if a future edit
  // breaks that invariant, fail closed rather than silently succeed
  // with an empty body.
  if (sError[0] == '\0') {
    std::snprintf(sError, sizeof(sError), "tls retry exhausted");
  }
  wifiService.noteRequestFailed();
  return result;
}

bool resolveRedirect(const char* url, char* outUrl, size_t outUrlLen,
                     uint32_t timeoutMs) {
  if (outUrl && outUrlLen > 0) outUrl[0] = '\0';
  if (!outUrl || outUrlLen == 0 || !isHttp(url)) {
    return false;
  }
  if (!wifiService.connect()) {
    wifiService.noteRequestFailed();
    return false;
  }

  const bool https = isHttps(url);
  badge::TlsSession tls("OTAHttp::resolveRedirect", https ? timeoutMs : 0);
  if (https && !tls.acquired()) {
    Serial.printf("[ota-http] resolveRedirect: tls slot busy (%s)\n", url);
    wifiService.noteRequestFailed();
    return false;
  }

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  bool began = false;
  String urlStr(url);
  if (https) {
    HeapDiag::printSummary("resolveRedirect pre-TLS");
    secure.setInsecure();
    secure.setHandshakeTimeout(4);
    began = http.begin(secure, urlStr);
  } else {
    began = http.begin(plain, urlStr);
  }
  if (!began) {
    wifiService.noteRequestFailed();
    return false;
  }
  applyCommonOptionsNoRedirect(http, timeoutMs);
  http.addHeader("Range", "bytes=0-0");

  if (https) {
    badge::kickIdleTaskWatchdog();
  }
  const int code = http.GET();
  const String location = http.getLocation();
  http.end();

  if (code < 300 || code >= 400 || location.length() == 0) {
    wifiService.noteRequestFailed();
    return false;
  }
  if (static_cast<size_t>(location.length()) >= outUrlLen) {
    Serial.printf("[ota-http] redirect too long: %u >= %u\n",
                  (unsigned)location.length(), (unsigned)outUrlLen);
    wifiService.noteRequestFailed();
    return false;
  }
  std::strncpy(outUrl, location.c_str(), outUrlLen - 1);
  outUrl[outUrlLen - 1] = '\0';
  wifiService.noteRequestOk();
  return true;
}

// ── Stream ─────────────────────────────────────────────────────────────────

Stream::Stream() = default;

Stream::~Stream() { close(); }

bool Stream::open(const char* url, uint32_t timeoutMs, size_t rangeStart) {
  close();
  lastError_[0] = '\0';
  contentLength_ = 0;
  httpCode_ = 0;

  if (!isHttp(url)) {
    std::snprintf(lastError_, sizeof(lastError_),
                  "url must start with http(s)://");
    return false;
  }
  if (!wifiService.connect()) {
    std::snprintf(lastError_, sizeof(lastError_), "wifi unavailable");
    return false;
  }

  const bool https = isHttps(url);

  // Acquire the process-wide TLS gate for the whole lifetime of the
  // stream. Held until close() (which is called from the destructor and
  // from open()'s internal `close()` on every retry path) so the gate
  // is freed as soon as the underlying HTTPClient is torn down.
  if (https) {
    tls_ = new badge::TlsSession("OTAHttp::Stream::open", timeoutMs);
    if (!tls_->acquired()) {
      std::snprintf(lastError_, sizeof(lastError_), "tls slot busy");
      delete tls_;
      tls_ = nullptr;
      wifiService.noteRequestFailed();
      return false;
    }
  }

  // Bounded retry on alloc-class failures inside the same TlsSession.
  // Stream::open is the entry point for big asset / firmware downloads,
  // so a single transient mbedTLS alloc fail at handshake time
  // (the dominant Bug A signature) shouldn't propagate out as a hard
  // open failure — the outer install retry loop will close+reopen with
  // a fresh Range header anyway, but that path is more expensive
  // (FATFS .tmp truncate, fresh SHA context). Internal retry catches
  // the cheap case before it reaches the outer loop.
  constexpr int kMaxAttempts = 3;
  constexpr uint32_t kHeapRecoveryWindowMs = 1500;
  size_t prevLargestInternal = 0;

  String urlStr(url);
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    if (https && attempt > 1) {
      badge::kickIdleTaskWatchdog();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (https && attempt == 1) {
      HeapDiag::printSummary("Stream::open pre-TLS");
    }
    http_ = new HTTPClient();
    bool began = false;
    if (https) {
      auto* sec = new WiFiClientSecure();
      sec->setInsecure();
      // Per-attempt handshake budget. arduino-esp32's default is 30 s
      // (longer than the IDLE-task watchdog, 5 s). The body-stream
      // window is governed by `timeoutMs` elsewhere; the handshake
      // itself should never outlive the WDT — a stuck handshake on
      // a half-broken AP starves IDLE0 on this worker's core and
      // tripped the TASK_WDT in observation. 4 s is comfortably
      // under WDT and the retry loop below covers the transient
      // alloc-class failure that the gate is for.
      sec->setHandshakeTimeout(4);
      secure_ = sec;
      began = http_->begin(*sec, urlStr);
    } else {
      auto* p = new WiFiClient();
      plain_ = p;
      began = http_->begin(*p, urlStr);
    }
    if (!began) {
      std::snprintf(lastError_, sizeof(lastError_), "http begin failed");
      close();
      wifiService.noteRequestFailed();
      return false;
    }
    applyCommonOptions(*http_, timeoutMs);
    if (rangeStart > 0) {
      char rangeHdr[40];
      std::snprintf(rangeHdr, sizeof(rangeHdr), "bytes=%u-",
                    static_cast<unsigned>(rangeStart));
      http_->addHeader("Range", rangeHdr);
    }

    if (https) {
      badge::kickIdleTaskWatchdog();
    }
    const int code = http_->GET();
    httpCode_ = code;
    if (code <= 0) {
      // Plain HTTP and final-attempt failures: surface as before.
      // HTTPS gets the alloc-class retry — same semantics as getJson.
      if (!https || attempt == kMaxAttempts) {
        std::snprintf(lastError_, sizeof(lastError_),
                      "http error %d (%s)", code,
                      HTTPClient::errorToString(code).c_str());
        // Tear down the HTTP client + plain/secure but DO NOT release
        // the TLS gate yet — close() handles both.
        if (http_) { http_->end(); delete http_; http_ = nullptr; }
        if (plain_) { delete plain_; plain_ = nullptr; }
        if (secure_) { delete secure_; secure_ = nullptr; }
        body_ = nullptr;
        if (tls_) { delete tls_; tls_ = nullptr; }
        wifiService.noteRequestFailed();
        return false;
      }

      // Tear down the HTTP client + secure so mbedTLS releases its
      // internal allocations back to the heap. Keep the TlsSession.
      http_->end();
      delete http_;
      http_ = nullptr;
      if (secure_) { delete secure_; secure_ = nullptr; }
      body_ = nullptr;

      const size_t largestNow = heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (attempt > 1 && largestNow <= prevLargestInternal) {
        std::snprintf(lastError_, sizeof(lastError_),
                      "tls retry stall (heap=%u)",
                      static_cast<unsigned>(largestNow));
        if (tls_) { delete tls_; tls_ = nullptr; }
        wifiService.noteRequestFailed();
        return false;
      }
      prevLargestInternal = largestNow;

      const uint32_t deadline = millis() + kHeapRecoveryWindowMs;
      while (!badge::tlsHeapHeadroomOk()) {
        if ((int32_t)(millis() - deadline) >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (https) {
        badge::kickIdleTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }
    // 200 OK and 206 Partial Content are both acceptable. Anything else
    // (including 416 Range Not Satisfiable) is a hard fail — the caller
    // should restart from offset 0 by reopening with rangeStart=0.
    if (code != 200 && code != 206) {
      std::snprintf(lastError_, sizeof(lastError_), "http status %d", code);
      close();
      wifiService.noteRequestFailed();
      return false;
    }

    const int len = http_->getSize();
    contentLength_ = (len > 0) ? static_cast<size_t>(len) : 0;
    body_ = http_->getStreamPtr();
    return true;
  }

  // Defensive: every loop path returns. Fail closed on any future
  // edit that breaks that invariant.
  std::snprintf(lastError_, sizeof(lastError_), "tls retry exhausted");
  close();
  wifiService.noteRequestFailed();
  return false;
}

bool Stream::connected() const {
  if (!http_) return false;
  return http_->connected();
}

int Stream::read(uint8_t* buf, size_t len) {
  if (!body_ || !buf || len == 0) return 0;
  // Per-read patience window. 10 s is generous enough that a brief
  // mid-frame WiFi glitch (e.g. AP roaming, neighbouring badge
  // hammering 2.4 GHz with IR/USB harmonics) doesn't immediately
  // collapse the chunk; short enough that a truly dead socket gets
  // surfaced to the caller's outer retry loop within a sensible
  // bound. The caller (AssetRegistry installer) wraps reads in a
  // resume-with-Range retry, so a surfaced timeout here is recoverable
  // rather than fatal.
  const uint32_t kReadWindow = 10000;
  uint32_t deadline = millis() + kReadWindow;
  size_t total = 0;
  while (total < len) {
    int avail = body_->available();
    if (avail <= 0) {
      if ((int32_t)(millis() - deadline) > 0) break;
      if (!http_->connected() && body_->available() <= 0) break;
      delay(2);
      continue;
    }
    int want = static_cast<int>(len - total);
    if (avail < want) want = avail;
    int got = body_->readBytes(buf + total, want);
    if (got <= 0) break;
    total += got;
    deadline = millis() + kReadWindow;
  }
  return static_cast<int>(total);
}

void Stream::close() {
  if (http_) {
    http_->end();
    delete http_;
    http_ = nullptr;
  }
  if (plain_) {
    delete plain_;
    plain_ = nullptr;
  }
  if (secure_) {
    delete secure_;
    secure_ = nullptr;
  }
  body_ = nullptr;
  // Release the TLS gate AFTER the HTTPClient + WiFiClientSecure are
  // fully torn down. mbedTLS holds onto its internal allocations until
  // the secure client is destroyed; releasing the gate before that
  // would let another opener race ahead while the heap is still in
  // its peak resident-set state.
  if (tls_) {
    delete tls_;
    tls_ = nullptr;
  }
}

}  // namespace ota
