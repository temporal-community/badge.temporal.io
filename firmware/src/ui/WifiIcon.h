#pragma once

#include <stdint.h>

// ============================================================
//  WiFi indicator icon (11 × 6)
//
//  Sourced verbatim from
//  Replay-26-Badge_QAFW/QA-Firmware/assets/wifi.xbm — the slim arched
//  three-arc glyph used in the reference badge's status header. Painted
//  by OLEDLayout::drawWifiIcon; disconnected/busy cues are layered on
//  top in code (diagonal slash for not-connected, blinking dot for
//  active sync).
// ============================================================

namespace WifiIcon {

constexpr int kWidth  = 11;
constexpr int kHeight = 6;

// Full three-arc glyph — top arc + caps, middle arc + caps, bottom arc, dot.
// Drawn at maximum signal strength.
static const unsigned char kBits[] = {
    0xfc, 0x01, 0x02, 0x02, 0xf9, 0x04,
    0x04, 0x01, 0x70, 0x00, 0x20, 0x00,
};

// Two-bar variant: top arc and its side caps blanked. Middle arc +
// caps + bottom arc + dot remain.
static const unsigned char kBitsLevel2[] = {
    0x00, 0x00, 0x00, 0x00, 0xf8, 0x00,
    0x04, 0x01, 0x70, 0x00, 0x20, 0x00,
};

// One-bar variant: only the bottom arc + dot. Conveys "barely there"
// while still being recognisable as the WiFi glyph.
static const unsigned char kBitsLevel1[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x70, 0x00, 0x20, 0x00,
};

// "No WiFi" variant — same arched glyph with a baked-in diagonal slash
// (QA-Firmware/assets/no-wifi.xbm). Drawn in place of kBits when the
// service is offline so the disconnect cue is part of the icon rather
// than an XOR-cut overlay.
static const unsigned char kBitsNoWifi[] = {
    0xdc, 0x01, 0xde, 0x03, 0xff, 0x07,
    0xdc, 0x01, 0x70, 0x00, 0x20, 0x00,
};

// Pick a 0..3 signal-bar level from a raw RSSI reading. Thresholds
// chosen so anything past "comfortable HTTP-from-the-other-side-of-
// the-conference-floor" reads as 3, marginal AP edges read as 1.
//   >= -55  : 3 bars (excellent)
//   >= -65  : 2 bars (good)
//   >= -80  : 1 bar  (weak)
//   <  -80  : 1 bar  (very weak — still drawn, never 0 while connected)
inline uint8_t levelFromRssi(int rssi) {
  if (rssi >= -55) return 3;
  if (rssi >= -65) return 2;
  return 1;
}

}  // namespace WifiIcon
