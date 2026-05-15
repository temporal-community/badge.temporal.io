#pragma once

// ============================================================
//  Battery icon overlays
//
//  Ported from Replay-26-Badge_QAFW/Main-Firmware/assets/battery_icons.h.
//  Used by OLEDLayout::drawBatteryIcon to render a state-aware battery
//  glyph (outline + inner fill that varies with charging/USB/SOC).
//
//  Fill positions are offsets inside the 7×4 px slot at (x+2, y+1) of
//  the 10×6 outline bitmap.
// ============================================================

namespace BatteryIcons {

// 10×6 outline (drawn always)
constexpr int kBatW = 10;
constexpr int kBatH = 6;
static const unsigned char kBatBits[] = {
    0xfe, 0x03, 0x03, 0x02, 0x03, 0x02, 0x03, 0x02, 0x03, 0x02, 0xfe, 0x03,
};

// 7×4 charging bolt (USB power present, battery accepting charge)
constexpr int kChargingW = 7;
constexpr int kChargingH = 4;
static const unsigned char kChargingBits[] = {0x76, 0x65, 0x53, 0x37};

// 1×4 exclamation point. Rows top→bottom: stem, stem, gap, dot. Drawn in
// background colour over a solid-filled inner slot to cut a negative-space
// "!" — saves flash vs storing a full 7×4 warning bitmap and reads cleaner.
constexpr int kExclamationW = 1;
constexpr int kExclamationH = 4;
static const unsigned char kExclamationBits[] = {0x00, 0x00, 0x00, 0x00};

// 7×4 solid fill (USB present + charge-complete)
constexpr int kFullW = 7;
constexpr int kFullH = 4;
static const unsigned char kFullBits[] = {0x7f, 0x7f, 0x7f, 0x7f};

// 3×4 numeric glyphs 0-9. The status header packs two of these around a
// 1px divider line into the 7px fill slot to display "[tens][divider]
// [units]" between 5-99 %. Hundreds are clamped — we cap display at 99.
constexpr int kNumW = 3;
constexpr int kNumH = 4;
static const unsigned char kNumBits[10][4] = {
    {0x00, 0x02, 0x02, 0x00},  // 0
    {0x04, 0x05, 0x05, 0x00},  // 1
    {0x00, 0x03, 0x04, 0x00},  // 2
    {0x00, 0x01, 0x03, 0x00},  // 3
    {0x02, 0x02, 0x00, 0x03},  // 4
    {0x00, 0x04, 0x03, 0x00},  // 5
    {0x00, 0x06, 0x00, 0x00},  // 6
    {0x00, 0x03, 0x03, 0x03},  // 7
    {0x00, 0x00, 0x02, 0x00},  // 8
    {0x00, 0x02, 0x00, 0x03},  // 9
};

}  // namespace BatteryIcons
