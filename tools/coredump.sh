#!/usr/bin/env bash
# @file coredump.sh
# @brief Pull and decode an ESP32-S3 crash core dump from a board.
#
# The build enables esp-coredump-to-flash, and the 8 MB partition table has a
# 64 KB `coredump` partition at 0x7F0000. On a panic (or a hard fault), the
# IDF writes a full ELF core dump there. `AT+DIAG` shows `coredump=YES` when
# one is present. This script reads that partition over USB and decodes it
# into a backtrace, mapping addresses against the matching firmware.elf.
#
# Usage:
#   ./tools/coredump.sh <env> <port>
#   ./tools/coredump.sh node_raw /dev/ttyACM0
#
# Decode needs the standalone esp-coredump tool:  pip install esp-coredump
# (without it, the raw dump is still saved so you can decode it elsewhere).
set -euo pipefail

ENV="${1:?usage: coredump.sh <env> <port>}"
PORT="${2:?usage: coredump.sh <env> <port>}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="$ROOT/.pio/build/$ENV/firmware.elf"
OUT="$ROOT/.pio/build/$ENV/coredump.bin"

# coredump partition (from default_8MB.csv): offset 0x7F0000, size 0x10000.
CD_OFFSET=0x7F0000
CD_SIZE=0x10000

PYIO="$(command -v pio >/dev/null 2>&1 && readlink -f "$(command -v pio)" | sed 's:/bin/pio$:/bin/python:' || true)"
[ -x "$PYIO" ] || PYIO="$HOME/.local/share/pipx/venvs/platformio/bin/python"
ESPTOOL="$(find "$HOME/.platformio/packages" -name esptool.py 2>/dev/null | head -1)"
[ -f "$ELF" ] || { echo "FATAL: $ELF not found (build $ENV first)" >&2; exit 1; }

# 1200-baud touch -> ROM USB-Serial/JTAG bootloader. On this board the bootloader
# enumerates as a DIFFERENT /dev/ttyACM* than the running app, so snapshot the
# ports, touch, then pick whichever port (re)appears as the bootloader.
BEFORE="$(ls /dev/ttyACM* 2>/dev/null || true)"
echo ">> 1200-baud touch on $PORT ..."
"$PYIO" - "$PORT" <<'PY' || true
import sys, time, serial
try:
    s = serial.Serial(sys.argv[1], 1200)
    s.setDTR(False); time.sleep(0.1); s.setRTS(False); time.sleep(0.1); s.close()
except Exception as e:
    print("touch note:", e)
PY
sleep 2
# Prefer a port that wasn't there before the touch (the bootloader); else the
# most recently created ttyACM; else the original.
BPORT=""
for p in $(ls /dev/ttyACM* 2>/dev/null || true); do
    echo "$BEFORE" | grep -qx "$p" || BPORT="$p"
done
[ -n "$BPORT" ] || BPORT="$(ls -t /dev/ttyACM* 2>/dev/null | head -1)"
[ -n "$BPORT" ] || BPORT="$PORT"
echo ">> bootloader port: $BPORT"

echo ">> reading coredump partition ($CD_SIZE @ $CD_OFFSET) ..."
"$PYIO" "$ESPTOOL" --chip esp32s3 --port "$BPORT" --baud 921600 \
    --before no_reset --after hard_reset \
    read_flash "$CD_OFFSET" "$CD_SIZE" "$OUT"
echo ">> raw dump saved: $OUT"

# NOTE: --chip is a GLOBAL option (before the subcommand); the .elf is
# positional. The .elf must be the EXACT build that was running when it crashed
# (esp-coredump verifies an app-SHA match), so don't rebuild between flashing and
# decoding.
echo ">> decoding (elf must match the crashed build) ..."
if "$PYIO" -c "import esp_coredump" 2>/dev/null; then
    "$PYIO" -m esp_coredump --chip esp32s3 info_corefile \
        --core "$OUT" --core-format raw "$ELF" || {
        echo "!! decode failed — empty partition, or firmware.elf != crashed build."; }
elif command -v esp-coredump >/dev/null 2>&1; then
    esp-coredump --chip esp32s3 info_corefile \
        --core "$OUT" --core-format raw "$ELF"
else
    echo "!! 'esp-coredump' not installed. Install with:  pip install esp-coredump"
    echo "   then:  esp-coredump --chip esp32s3 info_corefile \\"
    echo "            --core $OUT --core-format raw $ELF"
fi
