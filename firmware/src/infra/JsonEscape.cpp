#include "JsonEscape.h"

#include <stdint.h>
#include <string.h>

void jsonEscapeString(const char* src, char* out, size_t outCap) {
    if (!out || outCap == 0) return;
    out[0] = '\0';
    if (!src) return;

    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < outCap; i++) {
        const char c = src[i];
        const char* esc = nullptr;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default: break;
        }
        if (esc) {
            if (o + strlen(esc) >= outCap) break;
            while (*esc) out[o++] = *esc++;
        } else if ((uint8_t)c >= 0x20) {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}
