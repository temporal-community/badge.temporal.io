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

#include "esp32-hal-cpu.h"

#include "../api/WiFiService.h"
#include "../infra/PsramAllocator.h"

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

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  bool began = false;
  String urlStr(url);
  if (isHttps(url)) {
    secure.setInsecure();
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

  const int code = http.GET();
  result.httpCode = code;
  if (code <= 0) {
    std::snprintf(sError, sizeof(sError),
                  "http error %d (%s)", code,
                  HTTPClient::errorToString(code).c_str());
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }
  if (code != 200) {
    // Drain a bit of the response body so the caller can see what the
    // server actually said (e.g. raw.githubusercontent.com 400s
    // sometimes include "Header Or Cookie Too Large" or similar — the
    // bare status code alone tells us nothing useful).
    String body = http.getString();
    Serial.printf("[ota-http] %d body[%u]: %.*s\n", code,
                  (unsigned)body.length(),
                  body.length() > 200 ? 200 : (int)body.length(),
                  body.c_str());
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

  // Use HTTPClient::getString() so the library handles chunked
  // transfer-encoding, content-length=-1, and connection-close edges
  // for us. Reading raw from getStreamPtr() bypassed the chunked
  // decoder, which on Cloudflare-fronted hosts (jsDelivr) interleaved
  // chunk-size markers (`28d\r\n`...`\r\n0\r\n`) into the buffer and
  // tripped ArduinoJson with InvalidInput.
  String s = http.getString();
  if (s.length() > maxBytes) {
    std::snprintf(sError, sizeof(sError),
                  "response too large (%u > %u)",
                  (unsigned)s.length(), (unsigned)maxBytes);
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }
  // Body lands in PSRAM when available so a 50-100 KB JSON response
  // doesn't gnaw at internal heap. allocPreferPsram falls back to
  // internal malloc for tiny (<4 KB) buffers and for boards without
  // PSRAM. Caller frees with std::free(), which is safe for both
  // heap regions on ESP-IDF.
  size_t pos = s.length();
  char* buf = static_cast<char*>(BadgeMemory::allocPreferPsram(pos + 1));
  if (!buf) {
    std::snprintf(sError, sizeof(sError), "out of memory");
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }
  std::memcpy(buf, s.c_str(), pos);
  buf[pos] = '\0';
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

  http_ = new HTTPClient();
  bool began = false;
  String urlStr(url);
  if (isHttps(url)) {
    auto* sec = new WiFiClientSecure();
    sec->setInsecure();
    // Cap the TLS handshake at the per-request timeout (in seconds).
    // The arduino-esp32 default is 30s which is fine, but tying it to
    // the caller's timeoutMs keeps a stuck handshake from outliving
    // the rest of the budget.
    sec->setHandshakeTimeout(timeoutMs / 1000);
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

  const int code = http_->GET();
  httpCode_ = code;
  if (code <= 0) {
    std::snprintf(lastError_, sizeof(lastError_),
                  "http error %d (%s)", code,
                  HTTPClient::errorToString(code).c_str());
    close();
    wifiService.noteRequestFailed();
    return false;
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
}

}  // namespace ota
