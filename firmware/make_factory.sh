#!/usr/bin/env bash
# Produce a single fully-extracted 16MB factory image for the badge.
#
# Usage:
#   ./make_factory.sh [env] [--no-build] [--no-fs]
#     env         PlatformIO env (default: replay2026)
#     --no-build  skip pio build, reuse last .pio/build/<env> artifacts
#     --no-fs     omit the FAT filesystem image (apps/lib/doom1.wad).
#                 Resulting badge will boot but show an empty apps menu and
#                 doom won't run until you push a fatfs over USB.
#
# Output: firmware/factory_<env>_16MB.bin
#
# Layout: bootloader@0x0, partitions@0x8000, app0@0x10000, ffat@(env-specific
# offset parsed from the partitions CSV referenced in platformio.ini),
# padded with 0xFF to 16MB so it can be flashed from 0x0 in one shot.
#
# Flash params: keep (don't rewrite the bootloader's DIO/80m header — ESP32-S3
# with OPI PSRAM ships a DIO bootloader on purpose; QIO collides with the
# OPI PSRAM data lines until the second stage reconfigures pins).
#
# otadata (0xe000) and coredump (0xFD0000) are intentionally left as 0xFF —
# blank otadata means "boot app0" to the IDF bootloader.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIO="$HOME/.platformio/penv/bin/pio"
ESPTOOL="$HOME/.platformio/penv/bin/esptool"

ENV="replay2026"
DO_BUILD=1
DO_FS=1
for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --no-fs)    DO_FS=0 ;;
    -h|--help)
      sed -n '2,22p' "$0"
      exit 0
      ;;
    -*)
      echo "ERROR: unknown flag $arg" >&2
      exit 2
      ;;
    *) ENV="$arg" ;;
  esac
done

if [[ ! -x "$ESPTOOL" ]]; then
  echo "ERROR: esptool not found at $ESPTOOL — run ignition/setup.sh" >&2
  exit 1
fi

BUILD_DIR="$SCRIPT_DIR/.pio/build/$ENV"
OUT="$SCRIPT_DIR/factory_${ENV}_16MB.bin"

# ── Resolve which partitions CSV this env uses ────────────────────────────────
# Pull the value out of any [<section>] block (env:<name> or base). Returns
# empty if the section doesn't define board_build.partitions.
section_partitions_csv() {
  local section="$1" in_sec=0 csv=""
  while IFS= read -r line; do
    if [[ "$line" =~ ^\[${section}\] ]]; then in_sec=1; continue; fi
    if [[ $in_sec -eq 1 && "$line" =~ ^\[ ]]; then break; fi
    if [[ $in_sec -eq 1 && "$line" =~ ^[[:space:]]*board_build\.partitions[[:space:]]*=[[:space:]]*(.*)$ ]]; then
      csv="${BASH_REMATCH[1]}"; csv="${csv%%[[:space:]]*}"
    fi
  done < "$SCRIPT_DIR/platformio.ini"
  echo "$csv"
}

# Follow `extends = env:<other>` from the section so inherited values work.
section_extends() {
  local section="$1" in_sec=0 ext=""
  while IFS= read -r line; do
    if [[ "$line" =~ ^\[${section}\] ]]; then in_sec=1; continue; fi
    if [[ $in_sec -eq 1 && "$line" =~ ^\[ ]]; then break; fi
    if [[ $in_sec -eq 1 && "$line" =~ ^[[:space:]]*extends[[:space:]]*=[[:space:]]*(.*)$ ]]; then
      ext="${BASH_REMATCH[1]}"; ext="${ext%%[[:space:]]*}"
    fi
  done < "$SCRIPT_DIR/platformio.ini"
  echo "$ext"
}

# Walk env:<env> → its extends parent → … → base, returning the first
# board_build.partitions found. Mirrors PlatformIO's own resolution order.
resolve_partitions_csv() {
  local section="env:$1"
  local seen=" "
  while [[ -n "$section" ]]; do
    case "$seen" in *" $section "*) break ;; esac
    seen="$seen$section "
    local csv
    csv="$(section_partitions_csv "$section")"
    if [[ -n "$csv" ]]; then echo "$csv"; return; fi
    section="$(section_extends "$section")"
  done
  section_partitions_csv "base"
}

# Read offset (col 4) for partition <name> from a partitions CSV.
# CSV format: Name,Type,SubType,Offset,Size,Flags
partition_offset() {
  local csv="$1" name="$2"
  awk -F, -v n="$name" '
    /^[[:space:]]*#/ {next}
    /^[[:space:]]*$/ {next}
    {
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $1)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $4)
      if ($1 == n) { print $4; exit }
    }' "$csv"
}

PARTITIONS_CSV_NAME="$(resolve_partitions_csv "$ENV")"
if [[ -z "$PARTITIONS_CSV_NAME" ]]; then
  echo "ERROR: could not resolve board_build.partitions for env=$ENV" >&2
  exit 1
fi
PARTITIONS_CSV="$SCRIPT_DIR/$PARTITIONS_CSV_NAME"
if [[ ! -f "$PARTITIONS_CSV" ]]; then
  echo "ERROR: partitions CSV not found: $PARTITIONS_CSV" >&2
  exit 1
fi
FFAT_OFFSET="$(partition_offset "$PARTITIONS_CSV" ffat)"
echo "==> env=$ENV  partitions=$PARTITIONS_CSV_NAME  ffat=${FFAT_OFFSET:-<none>}"

# ── Build firmware + filesystem ───────────────────────────────────────────────
if [[ "$DO_BUILD" -eq 1 ]]; then
  if [[ ! -x "$PIO" ]]; then
    echo "ERROR: pio not found at $PIO — run ignition/setup.sh" >&2
    exit 1
  fi
  echo "==> Building env=$ENV (firmware)"
  "$PIO" run -e "$ENV" -d "$SCRIPT_DIR"
  if [[ "$DO_FS" -eq 1 ]]; then
    echo "==> Building env=$ENV (filesystem image)"
    "$PIO" run -e "$ENV" -t buildfs -d "$SCRIPT_DIR"
  fi
fi

for f in bootloader.bin partitions.bin firmware.bin; do
  if [[ ! -f "$BUILD_DIR/$f" ]]; then
    echo "ERROR: missing $BUILD_DIR/$f (build first, or drop --no-build)" >&2
    exit 1
  fi
done

# ── Locate fatfs.bin (or littlefs.bin / spiffs.bin as fallbacks) ─────────────
FS_IMAGE=""
if [[ "$DO_FS" -eq 1 ]]; then
  for candidate in fatfs.bin littlefs.bin spiffs.bin; do
    if [[ -f "$BUILD_DIR/$candidate" ]]; then
      FS_IMAGE="$BUILD_DIR/$candidate"
      break
    fi
  done
  if [[ -z "$FS_IMAGE" ]]; then
    echo "ERROR: no filesystem image found in $BUILD_DIR" >&2
    echo "       Expected fatfs.bin (or pass --no-fs to skip)" >&2
    exit 1
  fi
  if [[ -z "$FFAT_OFFSET" ]]; then
    echo "ERROR: have $FS_IMAGE but no ffat partition in $PARTITIONS_CSV_NAME" >&2
    exit 1
  fi
  FS_SIZE=$(stat -c%s "$FS_IMAGE" 2>/dev/null || stat -f%z "$FS_IMAGE")
  echo "==> Filesystem image: ${FS_IMAGE##*/} ($FS_SIZE bytes) -> $FFAT_OFFSET"
fi

# ── Merge ─────────────────────────────────────────────────────────────────────
echo "==> Merging into $OUT"
MERGE_ARGS=(
  --chip esp32s3 merge-bin
  --output "$OUT"
  --flash-mode keep
  --flash-freq keep
  --flash-size keep
  --pad-to-size 16MB
  0x0     "$BUILD_DIR/bootloader.bin"
  0x8000  "$BUILD_DIR/partitions.bin"
  0x10000 "$BUILD_DIR/firmware.bin"
)
if [[ -n "$FS_IMAGE" ]]; then
  MERGE_ARGS+=("$FFAT_OFFSET" "$FS_IMAGE")
fi
"$ESPTOOL" "${MERGE_ARGS[@]}"

SIZE=$(stat -c%s "$OUT" 2>/dev/null || stat -f%z "$OUT")
printf '==> Done. %s (%s bytes = %s MB)\n' "$OUT" "$SIZE" "$((SIZE / 1024 / 1024))"
