#!/usr/bin/env bash
# setup.sh — First-time setup for Temporal Badge firmware toolchain.
#
# Installs: PlatformIO, Temporal CLI, Python worker deps.
# Safe to re-run — skips anything already installed.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOLD=$(tput bold 2>/dev/null || echo ""); RESET=$(tput sgr0 2>/dev/null || echo "")
GREEN="\033[32m"; YELLOW="\033[33m"; RED="\033[31m"; NC="\033[0m"
ok()   { echo -e "  ${GREEN}✓${NC}  $*"; }
skip() { echo -e "  ${YELLOW}–${NC}  $*  (already installed)"; }
fail() { echo -e "  ${RED}✗${NC}  $*" >&2; exit 1; }
step() { echo; echo "  ${BOLD}$*${RESET}"; }

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

echo; echo "  ${BOLD}Temporal Badge — Setup${RESET}"; echo "  ══════════════════════"

# Python
step "Python"
PYTHON=""
for cmd in python3 python; do
    if command -v "$cmd" &>/dev/null; then
        v=$("$cmd" -c "import sys; print(sys.version_info.major, sys.version_info.minor)")
        [[ "${v%% *}" -ge 3 && "${v##* }" -ge 9 ]] && PYTHON="$cmd" && break
    fi
done
[[ -z "$PYTHON" ]] && fail "Python 3.9+ required. Install from https://python.org"
skip "Python ($("$PYTHON" --version))"

# PlatformIO
step "PlatformIO"
PIO_BIN="${HOME}/.platformio/penv/bin/pio"
if [[ -x "$PIO_BIN" ]] && "$PIO_BIN" --version &>/dev/null; then
    skip "PlatformIO ($("$PIO_BIN" --version))"
else
    echo "  Installing PlatformIO..."
    curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
    "$PYTHON" /tmp/get-platformio.py
    rm -f /tmp/get-platformio.py
    ok "PlatformIO installed"
fi

# Temporal CLI
step "Temporal CLI"
TEMPORAL_BIN="$(resolve_temporal_cmd || true)"
if [[ -n "$TEMPORAL_BIN" ]]; then
    skip "temporal ($("$TEMPORAL_BIN" --version 2>/dev/null | head -1))"
elif command -v brew &>/dev/null; then
    echo "  Installing via Homebrew..."
    brew install temporal
    TEMPORAL_BIN="$(resolve_temporal_cmd || true)"
    [[ -n "$TEMPORAL_BIN" ]] || fail "Temporal CLI install completed but temporal was not found."
    ok "Temporal CLI installed"
else
    echo "  Installing Temporal CLI..."
    TDIR="${HOME}/.local/bin"; mkdir -p "$TDIR"
    curl -sSf https://temporal.download/cli.sh | sh -s -- --install-dir "$TDIR"
    [[ -x "$TDIR/temporal" ]] || fail "Temporal CLI install did not create $TDIR/temporal"
    ok "Temporal CLI installed to $TDIR"
    echo "  start.sh will use this path directly; adding it to PATH is optional."
fi

# Virtualenv
step "Virtualenv"
VENV_DIR="$SCRIPT_DIR/.venv"
if [[ -d "$VENV_DIR" ]] && "$VENV_DIR/bin/python" -c "import temporalio, rich, serial, certifi" &>/dev/null; then
    skip "Virtualenv already set up at ignition/.venv"
else
    if [[ ! -d "$VENV_DIR" ]]; then
        echo "  Creating virtualenv..."
        "$PYTHON" -m venv "$VENV_DIR"
    fi
    echo "  Installing dependencies into virtualenv..."
    "$VENV_DIR/bin/python" -m pip install -q --upgrade pip setuptools wheel
    "$VENV_DIR/bin/python" -m pip install -q -r "$SCRIPT_DIR/flash_worker/requirements.txt"
    ok "Virtualenv ready at ignition/.venv  (temporalio, rich, pyserial, certifi)"
fi

CERT_FILE="$("$VENV_DIR/bin/python" -c "import certifi; print(certifi.where())")"
export SSL_CERT_FILE="$CERT_FILE"
export REQUESTS_CA_BUNDLE="$CERT_FILE"
ok "Python HTTPS trust configured with certifi"

# Vendor upstream MicroPython sources for full module surface (network, ssl,
# bluetooth, _thread, etc.). Safe to skip if it fails (offline checkout) — the
# embed port still builds without these and the gating flags default to off.
step "MicroPython sources"
FW_DIR="$SCRIPT_DIR/../firmware"
FETCH_SCRIPT="$FW_DIR/scripts/fetch_micropython_sources.py"
if [[ -x "$FETCH_SCRIPT" ]]; then
    if "$VENV_DIR/bin/python" "$FETCH_SCRIPT"; then
        ok "Upstream MicroPython sources vendored (or already up to date)"
    else
        echo -e "  ${YELLOW}–${NC}  fetch_micropython_sources.py failed (offline?). Network/BT/thread modules will stay disabled until you re-run this step."
        echo "     Try: cd ignition && ./doctor.sh"
    fi
fi

# Settings file
step "Configuration"
SETTINGS="$FW_DIR/settings.txt"; EXAMPLE="$FW_DIR/settings.txt.example"
if [[ -f "$SETTINGS" ]]; then
    skip "firmware/settings.txt already exists"
elif [[ -f "$EXAMPLE" ]]; then
    cp "$EXAMPLE" "$SETTINGS"; ok "Created firmware/settings.txt from example"
    echo; echo "  Optional local badge settings live in ${BOLD}firmware/settings.txt${RESET}."
fi

echo; echo "  ${BOLD}Setup complete.${RESET}"
echo; echo "  Next: run ${BOLD}./doctor.sh${RESET} if you want a preflight check, then ${BOLD}./start.sh${RESET}"
echo
