#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdlib>

#include "esp_heap_caps.h"

namespace BadgeMemory {

static constexpr size_t kInternalFallbackMaxBytes = 4096;

inline void* allocPreferPsram(size_t bytes) {
    if (bytes == 0) return nullptr;
    void* p = ps_malloc(bytes);
    if (!p && bytes > kInternalFallbackMaxBytes) return nullptr;
    return p ? p : malloc(bytes);
}

struct PsramAllocator {
    void* allocate(size_t size) {
        return allocPreferPsram(size);
    }

    void deallocate(void* ptr) {
        free(ptr);
    }

    void* reallocate(void* ptr, size_t newSize) {
        if (!ptr) return allocate(newSize);
        if (newSize == 0) {
            deallocate(ptr);
            return nullptr;
        }
        void* p = heap_caps_realloc(
            ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p && newSize > kInternalFallbackMaxBytes) return nullptr;
        return p ? p : realloc(ptr, newSize);
    }
};

using PsramJsonDocument = BasicJsonDocument<PsramAllocator>;

}  // namespace BadgeMemory
