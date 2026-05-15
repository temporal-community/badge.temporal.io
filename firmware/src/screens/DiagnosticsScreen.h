#pragma once
#include "Screen.h"

// ─── Diagnostics screen (live hardware/network stats) ───────────────────────

class DiagnosticsScreen : public Screen {
 public:
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenDiagnostics; }
  bool showCursor() const override { return false; }
};
