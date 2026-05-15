# Legacy Doom Design Notes

This file is an early design sketch for the Doom port. The current implemented
architecture lives in [`../src/doom/README.md`](../src/doom/README.md), and
that file should be treated as authoritative for build flags, file locations,
resource ownership, and runtime behavior.

Design notes

Doom must not call SSD1306 / IS31FL3731 drivers directly.
Input is pulled through get_input() each tick.
Tasking model (App mode)
When entering Doom mode:

Create a single doom_task pinned to a chosen core (recommend core 1) with medium priority.
Doom “owns” the app mode until exit combo triggers or firmware requests exit.
When exiting:

Signal stop flag
Wait for task to end
Ensure callbacks stop being called
Timing:

Default 20 FPS due to SSD1306 I2C bandwidth.
Use vTaskDelayUntil() for stable pacing.
Rendering pipeline (SSD1306 128×64 1‑bit)
Internal render format
Standardize to internal 8-bit luminance.

Plan

DoomGeneric provides a source frame.
Convert to uint8_t luma_src[SRC_W*SRC_H] in PSRAM.
Downsample to 128×64 luma.
Ordered dither (Bayer 8×8) to 1bpp SSD1306 format (1024 bytes).
Call oled_present().
Internal resolution
Start with:

SRC_W=160, SRC_H=100
Downsampling
Box filter / area average (avoid nearest-neighbor shimmer).

Ordered dithering
Bayer 8×8 matrix values 0..63:

threshold = bayer * 4 (0..252)
out = luma > threshold
Pack bits in SSD1306 page format:

byte index: page * 128 + x, page = y >> 3
bit: 1 << (y & 7)
Input mapping (no joystick click, 4 buttons)
Buttons bitmask from get_input():

bit0 = UP
bit1 = DOWN
bit2 = LEFT
bit3 = RIGHT
Mapping:

Joystick Y: forward/back
Joystick X: turn
UP: FIRE
DOWN: USE/OPEN
LEFT (hold): STRAFE modifier (stick X becomes strafe)
RIGHT (hold): WEAPON modifier (stick Y selects prev/next weapon)
Analog processing:

deadzone ~0.15
curve: x = sign(x) * x*x
clamp [-1..1]
Weapon selection repeat:

threshold ~0.5
debounce/repeat ~200 ms
Always run:

recommended enabled (run_by_default=1).
Exit combo (must be reliable)
Example:

Hold LEFT + RIGHT for 1500 ms => exit Doom mode
Implemented in wrapper:

track combo-held time per tick
call doom_app_exit() when reached
8×8 LED matrix HUD (IS31FL3731)
Goal: move HUD burden off OLED.

Suggested mapping:

Row 0: keys (3 pixels: R/B/Y)
Rows 1–3: health bar (0–100)
Rows 4–6: ammo bar
Row 7: flash on damage / pickup
Implementation:

v1: simple blink + damage flash via events
v2: extract real health/ammo/keys from engine state if accessible
matrix_present(rows8) where rows8[8] are bitmasks per row.

Vibration motor
Event-driven:

damage: 80 ms @ strength 200
pickup: 30 ms @ strength 80
If engine events aren’t easily accessible:

v1: vibrate on FIRE press (tiny) and/or low-health periodic warning
v2: hook real damage/pickup events
WAD loading from FAT filesystem (important)
Assumptions
The main firmware mounts FAT FS via ESP‑IDF VFS, e.g.:
SD card mounted at /sd
or wear-leveled FAT partition at /fat
Doom app receives a full WAD path, e.g. /sd/doom/doom1.wad.
Implementation approach (v1)
Use standard C stdio through VFS:

FILE *f = fopen(cfg->wad_path, "rb");
fread, fseek, ftell, fclose
This is the simplest and most “library-like” because:

Doom app doesn’t mount FAT; it only consumes a path.
The main firmware can swap storage backend without changing Doom code (as long as VFS paths work).
Optional v2: WAD discovery
If wad_base_dir is provided (e.g. /sd/doom):

implement a simple directory scan (opendir/readdir)
allow selecting WAD by name before starting Doom mode (Do not implement UI selection inside Doom component unless explicitly requested; keep it as helper functions.)
DoomGeneric HAL implementation plan
Implement required platform functions in doom_hal.c (exact names depend on DoomGeneric version; adapt accordingly):

Timing:
ticks ms from esp_timer_get_time()/1000
Sleep:
vTaskDelay(pdMS_TO_TICKS(ms))
Drawing/frame ready hook:
convert -> downsample -> dither -> oled_present()
update matrix HUD
Input:
call cfg->get_input() and map to Doom key states
File I/O:
open/read/seek/close through stdio for cfg->wad_path
Keep strict layering

doom_hal.c talks to DoomGeneric + calls into doom_render.c, doom_hud_matrix.c, and callback pointers.
Memory placement
Use PSRAM for large buffers:

luma_src (SRC_W*SRC_H)
optional intermediate luma buffers
SSD1306 1bpp framebuffer (1024 bytes):

can be internal RAM; PSRAM acceptable.
Use:

heap_caps_malloc(size, MALLOC_CAP_SPIRAM) for big allocations
Implementation steps (for the implementing agent)
Create component skeleton + public API.
Vendor DoomGeneric (or add as subcomponent).
Implement doom_task with enter/exit lifecycle.
Implement input mapping + exit combo.
Implement rendering: luma conversion, downsample, Bayer dither, SSD1306 packing.
Implement matrix HUD (minimal v1 ok).
Implement vibration events (minimal v1 ok).
Implement WAD open via FAT FS path (cfg->wad_path) using stdio.
Verify repeated enter/exit without leaks or I2C lockups.
Acceptance criteria
Doom mode loads WAD from FAT path and starts.
OLED displays dithered frames at ~20 FPS.
Controls work per mapping.
Exit combo returns to main firmware reliably.
WAD is read via VFS path; Doom component does not mount/unmount FAT.
No heap growth across repeated enter/exit.
Performance notes
Full-frame SSD1306 updates are bandwidth-limited; 20 FPS is realistic at 400 kHz I2C.
Optimize dithering loops; avoid floats inside inner pixel loops.
Consider 1 MHz I2C or dirty-page updates if needed later.




# Doom “App Mode” on ESP32‑S3 (ESP‑IDF) — Library‑like Integration Design

## Goal
Integrate a Doom port on ESP32‑S3 (ESP32‑S3‑WROOM‑1‑N16R8 / “16n8” with PSRAM) as an **app mode** inside an existing ESP‑IDF firmware.

**Key requirements**
- Doom must be **library-like**: it must not own/initialize core peripherals globally beyond what the Doom module needs internally.
- The existing firmware already has handlers for:
  - SSD1306 128×64 monochrome OLED over I2C
  - IS31FL3731 8×8 monochrome LED matrix (I2C)
  - 2-axis analog joystick (2 ADC channels)
  - 4 D-pad buttons (GPIO)
  - vibration motor (GPIO/PWM/etc)
  - IMU (optional)
- Doom runs as an **app mode**: when entered, it takes over the display + inputs; when exited, all resources are cleanly released and control returns to the main firmware.
- Use latest ESP‑IDF.
- WADs should be stored on and loaded from an existing **FAT filesystem** (e.g. SD card or FAT partition mounted via ESP‑IDF VFS).

**Recommended codebase direction**
- Use a **DoomGeneric**-based port (example known-good S3 + ESP-IDF structure):
  https://github.com/Komedenden/esp32-s3-doom-port
  (Use as reference; implement a clean component wrapper and HAL bindings.)

---

## High-level architecture

### Modules
Create a new ESP‑IDF component:

- `components/doom_app/`
  - `include/doom_app.h` (public API)
  - `doom_app.c` (mode lifecycle + task + glue)
  - `doom_hal.c` (DoomGeneric HAL: time, input, draw, file I/O)
  - `doom_render.c` (downsample + ordered dithering to SSD1306 1‑bit framebuffer)
  - `doom_hud_matrix.c` (8×8 LED matrix HUD)
  - `doom_events.c` (vibration + optional IMU mapping)
  - `doom_wad.c` (WAD selection + file open helpers)
  - `CMakeLists.txt`

Bring DoomGeneric sources as either:
- a subcomponent `components/doomgeneric/` (preferred), OR
- vendored inside `components/doom_app/doomgeneric/` if you want a single component.

**Important boundary**
- Doom module produces “frame” + “events”
- Existing firmware owns actual device drivers and passes function pointers / callbacks.

---

## Public API (library-like)

### `doom_app.h`
Expose an API suitable for app-mode switching and WAD selection.

```c
typedef struct {
    // Display callbacks into existing firmware
    // Called with SSD1306 framebuffer: 1024 bytes (128*64/8)
    void (*oled_present)(const uint8_t *fb_1bpp_128x64, void *user);
    void *oled_user;

    // LED matrix HUD
    void (*matrix_present)(const uint8_t pixels_8x8[8], void *user); // 8 bytes bitmask rows
    void *matrix_user;

    // Vibration control (simple)
    void (*vibrate)(uint32_t duration_ms, uint8_t strength_0_255, void *user);
    void *vibrate_user;

    // Input providers (polling)
    void (*get_input)(void *user, float *joy_x, float *joy_y, uint8_t *buttons_bitmask);
    void *input_user;

    // Optional: IMU provider (can be NULL)
    void (*get_imu)(void *user, float *roll, float *pitch, float *yaw);
    void *imu_user;

    // WAD source via FAT FS path (preferred)
    // Example mount: "/sd", wad: "/sd/doom/doom1.wad"
    const char *wad_path;

    // App tuning
    uint32_t target_fps;          // e.g. 20
    uint8_t  run_by_default;      // 1 = always run
    uint8_t  allow_exit_combo;    // 1 = enable exit combo

    // Optional: base directory for browsing WADs later (v2 feature)
    const char *wad_base_dir;     // e.g. "/sd/doom"
} doom_app_config_t;

typedef enum {
    DOOM_APP_OK = 0,
    DOOM_APP_ERR = -1,
} doom_app_status_t;

doom_app_status_t doom_app_init(const doom_app_config_t *cfg);
doom_app_status_t doom_app_enter(void);
doom_app_status_t doom_app_exit(void);
doom_app_status_t doom_app_deinit(void);

bool doom_app_is_running(void);
