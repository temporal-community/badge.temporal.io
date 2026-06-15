#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef BADGE_HEAP_DIAG_VERBOSE
#define BADGE_HEAP_DIAG_VERBOSE 0
#endif

namespace HeapDiag {

#if BADGE_HEAP_DIAG_VERBOSE
// Verbose diagnostics (boot milestones, pre-TLS, full region maps).
// Enable with: -DBADGE_HEAP_DIAG_VERBOSE=1 on the compiler command line
// or in platformio.ini build_flags.

// Print a one-line summary: internal free/largest/min-ever + PSRAM free/largest.
void printSummary(const char* tag);

// Print the full ESP-IDF per-region heap info table (internal + PSRAM).
void printRegionMap(const char* tag);

// Multi-line snapshot: internal + PSRAM totals and TLS-ready heuristic.
void printSnapshot(const char* tag);
#else
inline void printSummary(const char*) {}
inline void printRegionMap(const char*) {}
inline void printSnapshot(const char*) {}
#endif

// Always logged when BadgeMemory allocation fails (or other callers opt in).
// Compact one-shot line; does not honor BADGE_HEAP_DIAG_VERBOSE.
void printAllocFailure(const char* context, size_t requested_bytes);

}  // namespace HeapDiag
