#include "../../hardware/Inputs.h"

#include "temporalbadge_runtime.h"

extern Inputs inputs;

// ── Buttons ─────────────────────────────────────────────────────────────────

namespace {

constexpr int kBtnRight = 0;
constexpr int kBtnDown = 1;
constexpr int kBtnLeft = 2;
constexpr int kBtnUp = 3;
constexpr int kBtnConfirm = 4;
constexpr int kBtnCancel = 5;

bool physicalHwIndexForButtonId(int button_id, uint8_t* hw_idx) {
    if (!hw_idx) return false;
    switch (button_id)
    {
    case kBtnRight:
        *hw_idx = 3;
        return true;
    case kBtnDown:
        *hw_idx = 1;
        return true;
    case kBtnLeft:
        *hw_idx = 2;
        return true;
    case kBtnUp:
        *hw_idx = 0;
        return true;
    case kBtnConfirm:
        *hw_idx = inputs.confirmCancelSwapped() ? 3 : 1;
        return true;
    case kBtnCancel:
        *hw_idx = inputs.confirmCancelSwapped() ? 1 : 3;
        return true;
    default:
        return false;
    }
}

}  // namespace

extern "C" int temporalbadge_runtime_button_state(int button_id)
{
    inputs.service();
    const Inputs::ButtonStates &s = inputs.buttons();
    switch (button_id)
    {
    case kBtnRight:
        return s.right ? 1 : 0;
    case kBtnDown:
        return s.down ? 1 : 0;
    case kBtnLeft:
        return s.left ? 1 : 0;
    case kBtnUp:
        return s.up ? 1 : 0;
    case kBtnConfirm:
        return s.confirm ? 1 : 0;
    case kBtnCancel:
        return s.cancel ? 1 : 0;
    default:
        return -1;
    }
}

extern "C" int temporalbadge_runtime_button_pressed(int button_id)
{
    inputs.service();
    const Inputs::ButtonEdges &e = inputs.edges();
    bool pressed = false;
    uint8_t hw_idx = 0xFF;
    switch (button_id)
    {
    case kBtnRight:
        pressed = e.rightPressed;
        hw_idx = 3;
        break;
    case kBtnDown:
        pressed = e.downPressed;
        hw_idx = 1;
        break;
    case kBtnLeft:
        pressed = e.leftPressed;
        hw_idx = 2;
        break;
    case kBtnUp:
        pressed = e.upPressed;
        hw_idx = 0;
        break;
    case kBtnConfirm:
        pressed = e.confirmPressed;
        hw_idx = inputs.confirmCancelSwapped() ? 3 : 1;
        break;
    case kBtnCancel:
        pressed = e.cancelPressed;
        hw_idx = inputs.confirmCancelSwapped() ? 1 : 3;
        break;
    default:
        return -1;
    }
    if (pressed)
    {
        inputs.clearPressEdge(hw_idx);
    }
    return pressed ? 1 : 0;
}

extern "C" int temporalbadge_runtime_button_held_ms(int button_id)
{
    inputs.service();
    uint8_t hw_idx = 0xFF;
    if (!physicalHwIndexForButtonId(button_id, &hw_idx)) {
        return -1;
    }
    return (int)inputs.heldMs(hw_idx);
}

// ── Joystick ────────────────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_joy_x(void)
{
    inputs.service();
    return inputs.joyX();
}

extern "C" int temporalbadge_runtime_joy_y(void)
{
    inputs.service();
    return inputs.joyY();
}
