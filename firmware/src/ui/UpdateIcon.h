#pragma once

// ============================================================
//  Firmware-update indicator icon (7 × 6)
//
//  Drawn immediately to the left of the WiFi glyph in the status
//  header when `BadgeOTA::updateAvailable()` is true. Shape: a small
//  down-arrow over a baseline ("download into device"). XBM byte
//  order is LSB-first per row.
//
//  ▼▼▼▼▼▼▼   row 0:  0x7e (....111111.)
//   ▼▼▼▼▼    row 1:  0x3c
//    ▼▼▼     row 2:  0x18
//     ▼      row 3:  0x08
//             row 4:  blank
//  ▼▼▼▼▼▼▼   row 5:  0x7e  (baseline)
// ============================================================

namespace UpdateIcon {

constexpr int kWidth  = 10;
constexpr int kHeight = 6;
static const unsigned char kBits[] = {
    0x57,
    0x03,
    0x71,
    0x03,
    0x03,
    0x01,
    0x45,
    0x00,
    0x55,
    0x00,
    0x29,
    0x00,
};

}  // namespace UpdateIcon
