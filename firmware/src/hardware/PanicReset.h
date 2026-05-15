#pragma once

class oled;

namespace PanicReset {
void begin(oled* display);
bool rebootPending();
}
