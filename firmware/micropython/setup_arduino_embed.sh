#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MP_DIR="${SCRIPT_DIR}/micropython_repo"
# Match firmware CLAUDE.md / embed port unless overridden.
MICROPYTHON_TAG="${MICROPYTHON_TAG:-v1.27.0}"
MICROPYTHON_REPO_URL="${MICROPYTHON_REPO_URL:-https://github.com/micropython/micropython.git}"
LIB_DIR="${REPO_ROOT}/lib/micropython_embed"
LIB_SRC_DIR="${LIB_DIR}/src"
# Canonical MicroPython port config lives in the generated library source tree.
MPCONFIG_SRC="${LIB_SRC_DIR}/mpconfigport.h"
MPHAL_SRC="${SCRIPT_DIR}/embed_config/mphalport.c"
EMBED_UTIL_SRC="${SCRIPT_DIR}/embed_config/embed_util.c"
MODMACHINE_SRC="${SCRIPT_DIR}/embed_config/modmachine_esp32_subset.c"
USERMODS_DIR="${SCRIPT_DIR}/usermods/temporalbadge"
ESP_IDF_REF="${ESP_IDF_REF:-v5.4.1}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--mp-dir PATH] [--no-clone]

Generate lib/micropython_embed from MicroPython ports/embed for Arduino/PlatformIO.

MicroPython source resolution (in order):
  1. --mp-dir PATH if you pass it
  2. Git submodule at firmware/micropython/micropython_repo — run from repo root:
       git submodule update --init firmware/micropython/micropython_repo
     This script runs that automatically when the path is missing/empty.
  3. Shallow git clone into micropython_repo/ (skipped with --no-clone)

Pinned version: parent repo records the submodule commit (target: ${MICROPYTHON_TAG}).

Options:
  --mp-dir PATH   Path to MicroPython repo (default: ${MP_DIR})
  --no-clone      Do not clone; only try submodule init + fail if still missing
EOF
}

NO_CLONE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mp-dir)
      MP_DIR="$2"
      shift 2
      ;;
    --no-clone)
      NO_CLONE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ ! -d "${MP_DIR}" ]]; then
  if [[ -f "${REPO_ROOT}/.gitmodules" ]] && grep -qF 'micropython/micropython_repo' "${REPO_ROOT}/.gitmodules" 2>/dev/null; then
    echo "MicroPython sources missing — initializing submodule at firmware/micropython/micropython_repo..."
    if git -C "${REPO_ROOT}" submodule update --init --recursive firmware/micropython/micropython_repo; then
      :
    else
      echo "Submodule init failed. From repo root try:" >&2
      echo "  git submodule update --init firmware/micropython/micropython_repo" >&2
    fi
  fi
fi

if [[ ! -f "${MP_DIR}/py/mpconfig.h" ]]; then
  if [[ "${NO_CLONE}" -eq 1 ]]; then
    echo "MicroPython sources not available: ${MP_DIR} (missing py/mpconfig.h)" >&2
    exit 1
  fi
  if [[ -d "${MP_DIR}" ]]; then
    echo "Directory exists but is not a full MicroPython tree: ${MP_DIR}" >&2
    echo "Remove it or run: git submodule update --init firmware/micropython/micropython_repo" >&2
    exit 1
  fi
  echo "MicroPython repo not found — shallow cloning ${MICROPYTHON_TAG} into ${MP_DIR}..."
  if ! git clone --depth 1 --branch "${MICROPYTHON_TAG}" "${MICROPYTHON_REPO_URL}" "${MP_DIR}"; then
    echo "Clone failed. Check network, or set MICROPYTHON_TAG to a valid tag." >&2
    echo "You can also pass --mp-dir /path/to/your/micropython checkout." >&2
    exit 1
  fi
fi

if [[ ! -f "${MP_DIR}/py/mpconfig.h" ]]; then
  echo "Directory ${MP_DIR} does not look like a MicroPython source tree (missing py/mpconfig.h)." >&2
  exit 1
fi

if [[ ! -f "${MPCONFIG_SRC}" ]]; then
  echo "Missing config file: ${MPCONFIG_SRC}" >&2
  exit 1
fi

if [[ ! -f "${MPHAL_SRC}" ]]; then
  echo "Missing mphal override: ${MPHAL_SRC}" >&2
  exit 1
fi

resolve_idf_path() {
  local candidate=""

  if [[ -n "${IDF_PATH:-}" && -d "${IDF_PATH}/components" ]]; then
    printf '%s\n' "${IDF_PATH}"
    return 0
  fi

  candidate="${SCRIPT_DIR}/esp-idf"
  if [[ -d "${candidate}/components" ]]; then
    printf '%s\n' "${candidate}"
    return 0
  fi

  candidate="${HOME}/.platformio/packages/framework-espidf"
  if [[ -d "${candidate}/components" ]]; then
    printf '%s\n' "${candidate}"
    return 0
  fi

  candidate="${SCRIPT_DIR}/esp-idf"
  echo "ESP-IDF not found locally; cloning ${ESP_IDF_REF} into ${candidate}..."
  git clone --depth 1 --branch "${ESP_IDF_REF}" https://github.com/espressif/esp-idf.git "${candidate}"
  if [[ -d "${candidate}/components" ]]; then
    printf '%s\n' "${candidate}"
    return 0
  fi

  return 1
}

ESP_IDF_PATH="$(resolve_idf_path)"
if [[ -z "${ESP_IDF_PATH}" || ! -d "${ESP_IDF_PATH}/components" ]]; then
  echo "Could not resolve a usable ESP-IDF path." >&2
  exit 1
fi
export IDF_PATH="${ESP_IDF_PATH}"

# For host-side qstr preprocessing of ESP32 machine modules, add broad ESP-IDF headers.
# ESP-IDF v5.4+ moved register-level headers (soc/ledc_reg.h et al.) into
# components/soc/esp32s3/register/, so that path must be on the include list
# in addition to the older components/*/include / esp32s3/include paths.
declare -a IDF_INCLUDE_DIRS=()
for d in \
  "${IDF_PATH}"/components/*/include \
  "${IDF_PATH}"/components/*/*/include \
  "${IDF_PATH}"/components/*/esp32s3/include \
  "${IDF_PATH}"/components/*/*/esp32s3/include \
  "${IDF_PATH}"/components/*/esp32s3/register \
  "${IDF_PATH}"/components/*/*/esp32s3/register; do
  if [[ -d "${d}" ]]; then
    IDF_INCLUDE_DIRS+=("${d}")
  fi
done

# ESP-IDF public headers expect sdkconfig.h from an SDK configuration profile.
# Reuse the Arduino ESP32-S3 profile when available so host-side preprocessing works.
ARDUINO_ESP32_ROOT="${HOME}/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3"
if [[ ! -d "${ARDUINO_ESP32_ROOT}" ]]; then
  ARDUINO_ESP32_ROOT="${HOME}/.platformio/packages/framework-arduinoespressif32-libs/esp32s3"
fi
SDKCONFIG_VARIANT="${SDKCONFIG_VARIANT:-qio_qspi}"
SDKCONFIG_INCLUDE_DIR="${ARDUINO_ESP32_ROOT}/${SDKCONFIG_VARIANT}/include"
if [[ -f "${SDKCONFIG_INCLUDE_DIR}/sdkconfig.h" ]]; then
  IDF_INCLUDE_DIRS+=("${SDKCONFIG_INCLUDE_DIR}")

# FreeRTOS config headers needed by host-side QSTR preprocessing of WDT/thread port files.
FREERTOS_CFG_DIR="${ARDUINO_ESP32_ROOT}/include/freertos/config/include"
if [[ -d "${FREERTOS_CFG_DIR}" ]]; then
  IDF_INCLUDE_DIRS+=("${FREERTOS_CFG_DIR}")
fi
FREERTOS_CFG_ESP32S3_DIR="${ARDUINO_ESP32_ROOT}/include/freertos/config/xtensa/include"
if [[ -d "${FREERTOS_CFG_ESP32S3_DIR}" ]]; then
  IDF_INCLUDE_DIRS+=("${FREERTOS_CFG_ESP32S3_DIR}")
fi
else
  for d in "${ARDUINO_ESP32_ROOT}"/*/include; do
    if [[ -f "${d}/sdkconfig.h" ]]; then
      IDF_INCLUDE_DIRS+=("${d}")
      SDKCONFIG_INCLUDE_DIR="${d}"
      break
    fi
  done
fi

if [[ ${#IDF_INCLUDE_DIRS[@]} -eq 0 ]]; then
  echo "No ESP-IDF include directories discovered under ${IDF_PATH}" >&2
  exit 1
fi

# Host-side QSTR preprocessing runs `gcc -E` over ports/esp32/*.c without pulling
# esp_idf_version.h through the normal include chain. machine_pwm.c uses
# ESP_IDF_VERSION_VAL(...) in #if — force-include the header on every unit.
ESP_IDF_VERSION_H="${IDF_PATH}/components/esp_common/include/esp_idf_version.h"
if [[ ! -f "${ESP_IDF_VERSION_H}" ]]; then
  echo "Missing ESP-IDF version header (need esp_common): ${ESP_IDF_VERSION_H}" >&2
  exit 1
fi

IDF_INCLUDE_FLAGS=""
for d in "${IDF_INCLUDE_DIRS[@]}"; do
  IDF_INCLUDE_FLAGS+=" -I${d}"
done

echo "============================================================"
echo " MicroPython Embed Package Generator (Replay Badge)"
echo "============================================================"
echo ""
echo "  MicroPython repo:  ${MP_DIR}"
echo "  Output library:    ${LIB_DIR}"
echo "  ESP-IDF path:      ${IDF_PATH}"
echo "  Usermod dir:       ${USERMODS_DIR}"
if [[ -f "${SDKCONFIG_INCLUDE_DIR}/sdkconfig.h" ]]; then
  echo "SDK config path:  ${SDKCONFIG_INCLUDE_DIR}"
fi

mkdir -p "${LIB_SRC_DIR}"

TMP_BACKUP=""
if [[ -f "${MP_DIR}/mpconfigport.h" ]]; then
  TMP_BACKUP="${MP_DIR}/mpconfigport.h.replay-backup"
  cp "${MP_DIR}/mpconfigport.h" "${TMP_BACKUP}"
fi

TMP_MPHAL_BACKUP=""
if [[ -f "${MP_DIR}/mphalport.h" ]]; then
  TMP_MPHAL_BACKUP="${MP_DIR}/mphalport.h.replay-backup"
  cp "${MP_DIR}/mphalport.h" "${TMP_MPHAL_BACKUP}"
fi

cleanup() {
  if [[ -n "${TMP_BACKUP}" && -f "${TMP_BACKUP}" ]]; then
    mv "${TMP_BACKUP}" "${MP_DIR}/mpconfigport.h"
  else
    rm -f "${MP_DIR}/mpconfigport.h"
  fi

  if [[ -n "${TMP_MPHAL_BACKUP}" && -f "${TMP_MPHAL_BACKUP}" ]]; then
    mv "${TMP_MPHAL_BACKUP}" "${MP_DIR}/mphalport.h"
  else
    rm -f "${MP_DIR}/mphalport.h"
  fi
}
trap cleanup EXIT

cp "${MPCONFIG_SRC}" "${MP_DIR}/mpconfigport.h"
export MP_DIR_FOR_QSTR_PATCH="${MP_DIR}"
# Host QSTR preprocessing cannot expand `ports/esp32/modmachine.c` (pulls in FreeRTOS).
# Rewrite only the MicroPython-build copy; `lib/.../mpconfigport.h` stays full-featured for firmware.
python3 - <<'PY'
from pathlib import Path
import os
import re
path = Path(os.environ["MP_DIR_FOR_QSTR_PATCH"]) / "mpconfigport.h"
text = path.read_text()
text, n = re.subn(
    r'(#define\s+MICROPY_PY_MACHINE_INCLUDEFILE\s+)"[^"]+"',
    r'\1"port/modmachine_esp32_subset.c"',
    text,
    count=1,
)
assert n == 1, "expected to patch MICROPY_PY_MACHINE_INCLUDEFILE once"
text, n = re.subn(r"(#define\s+MICROPY_PY_MACHINE_RESET\s+)\(\s*1\s*\)", r"\1(0)", text, count=1)
assert n == 1, "expected to patch MICROPY_PY_MACHINE_RESET once"
text, n = re.subn(
    r"(#define\s+MICROPY_PY_MACHINE_BARE_METAL_FUNCS\s+)\(\s*1\s*\)",
    r"\1(0)",
    text,
    count=1,
)
assert n == 1, "expected to patch MICROPY_PY_MACHINE_BARE_METAL_FUNCS once"
# Threading pulls in FreeRTOS via mpthreadport.h which the host gcc cannot resolve.
# ESP-NOW and full network pull in WiFi/esp_now.h which also need IDF runtime headers.
# Disable for the QSTR host pass; the firmware build uses the real mpconfigport.h.
text, n = re.subn(
    r"(#define\s+REPLAY_ENABLE_THREAD\s+)1",
    r"\g<1>0",
    text,
    count=1,
)
text = re.sub(
    r"(#define\s+REPLAY_ENABLE_ESPNOW\s+)1",
    r"\g<1>0",
    text,
    count=1,
)
text = re.sub(
    r"(#define\s+REPLAY_ENABLE_FULL_NETWORK\s+)1",
    r"\g<1>0",
    text,
    count=1,
)
text = re.sub(
    r"(#define\s+REPLAY_ENABLE_BLUETOOTH\s+)1",
    r"\g<1>0",
    text,
    count=1,
)
# Also disable WDT for host pass (esp_task_wdt.h needs FreeRTOS)
text, n2 = re.subn(
    r"(#define\s+MICROPY_PY_MACHINE_WDT\s+)\(1\)",
    r"\1(0)",
    text,
    count=1,
)
path.write_text(text)
PY
cp "${SCRIPT_DIR}/embed_config/mphalport.h" "${MP_DIR}/ports/embed/port/mphalport.h"
cp "${MP_DIR}/ports/embed/port/mphalport.h" "${MP_DIR}/mphalport.h"
cp "${SCRIPT_DIR}/embed_config/replay_bdev.c" "${MP_DIR}/ports/embed/port/replay_bdev.c"
cp "${SCRIPT_DIR}/embed_config/replay_os_includefile.c" "${MP_DIR}/ports/embed/port/replay_os_includefile.c"
cp "${SCRIPT_DIR}/embed_config/replay_machine_qstr.c" "${MP_DIR}/ports/embed/port/replay_machine_qstr.c"
cp "${SCRIPT_DIR}/embed_config/replay_extra_rootpointers.c" "${MP_DIR}/ports/embed/port/replay_extra_rootpointers.c"
cp "${SCRIPT_DIR}/embed_config/replay_phase1_qstr.c" "${MP_DIR}/ports/embed/port/replay_phase1_qstr.c"
mkdir -p "${MP_DIR}/port"
cp "${MODMACHINE_SRC}" "${MP_DIR}/port/modmachine_esp32_subset.c"

# Drop stale genhdr (e.g. moduledefs.collected hash) so MODULE_DEF_* matches mpconfigport.
rm -rf "${MP_DIR}/build-embed"

# ── Run the build using our custom makefile ──────────────────────────────────
# replay_embed.mk extends embed.mk with:
#   - MICROPY_VFS_FAT=1 (oofatfs QSTR auto-discovery)
#   - SRC_QSTR for readline, pyexec, sys_stdio_mphal, timeutils
#   - Extended packaging to copy VFS/oofatfs/shared sources
(
  cd "${MP_DIR}"
  # Absolute -include so QSTR cpp sees ESP_IDF_VERSION_VAL before any #if in
  # ports/esp32/machine_*.c (MicroPython v1.27+).
  IDF_CPP_DEFS="-include ${ESP_IDF_VERSION_H} -DESP_PLATFORM=1 -DCONFIG_IDF_TARGET_ESP32S3=1 -DCONFIG_IDF_TARGET_ARCH_XTENSA=1"
  make -f "${SCRIPT_DIR}/replay_embed.mk" \
    REPLAY_QSTR_CFLAGS="${IDF_CPP_DEFS}" \
    REPLAY_IDF_INCLUDE_FLAGS="${IDF_INCLUDE_FLAGS}" \
    MICROPYTHON_TOP="${MP_DIR}" \
    PACKAGE_DIR="${LIB_SRC_DIR}" \
    USERMODS_DIR="${USERMODS_DIR}"
)

# Board pin table for `machine.Pin.board` and `genhdr/pins.h` (ESP32-S3 generic / XIAO-class defaults).
GENHDR_DIR="${LIB_SRC_DIR}/genhdr"
mkdir -p "${GENHDR_DIR}"
python3 "${MP_DIR}/ports/esp32/boards/make-pins.py" \
  --board-csv "${SCRIPT_DIR}/embed_config/temporal_badge_pins.csv" \
  --prefix "${MP_DIR}/ports/esp32/boards/pins_prefix.c" \
  --output-source "${GENHDR_DIR}/pins.c" \
  --output-header "${GENHDR_DIR}/pins.h"

# Replay embed: upstream ADCBlock connect uses mp_hal_pin_obj_t with integer -1 sentinel and
# passes gpio_id straight into madc_search_helper (gpio_num_t). Our mphal uses void* pins.
python3 - <<PY
from pathlib import Path
lib = Path("${LIB_SRC_DIR}")
p = lib / "extmod" / "machine_adc_block.c"
if p.is_file():
    t = p.read_text()
    t = t.replace(
        "mp_hal_pin_obj_t pin = -1;",
        "mp_hal_pin_obj_t pin = (mp_hal_pin_obj_t)(intptr_t)-1;",
    )
    p.write_text(t)
p = lib / "ports" / "esp32" / "machine_adc_block.c"
if p.is_file():
    t = p.read_text()
    old = "    const machine_adc_obj_t *adc = madc_search_helper(self, channel_id, gpio_id);"
    new = """    gpio_num_t replay_adc_gpio = (gpio_num_t)-1;
    if ((intptr_t)gpio_id != -1) {
        replay_adc_gpio = (gpio_num_t)machine_pin_get_id((mp_obj_t)(uintptr_t)gpio_id);
    }
    const machine_adc_obj_t *adc = madc_search_helper(self, channel_id, replay_adc_gpio);"""
    if old in t:
        t = t.replace(old, new)
        p.write_text(t)
PY

# Arduino ESP-IDF headers: machine_rtc.c uses RTC_NOINIT_ATTR from esp_attr.h (not always pulled in).
python3 - <<PY
from pathlib import Path
lib = Path("${LIB_SRC_DIR}")
p = lib / "ports" / "esp32" / "machine_rtc.c"
if p.is_file():
    t = p.read_text()
    needle = '#include "driver/gpio.h"'
    ins = needle + '\n#include "esp_attr.h"'
    if needle in t and 'esp_attr.h' not in t:
        p.write_text(t.replace(needle, ins, 1))
PY

# Replay: fix -Werror=return-type in modespnow.c _get_singleton (upstream bug)
python3 - <<PY
from pathlib import Path
lib = Path("${LIB_SRC_DIR}")
p = lib / "ports" / "esp32" / "modespnow.c"
if p.is_file():
    t = p.read_text()
    old = "static esp_espnow_obj_t *_get_singleton() {\n    return MP_STATE_PORT(espnow_singleton);\n}"
    new = "static esp_espnow_obj_t *_get_singleton(void) {\n    return MP_STATE_PORT(espnow_singleton);\n}"
    if old in t:
        t = t.replace(old, new)
        p.write_text(t)
PY

# Replay: network.WLAN must coexist with the Arduino C-layer WiFi (WiFiService),
# which already creates the default esp_netif and starts esp_wifi at boot
# (main.cpp does esp_netif_init + esp_event_loop_create_default; WiFiService runs
# WiFi.begin()). Upstream esp_initialise_wifi() unconditionally calls
# esp_netif_create_default_wifi_sta()/_ap() and esp_wifi_init(), so a REPL
# `network.WLAN(...)` trips the duplicate-if_key assert in wifi_default.c and
# panic-reboots. Patch it to reuse existing default netifs, tolerate an
# already-initialised driver, and reflect a running driver into the module's
# state so active()/status() don't block on a STA_START event that already fired.
python3 - <<PY
from pathlib import Path
lib = Path("${LIB_SRC_DIR}")
p = lib / "ports" / "esp32" / "network_wlan.c"
if p.is_file():
    t = p.read_text()

    sta_old = "        wlan_sta_obj.netif = esp_netif_create_default_wifi_sta();"
    sta_new = (
        "        // Replay: never create the default STA netif. The Arduino C-layer\n"
        "        // (WiFiGeneric wifiLowLevelInit) unconditionally creates BOTH the\n"
        "        // STA and AP default netifs whenever WiFi comes up and tracks them\n"
        "        // in its own private table; if MicroPython also creates one, the\n"
        "        // C-layer's later create hits the duplicate-if_key assert in\n"
        "        // esp_netif and panics the whole badge. Reuse the C-layer's netif\n"
        "        // (may be NULL until WiFi is up; the WIFI_EVENT handlers below\n"
        "        // refresh it on every start so the cached pointer never dangles).\n"
        '        wlan_sta_obj.netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");'
    )
    assert sta_old in t, "network_wlan.c: STA netif create line not found (upstream changed?)"
    t = t.replace(sta_old, sta_new, 1)

    ap_old = "        wlan_ap_obj.netif = esp_netif_create_default_wifi_ap();"
    ap_new = (
        "        // Replay: reuse-only (see STA note above); never create the AP\n"
        "        // netif — the C-layer creates it and a duplicate key panics.\n"
        '        wlan_ap_obj.netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");'
    )
    assert ap_old in t, "network_wlan.c: AP netif create line not found (upstream changed?)"
    t = t.replace(ap_old, ap_new, 1)

    init_old = (
        "        esp_exceptions(esp_wifi_init(&cfg));\n"
        "        esp_exceptions(esp_wifi_set_storage(WIFI_STORAGE_RAM));\n"
        "\n"
        '        ESP_LOGD("modnetwork", "Initialized");\n'
        "        wifi_initialized = 1;"
    )
    init_new = (
        "        // Replay: do NOT initialise esp_wifi here. The Arduino C-layer\n"
        "        // (WiFiGeneric wifiLowLevelInit / WiFiService) is the sole owner of\n"
        "        // esp_wifi_init + the default netifs. If MicroPython calls\n"
        "        // esp_wifi_init first, the C-layer's later init returns\n"
        "        // ESP_ERR_WIFI_INIT_STATE, WiFi.mode() bails before creating netifs,\n"
        "        // and connect() fails. network.WLAN delegates ALL radio control to\n"
        "        // WiFiService (see network_wlan_connect / network_wlan_active).\n"
        "        (void)cfg;\n"
        "\n"
        "        // Replay: if a Wi-Fi mode is already set the C-layer is running\n"
        "        // (boot auto-connect), so sync the module statics for\n"
        "        // active()/status()/isconnected().\n"
        "        wifi_mode_t replay_wifi_mode = WIFI_MODE_NULL;\n"
        "        if (esp_wifi_get_mode(&replay_wifi_mode) == ESP_OK && replay_wifi_mode != WIFI_MODE_NULL) {\n"
        "            wifi_started = true;\n"
        "            if (replay_wifi_mode & WIFI_MODE_STA) {\n"
        "                wlan_sta_obj.active = true;\n"
        "            }\n"
        "            if (replay_wifi_mode & WIFI_MODE_AP) {\n"
        "                wlan_ap_obj.active = true;\n"
        "            }\n"
        "            wifi_ap_record_t replay_ap_info;\n"
        "            if (esp_wifi_sta_get_ap_info(&replay_ap_info) == ESP_OK) {\n"
        "                wifi_sta_connected = true;\n"
        "            }\n"
        "        }\n"
        "\n"
        '        ESP_LOGD("modnetwork", "Initialized");\n'
        "        wifi_initialized = 1;"
    )
    assert init_old in t, "network_wlan.c: wifi init block not found (upstream changed?)"
    t = t.replace(init_old, init_new, 1)

    # Replay: active() no longer drives esp_wifi (the C-layer owns the radio), so
    # the whole upstream body is replaced with a delegating one. This also removes
    # the STA_START busy-wait that previously hard-froze the REPL.
    active_old = (
        "static mp_obj_t network_wlan_active(size_t n_args, const mp_obj_t *args) {\n"
        "    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);\n"
        "\n"
        "    wifi_mode_t mode;\n"
        "    if (!wifi_started) {\n"
        "        mode = WIFI_MODE_NULL;\n"
        "    } else {\n"
        "        esp_exceptions(esp_wifi_get_mode(&mode));\n"
        "    }\n"
        "\n"
        "    int bit = (self->if_id == ESP_IF_WIFI_STA) ? WIFI_MODE_STA : WIFI_MODE_AP;\n"
        "\n"
        "    if (n_args > 1) {\n"
        "        bool active = mp_obj_is_true(args[1]);\n"
        "        mode = active ? (mode | bit) : (mode & ~bit);\n"
        "        if (mode == WIFI_MODE_NULL) {\n"
        "            if (wifi_started) {\n"
        "                esp_exceptions(esp_wifi_stop());\n"
        "                wifi_started = false;\n"
        "            }\n"
        "        } else {\n"
        "            esp_exceptions(esp_wifi_set_mode(mode));\n"
        "            if (!wifi_started) {\n"
        "                esp_exceptions(esp_wifi_start());\n"
        "                wifi_started = true;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        // Wait for the interface to be in the correct state.\n"
        "        while (self->active != active) {\n"
        "            MICROPY_EVENT_POLL_HOOK;\n"
        "        }\n"
        "    }\n"
        "\n"
        "    return (mode & bit) ? mp_const_true : mp_const_false;\n"
        "}"
    )
    active_new = (
        "static mp_obj_t network_wlan_active(size_t n_args, const mp_obj_t *args) {\n"
        "    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);\n"
        "\n"
        "    if (n_args > 1) {\n"
        "        bool active = mp_obj_is_true(args[1]);\n"
        "        // Replay: never poke esp_wifi here — the Arduino C-layer\n"
        "        // (WiFiService) owns radio bring-up. active(True) only records\n"
        "        // intent; the association happens in connect(), which routes\n"
        "        // through WiFiService. active(False) tears the STA link down.\n"
        "        // Not touching esp_wifi means we can't double-init the shared\n"
        "        // driver or fight the C-layer's WiFi.mode() cycling.\n"
        "        if (!active && self->if_id == ESP_IF_WIFI_STA) {\n"
        "            replay_wifi_sta_disconnect();\n"
        "        }\n"
        "        self->active = active;\n"
        "    }\n"
        "\n"
        "    // self->active is kept current by the WIFI_EVENT STA/AP start/stop\n"
        "    // handlers, so it reflects the C-layer's real radio state without us\n"
        "    // querying esp_wifi (which would raise if the C-layer has since\n"
        "    // deinitialised the driver).\n"
        "    return self->active ? mp_const_true : mp_const_false;\n"
        "}"
    )
    assert active_old in t, "network_wlan.c: active() body not found (upstream changed?)"
    t = t.replace(active_old, active_new, 1)

    # Replay: since esp_initialise_wifi() runs once but the Arduino C-layer
    # destroys/recreates the default netifs on every WiFi.mode() cycle, refresh
    # our cached handle from the registry whenever the driver (re)starts.
    sta_start_old = (
        "        case WIFI_EVENT_STA_START:\n"
        '            ESP_LOGI("wifi", "STA_START");\n'
        "            wlan_sta_obj.active = true;\n"
        "            wifi_sta_reconnects = 0;\n"
        "            break;"
    )
    sta_start_new = (
        "        case WIFI_EVENT_STA_START:\n"
        '            ESP_LOGI("wifi", "STA_START");\n'
        "            wlan_sta_obj.active = true;\n"
        "            wifi_sta_reconnects = 0;\n"
        "            // Replay: re-resolve the C-layer-owned STA netif on every\n"
        "            // (re)start so our cached pointer never dangles.\n"
        '            wlan_sta_obj.netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");\n'
        "            break;"
    )
    assert sta_start_old in t, "network_wlan.c: STA_START case not found (upstream changed?)"
    t = t.replace(sta_start_old, sta_start_new, 1)

    sta_stop_old = (
        "        case WIFI_EVENT_STA_STOP:\n"
        "            wlan_sta_obj.active = false;\n"
        "            break;"
    )
    sta_stop_new = (
        "        case WIFI_EVENT_STA_STOP:\n"
        "            wlan_sta_obj.active = false;\n"
        "            // Replay: the C-layer destroys the netif on WiFi.mode(WIFI_OFF);\n"
        "            // drop our cached handle so the next start re-resolves it.\n"
        '            wlan_sta_obj.netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");\n'
        "            break;"
    )
    assert sta_stop_old in t, "network_wlan.c: STA_STOP case not found (upstream changed?)"
    t = t.replace(sta_stop_old, sta_stop_new, 1)

    ap_start_old = (
        "        case WIFI_EVENT_AP_START:\n"
        "            wlan_ap_obj.active = true;\n"
        "            break;"
    )
    ap_start_new = (
        "        case WIFI_EVENT_AP_START:\n"
        "            wlan_ap_obj.active = true;\n"
        '            wlan_ap_obj.netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");\n'
        "            break;"
    )
    assert ap_start_old in t, "network_wlan.c: AP_START case not found (upstream changed?)"
    t = t.replace(ap_start_old, ap_start_new, 1)

    # Replay: declare the C-layer WiFiService bridges so connect()/disconnect()/
    # active() can delegate. Inserted just before esp_initialise_wifi().
    extern_old = "void esp_initialise_wifi(void) {"
    extern_new = (
        "// Replay: bridges into the Arduino C-layer WiFiService (firmware/src/api).\n"
        "// On this badge the C-layer owns esp_wifi init + the default netifs, so\n"
        "// network.WLAN delegates association to it rather than driving esp_wifi.\n"
        "extern int replay_wifi_sta_connect(const char *ssid, const char *password);\n"
        "extern void replay_wifi_sta_disconnect(void);\n"
        "\n"
        "void esp_initialise_wifi(void) {"
    )
    assert extern_old in t, "network_wlan.c: esp_initialise_wifi definition not found (upstream changed?)"
    t = t.replace(extern_old, extern_new, 1)

    # Replay: route connect() through WiFiService instead of esp_wifi_set_config/
    # esp_wifi_connect. Blocks on the MicroPython exec task until associated or
    # timed out so a pasted REPL snippet (connect -> scan -> ifconfig) stays
    # coherent; result is observable via isconnected()/status()/ifconfig().
    connect_old = (
        "    wifi_config_t wifi_sta_config = {0};\n"
        "\n"
        "    // configure any parameters that are given\n"
        "    if (n_args > 1) {\n"
        "        size_t len;\n"
        "        const char *p;\n"
        "        if (args[ARG_ssid].u_obj != mp_const_none) {\n"
        "            p = mp_obj_str_get_data(args[ARG_ssid].u_obj, &len);\n"
        "            memcpy(wifi_sta_config.sta.ssid, p, MIN(len, sizeof(wifi_sta_config.sta.ssid)));\n"
        "        }\n"
        "        if (args[ARG_key].u_obj != mp_const_none) {\n"
        "            p = mp_obj_str_get_data(args[ARG_key].u_obj, &len);\n"
        "            memcpy(wifi_sta_config.sta.password, p, MIN(len, sizeof(wifi_sta_config.sta.password)));\n"
        "        }\n"
        "        if (args[ARG_bssid].u_obj != mp_const_none) {\n"
        "            p = mp_obj_str_get_data(args[ARG_bssid].u_obj, &len);\n"
        "            if (len != sizeof(wifi_sta_config.sta.bssid)) {\n"
        "                mp_raise_ValueError(NULL);\n"
        "            }\n"
        "            wifi_sta_config.sta.bssid_set = 1;\n"
        "            memcpy(wifi_sta_config.sta.bssid, p, sizeof(wifi_sta_config.sta.bssid));\n"
        "        }\n"
        "        esp_exceptions(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config));\n"
        "    }\n"
        "\n"
        "    esp_exceptions(esp_netif_set_hostname(wlan_sta_obj.netif, mod_network_hostname_data));\n"
        "\n"
        "    wifi_sta_reconnects = 0;\n"
        "    // connect to the WiFi AP\n"
        "    MP_THREAD_GIL_EXIT();\n"
        "    esp_exceptions(esp_wifi_connect());\n"
        "    MP_THREAD_GIL_ENTER();\n"
        "    wifi_sta_connect_requested = true;\n"
        "\n"
        "    return mp_const_none;"
    )
    connect_new = (
        "    // Replay: route association through the Arduino C-layer (WiFiService);\n"
        "    // it is the sole owner of esp_wifi init + netifs on this badge, so\n"
        "    // esp_wifi_set_config/esp_wifi_connect from here would fight it. We\n"
        "    // forward only the SSID/key (BSSID pinning is not supported via the\n"
        "    // C-layer path) and deliberately do NOT set wifi_sta_connect_requested,\n"
        "    // so MicroPython's STA_DISCONNECTED auto-reconnect stays off and does\n"
        "    // not race the C-layer.\n"
        "    char replay_ssid[33] = {0};\n"
        "    char replay_key[64] = {0};\n"
        "    if (n_args > 1) {\n"
        "        size_t len;\n"
        "        const char *p;\n"
        "        if (args[ARG_ssid].u_obj != mp_const_none) {\n"
        "            p = mp_obj_str_get_data(args[ARG_ssid].u_obj, &len);\n"
        "            memcpy(replay_ssid, p, MIN(len, sizeof(replay_ssid) - 1));\n"
        "        }\n"
        "        if (args[ARG_key].u_obj != mp_const_none) {\n"
        "            p = mp_obj_str_get_data(args[ARG_key].u_obj, &len);\n"
        "            memcpy(replay_key, p, MIN(len, sizeof(replay_key) - 1));\n"
        "        }\n"
        "    }\n"
        "    if (replay_ssid[0] == 0) {\n"
        '        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("connect requires an SSID"));\n'
        "    }\n"
        "\n"
        "    MP_THREAD_GIL_EXIT();\n"
        "    replay_wifi_sta_connect(replay_ssid, replay_key);\n"
        "    MP_THREAD_GIL_ENTER();\n"
        "\n"
        "    return mp_const_none;"
    )
    assert connect_old in t, "network_wlan.c: connect() body not found (upstream changed?)"
    t = t.replace(connect_old, connect_new, 1)

    disconnect_old = (
        "static mp_obj_t network_wlan_disconnect(mp_obj_t self_in) {\n"
        "    wifi_sta_connect_requested = false;\n"
        "    esp_exceptions(esp_wifi_disconnect());\n"
        "    return mp_const_none;\n"
        "}"
    )
    disconnect_new = (
        "static mp_obj_t network_wlan_disconnect(mp_obj_t self_in) {\n"
        "    wifi_sta_connect_requested = false;\n"
        "    // Replay: tear down via the C-layer (WiFiService) which owns the radio.\n"
        "    replay_wifi_sta_disconnect();\n"
        "    return mp_const_none;\n"
        "}"
    )
    assert disconnect_old in t, "network_wlan.c: disconnect() body not found (upstream changed?)"
    t = t.replace(disconnect_old, disconnect_new, 1)

    p.write_text(t)
PY

# MicroPython core includes these as angle-bracket headers.
# Skip redundant copy when canonical source already lives at destination.
if [[ "${MPCONFIG_SRC}" != "${LIB_SRC_DIR}/mpconfigport.h" ]]; then
  cp "${MPCONFIG_SRC}" "${LIB_SRC_DIR}/mpconfigport.h"
fi
cp "${LIB_SRC_DIR}/port/mphalport.h" "${LIB_SRC_DIR}/mphalport.h"
cp "${MPHAL_SRC}" "${LIB_SRC_DIR}/port/mphalport.c"
cp "${MODMACHINE_SRC}" "${LIB_SRC_DIR}/port/modmachine_esp32_subset.c"

# Replay: bracket mp_embed_exec_str with VM hooks (see MicroPythonBridge.cpp).
if [[ -f "${EMBED_UTIL_SRC}" ]]; then
  cp "${EMBED_UTIL_SRC}" "${LIB_SRC_DIR}/port/embed_util.c"
fi

# Replay: MicroPython native block device wrapper is auto-copied via ports/embed/port/*.[ch]

# # Event-driven REPL needs an extra GC root pointer for repl_line.
# ROOT_PTRS_FILE="${LIB_SRC_DIR}/genhdr/root_pointers.h"
# if [[ -f "${ROOT_PTRS_FILE}" ]]; then
#   if ! rg -q '^vstr_t \*repl_line;$' "${ROOT_PTRS_FILE}"; then
#     echo "vstr_t *repl_line;" >> "${ROOT_PTRS_FILE}"
#   fi
# fi

# (Removed custom root pointers step: VFS roots are auto-discovered via SRC_QSTR now)

# (Removed custom QSTR step: FlashBdev is now auto-discovered from replay_bdev.c)
# ── PlatformIO library manifest ─────────────────────────────────────────────
LIB_JSON_TEMPLATE="${SCRIPT_DIR}/library.json.embed"
if [[ -f "${LIB_JSON_TEMPLATE}" ]]; then
  cp "${LIB_JSON_TEMPLATE}" "${LIB_DIR}/library.json"
else
  echo "Warning: missing ${LIB_JSON_TEMPLATE}; leaving ${LIB_DIR}/library.json unchanged." >&2
fi

cat > "${LIB_SRC_DIR}/micropython_embed.h" <<'EOF'
/* Trigger header for PlatformIO local library detection. */
#include "port/micropython_embed.h"
EOF

echo "============================================================"
echo " Done!  Generated library at:"
echo "   ${LIB_DIR}"
echo ""
echo " Next steps:"
echo "   pio run -e esp32-s3-devkitc-1-n16r8"
echo ""
echo " Re-run this script if you:"
echo "   - Add new MP_QSTR_* names in modtemporalbadge.c"
echo "   - Change mpconfigport.h"
echo "   - Update the MicroPython submodule"
echo "============================================================"
