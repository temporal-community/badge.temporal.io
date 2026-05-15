// BadgeUID.h — Public interface for the BadgeUID module
// See FR-006 in spec.md
//
// Reads the ESP32-S3 base MAC (6 bytes, factory-burned for FCC compliance) and
// derives two representations:
//   uid_hex     — 12-char lowercase MAC hex; used locally and over IR.
//   badge_uuid  — 36-char canonical UUID v5 (RFC 4122 DNS namespace, input is
//                 the lowercase MAC hex). Kept for local identity files and
//                 compatibility with existing badge data.
//
// HALT behavior: read_uid() displays an error on the OLED and enters an infinite loop
// if the MAC read fails. It must be called before any other module runs.

#pragma once
#include <Arduino.h>

#define UID_SIZE        6   // base MAC is 6 bytes
#define UID_HEX_LEN     12
#define BADGE_UUID_LEN  36

extern uint8_t uid[UID_SIZE];
extern char    uid_hex[UID_HEX_LEN + 1];
extern char    badge_uuid[BADGE_UUID_LEN + 1];

// Read base MAC from eFuse, populate uid / uid_hex / badge_uuid.
// Halts with display error if the MAC read fails. Must be called in setup()
// before u8g2.begin() for the error display to work; u8g2 is initialized
// inside read_uid() on the error path.
void read_uid();

// Derive a canonical UUID v5 string from a 12-char lowercase MAC hex string.
// out must have room for 37 bytes (36 chars + null).
void mac_hex_to_uuid(const char* mac_hex_12, char out[BADGE_UUID_LEN + 1]);
