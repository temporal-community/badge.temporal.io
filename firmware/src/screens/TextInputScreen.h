#pragma once
#include "Screen.h"

// ─── On-screen keyboard (reusable text input) ──────────────────────────────

class TextInputScreen : public Screen {
 public:
  using Submit = void (*)(const char* text, void* user);
  void configure(const char* title, char* buffer, uint16_t capacity,
                 Submit onDone, void* user = nullptr);

  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenTextInput; }
  bool showCursor() const override { return false; }

 private:
  // Content layers + 6 Emoji pages (14 glyphs each, curated from
  // supported glyphs) + Help (button-binding guide, last in the
  // cycle). Emoji/Help render with different cell geometry from the
  // text rows.
  //
  // Each emoji page is its own layer so the Y cycle treats paging
  // uniformly with text layer navigation — works in Grid mode (keys)
  // and Mouse mode (same button) without needing
  // joystick-edge wrap tricks that only fire in Grid.
  enum class Layer : uint8_t {
    Lower, Upper, Digits, Symbol,
    Emoji1, Emoji2, Emoji3, Emoji4, Emoji5, Emoji6,
    Help,
    kCount
  };
  // Grid = joystick-navigates-cells; Mouse = joystick-drives-absolute
  // cursor AND above-the-keyboard-region = text-cursor insertion.
  enum class Mode  : uint8_t { Grid, Mouse };

  // Shift state. `OneShot` auto-reverts to
  // `None` after the next letter commit; `Locked` persists until
  // SHIFT is tapped again. Tap SHIFT from `None` → `OneShot`; tap
  // again within the double-tap window or hold → `Locked`; tap
  // from `Locked` → `None`.
  enum class ShiftState : uint8_t { None, OneShot, Locked };

  // Action cells on the Qwerty layout's bottom row. A cell is either
  // a plain-char glyph (`KeyAction::None`, see `kKeyGridQwerty`) or
  // one of these verbs, resolved by `actionAt(row, col)`.
  enum class KeyAction : uint8_t {
    None, Shift, LayerCycle, Emoji, Space, Backspace, Submit,
  };

  static constexpr uint8_t kGridCols = 10;
  static constexpr uint8_t kGridRows = 4;
  static constexpr uint16_t kJoyDeadband = 500;

  char* buf_ = nullptr;
  uint16_t cap_ = 0;
  uint16_t len_ = 0;
  // Byte offset into `buf_` where the next insert lands and where the
  // caret renders in the typed-buffer echo. `cursorPos_` is a BYTE
  // index, not a glyph index — UTF-8-aware navigation moves it over
  // whole codepoints. Range: [0, len_]. On `configure`/`onEnter` it
  // snaps to `len_` (append mode) so existing call sites that didn't
  // exercise inline editing keep their old behavior.
  uint16_t cursorPos_ = 0;
  // Title is COPIED on configure() rather than held as a `const char*`.
  // Callers (notably WifiScreen::onResume) build the title in a stack
  // buffer and pass it through; storing just the pointer left `title_`
  // dangling the moment the caller returned, and the next paint drew
  // whatever stack bytes happened to be there as garbage in the header
  // pill. 48 covers every current call site (longest is
  // "Pwd for <SSID up to 20 chars>" → ~30 chars) with headroom.
  char title_[48] = {};
  Submit onDone_ = nullptr;
  void* user_ = nullptr;

  Layer layer_ = Layer::Lower;
  // `mode_` is sticky across keyboard pushes: if you toggle into
  // Mouse mode, the next entry into `TextInputScreen` (within the
  // same boot) re-opens in Mouse mode. Initial default on first
  // boot is Grid. `onEnter` deliberately does NOT reset this.
  Mode  mode_  = Mode::Grid;
  ShiftState shift_ = ShiftState::None;
  uint32_t shiftTapMs_ = 0;          // last SHIFT-press timestamp, for double-tap → Locked
  uint8_t gridCol_ = 0;
  uint8_t gridRow_ = 0;
  // Grid-mode analogue of "mouse is above the keyboard" — when
  // pushing the joystick UP at `gridRow_ == 0` we lift the
  // selector off the keyboard into the composer area. Joystick
  // left/right then steps `cursorPos_` by one glyph per nav tick
  // (UTF-8-aware via `cursorMoveLeft`/`cursorMoveRight`); down
  // returns control to grid row 0. Persists across the same
  // keyboard entry; reset on `onEnter`.
  bool gridInTextCursor_ = false;

  // `emojiPage_` is implicit — derive from `layer_` via `emojiPage()`.
  // No longer a separate member since pages are first-class layers
  // in the `Layer` enum (`Emoji1..Emoji6`).
  bool isEmojiLayer() const {
    const uint8_t n = static_cast<uint8_t>(layer_);
    return n >= static_cast<uint8_t>(Layer::Emoji1) &&
           n <= static_cast<uint8_t>(Layer::Emoji6);
  }
  uint8_t emojiPage() const {
    // Returns 0..5 for the 6 emoji pages; 0 as a safe default for
    // non-emoji layers so callers don't need their own guards.
    if (!isEmojiLayer()) return 0;
    return static_cast<uint8_t>(
        static_cast<uint8_t>(layer_) -
        static_cast<uint8_t>(Layer::Emoji1));
  }
  float mouseX_ = 64.0f;
  float mouseY_ = 40.0f;

  uint32_t lastJoyMs_ = 0;
  // Y-hold state. We drive long-press detection from
  // `Inputs::heldMs()` rather than our own millis timer + `e.yPressed`
  // because `e.yPressed` gets FAKE rising edges from
  // `Inputs::applyKeyRepeat()` every ~325 ms while held — those would
  // reset any locally-tracked start timestamp. `heldMs()` reads from
  // `heldSinceMs_`, which is only touched on real press/release
  // transitions, so it's immune to key-repeat.
  bool upWasHeld_ = false;     // previous-frame physical hold state
  bool upHoldFired_ = false;   // mode-toggle already fired this hold

  // `commitChar` is the 1-byte path used by the ASCII content grids.
  // `commitBytes` is the N-byte path used by emoji (multi-byte UTF-8
  // sequences). Both insert at `cursorPos_`, advance `cursorPos_`
  // and `len_` by the inserted byte count, and clip to capacity.
  void commitChar(char c);
  void commitBytes(const char* bytes);
  // UTF-8-aware backspace: removes the glyph immediately BEFORE
  // `cursorPos_` by walking back over continuation bytes
  // (`10xxxxxx`) until a lead byte is found, then shifting.
  void backspace();
  // Walk `cursorPos_` one UTF-8 glyph in either direction, clamped
  // to [0, len_]. Used by text-cursor navigation (mouse above the
  // keyboard area).
  void cursorMoveLeft();
  void cursorMoveRight();
  void cycleLayer();           // forward: Lower → Upper → ... → Help → Lower
  void cycleEmojiPage();       // Emoji1 → ... → Emoji6 → Emoji1

  void renderQwerty(oled& d);

  // Typed-buffer echo. Renders the
  // last ~14 chars of `buf_` centered around `cursorPos_` and draws
  // a blinking caret at the cursor.
  void drawTypedBufferEcho(oled& d);

  void handleInputQwerty(const Inputs& inputs, GUIManager& gui);

  // Action-row dispatch. Returns the action for cell (row, col);
  // `KeyAction::None` means "plain character cell from `kKeyGridQwerty`".
  KeyAction actionAt(uint8_t row, uint8_t col) const;
  void doAction(KeyAction action, GUIManager& gui);

  // Apply shift state to a letter before committing. Consumes OneShot.
  char applyShift(char c);
};
