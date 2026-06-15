// BadgeUID.cpp — base MAC read + UUID v5 derivation

#include "BadgeUID.h"
#include "../hardware/oled.h"

#include "esp_mac.h"
#include "esp_err.h"
#include "mbedtls/sha1.h"
#include "mbedtls/version.h"

#include <string.h>

extern oled badgeDisplay;

uint8_t uid[UID_SIZE];
char    uid_hex[UID_HEX_LEN + 1];
char    badge_uuid[BADGE_UUID_LEN + 1];

// RFC 4122 DNS namespace UUID — must match _BADGE_NS in
// registrationScanner/workflows/_shared.py so on-device derivation
// matches the server's normalize_badge_id() output for the same input.
static const uint8_t DNS_NS[16] = {
  0x6b, 0xa7, 0xb8, 0x10,  0x9d, 0xad,  0x11, 0xd1,
  0x80, 0xb4,  0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8,
};

static void format_mac_hex(const uint8_t mac[6], char out[UID_HEX_LEN + 1]) {
  static const char H[] = "0123456789abcdef";
  for (int i = 0; i < 6; i++) {
    out[i * 2]     = H[mac[i] >> 4];
    out[i * 2 + 1] = H[mac[i] & 0x0F];
  }
  out[UID_HEX_LEN] = '\0';
}

void mac_hex_to_uuid(const char* mac_hex_12, char out[BADGE_UUID_LEN + 1]) {
  uint8_t buf[16 + UID_HEX_LEN];
  memcpy(buf, DNS_NS, 16);
  memcpy(buf + 16, mac_hex_12, UID_HEX_LEN);

  uint8_t digest[20];
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER < 0x03000000
  mbedtls_sha1_ret(buf, sizeof(buf), digest);
#else
  mbedtls_sha1(buf, sizeof(buf), digest);
#endif

  // RFC 4122 §4.3: stamp version 5 and variant bits over the SHA-1 digest.
  digest[6] = (digest[6] & 0x0F) | 0x50;
  digest[8] = (digest[8] & 0x3F) | 0x80;

  static const char H[] = "0123456789abcdef";
  int p = 0;
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
    out[p++] = H[digest[i] >> 4];
    out[p++] = H[digest[i] & 0x0F];
  }
  out[BADGE_UUID_LEN] = '\0';
}

void read_uid() {
  esp_err_t err = esp_efuse_mac_get_default(uid);
  if (err != ESP_OK) {
    Serial.println("base MAC read failed");
    badgeDisplay.clearBuffer();
    badgeDisplay.setFontPreset(FONT_SMALL);
    badgeDisplay.drawStr(0, 10, "MAC read failed.");
    badgeDisplay.drawStr(0, 24, "Please reboot.");
    badgeDisplay.sendBuffer();
    while (true) delay(1000);
  }
  format_mac_hex(uid, uid_hex);
  mac_hex_to_uuid(uid_hex, badge_uuid);
}
