#include "WifiScreen.h"

#include <WiFi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../api/WiFiService.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../infra/BadgeConfig.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/UIFonts.h"
#include "ScreenRefs.h"
#include "TextInputScreen.h"

namespace {
constexpr uint8_t kRowHeight = 11;
constexpr uint8_t kContentY = OLEDLayout::kContentTopY;
constexpr int kValueRightX = 119;
constexpr int kRowIndentX = 16;
constexpr int kHeaderIndentX = 4;
constexpr int kDiagValX = 50;
constexpr uint16_t kJoyDeadband = 600;
constexpr uint32_t kRampStartMs = 350;
constexpr uint32_t kRampMinMs = 100;
constexpr uint32_t kRampStepMs = 50;

// 5×5 chevron pair shared with SettingsScreen — we redraw them here
// to keep the screen self-contained rather than reaching across into
// the SettingsScreen TU.
constexpr uint8_t kChevronW = 5;
constexpr uint8_t kChevronH = 5;
const uint8_t kChevronDownBits[] PROGMEM = {0x00, 0x1F, 0x0E, 0x04, 0x00};
const uint8_t kChevronRightBits[] PROGMEM = {0x01, 0x03, 0x07, 0x03, 0x01};

inline bool savedSlotMatchesLiveWifi(uint8_t slot) {
  if (!wifiService.isConnected()) return false;
  const char* sv = badgeConfig.wifiSsidAt(slot);
  if (!sv || !sv[0]) return false;
  const String live = WiFi.SSID();
  return live.length() > 0 && live == sv;
}

void wifiTextSubmitTrampoline(const char* text, void* user) {
  if (auto* self = static_cast<WifiScreen*>(user)) {
    self->onTextSubmit(text);
  }
}
}  // namespace

// Action label / iteration helpers live as `WifiScreen` statics
// because `Action` is a private nested enum — anonymous-namespace
// helpers can't name it.
const char* WifiScreen::labelForAction(Action a) {
  switch (a) {
    case Action::kConnect:      return "Connect";
    case Action::kForget:       return "Forget";
    case Action::kEditPassword: return "Edit Password";
    case Action::kMoveUp:       return "Move Up";
    case Action::kMoveDown:     return "Move Down";
  }
  return "?";
}

WifiScreen::Action WifiScreen::actionAt(uint8_t indexInList) {
  switch (indexInList) {
    case 0: return Action::kConnect;
    case 1: return Action::kEditPassword;
    case 2: return Action::kMoveUp;
    case 3: return Action::kMoveDown;
    case 4: return Action::kForget;
  }
  return Action::kConnect;
}

void WifiScreen::onEnter(GUIManager& gui) {
  (void)gui;
  expandedSlot_ = -1;
  pendingSlot_ = -1;
  pendingInput_ = PendingInput::kNone;
  pendingSsid_[0] = '\0';
  cursor_ = 0;
  scroll_ = 0;
  joyRamp_.reset();
  rebuildRows();
}

void WifiScreen::onResume(GUIManager& gui) {
  // Coming back from the keyboard or from a connect attempt: rebuild
  // so newly-added or freshly-deleted rows render.
  rebuildRows();
  clampCursor();

  // Two-stage SSID + password add: the SSID keyboard's submit set
  // pendingInput_ = kAddPassword. We can't push the second keyboard
  // from inside the first keyboard's submit callback (the keyboard
  // pops itself immediately after firing the callback, which would
  // also pop the second keyboard we just pushed). Doing it from
  // onResume — which only fires after the first keyboard has
  // finished popping — keeps the stack ordering sane.
  if (pendingInput_ == PendingInput::kAddPassword && pendingSsid_[0] != '\0') {
    // Clear the marker first so a cancelled password screen doesn't
    // re-trigger this branch on the *next* onResume — that would
    // trap the user in an inescapable keyboard loop.
    // `onTextSubmit` infers the right intent from `pendingSsid_`
    // being non-empty, so it doesn't need pendingInput_ here.
    pendingInput_ = PendingInput::kNone;
    char title[40];
    snprintf(title, sizeof(title), "Pwd for %.20s", pendingSsid_);
    inputBuf_[0] = '\0';
    sTextInput.configure(title, inputBuf_, sizeof(inputBuf_),
                         &wifiTextSubmitTrampoline, this);
    gui.pushScreen(kScreenTextInput);
  } else if (pendingSsid_[0] != '\0' &&
             pendingInput_ == PendingInput::kNone) {
    // The user cancelled the password keyboard mid-flow. Drop the
    // half-entered SSID so the next add-network attempt starts from
    // scratch instead of silently inheriting yesterday's typo.
    pendingSsid_[0] = '\0';
  }
}

void WifiScreen::rebuildRows() {
  rowCount_ = 0;
  for (uint8_t line = 0; line < kDiagnosticRowCount && rowCount_ < kMaxRows;
       ++line) {
    Row& dr = rows_[rowCount_++];
    dr.kind = RowKind::kDiagnostics;
    dr.slot = line;
    dr.action = Action::kConnect;
  }
  // Master enable toggle — quick way to disable auto-connect on boot
  // without forgetting every saved network.
  {
    Row& r = rows_[rowCount_++];
    r.kind = RowKind::kEnableToggle;
    r.slot = 0;
    r.action = Action::kConnect;
  }
  for (uint8_t i = 0;
       i < Config::kMaxWifiNetworks && rowCount_ < kMaxRows; ++i) {
    Row& r = rows_[rowCount_++];
    r.kind = RowKind::kSlot;
    r.slot = i;
    r.action = Action::kConnect;
    if (expandedSlot_ == static_cast<int8_t>(i)) {
      // Inline action rows live directly under the expanded slot
      // header. We surface every action even when the slot is
      // unconfigured — Connect / EditPassword still make sense for
      // an empty SSID (they're UX dead-ends but harmless), and
      // suppressing rows here would make cursor math conditional in
      // a way that's easy to get wrong.
      const uint8_t saved = badgeConfig.wifiNetworkCount();
      const bool isOnlySaved = (badgeConfig.wifiSlotConfigured(i) && saved == 1);
      for (uint8_t a = 0; a < kActionsPerSlot && rowCount_ < kMaxRows; ++a) {
        const Action act = actionAt(a);
        // Hide reorder rows that would be no-ops, plus EditPassword
        // for unconfigured slots (there's no SSID yet to attach a
        // password to). Forget / Connect remain visible so the user
        // can still close the expansion via "Forget" if they typed
        // an SSID by mistake.
        if (act == Action::kMoveUp && i == 0) continue;
        if (act == Action::kMoveDown &&
            (i + 1 >= Config::kMaxWifiNetworks ||
             !badgeConfig.wifiSlotConfigured(i + 1))) {
          continue;
        }
        if (act == Action::kEditPassword &&
            !badgeConfig.wifiSlotConfigured(i)) {
          continue;
        }
        if (act == Action::kForget &&
            !badgeConfig.wifiSlotConfigured(i)) {
          continue;
        }
        // Suppress Forget when this is the only saved network and it
        // would leave the user with no creds; not strictly necessary
        // but avoids a "forgot to save" surprise.
        (void)isOnlySaved;

        Row& ar = rows_[rowCount_++];
        ar.kind = RowKind::kSlotAction;
        ar.slot = i;
        ar.action = act;
      }
    }
  }
  // Trailing utility rows.
  if (rowCount_ < kMaxRows && badgeConfig.wifiNetworkCount() <
                                  Config::kMaxWifiNetworks) {
    Row& r = rows_[rowCount_++];
    r.kind = RowKind::kAddNetwork;
    r.slot = 0;
    r.action = Action::kConnect;
  }
  if (rowCount_ < kMaxRows && badgeConfig.wifiNetworkCount() > 0) {
    Row& r = rows_[rowCount_++];
    r.kind = RowKind::kConnectNow;
    r.slot = 0;
    r.action = Action::kConnect;
  }
}

void WifiScreen::clampCursor() {
  if (rowCount_ == 0) {
    cursor_ = 0;
    scroll_ = 0;
    return;
  }
  if (cursor_ >= rowCount_) cursor_ = rowCount_ - 1;
  if (scroll_ > cursor_) scroll_ = cursor_;
}

void WifiScreen::pushSsidEditor(GUIManager& gui) {
  pendingInput_ = PendingInput::kAddSsid;
  inputBuf_[0] = '\0';
  sTextInput.configure("WiFi SSID", inputBuf_, sizeof(inputBuf_),
                       &wifiTextSubmitTrampoline, this);
  gui.pushScreen(kScreenTextInput);
}

void WifiScreen::pushPasswordEditor(GUIManager& gui, const char* title) {
  inputBuf_[0] = '\0';
  sTextInput.configure(title, inputBuf_, sizeof(inputBuf_),
                       &wifiTextSubmitTrampoline, this);
  gui.pushScreen(kScreenTextInput);
}

void WifiScreen::onTextSubmit(const char* text) {
  if (!text) {
    pendingInput_ = PendingInput::kNone;
    return;
  }
  switch (pendingInput_) {
    case PendingInput::kAddSsid: {
      // Stash and let onResume push the password editor — we can't
      // push another screen from inside the keyboard's onDone (the
      // keyboard pops itself immediately afterwards).
      strncpy(pendingSsid_, text, sizeof(pendingSsid_) - 1);
      pendingSsid_[sizeof(pendingSsid_) - 1] = '\0';
      if (pendingSsid_[0] == '\0') {
        pendingInput_ = PendingInput::kNone;
        return;
      }
      pendingInput_ = PendingInput::kAddPassword;
      break;
    }
    case PendingInput::kAddPassword: {
      const int8_t slot = badgeConfig.addWifiNetwork(pendingSsid_, text);
      pendingSsid_[0] = '\0';
      pendingInput_ = PendingInput::kNone;
      if (slot >= 0) {
        cursor_ = static_cast<uint8_t>(slot) + 1 + kDiagnosticRowCount;
      }
      break;
    }
    case PendingInput::kEditPassword: {
      // Skip the write when the user submitted an empty string — the
      // editor always opens blank (we deliberately don't pre-fill an
      // existing password), so the easy footgun is opening Edit
      // Password to peek and pressing Send. That used to silently
      // wipe the stored password via `setWifiCredentialsAt(..., "")`
      // (which strncpy's "" into slot.pass and persistWifiSlot then
      // removes the NVS pwd blob). If the user actually wants to
      // clear a slot, they'd use Forget.
      if (pendingSlot_ >= 0 &&
          static_cast<uint8_t>(pendingSlot_) < Config::kMaxWifiNetworks &&
          text && text[0] != '\0') {
        badgeConfig.setWifiCredentialsAt(
            static_cast<uint8_t>(pendingSlot_), nullptr, text);
      }
      pendingSlot_ = -1;
      pendingInput_ = PendingInput::kNone;
      break;
    }
    case PendingInput::kNone:
      // Reached when the password phase of an add-network flow fired
      // (we cleared pendingInput_ before pushing the keyboard so a
      // cancel doesn't loop). Recover the intent from `pendingSsid_`.
      if (pendingSsid_[0] != '\0') {
        const int8_t slot = badgeConfig.addWifiNetwork(pendingSsid_, text);
        pendingSsid_[0] = '\0';
        if (slot >= 0) {
          cursor_ = static_cast<uint8_t>(slot) + 1 + kDiagnosticRowCount;
        }
      }
      break;
  }
}

bool WifiScreen::tryConnect(GUIManager& gui) {
  (void)gui;
  lastConnectAttemptMs_ = millis();
  return wifiService.connectSavedNetworksAsync();
}

bool WifiScreen::tryConnectToSlot(GUIManager& gui, uint8_t slot) {
  (void)gui;
  lastConnectAttemptMs_ = millis();
  // Async path drives the popup overlay in render(); refusal to start
  // (e.g. attempt already in flight) just leaves the existing popup
  // up — that's the right UX, not a separate error.
  return wifiService.connectToSlotAsync(slot);
}

namespace {
const char* phaseLabel(WiFiService::Phase p) {
  switch (p) {
    case WiFiService::Phase::kIdle:           return "";
    case WiFiService::Phase::kStarting:       return "Starting";
    case WiFiService::Phase::kAttempting:     return "Connecting";
    case WiFiService::Phase::kAuthenticating: return "Authenticating";
    case WiFiService::Phase::kConnected:      return "Connected";
    case WiFiService::Phase::kFailed:         return "Failed";
  }
  return "";
}

constexpr uint32_t kOverlayLingerMs = 2500;
}  // namespace

bool WifiScreen::overlayActive() const {
  const auto p = wifiService.phase();
  if (p == WiFiService::Phase::kIdle) return false;
  if (p == WiFiService::Phase::kConnected ||
      p == WiFiService::Phase::kFailed) {
    // Auto-dismiss the overlay a couple of seconds after we land in a
    // terminal phase so the screen returns to its normal list. The
    // service keeps `phase_` set; we just stop drawing.
    return (millis() - wifiService.phaseChangedMs()) < kOverlayLingerMs;
  }
  return true;
}

void WifiScreen::drawConnectOverlay(oled& d) {
  // Centered modal, ~118 × 38, leaves the header pill visible above
  // and the footer rule below.
  constexpr int kBoxW = 118;
  constexpr int kBoxH = 38;
  constexpr int kBoxX = (OLEDLayout::kScreenW - kBoxW) / 2;
  constexpr int kBoxY = 14;

  // Knockout fill so the underlying list rows don't bleed through.
  d.setDrawColor(0);
  d.drawBox(kBoxX, kBoxY, kBoxW, kBoxH);
  d.setDrawColor(1);
  d.drawRFrame(kBoxX, kBoxY, kBoxW, kBoxH, 2);

  const auto phase = wifiService.phase();
  const char* ssid = wifiService.phaseSsid();
  const char* status = wifiService.phaseStatusText();

  // Title bar: inverted strip with "WiFi · <SSID>".
  constexpr int kStripH = 9;
  d.drawBox(kBoxX, kBoxY, kBoxW, kStripH);
  d.setDrawColor(0);
  d.setFont(u8g2_font_5x7_tf);
  char title[40];
  if (ssid && ssid[0]) {
    std::snprintf(title, sizeof(title), "WiFi  %.18s", ssid);
  } else {
    std::snprintf(title, sizeof(title), "WiFi");
  }
  d.drawStr(kBoxX + 4, kBoxY + 7, title);
  d.setDrawColor(1);

  // Phase line: bold-ish (5x8 default font) heading.
  d.setFont(u8g2_font_6x10_tf);
  const char* label = phaseLabel(phase);
  d.drawStr(kBoxX + 4, kBoxY + kStripH + 10, label);

  // Spinner glyph at the right edge of the phase line while the
  // attempt is still in flight; replaced by a check / x mark on
  // terminal phases.
  if (phase == WiFiService::Phase::kStarting ||
      phase == WiFiService::Phase::kAttempting ||
      phase == WiFiService::Phase::kAuthenticating) {
    OLEDLayout::drawBusySpinner(d, kBoxX + kBoxW - 9,
                                kBoxY + kStripH + 6, millis() / 125);
  } else if (phase == WiFiService::Phase::kConnected) {
    // Tiny check: two diagonal strokes.
    const int cx = kBoxX + kBoxW - 12;
    const int cy = kBoxY + kStripH + 6;
    d.drawLine(cx, cy + 2, cx + 2, cy + 4);
    d.drawLine(cx + 2, cy + 4, cx + 6, cy);
  } else if (phase == WiFiService::Phase::kFailed) {
    const int cx = kBoxX + kBoxW - 11;
    const int cy = kBoxY + kStripH + 2;
    d.drawLine(cx, cy, cx + 5, cy + 5);
    d.drawLine(cx + 5, cy, cx, cy + 5);
  }

  // Status text — single line, fit-to-width so long IPs don't bleed
  // past the frame.
  d.setFont(u8g2_font_5x7_tf);
  char detail[64] = {};
  std::snprintf(detail, sizeof(detail), "%s",
                status && status[0] ? status : "");
  if (detail[0]) {
    OLEDLayout::fitText(d, detail, sizeof(detail), kBoxW - 8);
    d.drawStr(kBoxX + 4, kBoxY + kBoxH - 5, detail);
  }
}

void WifiScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setTextWrap(false);
  d.setDrawColor(1);

  // Header reflects the radio's current state.
  const bool online = wifiService.isConnected();
  OLEDLayout::drawHeader(d, "WIFI", online ? "ONLINE" : nullptr);

  d.setFont(u8g2_font_5x7_tf);
  const uint8_t visibleRows =
      (OLEDLayout::kFooterTopY - kContentY) / kRowHeight;

  if (cursor_ < scroll_) scroll_ = cursor_;
  if (cursor_ >= scroll_ + visibleRows) {
    scroll_ = static_cast<uint8_t>(cursor_ - visibleRows + 1);
  }
  const uint8_t maxScroll =
      rowCount_ > visibleRows ? rowCount_ - visibleRows : 0;
  if (scroll_ > maxScroll) scroll_ = maxScroll;

  for (uint8_t i = 0; i < visibleRows && (scroll_ + i) < rowCount_; ++i) {
    const uint8_t idx = scroll_ + i;
    const uint8_t y = kContentY + i * kRowHeight;
    const Row& r = rows_[idx];
    const bool selected = (idx == cursor_);

    if (selected) {
      OLEDLayout::drawSelectedRow(d, y, kRowHeight, /*x=*/0, /*w=*/123);
    }
    d.setDrawColor(selected ? 0 : 1);
    const int baseline = y + d.getAscent() + 2;

    char left[40] = {};
    char right[24] = {};

    switch (r.kind) {
      case RowKind::kDiagnostics: {
        const DiagnosticLine line = static_cast<DiagnosticLine>(r.slot);
        const char* tag = "?";
        char valBuf[40] = {};
        switch (line) {
          case DiagnosticLine::kLinkState:
            tag = "Link";
            snprintf(valBuf, sizeof(valBuf), "%s",
                     online ? "Connected" : "Not connected");
            break;
          case DiagnosticLine::kNetworkName:
            tag = "Network";
            if (online) {
              const String nw = WiFi.SSID();
              snprintf(valBuf, sizeof(valBuf), "%.31s", nw.c_str());
            }
            if (valBuf[0] == '\0') strncpy(valBuf, "---", sizeof(valBuf));
            valBuf[sizeof(valBuf) - 1] = '\0';
            break;
          case DiagnosticLine::kIpAddr:
            tag = "IP";
            if (online) {
              const String ipStr = WiFi.localIP().toString();
              snprintf(valBuf, sizeof(valBuf), "%.31s", ipStr.c_str());
            }
            if (valBuf[0] == '\0') strncpy(valBuf, "---", sizeof(valBuf));
            valBuf[sizeof(valBuf) - 1] = '\0';
            break;
          case DiagnosticLine::kRssi:
            tag = "RSSI";
            if (online) {
              const int dbm = wifiService.rssi();
              if (dbm != 0) snprintf(valBuf, sizeof(valBuf), "%d dBm", dbm);
              else strncpy(valBuf, "...", sizeof(valBuf));
            } else strncpy(valBuf, "---", sizeof(valBuf));
            valBuf[sizeof(valBuf) - 1] = '\0';
            break;
        }
        d.drawStr(kHeaderIndentX, baseline, tag);
        OLEDLayout::fitText(d, valBuf, sizeof(valBuf),
                            kValueRightX - kDiagValX + 4);
        d.drawStr(kDiagValX, baseline, valBuf);
        break;
      }
      case RowKind::kEnableToggle: {
        const bool on = badgeConfig.get(kWifiEnabled) != 0;
        d.drawStr(kHeaderIndentX, baseline, "Auto-connect");
        const char* val = on ? "On" : "Off";
        const int vw = d.getStrWidth(val);
        d.drawStr(kValueRightX - vw, baseline, val);
        break;
      }
      case RowKind::kSlot: {
        const bool open = (expandedSlot_ == static_cast<int8_t>(r.slot));
        const uint8_t* bits = open ? kChevronDownBits : kChevronRightBits;
        const int cy_base = y + (kRowHeight - kChevronH) / 2;
        const int cy = open ? cy_base : cy_base - 1;
        d.drawXBM(kHeaderIndentX, cy, kChevronW, kChevronH, bits);
        const char* ssid = badgeConfig.wifiSsidAt(r.slot);
        if (ssid && ssid[0]) {
          snprintf(left, sizeof(left), "%.20s", ssid);
          // Show the "active network" marker on whichever slot we're
          // actually associated with. Compares the radio's live SSID
          // against the slot's saved SSID — robust to per-slot
          // Connect picks (the user can land on any slot, not just 0)
          // and to the iterating boot path falling through to a
          // later slot.
          if (online) {
            String live = WiFi.SSID();
            if (live.length() > 0 && live == ssid) {
              snprintf(right, sizeof(right), "*");
            }
          }
        } else {
          snprintf(left, sizeof(left), "(empty slot %u)",
                   static_cast<unsigned>(r.slot + 1));
        }
        d.drawStr(kHeaderIndentX + kChevronW + 4, baseline, left);
        if (right[0]) {
          const int rw = d.getStrWidth(right);
          d.drawStr(kValueRightX - rw, baseline, right);
        }
        break;
      }
      case RowKind::kSlotAction: {
        if (r.action == Action::kConnect) {
          snprintf(left, sizeof(left), "%s",
                   savedSlotMatchesLiveWifi(r.slot) ? "Disconnect"
                                                    : labelForAction(
                                                          Action::kConnect));
        } else {
          snprintf(left, sizeof(left), "%s", labelForAction(r.action));
        }
        d.drawStr(kRowIndentX + 6, baseline, left);
        const char* hint = "go";
        if (r.action == Action::kForget) hint = "x";
        if (r.action == Action::kEditPassword) hint = "edit";
        if (r.action == Action::kMoveUp || r.action == Action::kMoveDown) {
          hint = r.action == Action::kMoveUp ? "up" : "dn";
        }
        if (r.action == Action::kConnect &&
            savedSlotMatchesLiveWifi(r.slot)) {
          hint = "disc";
        }
        const int hw = d.getStrWidth(hint);
        d.drawStr(kValueRightX - hw, baseline, hint);
        break;
      }
      case RowKind::kAddNetwork:
        d.drawStr(kHeaderIndentX, baseline, "+ Add Network");
        break;
      case RowKind::kConnectNow: {
        if (online) {
          d.drawStr(kHeaderIndentX, baseline, "Disconnect");
        } else {
          d.drawStr(kHeaderIndentX, baseline, "Connect Now");
        }
        const char* status = online ? "disc" : "go";
        const int sw = d.getStrWidth(status);
        d.drawStr(kValueRightX - sw, baseline, status);
        break;
      }
    }
    d.setDrawColor(1);
  }

  // Footer hint matches the row kind under the cursor.
  const Row& cur = rows_[cursor_ < rowCount_ ? cursor_ : 0];
  const char* footer = nullptr;
  const char* action = "select";
  switch (cur.kind) {
    case RowKind::kDiagnostics: {
      const DiagnosticLine line = static_cast<DiagnosticLine>(cur.slot);
      switch (line) {
        case DiagnosticLine::kLinkState:
          footer = "WiFi association status";
          break;
        case DiagnosticLine::kNetworkName:
          footer = "Current network name";
          break;
        case DiagnosticLine::kIpAddr:
          footer = "Local IP address";
          break;
        case DiagnosticLine::kRssi:
          footer = "Signal strength estimate";
          break;
      }
      action = "";
      break;
    }
    case RowKind::kEnableToggle:
      footer = (badgeConfig.get(kWifiEnabled) != 0)
                   ? "Auto-connect on boot is on"
                   : "Auto-connect on boot is off";
      action = "toggle";
      break;
    case RowKind::kSlot: {
      if (badgeConfig.wifiSlotConfigured(cur.slot)) {
        footer = (expandedSlot_ == static_cast<int8_t>(cur.slot))
                     ? "Close network actions"
                     : "Open network actions";
      } else {
        footer = "(empty slot)";
      }
      action = (expandedSlot_ == static_cast<int8_t>(cur.slot)) ? "close"
                                                                : "open";
      break;
    }
    case RowKind::kSlotAction:
      if (cur.action == Action::kConnect &&
          savedSlotMatchesLiveWifi(cur.slot)) {
        footer = "Disconnect from this WiFi";
      } else {
        footer = labelForAction(cur.action);
      }
      action =
          (cur.action == Action::kConnect &&
           savedSlotMatchesLiveWifi(cur.slot))
              ? "select"
              : "go";
      break;
    case RowKind::kAddNetwork:
      footer = "Add a saved network";
      action = "go";
      break;
    case RowKind::kConnectNow:
      footer = online ? "Disconnect radio" : "Try saved networks in order";
      action = "go";
      break;
  }
  OLEDLayout::drawActionFooter(d, footer ? footer : "WiFi", action);

  // Connect-progress overlay paints last so it sits above the list +
  // footer. Self-dismisses a couple of seconds after a terminal phase.
  if (overlayActive()) drawConnectOverlay(d);
}

void WifiScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                             int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  // While the connect-progress overlay is up, intercept input so a
  // stray confirm/back press doesn't queue a second connect or
  // navigate away mid-handshake. Cancel still has an out: it
  // dismisses the overlay early instead of popping the screen, which
  // matches what most users expect when a modal is in their face.
  if (overlayActive()) {
    // Cancel acks a finished attempt early; mid-handshake it's a
    // no-op so the user can't accidentally hide a connect in flight.
    // Confirm is swallowed entirely — re-pressing Connect during a
    // running attempt would just refuse anyway and is a UX dead end.
    if (e.cancelPressed) wifiService.dismissPhase();
    return;
  }

  if (e.cancelPressed) {
    if (expandedSlot_ >= 0) {
      expandedSlot_ = -1;
      rebuildRows();
      clampCursor();
      return;
    }
    gui.popScreen();
    return;
  }

  if (e.confirmPressed && cursor_ < rowCount_) {
    const bool online = wifiService.isConnected();
    const Row& r = rows_[cursor_];
    switch (r.kind) {
      case RowKind::kDiagnostics:
        return;
      case RowKind::kEnableToggle: {
        const int32_t now = badgeConfig.get(kWifiEnabled);
        badgeConfig.set(kWifiEnabled, now != 0 ? 0 : 1);
        badgeConfig.apply(kWifiEnabled);
        badgeConfig.save();
        return;
      }
      case RowKind::kSlot: {
        if (!badgeConfig.wifiSlotConfigured(r.slot)) {
          // Empty slot acts like "Add Network" at this position.
          pushSsidEditor(gui);
          return;
        }
        expandedSlot_ =
            (expandedSlot_ == static_cast<int8_t>(r.slot)) ? -1
                                                           : static_cast<int8_t>(r.slot);
        rebuildRows();
        clampCursor();
        return;
      }
      case RowKind::kSlotAction: {
        switch (r.action) {
          case Action::kConnect:
            if (savedSlotMatchesLiveWifi(r.slot)) {
              wifiService.disconnect();
              wifiService.dismissPhase();
              gui.requestRender();
              return;
            }
            tryConnectToSlot(gui, r.slot);
            return;
          case Action::kForget:
            badgeConfig.removeWifiNetwork(r.slot);
            expandedSlot_ = -1;
            rebuildRows();
            clampCursor();
            return;
          case Action::kEditPassword:
            pendingSlot_ = static_cast<int8_t>(r.slot);
            pendingInput_ = PendingInput::kEditPassword;
            pushPasswordEditor(gui, "WiFi Password");
            return;
          case Action::kMoveUp:
            badgeConfig.moveWifiNetworkUp(r.slot);
            expandedSlot_ = (r.slot == 0)
                                ? -1
                                : static_cast<int8_t>(r.slot - 1);
            rebuildRows();
            clampCursor();
            return;
          case Action::kMoveDown:
            badgeConfig.moveWifiNetworkDown(r.slot);
            expandedSlot_ = static_cast<int8_t>(r.slot + 1);
            rebuildRows();
            clampCursor();
            return;
        }
        return;
      }
      case RowKind::kAddNetwork:
        pushSsidEditor(gui);
        return;
      case RowKind::kConnectNow:
        if (online) {
          wifiService.disconnect();
          wifiService.dismissPhase();
          gui.requestRender();
          return;
        }
        tryConnect(gui);
        return;
    }
  }

  // Joystick Y scrolls the cursor with the standard ramped cadence.
  const int16_t joyDY =
      static_cast<int16_t>(inputs.joyY()) - 2047;
  const int8_t dir =
      (std::abs(joyDY) > static_cast<int16_t>(kJoyDeadband))
          ? (joyDY > 0 ? 1 : -1)
          : 0;
  if (joyRamp_.tick(dir, millis(), kRampStartMs, kRampMinMs, kRampStepMs)) {
    if (rowCount_ == 0) return;
    int16_t next = static_cast<int16_t>(cursor_) + dir;
    if (next < 0) next = 0;
    if (next >= rowCount_) next = rowCount_ - 1;
    cursor_ = static_cast<uint8_t>(next);
    gui.requestRender();
  }
}
