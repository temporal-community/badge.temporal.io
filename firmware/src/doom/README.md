# Doom Port — Architecture Reference

Doom runs on the Temporal Badge (ESP32-S3, 128x64 1-bit OLED, 8x8 LED matrix, 4 buttons + joystick, haptic motor). This document covers the port structure so a new chat can continue development.

---

## Build

```bash
~/.platformio/penv/bin/pio run -e echo          # build
~/.platformio/penv/bin/pio run -e echo -t upload # flash
~/.platformio/penv/bin/pio run -e echo -t compiledb  # clangd
```

Environment `echo` includes Doom. `echo-doom` is kept as a compatibility alias that extends
`echo-dev` and enables the internal debug menu. Key build flags:
- `-DBADGE_HAS_DOOM` — gates all doom code with `#ifdef`
- `-DFEATURE_SOUND` — enables the I2S PDM sound module (`DG_sound_module` / `DG_music_module`)
- `-DDOOMGENERIC_RESX=160 -DDOOMGENERIC_RESY=100` — internal render resolution (must be in `platformio.ini` build_flags, NOT just `library.json`, because sources are included via `build_src_filter` not LDF)
- `-Ilib/doomgeneric/src` — include path for doom headers

WAD file: `doom1.wad` lives on the badge's FAT filesystem at `/doom1.wad`. Upload via `pio run -e echo -t uploadfs` (place a local copy in `firmware/data/`).

---

## File Map

### Badge-side (`firmware/src/doom/`)

| File | Role |
|------|------|
| `DoomScreen.h/.cpp` | GUI screen (help, settings, launch). Registered as `kScreenDoom` in GUI.cpp. Three phases: `kHelp` → `kSettings` → `kRunning`. |
| `doom_app.h/.cpp` | Lifecycle manager. `doom_app_init/enter/exit/deinit`. Spawns the doom task on Core 1 (`kDoomTaskStack=32KB`, priority 5). Uses `setjmp/longjmp` for engine exit. |
| `doom_hal.cpp` | DoomGeneric HAL callbacks (`DG_DrawFrame`, `DG_GetKey`, `DG_SleepMs`, `DG_GetTicksMs`, `DG_Init`). Reads player state for HUD + haptics. Extracts `doom_pending_message` for matrix text. |
| `doom_render.h/.cpp` | RGBA→1bpp pipeline: box-filter downsample 160x100→128x64, gamma LUT, dithering (off/2x2/4x4), float auto-gamma. All settings in `doom_render_settings_t`. |
| `doom_input.h/.cpp` | Button/joystick→Doom key queue. Reads GPIOs directly (scheduler is paused). Exit combo: L+R hold 1.5s. |
| `doom_hud_matrix.h/.cpp` | 8x8 LED matrix HUD with icon-shaped health/armor/ammo/key pages + scrolling pickup text. |
| `doom_sound.cpp` | I2S PDM sound module. Implements `DG_sound_module` (WAD PCM → I2S on motor pin) and stub `DG_music_module`. |

### Engine (`firmware/lib/doomgeneric/src/`)

Vendored DoomGeneric. Key modifications:

| File | Change |
|------|--------|
| `i_video.h` | `SCREENWIDTH=160 SCREENHEIGHT=100` for `ESP_PLATFORM` |
| `r_main.c` | Clamped `scaledviewwidth/viewheight` to prevent buffer overflow when `setblocks*32 > SCREENWIDTH` |
| `v_video.c` | `V_DrawPatch/V_DrawPatchFlipped/V_CopyRect/V_DrawBlock/V_DrawRawScreen` — all clip or scale from 320x200 patch coords to 160x100 screen. `V_ORIGWIDTH=320 V_ORIGHEIGHT=200` constants. |
| `doom_esp32_mem.c` | PSRAM allocator for `viewangletox`, `visplanes`, `openings`, `drawsegs`, `states`, `mobjinfo` |
| `doom_esp32_file.c` | FAT filesystem file I/O |
| `doomgeneric.c` | `DG_ScreenBuffer` allocated from PSRAM |
| `hu_stuff.c` | One-line hook: copies `plr->message` to `doom_pending_message` before clearing, for LED matrix scroll text |
| `i_sound.c` | SDL_mixer include guarded with `!defined(ESP_PLATFORM)` |

---

## Rendering Pipeline

```
Doom 3D renderer → I_VideoBuffer (160x100, 8bpp indexed)
        ↓ I_FinishUpdate (palette lookup)
DG_ScreenBuffer (160x100, RGBA32, PSRAM)
        ↓ doom_render_frame()
    ┌─ box-filter downsample to 128x64 luma
    ├─ gamma LUT (gamma_low lifts shadows, gamma_high compresses/darkens highlights)
    ├─ float auto-gamma: threshold converges toward target white% via proportional gain
    │   • gameplay: targets 50% white/black
    │   • splash: targets splash_target% (separately tunable)
    │   • gain controlled by auto_gamma_speed (default 0.03)
    └─ quantize: threshold (sharp) or Bayer 2x2/4x4 dither → 1bpp SSD1306 page format
        ↓ DoomScreen::oledPresentCb
    memcpy to U8G2 buffer → sendBuffer()
```

Three render hints control behavior:
- `DOOM_HINT_GAMEPLAY` — full frame, uses configured dither mode + auto-gamma (target 50%)
- `DOOM_HINT_MENU` — crops to center region (configurable zoom 1.25x–3x), sharp threshold
- `DOOM_HINT_SPLASH` — full frame, uses splash dither mode + auto-gamma (target `splash_target%`)

---

## Render Settings (`doom_render_settings_t`)

All live-adjustable via `doom_render_settings()` pointer. UI in DoomScreen settings phase.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `dither` | enum | OFF | Gameplay: Off (threshold), 2x2, 4x4 |
| `splash_dither` | enum | OFF | Title/intermission dither mode |
| `gamma_low` | u8 0-4 | 3 | Shadow lift: 0=none, 4=extreme (pow exponents: 1.0, 0.7, 0.5, 0.35, 0.25) |
| `gamma_high` | i8 -4..4 | -1 | Highlights: negative=darken, 0=linear, positive=brighten |
| `threshold` | u8 | 120 | Binary cutoff when dither is off (0-255) |
| `menu_zoom` | u8 0-4 | 1 | 0=off, 1=1.25x, 2=1.5x, 3=2x, 4=3x |
| `auto_gamma` | u8 | 1 | Auto-adjust threshold per frame |
| `auto_gamma_speed` | float | 0.03 | Proportional gain for threshold controller (0.005-0.20) |
| `splash_target` | u8 | 50 | Target white% for splash screens (0=use gameplay 50%) |
| `haptic_fire` | u8 | 200 | Motor strength on weapon fire (0=off) |
| `haptic_dmg` | u8 | 255 | Motor strength on damage taken |
| `haptic_use` | u8 | 80 | Motor strength on door/switch use |
| `haptic_freq` | i32 | 0 | Motor PWM Hz (0=default) |
| `sound_enable` | u8 | 0 | I2S PDM sound output (0=off, 1=on) |
| `sound_volume` | u8 | 180 | PCM amplitude scale (0-255) |
| `sound_sample_rate` | u16 | 0 | Playback rate override (0=use lump rate, typically 11025) |
| `sound_pdm_duty` | u8 | 20 | PDM signal scale to limit motor spin (1-60) |

---

## Input Mapping

The pre-launch Doom screen still runs inside the badge GUI, so Play/Back use
the badge's semantic confirm/cancel aliases and Settings uses fixed X. Once
Doom starts, the Doom task reads GPIOs/ADC directly (Inputs service is
suspended) and uses physical badge buttons rather than the badge GUI aliases.
The tutorial's in-game controls therefore label the physical Y/A/X/B controls
that Doom will actually use after launch.

| Hardware | Doom Key | Context |
|----------|----------|---------|
| Joystick X | `KEY_LEFTARROW/RIGHTARROW` | Turn (gameplay), menu nav |
| Joystick Y | `KEY_UPARROW/DOWNARROW` | Move (gameplay), menu nav |
| UP button | `KEY_FIRE` | Shoot |
| DOWN button | `KEY_ENTER` + `KEY_USE` | Menu select + open doors |
| LEFT button | `KEY_STRAFE_L` | Strafe left |
| RIGHT button | `KEY_STRAFE_R` | Strafe right |
| UP+DOWN | `KEY_ESCAPE` | Menu back/dismiss |
| RIGHT hold + Joy Y | weapon cycle keys `'4'/'6'` | Cycle weapons |
| LEFT+RIGHT 1.5s | `doom_app_request_stop()` | Exit doom |

---

## Lifecycle & Service Management

### Launch (`DoomScreen::launchDoom`)
1. `gui.deactivate()` — stops GUI rendering
2. `doom_resources_enter()` — claims badge resources for Doom:
   - performance mode: 240 MHz, no PM light sleep while active
   - WiFi disconnected/off
   - IR hardware stopped/restored later (Doom sound uses `IR_TX_PIN` for I2S clock)
   - Inputs interrupts suspended; Doom reads GPIO/ADC directly
   - GUI/OLED plus background haptics, LED, IMU, sleep, battery, config, and file-browser services paused
3. `doom_app_init()` → allocates PSRAM, inits render/input/HUD
4. `doom_app_enter()` → spawns doom task on Core 1

### Frame loop (`DG_DrawFrame`, called by doom task)
1. Check `doom_pending_message` → `doom_hud_set_message()` for LED text scroll
2. `doom_render_frame()` — RGBA→1bpp
3. `oled_present()` — push to OLED
4. Read player state → `doom_hud_update(health, armor, ammo, keys)` (LED matrix)
5. Haptics: fire (extralight edge), damage (damagecount increase), use (usedown edge)
6. `doom_input_poll()` — fill key queue
7. `Haptics::checkPulseEnd()`

### Exit (detected in `main.cpp loop()`)
1. `doom_app_exit()` — signals stop, waits for task, frees PSRAM
2. `doom_app_deinit()` — clears config
3. `doom_resources_exit()` — restores scheduler policy, services, IR state, input interrupts, WiFi service, LED ambient ownership, and normal PM policy
4. `Haptics::off()`
5. Resync input GPIO state, then spin until all buttons are released
   (drain L+R exit combo) plus a short quiet window
6. `guiManager.activate()` → pushes grid menu

---

## LED Matrix HUD (`doom_hud_matrix.cpp`)

Reads real engine state from `doom_hal.cpp`. Two modes:

### Normal HUD mode

```
Page 0: filled heart meter for health
Page 1: filled shield meter for armor
Page 2: filled ammo glyph meter for current weapon ammo
Page 3: three key slots (R/B/Y as left/center/right columns)
```

- The idle HUD cycles every 1.1 seconds so each resource gets the full 8x8
  matrix instead of competing with tiny labels.
- Any state change temporarily switches to the relevant page: health for damage
  or healing, armor for armor changes, ammo for firing/reloads/pickups, and keys
  for key changes.
- Health uses the same heart-shaped visual language as health pickup flashes.
- Armor uses a shield, ammo uses a round/shell glyph, and keys use three stable
  slots so ownership is readable at a glance.
- Low health pulses the heart-page corners to be noticeable without stealing the
  whole matrix.
- Non-zero values always get at least one filled LED so low-but-present resources
  do not disappear.

### Scrolling text mode

When the engine sets `doom_pending_message` (pickup, cheat, status), the matrix switches to full-height (8px tall) text scrolling left at 18 px/sec. Uses a compact embedded font (A-Z, 0-9, punctuation). After the text scrolls off, reverts to normal HUD.

Message extraction: `hu_stuff.c` copies `plr->message` to `doom_pending_message` before clearing. `DG_DrawFrame` picks it up and calls `doom_hud_set_message()`.

---

## Sound System (`doom_sound.cpp`)

I2S PDM output through the motor coil, treating it as a crude 1-bit speaker.

### Architecture

- Implements `DG_sound_module` (Chocolate Doom's `sound_module_t` interface)
- `DG_music_module` is a no-op stub (no music playback)
- On `Init()`: tears down MCPWM haptics, configures I2S PDM TX on `MOTOR_PIN` (GPIO 6) with clock on `IR_TX_PIN` (GPIO 2, unused during Doom)
- On `Shutdown()`: destroys I2S channel

### WAD sound lump format

Doom digitized sounds (`DS*` prefix) have an 8-byte header:
- Bytes 0-1: format tag (0x0003)
- Bytes 2-3: sample rate (typically 11025 Hz)
- Bytes 4-7: sample count
- Bytes 8+: raw 8-bit unsigned mono PCM

`StartSound` resolves the lump via `W_CacheLumpNum`, parses the header, and feeds PCM to the I2S DMA buffer. Samples are converted from 8-bit unsigned to 16-bit signed, scaled by `sound_volume` and `sound_pdm_duty`.

### Tuning

- `sound_enable`: master on/off (default off — enable in settings menu)
- `sound_volume`: amplitude scale (0-255)
- `sound_sample_rate`: override playback rate (0=use lump rate). Higher = pitch up, lower = pitch down.
- `sound_pdm_duty`: overall signal attenuation to prevent motor spinning vs just making noise (1-60)
- Single-channel: new sounds preempt old ones by priority

---

## Key Gotchas

- **`library.json` defines are ignored** — doomgeneric sources compile via `build_src_filter`, not LDF. Resolution defines MUST be in `platformio.ini` `build_flags`.
- **`boolean` typedef clash** — Doom's `unsigned int boolean` conflicts with Arduino's `bool boolean`. Use `#define boolean doom_boolean_t` / `#undef boolean` when including doom headers from C++ files.
- **`setblocks*32` overflow** — original formula for `SCREENWIDTH=320`. Clamped in `r_main.c` to prevent `xtoviewangle[]` buffer overflow that corrupted `viewangletox` pointer.
- **V_DrawPatch scaling** — all patch drawing scales from 320x200 to 160x100 with nearest-neighbor. Clips to screen bounds instead of `I_Error`.
- **Doom resource mode** pauses scheduler services and main-loop polling while Doom runs. Doom owns OLED, matrix, input pins, haptics/sound, and the IR TX pin until exit restores the normal services.
- **Core affinity** — doom task runs on Core 1. Main loop (Core 0) skips normal polling and only monitors `doom_app_stop_requested()` for the exit path.
- **PSRAM** — `DG_ScreenBuffer` (64KB), zone memory (200KB), `viewangletox`/`visplanes`/`openings`/`drawsegs`/`states`/`mobjinfo` all in PSRAM.
- **I2S vs MCPWM** — sound module tears down MCPWM on MOTOR_PIN and claims it for I2S PDM. Haptic pulses won't work while sound is enabled (they use MCPWM). Could be unified by injecting max-amplitude bursts into the I2S buffer.

---

## Potential Future Work

- Extract and overlay Doom menu text using U8G2 fonts for maximum readability
- Unified haptic+sound path (inject vibration pulses into I2S buffer)
- Save/load game state to FAT filesystem
- Cheat code input via button combos
- Automap rendering optimized for 1bpp
- Settings persistence to NVS
