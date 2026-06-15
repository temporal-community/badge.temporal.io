#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdlib>

#include "HeapDiag.h"
#include "esp_heap_caps.h"

namespace BadgeMemory {

static constexpr size_t kInternalFallbackMaxBytes = 4096;

inline void* allocPreferPsram(size_t bytes) {
    if (bytes == 0) return nullptr;
    void* p = ps_malloc(bytes);
    if (p) return p;
    if (bytes > kInternalFallbackMaxBytes) {
        HeapDiag::printAllocFailure("BadgeMemory::allocPreferPsram(ps_malloc)",
                                    bytes);
        return nullptr;
    }
    p = malloc(bytes);
    if (!p) {
        HeapDiag::printAllocFailure("BadgeMemory::allocPreferPsram(internal)",
                                    bytes);
    }
    return p;
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
        if (p) return p;
        if (newSize > kInternalFallbackMaxBytes) {
            HeapDiag::printAllocFailure("BadgeMemory::realloc(spi_ram)", newSize);
            return nullptr;
        }
        p = realloc(ptr, newSize);
        if (!p) {
            HeapDiag::printAllocFailure("BadgeMemory::realloc(internal)",
                                        newSize);
        }
        return p;
    }
};

using PsramJsonDocument = BasicJsonDocument<PsramAllocator>;

}  // namespace BadgeMemory
