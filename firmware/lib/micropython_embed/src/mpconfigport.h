// MicroPython embed configuration for the Temporal Badge.
//
// Goal: expose the same module surface a normal `ports/esp32` MicroPython
// build does — `machine`, `network`, `socket`, `ssl`, `asyncio`, `_thread`,
// `espnow`, `bluetooth`, plus all the standard library modules — so users
// who connect with JumperIDE / ViperIDE / mpremote get a real REPL with no
// surprises. We're an embed port (no `make` driving us, sources are vendored
// under `lib/micropython_embed/src/` and built by PlatformIO), but the *config*
// matches the upstream esp32 port module by module.
//
// Feature gating: anything that requires upstream sources we don't vendor
// yet sits behind REPLAY_ENABLE_FULL_NETWORK / REPLAY_ENABLE_BLUETOOTH so
// the default build stays green. Run
// `firmware/scripts/fetch_micropython_sources.py` to vendor the rest, then
// flip those flags on. The script is invoked from `ignition/setup.sh` for
// fresh checkouts.
#pragma once

#include <stdint.h>

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_idf_version.h>
#include <hal/gpio_types.h>
#endif

typedef long mp_off_t;
#ifndef PATH_MAX
#define PATH_MAX 256
#endif
#define MICROPY_ALLOC_PATH_MAX ( 256 )

// ── Core selection ──────────────────────────────────────────────────────────
// 0 = MicroPython on core 0, Arduino main loop on core 1 (default)
// 1 = MicroPython on core 1, Arduino main loop on core 0
#ifndef MPY_RUN_ON_CORE
#define MPY_RUN_ON_CORE 0
#endif

// ── Feature gates ───────────────────────────────────────────────────────────
// Compile-time switches the user can flip from platformio.ini:
//   -DREPLAY_ENABLE_NETWORK_WLAN=0   disables the default network.WLAN
//                                    module surface.
//   -DREPLAY_ENABLE_FULL_NETWORK=1   adds socket / ssl / websocket /
//                                    webrepl. Requires the upstream
//                                    modsocket.c / modssl_mbedtls.c /
//                                    modwebsocket.c / modwebrepl.c to be
//                                    vendored.
//   -DREPLAY_ENABLE_BLUETOOTH=1      adds `bluetooth` (NimBLE) and `aioble`.
//                                    Requires modbluetooth_nimble.c and the
//                                    NimBLE port glue.
//   -DREPLAY_ENABLE_ESPNOW=1         adds the `espnow` module.
//   -DREPLAY_ENABLE_THREAD=1         adds `_thread` and a mpthreadport.c
//                                    backed by FreeRTOS tasks.
// Anything below that doesn't require new sources is on by default — the
// goal is to make the normal MicroPython surface available out of the box.

#ifndef REPLAY_ENABLE_FULL_NETWORK
#define REPLAY_ENABLE_FULL_NETWORK 0
#endif
#ifndef REPLAY_ENABLE_NETWORK_WLAN
#define REPLAY_ENABLE_NETWORK_WLAN 1
#endif
#ifndef REPLAY_ENABLE_BLUETOOTH
#define REPLAY_ENABLE_BLUETOOTH 0
#endif
#ifndef REPLAY_ENABLE_ESPNOW
#define REPLAY_ENABLE_ESPNOW 0
#endif
#ifndef REPLAY_ENABLE_THREAD
#define REPLAY_ENABLE_THREAD 0
#endif

// Step 1 (already on): bring in esp32 machine + WLAN scaffolding.
#ifndef REPLAY_ENABLE_ESP32_MACHINE_WIRELESS
#define REPLAY_ENABLE_ESP32_MACHINE_WIRELESS ( 1 )
#endif

// ── Core runtime features ───────────────────────────────────────────────────
#define MICROPY_ENABLE_GC ( 1 )
#define MICROPY_ENABLE_COMPILER ( 1 )
#define MICROPY_HELPER_REPL ( 1 )

#define MICROPY_REPL_EVENT_DRIVEN ( 1 )
#define MICROPY_REPL_AUTO_INDENT ( 1 )
#define MICROPY_REPL_EMACS_KEYS ( 1 )
#define MICROPY_EVENT_POLL_HOOK \
    do { \
        mp_handle_pending(true); \
    } while (0);
#define MICROPY_ERROR_REPORTING ( MICROPY_ERROR_REPORTING_DETAILED )
#define MICROPY_ENABLE_FINALISER ( 1 )
#define MICROPY_ENABLE_SOURCE_LINE  (1)

#ifndef MICROPY_OPT_COMPUTED_GOTO
#define MICROPY_OPT_COMPUTED_GOTO           (1)
#endif

// Split-heap GC matches the upstream esp32 port and lets the Python heap
// grow into PSRAM as needed instead of being capped at the initial slab.
#define MICROPY_GC_SPLIT_HEAP               (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO          (1)

// ── Builtins ────────────────────────────────────────────────────────────────
// MICROPY_PY_BUILTINS_FLOAT is set automatically by py/mpconfig.h when
// MICROPY_FLOAT_IMPL != NONE, so do NOT define it here (causes -Wmacro-redefined).
#define MICROPY_FLOAT_IMPL ( MICROPY_FLOAT_IMPL_FLOAT )
#define MICROPY_PY_BUILTINS_HELP ( 1 )
#define MICROPY_PY_BUILTINS_COMPILE ( 1 )
#define MICROPY_PY_BUILTINS_EVAL_EXEC ( 1 )
#define MICROPY_PY_BUILTINS_INPUT ( 1 )
#define MICROPY_PY_BUILTINS_BYTES_HEX ( 1 )
#define MICROPY_PY_MATH ( 1 )
#define MICROPY_PY_BUILTINS_STR_UNICODE ( 1 )
#define MICROPY_MODULE_WEAK_LINKS ( 1 )
#define MICROPY_PY_SYS_PATH ( 1 )
#define MICROPY_PY_SYS_EXIT ( 1 )
#define MICROPY_PY_SYS_PS1_PS2 ( 1 )
#define MICROPY_PY_FSTRINGS ( 1 )
#define MICROPY_PY_TSTRINGS ( 1 )

#define MICROPY_PY_ERRNO ( 1 )
#define MICROPY_PY_CMATH ( 1 )
#define MICROPY_PY_UCTYPES ( 1 )
#define MICROPY_PY_HEAPQ ( 1 )
#define MICROPY_PY_JSON ( 1 )
#define MICROPY_PY_BINASCII ( 1 )
#define MICROPY_PY_RANDOM ( 1 )
#define MICROPY_PY_RANDOM_EXTRA_FUNCS ( 1 )
#define MICROPY_PY_TIME ( 1 )

// asyncio is pure Python (extmod/asyncio/*.py) — wired in via frozen-modules
// path or the standard `_asyncio` extmod. The flag below pulls in the C
// helper that makes `asyncio.run()` interop with the event loop.
#define MICROPY_PY_ASYNCIO ( 1 )

// Standard `os` module — backed by VFS; provides listdir/stat/statvfs/uname.
#define MICROPY_PY_OS ( 1 )
#define MICROPY_PY_OS_UNAME ( 1 )
#define MICROPY_PY_OS_SYNC ( 1 )
#define MICROPY_PY_OS_INCLUDEFILE "port/replay_os_includefile.c"

// Time — ESP32 wall clock via gettimeofday, included into extmod/modtime.c.
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME ( 1 )
#define MICROPY_PY_TIME_TIME_TIME_NS            ( 1 )
#define MICROPY_PY_TIME_INCLUDEFILE "ports/esp32/modtime.c"

// Optional features that don't need new sources — mostly cleanup of the
// previously-curated subset.
#define MICROPY_PY_OS_DUPTERM        ( 1 )
#define MICROPY_PY_OS_DUPTERM_NOTIFY ( 1 )
#define MICROPY_KBD_EXCEPTION        ( 1 )

#if REPLAY_ENABLE_ESP32_MACHINE_WIRELESS
uint32_t replay_random_seed_init(void);
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (replay_random_seed_init())
#define MICROPY_PY_OS_URANDOM               (1)

// No SD card — keeps `ports/esp32/modmachine.c` from pulling `machine_sdcard`.
#define MICROPY_HW_ENABLE_SDCARD (0)

// ── machine.* — match upstream ESP32 (see micropython `ports/esp32/mpconfigport.h`) ──
#define MICROPY_PY_MACHINE (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE "ports/esp32/modmachine.c"
#define MICROPY_PY_MACHINE_RESET (1)
#define MICROPY_PY_MACHINE_BARE_METAL_FUNCS (1)
#define MICROPY_PY_MACHINE_DISABLE_IRQ_ENABLE_IRQ (1)
#define MICROPY_PY_MACHINE_MEMX (0)
#define MICROPY_PY_MACHINE_PIN_BASE (1)
#define MICROPY_PY_MACHINE_PIN_MAKE_NEW mp_pin_make_new
#define MICROPY_PY_MACHINE_SIGNAL (0)
#define MICROPY_PY_MACHINE_ADC (1)
#define MICROPY_PY_MACHINE_ADC_INCLUDEFILE "ports/esp32/machine_adc.c"
#define MICROPY_PY_MACHINE_ADC_ATTEN_WIDTH (1)
#define MICROPY_PY_MACHINE_ADC_INIT (1)
#define MICROPY_PY_MACHINE_ADC_DEINIT (1)
#define MICROPY_PY_MACHINE_ADC_READ (1)
#define MICROPY_PY_MACHINE_ADC_READ_UV (1)
#define MICROPY_PY_MACHINE_ADC_BLOCK (1)
#define MICROPY_PY_MACHINE_ADC_BLOCK_INCLUDEFILE "ports/esp32/machine_adc_block.c"
#define MICROPY_PY_MACHINE_BITSTREAM (0)
#define MICROPY_PY_MACHINE_DHT_READINTO (0)
#define MICROPY_PY_MACHINE_PULSE (0)
#define MICROPY_PY_MACHINE_PWM (1)
#define MICROPY_PY_MACHINE_PWM_DUTY (1)
#define MICROPY_PY_MACHINE_PWM_INCLUDEFILE "ports/esp32/machine_pwm.c"
// Hardware I2C disabled: legacy ESP-IDF i2c driver in ports/esp32/machine_i2c.c
// conflicts with Arduino Wire's driver_ng on boot ("driver_ng is not allowed
// to be used with this old driver"). SoftI2C remains available for Python.
#define MICROPY_PY_MACHINE_I2C (0)
#define MICROPY_PY_MACHINE_SOFTI2C (1)
#define MICROPY_PY_MACHINE_SPI (1)
#define MICROPY_PY_MACHINE_SOFTSPI (1)
#ifndef MICROPY_HW_ESP_NEW_I2C_DRIVER
#define MICROPY_HW_ESP_NEW_I2C_DRIVER (0)
#endif
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <driver/i2s_std.h>
#endif
#define MICROPY_PY_MACHINE_I2S (1)
#define MICROPY_PY_MACHINE_I2S_INCLUDEFILE "ports/esp32/machine_i2s.c"
#define MICROPY_PY_MACHINE_I2S_FINALISER (1)
#define MICROPY_PY_MACHINE_I2S_CONSTANT_RX (I2S_DIR_RX)
#define MICROPY_PY_MACHINE_I2S_CONSTANT_TX (I2S_DIR_TX)
#define MICROPY_PY_MACHINE_UART (1)
#define MICROPY_PY_MACHINE_UART_INCLUDEFILE "ports/esp32/machine_uart.c"
#define MICROPY_PY_MACHINE_UART_SENDBREAK (1)
#define MICROPY_PY_MACHINE_UART_IRQ (1)
#define MICROPY_PY_MACHINE_WDT (0)

// ── network ────────────────────────────────────────────────────────────────
// WLAN is useful on its own for badge-side SSID scans and simple network
// control from the REPL. Socket/SSL/WebREPL remain behind the full-network
// flag until we finish validating that larger surface in the embedded port.
#define MICROPY_PY_NETWORK (REPLAY_ENABLE_NETWORK_WLAN || REPLAY_ENABLE_FULL_NETWORK)
#if MICROPY_PY_NETWORK
#define MICROPY_PY_NETWORK_INCLUDEFILE "ports/esp32/modnetwork.h"
#define MICROPY_PY_NETWORK_MODULE_GLOBALS_INCLUDEFILE "ports/esp32/modnetwork_globals.h"
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "mpy-esp32s3"
#define MICROPY_PY_NETWORK_WLAN             (1)
#define MICROPY_PY_NETWORK_LAN              (0)
#define MICROPY_PY_NETWORK_PPP_LWIP         (0)
#else
#define MICROPY_PY_NETWORK_WLAN             (0)
#define MICROPY_PY_NETWORK_LAN              (0)
#define MICROPY_PY_NETWORK_PPP_LWIP         (0)
#endif

#if REPLAY_ENABLE_FULL_NETWORK
// socket, ssl, websocket, webrepl, mDNS — full kitchen-sink.
#define MICROPY_PY_SOCKET_EVENTS            (1)
#define MICROPY_PY_SSL                      (1)
#define MICROPY_SSL_MBEDTLS                 (1)
#define MICROPY_PY_WEBSOCKET                (1)
#define MICROPY_PY_WEBREPL                  (1)
#define MICROPY_PY_USSL                     (1)
#define MICROPY_PY_USSL_FINALISER           (1)
#else
#define MICROPY_PY_SOCKET_EVENTS            (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_SSL_MBEDTLS                 (0)
#define MICROPY_PY_WEBSOCKET                (0)
#define MICROPY_PY_WEBREPL                  (0)
#endif

// ── espnow ─────────────────────────────────────────────────────────────────
#if REPLAY_ENABLE_ESPNOW
#define MICROPY_PY_ESPNOW                   (1)
#else
#define MICROPY_PY_ESPNOW                   (0)
#endif

// ── bluetooth (NimBLE) ─────────────────────────────────────────────────────
#if REPLAY_ENABLE_BLUETOOTH
#define MICROPY_PY_BLUETOOTH                (1)
#define MICROPY_PY_BLUETOOTH_USE_SYNC_EVENTS (1)
#define MICROPY_PY_BLUETOOTH_USE_SYNC_EVENTS_WITH_INTERLOCK (1)
#define MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE (1)
#define MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING (1)
#define MICROPY_BLUETOOTH_NIMBLE            (1)
#define MICROPY_BLUETOOTH_NIMBLE_BINDINGS_ONLY (1)
#else
#define MICROPY_PY_BLUETOOTH                (0)
#endif

// ── threading ──────────────────────────────────────────────────────────────
#if REPLAY_ENABLE_THREAD
#define MICROPY_PY_THREAD                   (1)
#define MICROPY_PY_THREAD_GIL               (1)
#define MICROPY_PY_THREAD_GIL_VM_DIVISOR    (32)
#endif

#else
// Default path: keep host-safe embed config while staged esp32 port files land.
#define MICROPY_PY_OS_URANDOM               (0)
#endif

// sys.platform / sys.version used by ViperIDE reinit script.
#define MICROPY_PY_SYS_PLATFORM "esp32"
#define MICROPY_PY_SYS_VERSION ( 1 )

// ── VFS layer: standard MicroPython filesystem ─────────────────────────────
// This provides os.listdir, os.stat, os.statvfs, open(), file imports, etc.
// automatically via the VFS mount — no custom C shim code needed.
#define MICROPY_VFS ( 1 )
#define MICROPY_VFS_FAT ( 1 )
#define MICROPY_VFS_WRITABLE ( 1 )
#define MICROPY_READER_VFS ( 1 )
#define MICROPY_ENABLE_EXTERNAL_IMPORT ( 1 )
#define MICROPY_PY_IO ( 1 )
// VFS provides open() via mp_vfs_open_obj:
#define MICROPY_PY_BUILTINS_OPEN ( 1 )

// ── oofatfs configuration ──────────────────────────────────────────────────
// The oofatfs ff.h does `#include FFCONF_H` — this must resolve to the config.
#define FFCONF_H "ffconf.h"
// Long filename support (3 = heap-allocated buffer).
#define MICROPY_FATFS_ENABLE_LFN ( 3 )
// Relative path support: 2 = f_chdir() + f_getcwd().
#define MICROPY_FATFS_RPATH ( 2 )
// Flash sector size is 4096 bytes (wear-levelling sector).
#define MICROPY_FATFS_MAX_SS ( 4096 )
// No hardware RTC — use fixed timestamp.
#define MICROPY_FATFS_NORTC ( 1 )

// ── Compiler / parser tuning ────────────────────────────────────────────────
// Raise the parser stack depth and allocation limits so larger scripts compile
// in one shot instead of silently failing with MemoryError.
#define MICROPY_ALLOC_PARSE_RULE_INIT       ( 128 )
#define MICROPY_ALLOC_PARSE_RULE_INC        ( 32 )
#define MICROPY_ALLOC_PARSE_RESULT_INIT     ( 64 )
#define MICROPY_ALLOC_PARSE_RESULT_INC      ( 32 )
#define MICROPY_ALLOC_PARSE_CHUNK_INIT      ( 256 )
#define MICROPY_COMP_CONST_FOLDING          ( 1 )
#define MICROPY_COMP_MODULE_CONST           ( 1 )

// ── Needed on Xtensa/ESP32 ─────────────────────────────────────────────────
#define MICROPY_NLR_SETJMP ( 1 )
#define MICROPY_GCREGS_SETJMP ( 1 )

// ── Native codegen, libc, builtins (subset of ports/esp32/mpconfigport.h) ────
#define MICROPY_WARNINGS                    (1)
#define MICROPY_STREAMS_POSIX_API           (1)
#define MICROPY_USE_INTERNAL_ERRNO          (0)
#define MICROPY_USE_INTERNAL_PRINTF         (0)
#define MICROPY_SCHEDULER_DEPTH             (8)
#define MICROPY_SCHEDULER_STATIC_NODES      (1)

#define MICROPY_PY_STR_BYTES_CMP_WARN       (1)
#define MICROPY_PY_ALL_INPLACE_SPECIAL_METHODS (1)
#define MICROPY_PY_IO_BUFFEREDWRITER        (1)

#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)

// ── Misc ────────────────────────────────────────────────────────────────────
#define MICROPY_PERSISTENT_CODE_LOAD ( 0 )
#define MICROPY_ENABLE_SCHEDULER ( 1 )

#define MICROPY_HW_BOARD_NAME "Replay Badge"
#define MICROPY_HW_MCU_NAME "ESP32-S3"

// Badge firmware version — single source of truth lives in firmware/VERSION
// and is injected as -DBADGE_FIRMWARE_VERSION=... by
// scripts/inject_version.py (PlatformIO pre-build hook). The fallback
// below only kicks in when MicroPython is rebuilt outside our PlatformIO
// build (host-side QSTR pre-pass, etc.) — the firmware itself always
// gets the real version. Surfaced in the REPL banner via
// MICROPY_BANNER_MACHINE so external tooling can parse
// "Replay Badge v<X.Y.Z> with ESP32-S3" and offer firmware updates.
#ifndef BADGE_FIRMWARE_VERSION
#define BADGE_FIRMWARE_VERSION "dev"
#endif
#define MICROPY_BANNER_MACHINE \
    MICROPY_HW_BOARD_NAME " v" BADGE_FIRMWARE_VERSION " with " MICROPY_HW_MCU_NAME

// Long int support (needed for large FAT partition sizes).
#define MICROPY_LONGINT_IMPL ( MICROPY_LONGINT_IMPL_MPZ )

// readline.c uses MP_STATE_PORT(readline_hist); map to VM state like other ports.
#define MP_STATE_PORT MP_STATE_VM

// Python print() / mp_plat_print: emit raw bytes on the REPL stream (same path as pyexec
// OK, >, and \x04). Default MP_PLAT_PRINT_STRN uses mp_hal_stdout_tx_strn_cooked (\n -> \r\n),
// which breaks mpremote / ViperIDE raw REPL framing between OK and the trailing EOF markers.
#define MP_PLAT_PRINT_STRN( str, len ) mp_hal_stdout_tx_strn( ( str ), ( len ) )

// NDEBUG is defined as a build flag in platformio.ini build_flags_common so
// it's set before any TU sees <assert.h>. Defining it here was order-fragile
// (mpconfigport.h is reached via py/mpconfig.h → py/mpstate.h, which is often
// included after <assert.h> has already locked in its assert macro).
