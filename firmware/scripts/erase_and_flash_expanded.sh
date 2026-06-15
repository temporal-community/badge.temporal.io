#!/usr/bin/env bash
# erase_and_flash_expanded.sh — opt-in flash to the expanded 16 MB
# partition layout (partitions_replay_16MB_ver2.csv).
#
# Why erase first?
#   The partition table lives at flash offset 0x8000 and is read by
#   the bootloader at every boot. A regular `pio run -t upload` only
#   writes to the OTA app slot — it does NOT rewrite the partition
#   table. Switching from `echo` (3.84 MB slots, 6 MB ffat) to
#   `echo-expanded` (4.5 MB slots, 6.875 MB ffat) requires the new
#   partition table to actually take effect, which means erasing the
#   old one first.
#
# What this destroys:
#   - Everything on the badge: contacts, nametag, settings.txt,
#     downloaded assets (DOOM WAD, etc.), saved WiFi credentials.
#   The badge will boot fresh from the embedded initial filesystem
#   (provisionStartupFiles re-creates the standard files on first boot).
#
# Safety:
#   No way to brick the badge — esptool flashes the bootloader fresh
#   too. Worst case is the badge boots into "no FAT, will format
#   on first run" mode, which is the normal first-boot path.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PIO="${HOME}/.platformio/penv/bin/pio"
if [[ ! -x "$PIO" ]]; then
    echo "ERROR: PlatformIO not found at $PIO"
    echo "       Run ignition/setup.sh first."
    exit 1
fi

echo
echo "  ╔════════════════════════════════════════════════════════════╗"
echo "  ║  Flash expanded partition layout (16 MB)                   ║"
echo "  ║                                                            ║"
echo "  ║  This will ERASE THE ENTIRE FLASH and re-flash the badge   ║"
echo "  ║  with the bigger OTA slots (4.5 MB) and ffat (6.875 MB).   ║"
echo "  ║                                                            ║"
echo "  ║  All on-badge data will be lost:                           ║"
echo "  ║    - contacts, nametags, settings.txt                      ║"
echo "  ║    - downloaded assets (e.g. doom1.wad)                    ║"
echo "  ║    - saved WiFi credentials                                ║"
echo "  ║                                                            ║"
echo "  ║  WiFi can be re-entered after first boot via               ║"
echo "  ║  Settings → WiFi Setup. Assets re-download from the        ║"
echo "  ║  Asset Library tile.                                       ║"
echo "  ╚════════════════════════════════════════════════════════════╝"
echo

read -r -p "  Type 'EXPAND' to continue, anything else to abort: " confirm
if [[ "$confirm" != "EXPAND" ]]; then
    echo "  Aborted."
    exit 0
fi

cd "$FW_DIR"

echo
echo "==> Step 1/3: full chip erase (this is the destructive step)"
"$PIO" run -e echo-expanded -t erase

echo
echo "==> Step 2/3: flash firmware (echo-expanded)"
"$PIO" run -e echo-expanded -t upload

echo
echo "==> Step 3/3: flash filesystem image (initial /apps, /lib, /composer, etc.)"
"$PIO" run -e echo-expanded -t uploadfs

echo
echo "  ✓ Done. The badge will boot into the new expanded layout."
echo "  ✓ Re-enter WiFi via Settings → WiFi Setup."
