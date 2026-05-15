#ifdef BADGE_HAS_DOOM
#ifdef FEATURE_SOUND

#include <Arduino.h>
#include <cstring>
#include <cmath>

#include "doom_render.h"
#include "hardware/Haptics.h"

#define boolean doom_boolean_t
extern "C" {
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
}
#undef boolean

// Doom WAD digitized sound header (8 bytes before PCM data)
struct doom_sfx_header_t {
    uint16_t format;       // 0x0003
    uint16_t sample_rate;  // typically 11025
    uint32_t num_samples;
};

// MIDI note -> frequency LUT (octaves 0-8, 128 entries)
static const uint16_t kMidiFreq[128] = {
      8,   9,   9,  10,  10,  11,  12,  12,  13,  14,  15,  15,
     16,  17,  18,  19,  21,  22,  23,  25,  26,  28,  29,  31,
     33,  35,  37,  39,  41,  44,  46,  49,  52,  55,  58,  62,
     65,  69,  73,  78,  82,  87,  93,  98, 104, 110, 117, 123,
    131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247,
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,
   1047,1109,1175,1245,1319,1397,1480,1568,1661,1760,1865,1976,
   2093,2217,2349,2489,2637,2794,2960,3136,3322,3520,3729,3951,
   4186,4435,4699,4978,5274,5588,5920,6272,6645,7040,7459,7902,
   8372,8870,9397,9956,10548,11175,11840,12544,
};

// ── Haptic boost ─────────────────────────────────────────────────────────────
static uint32_t s_haptic_boost_until = 0;
static constexpr uint8_t kHapticBoostDuty = 128;

// ── CoilTone sound state ─────────────────────────────────────────────────────

static const uint8_t* s_pcm_data = nullptr;
static uint32_t s_pcm_len = 0;
static uint32_t s_pcm_pos = 0;
static uint16_t s_pcm_rate = 11025;
static int s_active_channel = -1;
static int s_active_priority = 0;
static uint32_t s_sound_start_ms = 0;

static bool s_sound_initialized = false;

// ── MUS music playback state ─────────────────────────────────────────────────

struct mus_header_t {
    uint8_t  id[4];        // "MUS\x1a"
    uint16_t score_len;
    uint16_t score_start;
    uint16_t primary_channels;
    uint16_t secondary_channels;
    uint16_t instrument_count;
};

static const uint8_t* s_mus_data = nullptr;
static uint32_t s_mus_len = 0;
static uint32_t s_mus_pos = 0;
static uint16_t s_mus_score_start = 0;
static bool s_mus_playing = false;
static bool s_mus_looping = false;
static uint32_t s_mus_last_tick_ms = 0;
static int32_t s_mus_delay_ms = 0;
static uint8_t s_mus_channel_vol[16];
static int8_t  s_mus_active_notes[16]; // note per channel, -1 = none
static uint8_t s_mus_channel_velocity[16];

// Which note/freq is currently sounding on the coil
static int s_mus_coil_note = -1;
static uint8_t s_mus_coil_vol = 0;

// MUS ticks per second (Doom uses 140 Hz MUS tick rate)
static constexpr int kMusTicksPerSec = 140;

// Variables required by i_sound.c when FEATURE_SOUND is enabled
extern "C" {
    int use_libsamplerate = 0;
    float libsamplerate_scale = 0.65f;
    void I_InitTimidityConfig(void) {}
}

// ── Frequency analysis for SFX ───────────────────────────────────────────────

// Temporarily boost CoilTone duty for haptic feedback
void doom_sound_haptic_boost(uint16_t duration_ms) {
    s_haptic_boost_until = millis() + duration_ms;
}

static uint8_t get_effective_duty(doom_render_settings_t* rs) {
    if (millis() < s_haptic_boost_until) return kHapticBoostDuty;
    uint8_t d = rs->sound_duty;
    return d < 1 ? 1 : d;
}

static uint32_t estimate_frequency(const uint8_t* pcm, uint32_t len,
                                   uint32_t offset, uint16_t sample_rate,
                                   uint32_t window_samples) {
    if (offset >= len) return 0;
    uint32_t end = offset + window_samples;
    if (end > len) end = len;
    if (end - offset < 8) return 0;

    int crossings = 0;
    bool prev_above = (pcm[offset] >= 128);
    for (uint32_t i = offset + 1; i < end; i++) {
        bool above = (pcm[i] >= 128);
        if (above != prev_above) {
            crossings++;
            prev_above = above;
        }
    }

    uint32_t num_samples = end - offset;
    uint32_t freq = (uint32_t)((uint64_t)crossings * sample_rate / (2 * num_samples));
    return freq;
}

static bool is_silence(const uint8_t* pcm, uint32_t len,
                       uint32_t offset, uint32_t window_samples) {
    if (offset >= len) return true;
    uint32_t end = offset + window_samples;
    if (end > len) end = len;

    uint32_t energy = 0;
    for (uint32_t i = offset; i < end; i++) {
        int dev = (int)pcm[i] - 128;
        energy += (uint32_t)(dev * dev);
    }
    return energy / (end - offset) < 16;
}

// Pick the highest-pitched active note across non-percussion MUS channels,
// ignoring bass notes below the cutoff. Always update the coil so rapid
// note changes are heard even when the winning note number doesn't change.
static constexpr int kMusBassNoteCutoff = 48; // ignore notes below C3

static void mus_update_coil(doom_render_settings_t* rs) {
    int best_note = -1;
    uint8_t best_vol = 0;

    for (int ch = 0; ch < 16; ch++) {
        if (ch == 15) continue;
        int note = s_mus_active_notes[ch];
        if (note < kMusBassNoteCutoff) continue;
        if (s_mus_channel_vol[ch] == 0) continue;
        uint8_t vol = (uint8_t)((uint16_t)s_mus_channel_vol[ch] *
                                s_mus_channel_velocity[ch] / 127);
        if (note > best_note) {
            best_note = note;
            best_vol = vol;
        }
    }

    // If no melody note, fall back to any note (so riffs that go below cutoff still play)
    if (best_note < 0) {
        for (int ch = 0; ch < 16; ch++) {
            if (ch == 15) continue;
            int note = s_mus_active_notes[ch];
            if (note < 0 || s_mus_channel_vol[ch] == 0) continue;
            uint8_t vol = (uint8_t)((uint16_t)s_mus_channel_vol[ch] *
                                    s_mus_channel_velocity[ch] / 127);
            if (note > best_note) {
                best_note = note;
                best_vol = vol;
            }
        }
    }

    if (best_note < 0 || best_vol == 0) {
        if (s_mus_coil_note >= 0) {
            if (!s_pcm_data) CoilTone::noTone();
            s_mus_coil_note = -1;
        }
        return;
    }

    if (s_pcm_data) return;

    s_mus_coil_note = best_note;
    s_mus_coil_vol = best_vol;

    uint32_t freq = (best_note < 128) ? kMidiFreq[best_note] : 440;
    int8_t oct = rs->sound_octave;
    if (oct > 0) freq <<= oct;
    else if (oct < 0) freq >>= (-oct);
    if (freq < 80) freq = 80;
    if (freq > 8000) freq = 8000;

    CoilTone::tone(freq, 0, get_effective_duty(rs));
}

// Process MUS events up to the current time
static void mus_tick(doom_render_settings_t* rs) {
    if (!s_mus_playing || !s_mus_data) return;

    uint32_t now = millis();
    int32_t dt = (int32_t)(now - s_mus_last_tick_ms);
    s_mus_last_tick_ms = now;

    s_mus_delay_ms -= dt;

    while (s_mus_delay_ms <= 0) {
        if (s_mus_pos >= s_mus_len) {
            if (s_mus_looping) {
                s_mus_pos = s_mus_score_start;
                memset(s_mus_active_notes, -1, sizeof(s_mus_active_notes));
            } else {
                s_mus_playing = false;
                memset(s_mus_active_notes, -1, sizeof(s_mus_active_notes));
                mus_update_coil(rs);
                return;
            }
        }

        uint8_t event_byte = s_mus_data[s_mus_pos++];
        uint8_t event_type = (event_byte >> 4) & 0x07;
        uint8_t channel = event_byte & 0x0F;
        bool last = (event_byte & 0x80) != 0;

        switch (event_type) {
            case 0: { // Release key
                if (s_mus_pos >= s_mus_len) break;
                s_mus_pos++; // skip note byte
                s_mus_active_notes[channel] = -1;
                break;
            }
            case 1: { // Press key
                if (s_mus_pos >= s_mus_len) break;
                uint8_t note_byte = s_mus_data[s_mus_pos++];
                bool has_vol = (note_byte & 0x80) != 0;
                uint8_t note = note_byte & 0x7F;
                if (has_vol && s_mus_pos < s_mus_len) {
                    s_mus_channel_velocity[channel] = s_mus_data[s_mus_pos++] & 0x7F;
                }
                s_mus_active_notes[channel] = note;
                break;
            }
            case 2: { // Pitch wheel (skip)
                if (s_mus_pos < s_mus_len) s_mus_pos++;
                break;
            }
            case 3: { // System event (skip)
                if (s_mus_pos < s_mus_len) s_mus_pos++;
                break;
            }
            case 4: { // Controller change
                if (s_mus_pos + 1 < s_mus_len) {
                    uint8_t ctrl = s_mus_data[s_mus_pos++];
                    uint8_t val = s_mus_data[s_mus_pos++];
                    if (ctrl == 3) { // Volume
                        s_mus_channel_vol[channel] = val;
                    }
                } else {
                    s_mus_pos = s_mus_len;
                }
                break;
            }
            case 6: { // Score end
                if (s_mus_looping) {
                    s_mus_pos = s_mus_score_start;
                    memset(s_mus_active_notes, -1, sizeof(s_mus_active_notes));
                } else {
                    s_mus_playing = false;
                    memset(s_mus_active_notes, -1, sizeof(s_mus_active_notes));
                }
                mus_update_coil(rs);
                return;
            }
            default: break;
        }

        // If this was the last event before a delay, read the delay
        if (last) {
            uint32_t delay_ticks = 0;
            while (s_mus_pos < s_mus_len) {
                uint8_t b = s_mus_data[s_mus_pos++];
                delay_ticks = (delay_ticks << 7) | (b & 0x7F);
                if (!(b & 0x80)) break;
            }
            s_mus_delay_ms += (int32_t)(delay_ticks * 1000 / kMusTicksPerSec);
            mus_update_coil(rs);
            break;
        }
    }

    mus_update_coil(rs);
}

// ── sound_module_t implementation ────────────────────────────────────────────

static doom_boolean_t ESP_SndInit(doom_boolean_t use_sfx_prefix) {
    (void)use_sfx_prefix;
    doom_render_settings_t* rs = doom_render_settings();
    if (!rs->sound_enable) return (doom_boolean_t)false;

    s_sound_initialized = true;
    Serial.println("[doom_sound] CoilTone sound initialized");
    return (doom_boolean_t)true;
}

static void ESP_SndShutdown(void) {
    CoilTone::noTone();
    s_sound_initialized = false;
    s_pcm_data = nullptr;
    s_active_channel = -1;
    s_mus_playing = false;
    Serial.println("[doom_sound] shutdown");
}

static int ESP_SndGetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "DS%s", sfx->name);
    for (char* p = namebuf; *p; p++) *p = toupper(*p);
    return W_CheckNumForName(namebuf);
}

static void ESP_SndUpdate(void) {
    if (!s_sound_initialized) return;

    doom_render_settings_t* rs = doom_render_settings();
    if (!rs->sound_enable) {
        CoilTone::noTone();
        s_pcm_data = nullptr;
        s_active_channel = -1;
        return;
    }

    // Handle active SFX playback
    if (!s_pcm_data) return;

    uint32_t elapsed_ms = millis() - s_sound_start_ms;
    uint32_t expected_pos = (uint32_t)((uint64_t)elapsed_ms * s_pcm_rate / 1000);

    if (expected_pos >= s_pcm_len) {
        s_pcm_data = nullptr;
        s_active_channel = -1;
        // Resume music if playing
        if (s_mus_playing) mus_update_coil(rs);
        else CoilTone::noTone();
        return;
    }

    s_pcm_pos = expected_pos;

    uint32_t window = s_pcm_rate / 250;
    if (window < 32) window = 32;

    if (is_silence(s_pcm_data, s_pcm_len, s_pcm_pos, window)) {
        if (s_mus_playing) mus_update_coil(rs);
        else CoilTone::noTone();
        return;
    }

    uint32_t freq = estimate_frequency(s_pcm_data, s_pcm_len,
                                       s_pcm_pos, s_pcm_rate, window);

    // Apply pitch shift from settings (sound_sample_rate as divisor: lower = lower pitch)
    int16_t pitch_adj = rs->sound_sample_rate;
    if (pitch_adj > 0) {
        freq = (uint32_t)((uint64_t)freq * pitch_adj / 11025);
    }

    if (freq < 300) freq = 300;
    if (freq > 18000) freq = 18000;

    CoilTone::tone(freq, 0, get_effective_duty(rs));
}

static void ESP_SndUpdateSoundParams(int channel, int vol, int sep) {
    (void)channel; (void)vol; (void)sep;
}

static int ESP_SndStartSound(sfxinfo_t* sfx, int channel, int vol, int sep) {
    (void)sep;
    doom_render_settings_t* rs = doom_render_settings();
    if (!rs->sound_enable || !s_sound_initialized) return -1;
    if (sfx->lumpnum < 0) return -1;

    uint8_t* lump = (uint8_t*)W_CacheLumpNum(sfx->lumpnum, PU_CACHE);
    if (!lump) return -1;

    int lump_len = W_LumpLength(sfx->lumpnum);
    if (lump_len < 8) return -1;

    doom_sfx_header_t hdr;
    memcpy(&hdr, lump, sizeof(hdr));

    if (hdr.format != 3 || hdr.num_samples == 0) return -1;
    if ((int)(8 + hdr.num_samples) > lump_len)
        hdr.num_samples = lump_len - 8;

    if (s_pcm_data && sfx->priority < s_active_priority) return -1;

    s_pcm_data = lump + 8;
    s_pcm_len = hdr.num_samples;
    s_pcm_pos = 0;
    s_pcm_rate = hdr.sample_rate;
    s_active_channel = channel;
    s_active_priority = sfx->priority;
    s_sound_start_ms = millis();

    uint32_t window = s_pcm_rate / 250;
    if (window < 32) window = 32;
    uint32_t freq = estimate_frequency(s_pcm_data, s_pcm_len, 0, s_pcm_rate, window);

    int16_t pitch_adj = rs->sound_sample_rate;
    if (pitch_adj > 0) {
        freq = (uint32_t)((uint64_t)freq * pitch_adj / 11025);
    }
    if (freq < 100) freq = 100;
    if (freq > 8000) freq = 8000;

    CoilTone::tone(freq, 0, get_effective_duty(rs));

    return channel;
}

static void ESP_SndStopSound(int channel) {
    if (channel == s_active_channel) {
        s_pcm_data = nullptr;
        s_active_channel = -1;
        doom_render_settings_t* rs = doom_render_settings();
        if (s_mus_playing) mus_update_coil(rs);
        else CoilTone::noTone();
    }
}

static doom_boolean_t ESP_SndSoundIsPlaying(int channel) {
    return (doom_boolean_t)(channel == s_active_channel && s_pcm_data != nullptr);
}

static void ESP_SndCacheSounds(sfxinfo_t* sounds, int num_sounds) {
    (void)sounds; (void)num_sounds;
}

// ── Module tables ────────────────────────────────────────────────────────────

static snddevice_t s_sound_devices[] = { SNDDEVICE_SB };

extern "C" {
sound_module_t DG_sound_module = {
    .sound_devices     = s_sound_devices,
    .num_sound_devices = 1,
    .Init              = ESP_SndInit,
    .Shutdown          = ESP_SndShutdown,
    .GetSfxLumpNum     = ESP_SndGetSfxLumpNum,
    .Update            = ESP_SndUpdate,
    .UpdateSoundParams = ESP_SndUpdateSoundParams,
    .StartSound        = ESP_SndStartSound,
    .StopSound         = ESP_SndStopSound,
    .SoundIsPlaying    = ESP_SndSoundIsPlaying,
    .CacheSounds       = ESP_SndCacheSounds,
};
}

// ── Music module — MUS parser ────────────────────────────────────────────────

static doom_boolean_t ESP_MusInit(void) {
    memset(s_mus_active_notes, -1, sizeof(s_mus_active_notes));
    memset(s_mus_channel_vol, 100, sizeof(s_mus_channel_vol));
    memset(s_mus_channel_velocity, 127, sizeof(s_mus_channel_velocity));
    return (doom_boolean_t)true;
}

static void ESP_MusShutdown(void) {
    s_mus_playing = false;
    s_mus_data = nullptr;
    if (!s_pcm_data) CoilTone::noTone();
}

static void ESP_MusSetVolume(int vol) { (void)vol; }
static void ESP_MusPause(void) { s_mus_playing = false; }
static void ESP_MusResume(void) {
    if (s_mus_data) {
        s_mus_playing = true;
        s_mus_last_tick_ms = millis();
    }
}

static void* ESP_MusRegisterSong(void* data, int len) {
    if (!data || len < (int)sizeof(mus_header_t)) return nullptr;

    mus_header_t hdr;
    memcpy(&hdr, data, sizeof(hdr));

    if (memcmp(hdr.id, "MUS\x1a", 4) != 0) return nullptr;

    // Store the raw lump data pointer
    return data;
}

static void ESP_MusUnregisterSong(void* handle) { (void)handle; }

static void ESP_MusPlaySong(void* handle, doom_boolean_t looping) {
    if (!handle) return;

    doom_render_settings_t* rs = doom_render_settings();
    if (!rs->sound_enable) return;

    mus_header_t hdr;
    memcpy(&hdr, handle, sizeof(hdr));

    s_mus_data = (const uint8_t*)handle;
    s_mus_score_start = hdr.score_start;
    s_mus_pos = hdr.score_start;
    // Estimate total length from the lump -- we don't know exactly, use score_len
    s_mus_len = hdr.score_start + hdr.score_len;
    s_mus_looping = (looping != 0);
    s_mus_delay_ms = 0;
    s_mus_last_tick_ms = millis();
    s_mus_coil_note = -1;

    memset(s_mus_active_notes, -1, sizeof(s_mus_active_notes));
    memset(s_mus_channel_vol, 100, sizeof(s_mus_channel_vol));
    memset(s_mus_channel_velocity, 127, sizeof(s_mus_channel_velocity));

    s_mus_playing = true;

    Serial.printf("[doom_sound] MUS playing, score_start=%d, score_len=%d, loop=%d\n",
                  hdr.score_start, hdr.score_len, looping);
}

static void ESP_MusStopSong(void) {
    s_mus_playing = false;
    memset(s_mus_active_notes, -1, sizeof(s_mus_active_notes));
    s_mus_coil_note = -1;
    if (!s_pcm_data) CoilTone::noTone();
}

static doom_boolean_t ESP_MusIsPlaying(void) {
    return (doom_boolean_t)s_mus_playing;
}

static void ESP_MusPoll(void) {
    doom_render_settings_t* rs = doom_render_settings();
    if (!rs->sound_enable) {
        if (s_mus_playing) {
            s_mus_playing = false;
            if (!s_pcm_data) CoilTone::noTone();
        }
        return;
    }
    mus_tick(rs);
}

static snddevice_t s_music_devices[] = { SNDDEVICE_SB };

extern "C" {
music_module_t DG_music_module = {
    .sound_devices     = s_music_devices,
    .num_sound_devices = 1,
    .Init              = ESP_MusInit,
    .Shutdown          = ESP_MusShutdown,
    .SetMusicVolume    = ESP_MusSetVolume,
    .PauseMusic        = ESP_MusPause,
    .ResumeMusic       = ESP_MusResume,
    .RegisterSong      = ESP_MusRegisterSong,
    .UnRegisterSong    = ESP_MusUnregisterSong,
    .PlaySong          = ESP_MusPlaySong,
    .StopSong          = ESP_MusStopSong,
    .MusicIsPlaying    = ESP_MusIsPlaying,
    .Poll              = ESP_MusPoll,
};
}

#endif // FEATURE_SOUND
#endif // BADGE_HAS_DOOM
