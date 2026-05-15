// User-tweaked main-menu ordering, persisted in NVS.
//
// Each entry maps a menu-item label (the visible string, e.g. "BOOP",
// "MATRIX APPS", or a dynamic-app title) to a signed int16 sort key.
// Lower keys render first; ties fall back to the order in which the
// rebuilder placed the items (see GUI.cpp::rebuildMainMenuFromRegistry).
//
// Storage uses an FNV-1a hash of the label as the NVS key (raw labels
// can exceed the 15-char NVS key limit). Collisions are extremely
// unlikely for the small label set the menu carries; on collision the
// last writer wins, which is acceptable since the user can always
// reorder again from the settings screen.
//
// The namespace is "menu_order" (NVS namespaces cap at 15 chars).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace MenuOrderStore {

// Sentinel returned when no override exists for the given label. Pick a
// value outside the realistic order range so callers can branch on it.
constexpr int16_t kNoOverride = INT16_MIN;

// Lookup an override for `label`. Returns kNoOverride when unset.
int16_t lookup(const char* label);

// Persist (or update) an override for `label`.
void put(const char* label, int16_t order);

// Forget the override for `label`, if any.
void erase(const char* label);

// Wipe every override. Used by "Reset menu order" in settings.
void clearAll();

// Number of overrides currently stored. Cheap; no allocation.
size_t count();

}  // namespace MenuOrderStore
