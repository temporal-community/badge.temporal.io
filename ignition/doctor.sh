#!/usr/bin/env bash
# doctor.sh - Check the local Ignition Python, certificate, PlatformIO, and
# Temporal setup before a badge flashing session.

set +e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOLD=$(tput bold 2>/dev/null || echo ""); RESET=$(tput sgr0 2>/dev/null || echo "")
GREEN="\033[32m"; YELLOW="\033[33m"; RED="\033[31m"; NC="\033[0m"
PASS=0
WARN=0
FAIL=0

ok() { echo -e "  ${GREEN}✓${NC}  $*"; PASS=$((PASS + 1)); }
warn() { echo -e "  ${YELLOW}!${NC}  $*"; WARN=$((WARN + 1)); }
bad() { echo -e "  ${RED}✗${NC}  $*"; FAIL=$((FAIL + 1)); }

resolve_temporal_cmd() {
    local candidates=()
    command -v temporal &>/dev/null && candidates+=("$(command -v temporal)")
    [[ -x "${HOME}/.local/bin/temporal" ]] && candidates+=("${HOME}/.local/bin/temporal")

    for c in "${candidates[@]}"; do
        if "${c}" --version >/dev/null 2>&1; then
            echo "${c}"
            return 0
        fi
    done
    return 1
}

echo
echo "  ${BOLD}Temporal Badge - Ignition Doctor${RESET}"
echo "  ═════════════════════════════════"

VENV_PYTHON="$SCRIPT_DIR/.venv/bin/python"
if [[ -x "$VENV_PYTHON" ]]; then
    PY_VERSION="$("$VENV_PYTHON" -c 'import sys; print(".".join(map(str, sys.version_info[:3])))' 2>/dev/null)"
    ok "Ignition virtualenv Python $PY_VERSION"
else
    bad "Ignition virtualenv is missing. Run: cd ignition && ./setup.sh"
fi

if [[ -x "$VENV_PYTHON" ]]; then
"$VENV_PYTHON" - <<'PY'
import importlib.util
mods = ["temporalio", "rich", "serial", "certifi"]
missing = [m for m in mods if importlib.util.find_spec(m) is None]
raise SystemExit(1 if missing else 0)
PY
    if [[ $? -eq 0 ]]; then
        ok "Python dependencies import cleanly"
    else
        bad "Python dependencies are incomplete. Run: cd ignition && ./setup.sh"
    fi

    "$VENV_PYTHON" - <<'PY'
import certifi
import ssl
import urllib.request

url = "https://raw.githubusercontent.com/micropython/micropython/v1.27.0/README.md"
ctx = ssl.create_default_context(cafile=certifi.where())
with urllib.request.urlopen(url, timeout=20, context=ctx) as resp:
    resp.read(64)
PY
    case $? in
        0) ok "Python HTTPS/certificate check against GitHub passed" ;;
        *) bad "Python HTTPS/certificate check failed. Re-run ./setup.sh, then check corporate proxy or OS trust settings." ;;
    esac
fi

PIO_BIN="${HOME}/.platformio/penv/bin/pio"
if [[ -x "$PIO_BIN" ]] && "$PIO_BIN" --version >/dev/null 2>&1; then
    ok "PlatformIO available at $PIO_BIN"
elif command -v pio >/dev/null 2>&1 && pio --version >/dev/null 2>&1; then
    ok "PlatformIO available on PATH"
else
    bad "PlatformIO not found. Run: cd ignition && ./setup.sh"
fi

TEMPORAL_BIN="$(resolve_temporal_cmd || true)"
if [[ -n "$TEMPORAL_BIN" ]]; then
    ok "Temporal CLI available at $TEMPORAL_BIN"
else
    bad "Temporal CLI not found. Run: cd ignition && ./setup.sh"
fi

if compgen -G "/dev/cu.usbmodem*" >/dev/null; then
    ok "Badge-like USB serial ports detected: $(ls /dev/cu.usbmodem* | tr '\n' ' ')"
else
    warn "No /dev/cu.usbmodem* ports detected. Connect badges before flashing."
fi

echo
echo "  ${BOLD}Summary:${RESET} ${PASS} passed, ${WARN} warning(s), ${FAIL} failed"
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
