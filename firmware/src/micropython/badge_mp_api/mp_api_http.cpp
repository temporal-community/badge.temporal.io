#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <string.h>

#include "../../api/TlsGate.h"
#include "../../api/WiFiService.h"
#include "../../infra/PsramBodySink.h"

#include "temporalbadge_runtime.h"

namespace {

constexpr size_t kHttpResponseMax = 8192;
constexpr uint32_t kHttpTimeoutMs = 15000;

char* s_http_response = nullptr;

const char* setResponse(const char* text) {
    if (!text) text = "";
    const size_t len = strlen(text);
    char* copy = static_cast<char*>(malloc(len + 1));
    if (!copy) {
        free(s_http_response);
        s_http_response = nullptr;
        return "";
    }
    memcpy(copy, text, len + 1);
    free(s_http_response);
    s_http_response = copy;
    return s_http_response;
}

const char* setResponseOwned(char* p) {
    free(s_http_response);
    s_http_response = p;
    return s_http_response ? s_http_response : "";
}

const char* setError(const char* msg) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}",
             msg ? msg : "http error");
    return setResponse(buf);
}

bool isHttps(const char* url) {
    return url && strncmp(url, "https://", 8) == 0;
}

const char* request(const char* method, const char* url, const char* body) {
    if (!url || !url[0]) return setError("missing url");
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return setError("url must start with http:// or https://");
    }
    if (!wifiService.connect()) {
        return setError("wifi not configured or unavailable");
    }

    const bool tls = isHttps(url);

    // Hold the process-wide TLS gate for the whole request when the URL
    // is HTTPS. Python apps share the gate with C-layer callers
    // (BadgeOTA, AssetRegistry, mp_api_http itself), so a `badge.http_get`
    // mid-OTA-check can no longer race mbedTLS's internal heap. Plain
    // http:// skips the gate.
    badge::TlsSession tlsSlot("mp_api_http::request",
                              tls ? kHttpTimeoutMs : 0);
    if (tls && !tlsSlot.acquired()) {
        wifiService.noteRequestFailed();
        return setError("tls slot busy");
    }

    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    if (tls) {
        secureClient.setInsecure();
        secureClient.setHandshakeTimeout(4);
        if (!http.begin(secureClient, url)) {
            wifiService.noteRequestFailed();
            return setError("http begin failed");
        }
    } else if (!http.begin(plainClient, url)) {
        wifiService.noteRequestFailed();
        return setError("http begin failed");
    }

    http.setTimeout(kHttpTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("User-Agent", "TemporalBadge/1.0");
    http.addHeader("Accept-Encoding", "identity");

    int code = -1;
    if (tls) {
        badge::kickIdleTaskWatchdog();
    }
    if (strcmp(method, "POST") == 0) {
        http.addHeader("Content-Type", "application/json");
        const char* payload = body ? body : "";
        code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(payload)),
                         strlen(payload));
    } else {
        code = http.GET();
    }

    if (code <= 0) {
        char err[96];
        snprintf(err, sizeof(err), "http request failed %d", code);
        http.end();
        wifiService.noteRequestFailed();
        return setError(err);
    }

    BadgeMemory::PsramBodySink sink(kHttpResponseMax, tls ? 4096u : 0u);
    if (!sink.ok()) {
        http.end();
        wifiService.noteRequestFailed();
        return setError("out of memory");
    }
    const int wr = http.writeToStream(&sink);
    if (wr < 0) {
        http.end();
        wifiService.noteRequestFailed();
        char err[112];
        snprintf(err, sizeof(err), "http read failed %d (%s)", wr,
                 HTTPClient::errorToString(wr).c_str());
        return setError(err);
    }
    if (tls) {
        badge::kickIdleTaskWatchdog();
    }
    http.end();
    wifiService.noteRequestOk();

    char* owned = sink.release();
    if (!owned) {
        return setError("out of memory");
    }
    return setResponseOwned(owned);
}

}  // namespace

extern "C" const char* temporalbadge_runtime_http_get(const char* url) {
    return request("GET", url, nullptr);
}

extern "C" const char* temporalbadge_runtime_http_post(const char* url,
                                                       const char* body) {
    return request("POST", url, body);
}
