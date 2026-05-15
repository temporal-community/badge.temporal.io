#pragma once
//
// WifiScreen — top-level WiFi management UI.
//
// Surfaces every saved WiFi network in `Config::kMaxWifiNetworks`
// slots and lets the user add new ones, edit credentials, reorder,
// or forget existing entries. The connection state of the radio is
// shown in the header so the user can verify a "Connect" press
// actually got online.
//
// Cursor model:
//   • Non-interactive status lines at the top: link, SSID, IP, RSSI.
//   • Auto-connect toggle, then slot list (expanded actions under one slot).
//   • One trailing "Add Network" row when at least one slot is free.
//   • One "Connect" action row at the bottom that re-runs the
//     iterating `WiFiService::connect()`.
//
// Pressing confirm on a saved network opens a sub-menu (rendered as
// an overlay-style action list) with Connect / Edit Password /
// Forget / Move Up / Move Down. Pressing confirm on "Add Network"
// pushes the on-screen keyboard for the SSID, then the password.
//
// The screen owns its own scratch buffer (`inputBuf_`) for the text-
// input round-trip so the keyboard's submit callback can re-enter
// us without leaking data through statics.

#include <stdint.h>

#include "JoyRamp.h"
#include "Screen.h"

class Config;

class WifiScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void onResume(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenWifi; }
  bool showCursor() const override { return false; }

  // Called by the static TextInput trampoline.
  void onTextSubmit(const char* text);

  // Called from the background connect task when the attempt finishes.
  void onConnectResult(bool ok);

 private:
  // Two-stage flow: when expanding a saved row, the per-slot action
  // menu replaces the slot's children in the list. This mirrors the
  // SettingsScreen "open a group" UX so the screen reads the same
  // way to a returning user.
  enum class Action : uint8_t {
    kConnect,
    kForget,
    kEditPassword,
    kMoveUp,
    kMoveDown,
  };

  // What the next text-input return should do.
  enum class PendingInput : uint8_t {
    kNone,
    kAddSsid,         // SSID for a fresh slot; password input follows
    kAddPassword,     // password for the slot we just added
    kEditPassword,    // password edit for an existing slot
  };

  void rebuildRows();
  bool tryConnect(GUIManager& gui);
  bool tryConnectToSlot(GUIManager& gui, uint8_t slot);
  void pushSsidEditor(GUIManager& gui);
  void pushPasswordEditor(GUIManager& gui, const char* title);
  void clampCursor();

  // Overlay painted on top of the WiFi list while a connect is in
  // flight (or within a few seconds of completion/failure). Drawn by
  // render() after the normal screen content.
  void drawConnectOverlay(oled& d);
  // True when the overlay is currently driving the screen — used to
  // gate input so confirm/back during a connect doesn't fight with
  // the worker. Back still pops the screen, but we treat it as "ack
  // and dismiss" first.
  bool overlayActive() const;

  static const char* labelForAction(Action a);
  static Action actionAt(uint8_t indexInList);

  // Number of expanded inline action rows under the focused slot.
  static constexpr uint8_t kActionsPerSlot = 5;

  // Non-interactive diagnostics at the top of the list (link / SSID / IP /
  // signal). `Row::slot` stores `DiagnosticLine` as uint8_t.
  static constexpr uint8_t kDiagnosticRowCount = 4;
  enum class DiagnosticLine : uint8_t {
    kLinkState = 0,
    kNetworkName,
    kIpAddr,
    kRssi
  };

  enum class RowKind : uint8_t {
    kDiagnostics,
    kEnableToggle,
    kSlot,
    kSlotAction,
    kAddNetwork,
    kConnectNow,
  };

  struct Row {
    RowKind kind;
    uint8_t slot;      // diagnostics: DiagnosticLine; else wifi slot index
    Action  action;    // valid for kSlotAction
  };

  // Worst-case: all diagnostic rows + toggle + max slot headers +
  // one fully expanded actions group + trailing utility rows (+ slack).
  // Must stay in sync with `Config::kMaxWifiNetworks` (four slots today).
  static constexpr uint8_t kMaxWifiSlotLimit = 4;
  static constexpr uint8_t kMaxRows =
      kDiagnosticRowCount + 1 + kMaxWifiSlotLimit + kActionsPerSlot + 2 + 2;

  Row rows_[kMaxRows];
  uint8_t rowCount_ = 0;
  uint8_t cursor_ = 0;
  uint8_t scroll_ = 0;
  int8_t  expandedSlot_ = -1;  // -1 when no slot expanded

  // Slot index whose password input we just pushed; remembered so
  // the keyboard's onSubmit knows which slot to update on return.
  int8_t pendingSlot_ = -1;
  PendingInput pendingInput_ = PendingInput::kNone;

  // Scratch SSID kept across the SSID → Password round-trip when
  // adding a new network. Sized for the WPA spec (32 bytes + NUL +
  // joystick-keyboard padding).
  char pendingSsid_[64] = {};

  JoyRamp joyRamp_;
  uint32_t lastConnectAttemptMs_ = 0;
  // 96 = WPA password max (63) + NUL + headroom; matches the
  // SettingsScreen scratch buffer used for the same purpose.
  char inputBuf_[96] = {};

  // ── Async connect state ─────────────────────────────────────────────────
  // Written from the background connect task (Core 0) and read from the
  // main-loop render / handleInput (Core 1).  A plain enum sized to one
  // byte with `volatile` is safe for this simple producer-consumer pattern
  // on ESP32 (coherent write-back cache, no reorder that crosses a core
  // boundary for single-byte stores).
  enum class ConnectState : uint8_t { kIdle, kConnecting, kSuccess, kFailed };
  volatile ConnectState connectState_ = ConnectState::kIdle;
  // millis() snapshot taken when a connect result (success or fail) arrives,
  // used to auto-clear the transient "FAILED" subtitle after a few seconds.
  uint32_t connectResultMs_ = 0;
};
