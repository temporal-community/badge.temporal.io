#include "MenuOrderStore.h"

#include <Arduino.h>
#include <nvs.h>
#include <cstdio>
#include <cstring>

// We bypass Arduino's `Preferences` wrapper here because its
// `begin()` calls `log_e("nvs_open failed: NOT_FOUND")` whenever a
// read-only open hits a namespace that doesn't exist yet. On a clean
// badge `lookup()` fires once per menu item during boot, so that
// turns into 20+ scary-looking errors in the serial log. Going
// straight to `nvs_open` lets us treat `ESP_ERR_NVS_NOT_FOUND` as the
// expected "no override saved" path silently.

namespace MenuOrderStore {

namespace {

constexpr const char* kNamespace = "menu_order";

uint32_t fnv1a(const char* s) {
  uint32_t h = 0x811C9DC5u;
  for (; s && *s; s++) {
    h ^= static_cast<uint8_t>(*s);
    h *= 0x01000193u;
  }
  return h;
}

void formatKey(const char* label, char out[16]) {
  std::snprintf(out, 16, "o%08lx", static_cast<unsigned long>(fnv1a(label)));
}

bool openRO(nvs_handle_t* out) {
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, out);
  return err == ESP_OK;
}

bool openRW(nvs_handle_t* out) {
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, out);
  return err == ESP_OK;
}

}  // namespace

int16_t lookup(const char* label) {
  if (!label || !*label) return kNoOverride;
  nvs_handle_t h = 0;
  if (!openRO(&h)) return kNoOverride;
  char key[16];
  formatKey(label, key);
  int16_t v = kNoOverride;
  esp_err_t err = nvs_get_i16(h, key, &v);
  nvs_close(h);
  return (err == ESP_OK) ? v : kNoOverride;
}

void put(const char* label, int16_t order) {
  if (!label || !*label) return;
  nvs_handle_t h = 0;
  if (!openRW(&h)) return;
  char key[16];
  formatKey(label, key);
  nvs_set_i16(h, key, order);
  nvs_commit(h);
  nvs_close(h);
}

void erase(const char* label) {
  if (!label || !*label) return;
  nvs_handle_t h = 0;
  if (!openRW(&h)) return;
  char key[16];
  formatKey(label, key);
  nvs_erase_key(h, key);
  nvs_commit(h);
  nvs_close(h);
}

void clearAll() {
  nvs_handle_t h = 0;
  if (!openRW(&h)) return;
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

size_t count() {
  nvs_handle_t h = 0;
  if (!openRO(&h)) return 0;
  // ESP-IDF NVS lacks a direct "entries used" API; existing callers
  // only need a boolean "has anything?" check via clearAll. Keep the
  // symbol so future code can grow into it without breaking the
  // header.
  nvs_close(h);
  return 0;
}

}  // namespace MenuOrderStore
