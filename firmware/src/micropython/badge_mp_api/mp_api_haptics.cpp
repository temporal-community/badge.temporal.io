#include <stdint.h>

#include "../../hardware/Haptics.h"

#include "temporalbadge_runtime.h"

// ── Haptics ─────────────────────────────────────────────────────────────────

extern "C" void temporalbadge_runtime_haptic_pulse(int strength, int duration_ms, int freq_hz)
{
    Haptics::shortPulse((int16_t)strength, (int16_t)duration_ms, (int32_t)freq_hz);
}

extern "C" int temporalbadge_runtime_haptic_strength(void)
{
    return Haptics::strength();
}

extern "C" void temporalbadge_runtime_haptic_set_strength(int value)
{
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255;
    Haptics::setStrength((uint8_t)value);
}

extern "C" void temporalbadge_runtime_haptic_off(void)
{
    Haptics::off();
}

extern "C" void temporalbadge_runtime_tone(int freq_hz, int duration_ms, int duty)
{
    if (freq_hz <= 0)
        return;
    uint16_t dur = (duration_ms >= 0) ? (uint16_t)duration_ms : 0;
    uint8_t d = (duty >= 0) ? (uint8_t)duty : CoilTone::kDefaultDuty;
    CoilTone::tone((uint32_t)freq_hz, dur, d);
}

extern "C" void temporalbadge_runtime_no_tone(void)
{
    CoilTone::noTone();
}

extern "C" int temporalbadge_runtime_tone_playing(void)
{
    return CoilTone::isPlaying() ? 1 : 0;
}
