#pragma once

#include <Arduino.h>

class oled;

namespace QRCodePlate {

constexpr int kDefaultSize = 64;

void draw(oled& d, const uint8_t* bits, int width, int height,
          int x = 0, int y = 0, int plateSize = kDefaultSize,
          bool divider = true);

}  // namespace QRCodePlate
