#!/usr/bin/env bash
# @file upload_flash.sh
# @brief Flash a PlatformIO build to a XIAO ESP32S3 + Wio-SX1262 over USB.
#
# Flashes over native USB WITHOUT the BOOT/RESET buttons (the SX1262 shield
# covers them on this kit). The XIAO ESP32S3 uses the chip's native USB, and
# esptool's normal auto-reset toggles DTR/RTS, which makes the USB port
# re-enumerate mid-connect and fails with "[Errno 19] No such device". This
# script instead:
#   1. Does a 1200-baud "touch" — the Arduino USB-CDC stack reboots into the
#      ROM USB-Serial/JTAG bootloader when the host opens the port at 1200 bps.
#   2. Flashes with esptool --before no_reset (board is already in the
#      bootloader, so no disruptive reset happens), --after hard_reset to run.
# If the touch doesn't land (e.g. board already in the bootloader), it falls
# back to flashing directly, then retries once after a default reset.
#
# Usage:
#   ./upload_flash.sh <env> <port>
#   ./upload_flash.sh node_raw /dev/ttyACM0
#   ./upload_flash.sh node_raw /dev/ttyACM1  (same image, other board)
#
# Prereq: `pio run -e <env>` artifacts exist (the script builds them if not).
set -euo pipefail

ENV="${1:?usage: upload_flash.sh <env> <port>}"
PORT="${2:?usage: upload_flash.sh <env> <port>}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # repo root (script lives in tools/)
BUILD="$ROOT/.pio/build/$ENV"

# Resolve the PlatformIO-managed Python, esptool, and boot_app0.
PYIO="$(command -v pio >/dev/null 2>&1 && readlink -f "$(command -v pio)" | sed 's:/bin/pio$:/bin/python:' || true)"
[ -x "$PYIO" ] || PYIO="$HOME/.local/share/pipx/venvs/platformio/bin/python"
ESPTOOL="$(find "$HOME/.platformio/packages" -name esptool.py 2>/dev/null | head -1)"
BOOTAPP="$(find "$HOME/.platformio/packages" -name boot_app0.bin 2>/dev/null | head -1)"

for f in "$PYIO" "$ESPTOOL" "$BOOTAPP"; do
    [ -e "$f" ] || { echo "FATAL: could not locate $f" >&2; exit 1; }
done

# Build if the app binary is missing.
if [ ! -f "$BUILD/firmware.bin" ]; then
    echo ">> building $ENV ..."
    pio run -e "$ENV"
fi

# ESP32-S3 flash layout (Arduino).
ARGS=(--flash_mode keep --flash_freq keep --flash_size keep
      0x0    "$BUILD/bootloader.bin"
      0x8000 "$BUILD/partitions.bin"
      0xe000 "$BOOTAPP"
      0x10000 "$BUILD/firmware.bin")

touch_1200() {
    echo ">> 1200-baud touch on $PORT to enter bootloader ..."
    "$PYIO" - "$PORT" <<'PY' || true
import sys, time, serial
port = sys.argv[1]
try:
    s = serial.Serial(port, 1200)
    s.setDTR(False); time.sleep(0.1)
    s.setRTS(False); time.sleep(0.1)
    s.close()
except Exception as e:
    print("touch note:", e)
PY
    # Wait for the ROM bootloader to re-enumerate the port.
    for _ in $(seq 1 20); do [ -e "$PORT" ] && break; sleep 0.25; done
    sleep 1
}

flash() {  # $1 = --before mode
    "$PYIO" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
        --before "$1" --after hard_reset write_flash "${ARGS[@]}"
}

touch_1200
echo ">> flashing $ENV -> $PORT ..."
# After the touch the board sits in the ROM USB-Serial/JTAG bootloader, where
# esptool's default reset is clean. no_reset is the fallback for the case where
# the board was already in the bootloader and the touch was a no-op.
if flash default_reset; then
    echo ">> OK: $ENV flashed to $PORT"
else
    echo ">> default_reset attempt failed; retrying with no_reset ..."
    flash no_reset && echo ">> OK (no_reset): $ENV flashed to $PORT"
fi
