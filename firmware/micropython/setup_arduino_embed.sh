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
path.write_text(text)
PY
cp "${SCRIPT_DIR}/embed_config/mphalport.h" "${MP_DIR}/ports/embed/port/mphalport.h"
cp "${MP_DIR}/ports/embed/port/mphalport.h" "${MP_DIR}/mphalport.h"
cp "${SCRIPT_DIR}/embed_config/replay_bdev.c" "${MP_DIR}/ports/embed/port/replay_bdev.c"
cp "${SCRIPT_DIR}/embed_config/replay_os_includefile.c" "${MP_DIR}/ports/embed/port/replay_os_includefile.c"
cp "${SCRIPT_DIR}/embed_config/replay_machine_qstr.c" "${MP_DIR}/ports/embed/port/replay_machine_qstr.c"
cp "${SCRIPT_DIR}/embed_config/replay_extra_rootpointers.c" "${MP_DIR}/ports/embed/port/replay_extra_rootpointers.c"
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
