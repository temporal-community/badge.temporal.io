#!/usr/bin/env bash
# build.sh — Build firmware for a target environment.
#
# Usage:
#   ./build.sh                   Build replay2026
#   ./build.sh replay2026 -n     Build replay2026, skip optional WiFi prompt
#   ./build.sh --help

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
rm -f /tmp/badge-build-*.log 2>/dev/null || true
LOG_FILE="$(mktemp /tmp/badge-build-XXXXXX.log)"
BOLD=$(tput bold 2>/dev/null || echo "")
RESET=$(tput sgr0 2>/dev/null || echo "")
GREEN="\033[32m"
RED="\033[31m"
YELLOW="\033[33m"
DIM="\033[2m"
NC="\033[0m"


# ── PlatformIO ────────────────────────────────────────────────────────────────

resolve_pio_cmd() {
    local candidates=()
    [[ -n "${PIO_CMD:-}" ]] && candidates+=("${PIO_CMD}")
    [[ -x "${HOME}/.platformio/penv/bin/pio" ]] && candidates+=("${HOME}/.platformio/penv/bin/pio")
    command -v pio &>/dev/null && candidates+=("$(command -v pio)")

    for c in "${candidates[@]}"; do
        if "${c}" --version >/dev/null 2>&1; then
            echo "${c}"
            return 0
        fi
    done
    return 1
}

PIO_BIN="$(resolve_pio_cmd || true)"
if [[ -z "$PIO_BIN" ]]; then
    echo
    echo "  ${RED}✗${NC}  PlatformIO not found."
    echo "       Run ${BOLD}cd ../ignition && ./setup.sh${RESET} to install it."
    echo
    exit 1
fi


# ── Optional build WiFi config helpers ────────────────────────────────────────

WIFI_ENV_FILE="${SCRIPT_DIR}/wifi.local.env"

read_env_setting() {
    local key="$1" file="$2"
    [[ -f "$file" ]] || return 0
    awk -F'=' -v k="$key" '
        $1 ~ ("^[[:space:]]*(export[[:space:]]+)?" k "[[:space:]]*$") {
            val=$0
            sub(/^[^=]*=/, "", val)
            sub(/^[[:space:]]+/, "", val)
            sub(/[[:space:]]+$/, "", val)
            gsub(/^["'\'']|["'\'']$/, "", val)
            print val
            exit
        }
    ' "$file"
}

write_build_env() {
    local ssid="$1" pass="$2" content
    content="$(cat <<EOF
# Optional local build-time WiFi config. Ignored by git.
# Used only by explicit MicroPython networking calls.
BADGE_WIFI_SSID=$ssid
BADGE_WIFI_PASS=$pass
EOF
)"
    echo "$content" > "$WIFI_ENV_FILE"
}

maybe_update_config() {
    [[ ! -t 0 ]] && return  # non-interactive

    local ssid pass input
    ssid="${BADGE_WIFI_SSID:-$(read_env_setting BADGE_WIFI_SSID "$WIFI_ENV_FILE")}"
    pass="${BADGE_WIFI_PASS:-${BADGE_WIFI_PASSWORD:-$(read_env_setting BADGE_WIFI_PASS "$WIFI_ENV_FILE")}}"

    echo
    if [[ -n "$ssid" ]]; then
        echo -n "  Optional build WiFi config exists — change it? [y/N]: "
    else
        echo -n "  Optional build WiFi config missing — create it now? [y/N]: "
    fi
    read -r answer
    [[ "$answer" != [yY]* ]] && return

    echo -n "  SSID [${ssid:+configured}]: "; read -r input; [[ -n "$input" ]] && ssid="$input"
    echo -n "  Password [${pass:+configured}] (Enter to keep): "; read -rsp "" input; echo
    [[ -n "$input" ]] && pass="$input"

    if [[ "$ssid" == *$'\n'* || "$pass" == *$'\n'* ]]; then
        echo "  error: values cannot contain newlines" >&2; exit 1
    fi

    write_build_env "$ssid" "$pass"
    echo "  Optional WiFi config saved to wifi.local.env."
}


# ── Arg parsing ───────────────────────────────────────────────────────────────

ENV="" SKIP_CONFIG=false
DEFAULT_ENV="replay2026"

for arg in "$@"; do
    case "$arg" in
        -n|--no-config) SKIP_CONFIG=true ;;
        -h|--help)
            echo "Usage: ./build.sh [env] [-n]"
            echo "  env    PlatformIO environment (default: replay2026)"
            echo "  -n     Skip optional WiFi config prompt"
            exit 0 ;;
        *)
            if [[ -z "$ENV" ]]; then ENV="$arg"
            else echo "error: unknown argument '$arg'" >&2; exit 1; fi ;;
    esac
done

if [[ -z "$ENV" ]]; then
    ENV="$DEFAULT_ENV"
fi

if [[ "$SKIP_CONFIG" != true && "${BADGE_BUILD_PROMPT_WIFI:-0}" == "1" ]]; then
    maybe_update_config
fi


# ── Build ─────────────────────────────────────────────────────────────────────

echo
echo "  ${BOLD}Building firmware${RESET}  [${ENV}]"
echo -n "  "

# Run pio, tee to log, show a simple progress indicator
SECONDS=0
"$PIO_BIN" run -e "$ENV" 2>&1 | tee "$LOG_FILE" | \
    grep -E "^(Compiling|Linking|Building|ERROR|error:|In file|warning:|SUCCESS|FAILED|Environment)" | \
    while IFS= read -r line; do
        case "$line" in
            Compiling*)  echo -n "." ;;
            Linking*)    echo -n " linking" ;;
            Building*)   echo -n " packaging" ;;
            SUCCESS*|*succeeded*) ;;
            ERROR*|error:*|FAILED*|*failed*)
                echo; echo "  ${RED}✗${NC}  $line" ;;
        esac
    done
echo

BUILD_EXIT=${PIPESTATUS[0]}
DURATION=$SECONDS


# ── Result ────────────────────────────────────────────────────────────────────

if [[ $BUILD_EXIT -eq 0 ]]; then
    echo
    echo -e "  ${GREEN}${BOLD}✓  Build complete${RESET}  ${DIM}(${DURATION}s)${NC}"

    # Show the memory summary table from the log
    echo
    grep -A 20 "Memory Type" "$LOG_FILE" 2>/dev/null | head -20 | \
        sed 's/^/  /' || true
    echo
else
    echo
    echo -e "  ${RED}${BOLD}✗  Build failed${RESET}"
    echo
    echo "  Last errors:"
    grep -E "error:|Error|FAILED" "$LOG_FILE" 2>/dev/null | tail -20 | sed 's/^/    /' || true
    echo
    echo -e "  ${DIM}Full log: $LOG_FILE${NC}"
    echo
    exit 1
fi

rm -f "$LOG_FILE"
