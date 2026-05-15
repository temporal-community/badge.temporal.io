#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <string.h>

#include "../../api/WiFiService.h"

#include "temporalbadge_runtime.h"

namespace {

constexpr size_t kHttpResponseMax = 8192;
constexpr uint32_t kHttpTimeoutMs = 15000;

char* s_http_response = nullptr;

const char* setResponse(const char* text) {
    if (!text) text = "";
    free(s_http_response);
    const size_t len = strlen(text);
    s_http_response = static_cast<char*>(malloc(len + 1));
    if (!s_http_response) return nullptr;
    memcpy(s_http_response, text, len + 1);
    return s_http_response;
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

    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    const bool tls = isHttps(url);
    if (tls) {
        secureClient.setInsecure();
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

    int code = -1;
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

    String payload = http.getString();
    http.end();
    wifiService.noteRequestOk();

    if (payload.length() > kHttpResponseMax) {
        return setError("response too large");
    }
    return setResponse(payload.c_str());
}

}  // namespace

extern "C" const char* temporalbadge_runtime_http_get(const char* url) {
    return request("GET", url, nullptr);
}

extern "C" const char* temporalbadge_runtime_http_post(const char* url,
                                                       const char* body) {
    return request("POST", url, body);
}
