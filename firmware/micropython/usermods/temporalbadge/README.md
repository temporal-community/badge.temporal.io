# temporalbadge user module

This module exposes badge hardware and firmware services to MicroPython as the
`badge` module. App authors usually call the Python API from
`firmware/initial_filesystem/apps/`; firmware authors touch this directory only
when adding or changing Python-visible bindings.

## Bridge Shape

Keep the layers narrow:

1. C++ firmware code owns hardware behavior and shared UI rendering.
2. `firmware/src/micropython/badge_mp_api/*.cpp` exposes `extern "C"`
   `temporalbadge_runtime_*` functions.
3. `temporalbadge_runtime.h` declares those functions for the C usermod.
4. `temporalbadge_hal.{c,h}` provides thin C pass-throughs.
5. `modtemporalbadge.c` converts Python arguments, raises Python exceptions,
   and registers functions in the module globals table.

Do not duplicate large C++ rendering assets or hardware logic in Python. For
example, `badge_ui.py` calls native `ui_header`, `ui_action_bar`, and glyph
helpers so MicroPython screens stay aligned with `OLEDLayout` and
`ButtonGlyphs`.

## Adding a Binding

When adding `badge.some_function(...)`:

1. Implement the runtime function in the matching
   `firmware/src/micropython/badge_mp_api/<category>.cpp` file.
2. Add declarations to `temporalbadge_runtime.h` and `temporalbadge_hal.h`.
3. Add the pass-through implementation to `temporalbadge_hal.c`.
4. Add the MicroPython wrapper and `MP_DEFINE_CONST_FUN_OBJ_*` in
   `modtemporalbadge.c`.
5. Add the function to the module globals table in `modtemporalbadge.c`.
6. Regenerate the MicroPython QSTR data when new Python-visible names are
   introduced.
7. Document the API in `firmware/initial_filesystem/docs/API_REFERENCE.md`.

If the upstream MicroPython checkout is unavailable, the existing embedded
library may still build from the checked-in generated QSTR header, but new
names need that header refreshed before PlatformIO can compile.

## App-Facing UI Helpers

Prefer adding reusable UI behavior in C++ first:

- `firmware/src/ui/OLEDLayout.{h,cpp}` for common OLED chrome, headers,
  footers, selected rows, and text fitting.
- `firmware/src/ui/ButtonGlyphs.{h,cpp}` for shape-based button icons.
- `firmware/src/ui/QRCodePlate.{h,cpp}` plus `identity/BadgeQR.{h,cpp}` for
  native QR helpers. Expose QR to Python through a draw-and-free wrapper so
  native code owns the PSRAM bitmap allocation and Python apps do not repaint
  scan targets in a tight loop.
- `firmware/initial_filesystem/lib/badge_ui.py` for the Python convenience
  wrapper and fallback behavior.

This keeps production C++ screens and MicroPython apps visually consistent.

## Build Checks

From `firmware/`:

```sh
python3 scripts/generate_startup_files.py
pio run -e replay2026
```

Run `replay2026` when validating the generic Apps menu, diagnostics, and
MicroPython tools.
