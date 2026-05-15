#pragma once

// ============================================================
//  Star icon (7 × 7) — assets/star1.xbm
//
//  Used as the bookend glyph at both ends of every footer rule
//  (drawFooterStars in OLEDLayout.cpp) and as the per-section
//  marker on map sections whose floors.md entry declares
//  `icon: temporal-icon`.
// ============================================================

namespace StarIcon {

constexpr int kWidth  = 7;
constexpr int kHeight = 7;
constexpr unsigned char kBits[] = {
    0x08, 0x08, 0x14, 0x63, 0x14, 0x08, 0x08,
};

}  // namespace StarIcon
