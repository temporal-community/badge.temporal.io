#include "QRCodePlate.h"

#include "../hardware/oled.h"

namespace QRCodePlate {

// Corner radius for the QR's white backing plate. Small enough that
// the rounded corners don't bite into the QR's quiet zone (callers
// size the plate so the QR sits at least 2 px in from each edge), big
// enough to read as "card" rather than "rectangle". Applied to every
// QR plate the badge draws so the visual language stays consistent.
constexpr int kPlateCornerRadius = 3;

void draw(oled& d, const uint8_t* bits, int width, int height,
          int x, int y, int plateSize, bool divider) {
  d.setDrawColor(1);
  d.drawRBox(x, y, plateSize, plateSize, kPlateCornerRadius);

  if (bits && width > 0 && height > 0) {
    int qrX = x + (plateSize - width) / 2;
    int qrY = y + (plateSize - height) / 2;
    if (qrX < x) qrX = x;
    if (qrY < y) qrY = y;

    d.setDrawColor(0);
    d.drawXBM(qrX, qrY, width, height, bits);
  }

  d.setDrawColor(1);
  if (divider) d.drawVLine(x + plateSize, y, plateSize);
}

}  // namespace QRCodePlate
