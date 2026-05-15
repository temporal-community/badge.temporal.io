#pragma once

#include <stdint.h>

// Boot starfield: 3-second random-star animation drawn directly to the
// OLED. Used as the boot splash in place of the WiFi/pairing/QR
// status text. Returns when the duration elapses.

namespace BootSplash {

void playStarfield(uint32_t durationMs = 2500, int numStars = 69);

}  // namespace BootSplash

