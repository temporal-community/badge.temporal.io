#!/usr/bin/env bash
# start.sh — Build and flash Temporal Badge firmware.
#
# One command does everything:
#   1. Starts Temporal server  (skipped if already running on :7233)
#   2. Starts the flash worker (skipped if already running)
#   3. Builds firmware + flashes all connected badges
#
# First time? Run ./setup.sh first.
#
# Usage:
#   ./start.sh                           Build replay2026 + flash all badges
#   ./start.sh -e replay2026             Explicit replay2026 environment
#   ./start.sh --firmware-dir /path      Different firmware directory
#   ./start.sh -y                        Skip the pre-flash Enter prompt
#   ./start.sh --no-build                Flash only (skip build)
#   ./start.sh --build-only              Build only (no flash)
#   ./start.sh --no-build --factory-image PATH
#                                           Flash a downloaded factory image
#   ./start.sh --build-and-flash         Explicit build + full filesystem flash
#   ./start.sh --wifi-ssid NAME --wifi-pass PASS
#                                           Embed build-time WiFi credentials

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOLD=$(tput bold 2>/dev/null || echo ""); RESET=$(tput sgr0 2>/dev/null || echo "")
GREEN="\033[32m"; YELLOW="\033[33m"; RED="\033[31m"; NC="\033[0m"
ok()     { echo -e "  ${GREEN}✓${NC}  $*"; }
waiting(){ echo -e "  ${YELLOW}…${NC}  $*"; }
fail()   { echo -e "  ${RED}✗${NC}  $*" >&2; }

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

cleanup_worker() {
    # Kill the worker — it's only needed during the flash run.
    # Temporal server stays alive so you can review logs at http://localhost:8233
    if [[ -f /tmp/temporal-badge-worker.pid ]]; then
        kill "$(cat /tmp/temporal-badge-worker.pid)" 2>/dev/null || true
        rm -f /tmp/temporal-badge-worker.pid
    fi
    pkill -f "flash_worker.worker" 2>/dev/null || true
}

trap cleanup_worker EXIT

FORWARD_ARGS=()
WIFI_SSID_ARG="${BADGE_WIFI_SSID:-}"
WIFI_PASS_ARG="${BADGE_WIFI_PASS:-${BADGE_WIFI_PASSWORD:-}}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wifi-ssid)
            [[ $# -ge 2 ]] || { fail "--wifi-ssid requires a value"; exit 2; }
            WIFI_SSID_ARG="$2"
            shift 2
            ;;
        --wifi-ssid=*)
            WIFI_SSID_ARG="${1#*=}"
            shift
            ;;
        --wifi-pass|--wifi-password)
            [[ $# -ge 2 ]] || { fail "$1 requires a value"; exit 2; }
            WIFI_PASS_ARG="$2"
            shift 2
            ;;
        --wifi-pass=*|--wifi-password=*)
            WIFI_PASS_ARG="${1#*=}"
            shift
            ;;
        *)
            FORWARD_ARGS+=("$1")
            shift
            ;;
    esac
done

if [[ -n "$WIFI_SSID_ARG" || -n "$WIFI_PASS_ARG" ]]; then
    if [[ -z "$WIFI_SSID_ARG" || -z "$WIFI_PASS_ARG" ]]; then
        fail "Set both --wifi-ssid and --wifi-pass, or neither."
        exit 2
    fi
    export BADGE_WIFI_SSID="$WIFI_SSID_ARG"
    export BADGE_WIFI_PASS="$WIFI_PASS_ARG"
fi

echo; echo "  ${BOLD}Temporal Badge — Build & Flash${RESET}"; echo "  ══════════════════════════════"; echo
if [[ -n "${BADGE_WIFI_SSID:-}" && -n "${BADGE_WIFI_PASS:-}" ]]; then
    ok "Build-time WiFi credentials enabled for SSID '${BADGE_WIFI_SSID}'"
fi

# ── Virtualenv ────────────────────────────────────────────────────────────────
VENV_PYTHON="$SCRIPT_DIR/.venv/bin/python"
if [[ ! -x "$VENV_PYTHON" ]]; then
    fail "Virtualenv not found at ignition/.venv"
    fail "Run ./setup.sh first to set everything up."
    exit 1
fi

# ── Temporal server ───────────────────────────────────────────────────────────
temporal_running() {
    "$VENV_PYTHON" -c "
import socket, sys
try:
    s = socket.create_connection(('localhost', 7233), timeout=1); s.close(); sys.exit(0)
except Exception: sys.exit(1)" 2>/dev/null
}

if temporal_running; then
    ok "Temporal server already running"
else
    TEMPORAL_BIN="$(resolve_temporal_cmd || true)"
    if [[ -z "$TEMPORAL_BIN" ]]; then
        fail "Temporal CLI not found. Run ./setup.sh to install it."; exit 1
    fi
    waiting "Starting Temporal server..."
    nohup "$TEMPORAL_BIN" server start-dev --namespace default --log-level error \
        > /tmp/temporal-badge-server.log 2>&1 </dev/null &
    echo "$!" > /tmp/temporal-badge-server.pid
    for i in $(seq 1 20); do
        temporal_running && break
        sleep 0.5
        [[ $i -eq 20 ]] && { fail "Temporal server failed to start."; fail "Logs: /tmp/temporal-badge-server.log"; exit 1; }
    done
    ok "Temporal server started"
fi

# ── Flash worker ──────────────────────────────────────────────────────────────
worker_running() { pgrep -f "flash_worker.worker" >/dev/null 2>&1; }

if worker_running; then
    waiting "Restarting flash worker to load latest code..."
    cleanup_worker
fi

waiting "Starting flash worker..."
cd "$SCRIPT_DIR"
"$VENV_PYTHON" -m flash_worker.worker > /tmp/temporal-badge-worker.log 2>&1 &
echo "$!" > /tmp/temporal-badge-worker.pid
sleep 1.5
if worker_running; then
    ok "Flash worker started"
else
    fail "Flash worker failed to start."
    fail "Logs: /tmp/temporal-badge-worker.log"
    exit 1
fi

echo
echo "  Workflow history: http://localhost:8233"
echo

# ── Run flash.py ──────────────────────────────────────────────────────────────
cd "$SCRIPT_DIR"
set +e
"$VENV_PYTHON" flash.py "${FORWARD_ARGS[@]}"
FLASH_EXIT=$?
set -e

# ── Cleanup ───────────────────────────────────────────────────────────────────
if [[ $FLASH_EXIT -eq 0 ]]; then
    ok "Ignition run complete"
else
    fail "Ignition run failed with exit code $FLASH_EXIT"
    fail "Worker log: /tmp/temporal-badge-worker.log"
    fail "Temporal log: /tmp/temporal-badge-server.log"
fi
echo "  Flash worker stopped."
echo "  Temporal is still running — browse logs at http://localhost:8233"
echo "  To stop Temporal: kill \$(cat /tmp/temporal-badge-server.pid)"
echo

exit $FLASH_EXIT
