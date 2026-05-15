#!/usr/bin/env bash
# bump_version.sh — bump the canonical firmware version.
#
# Usage:
#   ./scripts/bump_version.sh 0.1.3        # set explicit version
#   ./scripts/bump_version.sh patch        # 0.1.4 -> 0.1.3
#   ./scripts/bump_version.sh minor        # 0.1.4 -> 0.2.0
#   ./scripts/bump_version.sh major        # 0.1.4 -> 1.0.0
#
# Writes firmware/VERSION, stages it, and prints a suggested commit message.
# Does NOT commit or tag — the next `pio run` (via scripts/inject_version.py)
# picks up the new value automatically and surfaces it in:
#   - C++:    BootSplash, DiagnosticsScreen, etc. (via FIRMWARE_VERSION)
#   - Python: REPL banner "Replay Badge v<ver> with ESP32-S3"
#             (via BADGE_FIRMWARE_VERSION → MICROPY_BANNER_MACHINE)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VERSION_FILE="${FW_DIR}/VERSION"

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

[[ $# -eq 1 ]] || usage 1
[[ "$1" == "-h" || "$1" == "--help" ]] && usage 0

CUR="$(tr -d '[:space:]' < "$VERSION_FILE" 2>/dev/null || echo "0.0.0")"
ARG="$1"

bump_part() {
    local part="$1" cur="$2"
    IFS='.' read -r MA MI PA <<< "$cur"
    MA="${MA:-0}"; MI="${MI:-0}"; PA="${PA:-0}"
    case "$part" in
        major) echo "$((MA + 1)).0.0" ;;
        minor) echo "${MA}.$((MI + 1)).0" ;;
        patch) echo "${MA}.${MI}.$((PA + 1))" ;;
        *) return 1 ;;
    esac
}

if [[ "$ARG" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    NEW="$ARG"
elif NEW="$(bump_part "$ARG" "$CUR" 2>/dev/null)"; then
    :
else
    echo "error: '$ARG' is not a semver or one of: major / minor / patch" >&2
    exit 1
fi

if [[ "$NEW" == "$CUR" ]]; then
    echo "VERSION already $NEW — nothing to do."
    exit 0
fi

echo "$NEW" > "$VERSION_FILE"
echo "VERSION: $CUR -> $NEW"

if git -C "$FW_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$FW_DIR" add -- "$VERSION_FILE"
    cat <<EOF

  Staged firmware/VERSION. Suggested commit:

    git commit -m "chore: bump firmware to v${NEW}"
    git tag -a "v${NEW}" -m "v${NEW}"

EOF
fi
