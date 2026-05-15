#include "ModalScreen.h"

#include <Arduino.h>

#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/GUI.h"

void ModalScreen::onEnter(GUIManager& /*gui*/) {
  openedMs_ = millis();
}

void ModalScreen::render(oled& d, GUIManager& /*gui*/) {
  const auto chrome = OLEDLayout::drawModalChrome(
      d, boxX(), boxY(), boxW(), boxH(),
      title(), subhead(), openedMs_,
      actionStripH(), useFrame());
  drawBody(d, chrome);
  // Chips paint at kFooterBaseY=62, which sits inside the modal's
  // bottom action strip. drawFooterActions is swap-aware so BACK
  // lands on whichever physical button cancel-presses, primary on
  // confirm.
  OLEDLayout::drawFooterActions(d, xChip(), yChip(), bChip(), aChip());
}

void ModalScreen::handleInput(const Inputs& inputs, int16_t /*cursorX*/,
                              int16_t /*cursorY*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }
  if (e.yPressed) {
    onY(gui);
    return;
  }
  if (e.confirmPressed) {
    onConfirm(gui);
    return;
  }
}
