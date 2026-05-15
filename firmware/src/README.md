# firmware/src — orientation map

The badge's C++ firmware. Domain-organized: each top-level folder owns one
subsystem. This file is the public "where do I look" map.

## Folder map

| Folder | Owns |
|--------|------|
| `api/` | Network helpers for OTA, Community Apps, and explicit MicroPython `badge.http_get/post` calls. |
| `boops/` | Local IR contact exchange. Public facade in `BadgeBoops.h`. Implementation split: `BoopsJournal.cpp` (`/boops.json` + local writes), `BoopsProtocol.cpp` (state machine + frame codec), `BoopsHandlers.cpp` (per-type ops table for peer / exhibit / queue / kiosk / checkin / unknown), `BoopsFeedback.cpp` (marquee event ring, haptic/LED cues). `Internal.h` exposes intra-`boops/` symbols. |
| `hardware/` | Peripheral drivers + board definitions. `oled.cpp` (U8G2 wrapper), `LEDmatrix.{cpp,h}` + `LEDmatrixImages.h`, `Inputs.{cpp,h}`, `IMU.{cpp,h}`, `Haptics.{cpp,h}`, `Power.{cpp,h}`, `qrcode.{c,h}`, and `HardwareConfig.h`. |
| `hw/` | Lower-level NEC IR protocol (RX/TX state machines, encoder/decoder). Used by `ir/BadgeIR.cpp`. Kept separate because the files are protocol-abstraction-pure with no other dependencies. |
| `identity/` | Per-badge state. `BadgeUID.{cpp,h}` (eFuse MAC + UUID v5), `BadgeInfo.{cpp,h}` (`/badgeInfo.json` defaults + read-only UI source), `BadgeVersion.h`. |
| `infra/` | Process plumbing. `Scheduler.{cpp,h}` (cooperative service loop), `Filesystem.{cpp,h}` (FAT helpers + atomic writes), `BadgeConfig.{cpp,h}` (settings + font-family table), `Bitops.h`, `DebugLog.h`. |
| `ir/` | High-level IR transport. `BadgeIR.{cpp,h}` (Core 0 task + python queue + multi-word frame API for boop pairing). |
| `led/` | Saved ambient LED matrix runtime. `LEDAppRuntime.{h,cpp}` owns the persisted LED mode, poster frames for OLED previews, native animations, Game of Life/custom patterns, and foreground override restoration. |
| `messaging/` | Reserved for future local messaging surfaces. Native ping-backed messaging was removed for the offline firmware. |
| `micropython/` | C++ side of the MicroPython bridge. `ReplayMicropythonAPI.cpp` (thin pump aggregator), `MicroPythonBridge.cpp` (lifecycle + GC around foreground app runs), `StartupFiles.{cpp,h}` (generated startup files provisioned onto the FatFS `ffat` partition). `badge_mp_api/` (subdir) holds the per-subsystem HAL: `mp_api_display.cpp` / `mp_api_input.cpp` / `mp_api_led.cpp` / `mp_api_imu.cpp` / `mp_api_haptics.cpp` / `mp_api_mouse.cpp` / `mp_api_ir.cpp` / `mp_api_badge_data.cpp` / `mp_api_dev.cpp` plus shared `Internal.h`. The Python-side usermod itself (QSTR table, MP_DEFINE_CONST_FUN_OBJ_*, and `matrix_app_*` ambient LED bridge) lives outside `firmware/src/` at `firmware/micropython/usermods/temporalbadge/`. |
| `screens/` | GUI screens. Base class in `Screen.{h,cpp}`; one TU per screen (`BoopScreen.cpp`, `TextInputScreen.cpp`, `LEDScreen.cpp`, etc.). `EmojiText.{h,cpp}` + `TextInputLayouts.{h,cpp}` are shared screen helpers. |
| `ui/` | Display rendering layer above hardware drivers. `GUI.{cpp,h}` (GUIManager + screen stack), `BadgeDisplay.{cpp,h}` (legacy render mode dispatch + nametag overlay), `OLEDLayout.{h,cpp}` (shared OLED bands, selected rows, footers, fit helpers), `ButtonGlyphs.{h,cpp}` (shape-based button hints), `QRCodePlate.{h,cpp}` (scan-friendly OLED QR plate), `FontCatalog.{cpp,h}` (u8g2 font tables), `Images.h`, `graphics.h`. |
| `main.cpp` | Setup + loop entry. Stays at root. |
| `BadgeGlobals.cpp` | Global instance definitions for `oled badgeDisplay`, `Inputs inputs`, etc. Stays at root. |

## Offline Runtime

The badge boots straight into the local UI. There is no QR pairing gate, no
private Replay API transport, and no ping polling.

Contacts exchanged over IR are written to `/boops.json`. The badge owner's
editable identity lives in `/badgeInfo.json`; the badge UI only reads it.
Users update it over USB and can read `https://badge.temporal.io` for update
instructions.

Schedule, map, visual assets, MicroPython apps, GitHub-backed OTA, Community
Apps, and explicit hacker HTTP helpers remain available.


## How do I…

| Task | Files to touch |
|------|----------------|
| Add a new screen | `screens/Screen.h` (extend the base class), create `screens/<Name>Screen.{h,cpp}`, register in `ui/GUI.cpp:GUIManager::begin()`, add a `ScreenId` enum entry, hook into the main menu in `ui/GUI.cpp` if user-facing |
| Add a Python `badge.<thing>` API | `micropython/badge_mp_api/<category>.cpp` for the `temporalbadge_runtime_*` impl, then declare in `firmware/micropython/usermods/temporalbadge/temporalbadge_runtime.h`, wire the HAL pass-through in `temporalbadge_hal.{c,h}`, register the Python binding in `modtemporalbadge.c` (MP_DEFINE_CONST_FUN_OBJ_* + the globals table), and regenerate the MicroPython QSTR header if new Python-visible names were added |
| Add explicit MicroPython HTTP behavior | Keep it in `micropython/badge_mp_api/mp_api_http.cpp` or Python app code. Do not add background badge-owned API polling. |
| Add a native-backed Python UI helper | Put shared rendering in `ui/OLEDLayout.{h,cpp}`, `ui/ButtonGlyphs.{h,cpp}`, or `ui/QRCodePlate.{h,cpp}` first, expose a tiny `temporalbadge_runtime_ui_*` wrapper from `micropython/badge_mp_api/mp_api_display.cpp`, then make `initial_filesystem/lib/badge_ui.py` call that helper with a Python fallback |
| Add a new boop handler | Add a per-handler block in `boops/BoopsHandlers.cpp` (the `BOOP_HANDLER` macro takes Cap, lower, label, doFieldExchange), implement the four `<lower>_<callback>` functions, add a `BoopType` enum entry |
| Use the message keyboard | `screens/TextInputScreen.cpp` owns text entry, emoji pages, and joystick cursor movement over the keyboard |
| Change ambient LED behavior | `led/LEDAppRuntime.{h,cpp}` owns the persisted mode and restore path; `screens/LEDScreen.cpp` owns the OLED carousel/editor; MicroPython foreground drawing must use `led_override_begin()` / `led_override_end()` or `matrix_app_start()` |
| Bump a font / image | `ui/FontCatalog.cpp` for the font grid + flat catalog (the `fonts/` directory under `firmware/src/` is gone — fonts are u8g2 built-ins now), `ui/Images.h` for bitmaps |
| Debug IR/boops over serial | Flip `log_ir` or `log_boop` to `1` in `settings.txt`, then watch `firmware/serial_log.py` |
| Trace a runtime crash | Look at `[boot] reset_reason=N` on the next boot's serial. Decode addresses via `~/.platformio/penv/bin/pio device monitor` paired with `~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp-elf-addr2line -pfiaC -e firmware/.pio/build/replay2026/firmware.elf <addrs...>` |

## Threading model

- **Core 0** runs `irTask` (which drives `boops/BoopsProtocol.cpp`'s state machine) and the IR low-level RMT decoders in `hw/ir/`.
- **Core 1** runs the Arduino `loop()` task — GUI, explicit MicroPython HTTP when requested, and journal saves. **24 KB stack** via `SET_LOOP_TASK_STACK_SIZE(24*1024)` in `main.cpp`; keep the headroom for MicroPython and optional HTTP paths.
- `LEDAppRuntime` is the single long-lived ambient LED owner. Foreground surfaces such as the keyboard and MicroPython apps enter an override, draw, and then release back to the saved LED app state.
- The MicroPython `mpy_service_pump()` (in `micropython/ReplayMicropythonAPI.cpp`) runs while Python code is sleeping or polling; it services inputs, OLED/LED ownership, the mouse overlay, IMU, and haptics so apps stay responsive.

## Build / flash

The Temporal flash worker is the authoritative compile gate (local `.pio/build/<env>/` cache has masked header-dep failures during multiple refactors):
```bash
cd ignition && yes '' | ./start.sh -e replay2026
```

For local sanity check before flashing, force a clean rebuild:
```bash
cd firmware && ./build.sh replay2026 -n
```

## MicroPython app workflow

App source lives under `firmware/initial_filesystem/`, not `firmware/src/`.
When those files change, run:

```bash
cd firmware
black initial_filesystem/apps/<app> initial_filesystem/lib/badge_app.py initial_filesystem/lib/badge_ui.py
python3 -m py_compile initial_filesystem/apps/<app>/*.py initial_filesystem/lib/badge_app.py initial_filesystem/lib/badge_ui.py
python3 scripts/generate_startup_files.py
pio run -e replay2026
```

Commit the source files under `initial_filesystem/`; PlatformIO regenerates
the startup header and manifest during the build. Normal firmware does not show
the generic Apps menu; attendee-facing MicroPython apps need a `GUI.cpp`
launcher and a main-menu icon.
