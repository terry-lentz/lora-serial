#!/usr/bin/env bash
# @file upload_flash.sh
# @brief Flash — and enumerate — LoRa-Serial boards over USB. ONE friendly tool.
#
# Pass a board name (or nothing and let it auto-detect / ask), and it does the
# right thing per MCU:
#
#   - XIAO ESP32-S3 (`xiao`) — native-USB esptool flash, button-free (the SX1262
#     shield covers BOOT/RESET). esptool's normal auto-reset toggles DTR/RTS,
#     which re-enumerates the native-USB port mid-connect and fails ("[Errno 19]
#     No such device"). Instead we 1200-baud "touch" the port (the Arduino
#     USB-CDC stack then reboots into the ROM USB-Serial/JTAG bootloader) and
#     flash with --before no_reset, --after hard_reset (reset+retry fallback).
#
#   - Wio Tracker L1 (`l1`) — nRF52840 UF2 flash, no esptool. A 1200-baud touch
#     drops it into the Adafruit UF2 bootloader, which mounts a "TRACKER L1" USB
#     drive; copying the .uf2 onto it writes flash and reboots.
#
# Usage:  ./upload_flash.sh [board] [port]      (also: make flash [BOARD=] [PORT=])
#         ./upload_flash.sh --list              (also: make boards)
#   board : xiao | l1   (also accepts the raw env: node_raw | wio_l1). Omit to
#           auto-detect; if several boards are connected you're asked to pick.
#   port  : /dev/ttyACMx. Omit to auto-detect from the board's USB identity.
#   -l, --list   enumerate connected boards and exit.
#   -h, --help   show this help.
#
# Examples:
#   ./upload_flash.sh                     # one board plugged in -> flash it;
#                                         #   several -> interactive pick
#   ./upload_flash.sh l1                  # find the L1's port and flash it
#   ./upload_flash.sh xiao /dev/ttyACM0   # be fully explicit
#   ./upload_flash.sh --list
#
# Both boards identify themselves over USB (/dev/serial/by-id/usb-*LoRa-Serial_*),
# so the board<->port mapping is auto-detected whenever the app is running.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # repo root (script in tools/)

usage() {
    cat <<'EOF'
upload_flash.sh — flash & enumerate LoRa-Serial boards over USB.

Usage:
  upload_flash.sh [board] [port]     flash a board (auto-detect/ask if omitted)
  upload_flash.sh --list             list connected boards and exit
  upload_flash.sh --help

  board   xiao | l1   (also accepts a raw env: node_raw | wio_l1)
  port    /dev/ttyACMx

Examples:
  upload_flash.sh                    # one board -> flash it; several -> pick
  upload_flash.sh l1                 # find the L1's port and flash it
  upload_flash.sh xiao /dev/ttyACM0  # fully explicit

Also available as:  make flash [BOARD=xiao|l1] [PORT=/dev/ttyACMx]
                    make boards      # = upload_flash.sh --list
EOF
}

# ---- enumerate connected boards into parallel arrays -----------------------
# Each board's firmware sets a board-specific USB product string, so a plain
# /dev/serial/by-id scan tells us the type, port, and chip serial with no I/O to
# the board (no AT mode, no session reset).
declare -a B_NAME B_ENV B_PORT B_SERIAL
enumerate_boards() {
    B_NAME=(); B_ENV=(); B_PORT=(); B_SERIAL=()
    local l base name env serial
    shopt -s nullglob
    for l in /dev/serial/by-id/usb-Open_LoRa-Serial_*-if00 \
             /dev/serial/by-id/usb-Seeed_Studio_LoRa-Serial_*-if00; do
        base="$(basename "$l")"
        case "$base" in
            usb-Open_LoRa-Serial_*)
                name=xiao; env=node_raw; serial="${base#usb-Open_LoRa-Serial_}" ;;
            usb-Seeed_Studio_LoRa-Serial_*)
                name=l1;   env=wio_l1;   serial="${base#usb-Seeed_Studio_LoRa-Serial_}" ;;
            *) continue ;;
        esac
        B_NAME+=("$name"); B_ENV+=("$env")
        B_PORT+=("$(readlink -f "$l")"); B_SERIAL+=("${serial%-if00}")
    done
    shopt -u nullglob
}

list_boards() {   # human-readable enumeration (stdout)
    enumerate_boards
    if [ "${#B_NAME[@]}" -eq 0 ]; then
        echo "No LoRa-Serial boards found on USB."
        echo "(A board in its bootloader won't show here — flash it explicitly.)"
        return 0
    fi
    echo "Connected LoRa-Serial boards:"
    local i
    for i in "${!B_NAME[@]}"; do
        printf "  %d) %-4s  %-11s  %-13s  %s\n" "$((i + 1))" \
            "${B_NAME[$i]}" "(${B_ENV[$i]})" "${B_PORT[$i]}" "${B_SERIAL[$i]}"
    done
}

# Friendly board name -> PlatformIO env (raw env names pass through unchanged).
env_for_board() {
    case "$1" in
        xiao|node_raw|esp32|esp32s3)      echo node_raw ;;
        l1|wio_l1|tracker|wio|nrf52|nrf)  echo wio_l1 ;;
        "")                               echo "" ;;      # unknown -> detect
        *)                                echo "$1" ;;    # assume a raw env
    esac
}

# ---- parse args: [board|env] and/or [port], plus --list / --help -----------
BOARD_ARG=""; PORT=""
for a in "$@"; do
    case "$a" in
        -h|--help)       usage; exit 0 ;;
        -l|--list|list)  list_boards; exit 0 ;;
        /dev/*)          PORT="$a" ;;
        "")              ;;                   # empty (unset make var) — ignore
        *)               BOARD_ARG="$a" ;;
    esac
done

ENV="$(env_for_board "$BOARD_ARG")"

# ---- resolve whatever wasn't given (auto-detect / interactive pick) --------
enumerate_boards
N=${#B_NAME[@]}
if [ -n "$ENV" ] && [ -z "$PORT" ]; then
    # Board named, port not: find its port among the connected boards.
    for i in "${!B_ENV[@]}"; do
        [ "${B_ENV[$i]}" = "$ENV" ] && { PORT="${B_PORT[$i]}"; break; }
    done
    # A missing serial port is fatal for the esptool path (it needs the port),
    # but fine for the UF2 path — the L1 may already be in its bootloader (which
    # is a mass-storage drive, not a serial port, so it won't enumerate here).
    if [ -z "$PORT" ] && [ "$ENV" != wio_l1 ]; then
        echo "No '$BOARD_ARG' board found on USB. \`--list\` to see what's" \
            "connected, or pass a port explicitly." >&2; exit 1
    fi
elif [ -z "$ENV" ] && [ -n "$PORT" ]; then
    # Port given, board not: read the board back from the port's by-id name.
    tgt="$(readlink -f "$PORT" 2>/dev/null || echo "$PORT")"
    for i in "${!B_PORT[@]}"; do
        [ "${B_PORT[$i]}" = "$tgt" ] && { ENV="${B_ENV[$i]}"; break; }
    done
    [ -n "$ENV" ] || { echo "Couldn't identify the board on $PORT — name it:" \
        "\`upload_flash.sh xiao|l1 $PORT\`." >&2; exit 1; }
elif [ -z "$ENV" ] && [ -z "$PORT" ]; then
    # Nothing given: 0 -> error, 1 -> use it, many -> ask (or list if no TTY).
    if [ "$N" -eq 0 ]; then
        echo "No LoRa-Serial boards found on USB. Plug one in, or pass a port." >&2
        exit 1
    elif [ "$N" -eq 1 ]; then
        ENV="${B_ENV[0]}"; PORT="${B_PORT[0]}"
    elif [ -t 0 ]; then
        echo "Multiple boards connected:" >&2
        list_boards >&2
        read -rp "Flash which? [1-$N, q=quit]: " pick
        case "$pick" in
            q|Q|"")   echo "aborted." >&2; exit 1 ;;
            *[!0-9]*) echo "invalid choice." >&2; exit 1 ;;
        esac
        { [ "$pick" -ge 1 ] && [ "$pick" -le "$N" ]; } \
            || { echo "out of range." >&2; exit 1; }
        ENV="${B_ENV[$((pick - 1))]}"; PORT="${B_PORT[$((pick - 1))]}"
    else
        echo "Multiple boards connected — name one (xiao|l1) or pass a port:" >&2
        list_boards >&2
        exit 1
    fi
fi
# else: both board and port were given explicitly — use them as-is (also the
# path for a board that's wedged / already in its bootloader, which won't
# enumerate).

BUILD="$ROOT/.pio/build/$ENV"
echo ">> board=$ENV  port=$PORT"

# Resolve the PlatformIO-managed Python (ships pyserial) — BOTH paths use it for
# the USB reset "touch".
PYIO="$(command -v pio >/dev/null 2>&1 && readlink -f "$(command -v pio)" | sed 's:/bin/pio$:/bin/python:' || true)"
[ -x "$PYIO" ] || PYIO="$HOME/.local/share/pipx/venvs/platformio/bin/python"
[ -x "$PYIO" ] || { echo "FATAL: could not locate the PlatformIO python" >&2; exit 1; }

case "$ENV" in
    wio_l1) FAMILY=uf2 ;;
    *)      FAMILY=esp32 ;;
esac

# 1200-baud touch: opening the native-USB CDC port at 1200 bps makes the Arduino
# core reboot into its bootloader (ROM USB-Serial/JTAG on the ESP32-S3; the
# Adafruit UF2 bootloader on the nRF52). Harmless if already in the bootloader.
touch_1200() {
    "$PYIO" - "$PORT" <<'PY' || true
import sys, time, serial
try:
    s = serial.Serial(sys.argv[1], 1200); time.sleep(0.15)
    s.setDTR(False); time.sleep(0.15); s.close(); time.sleep(0.3)
except Exception as e:
    print("1200-touch note:", e)
PY
}

# ─────────────────────────── nRF52840 / UF2 path ───────────────────────────
if [ "$FAMILY" = uf2 ]; then
    # Need a .uf2. make_uf2.sh wraps the built .hex; build the env first if the
    # .hex is missing (make_uf2.sh only wraps, it doesn't build).
    [ -f "$BUILD/firmware.hex" ] || { echo ">> building $ENV ..."; pio run -e "$ENV"; }
    UF2="$BUILD/firmware.uf2"
    "$ROOT/tools/make_uf2.sh" "$ENV" "$UF2"

    # The bootloader mass-storage drive auto-mounts under /media/$USER (or
    # /run/media/$USER). Find it whether it's already there or appears after the
    # touch — its label is "TRACKER L1" (Adafruit UF2 bootloaders: *_BOOT too).
    find_uf2_drive() {
        find /media/"$USER" /run/media/"$USER" -maxdepth 1 \
            \( -iname '*TRACKER*' -o -iname '*NRF52BOOT*' -o -iname '*_BOOT' \) \
            2>/dev/null | head -1
    }
    DRIVE="$(find_uf2_drive || true)"
    if [ -n "$DRIVE" ]; then
        echo ">> UF2 bootloader already mounted: $DRIVE"
    else
        if [ -n "$PORT" ]; then
            echo ">> touching $PORT at 1200 baud to enter the UF2 bootloader ..."
            touch_1200
        else
            echo ">> $ENV isn't on a serial port — double-tap RST to enter the" \
                 "UF2 bootloader if it isn't already."
        fi
        echo ">> waiting for the UF2 drive (double-tap RST if it doesn't mount) ..."
        for _ in $(seq 1 50); do           # ~25 s for udisks to auto-mount
            DRIVE="$(find_uf2_drive || true)"
            [ -n "$DRIVE" ] && break
            sleep 0.5
        done
    fi
    # udisks sometimes doesn't auto-mount the bootloader volume — mount the
    # labeled block device ourselves (no root needed; udisksctl uses polkit).
    if [ -z "$DRIVE" ]; then
        dev="$(lsblk -rno NAME,LABEL 2>/dev/null \
               | grep -iE 'TRACKER|NRF52BOOT|_BOOT' \
               | awk '{print "/dev/"$1}' | head -1)"
        if [ -n "$dev" ]; then
            echo ">> auto-mount didn't fire; mounting $dev via udisksctl ..."
            udisksctl mount -b "$dev" >/dev/null 2>&1 || true
            for _ in $(seq 1 10); do
                DRIVE="$(find_uf2_drive || true)"
                [ -n "$DRIVE" ] && break
                sleep 0.5
            done
        fi
    fi
    [ -n "$DRIVE" ] || { echo "FATAL: could not find/mount the UF2 drive." >&2; exit 1; }
    echo ">> copying $(basename "$UF2") -> $DRIVE ..."
    cp "$UF2" "$DRIVE"/ && sync
    echo ">> OK: $ENV flashed (UF2 via $DRIVE)"
    exit 0
fi

# ─────────────────────────── ESP32-S3 / esptool path ───────────────────────────
# esptool: PREFER a version < 5. The release toolchain pins esptool<5 because
# v5's CLI/reset changes break this native-USB flash path (reset re-enumerates
# the port). PlatformIO may have BOTH a v4 registry package and a v5 git-src
# pin installed; a naive `find | head -1` can grab the v5 one. So probe each
# candidate's reported version and take the first major < 5, falling back to
# whatever exists (with a warning) if only a v5 is present.
ESPTOOL=""
esptool_first=""
while IFS= read -r cand; do
    [ -n "$cand" ] || continue
    [ -n "$esptool_first" ] || esptool_first="$cand"
    ver="$({ "$PYIO" "$cand" version 2>/dev/null \
             || "$PYIO" "$cand" --version 2>/dev/null; } \
           | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
    major="${ver%%.*}"
    case "$major" in
        ''|*[!0-9]*) ;;                       # unparseable — skip
        *) if [ "$major" -lt 5 ]; then ESPTOOL="$cand"; break; fi ;;
    esac
done < <(find "$HOME/.platformio/packages" -name esptool.py 2>/dev/null)
if [ -z "$ESPTOOL" ]; then
    ESPTOOL="$esptool_first"
    [ -z "$ESPTOOL" ] || echo "WARN: no esptool <5 found; using $ESPTOOL —" \
        "v5 may break this native-USB flash (release pins esptool<5)." >&2
fi
BOOTAPP="$(find "$HOME/.platformio/packages" -name boot_app0.bin 2>/dev/null | head -1)"

for f in "$ESPTOOL" "$BOOTAPP"; do
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

enter_bootloader() {
    echo ">> resetting $PORT into the ROM bootloader (no BOOT button) ..."
    "$PYIO" - "$PORT" <<'PY' || true
import sys, time, serial
port = sys.argv[1]
# Two independent ways to reach the ROM USB-Serial/JTAG bootloader, both relying
# on the (default-on) USBCDC auto-reset. We do both because either can miss:
# 1) 1200-baud touch  -> USBCDC _onLineCoding sees 1200 bps -> RESTART_BOOTLOADER.
try:
    s = serial.Serial(port, 1200)
    s.setDTR(False); time.sleep(0.1)
    s.setRTS(False); time.sleep(0.1)
    s.close(); time.sleep(0.3)
except Exception as e:
    print("1200-touch note:", e)
# 2) DTR/RTS walk -> USBCDC _onLineState state machine:
#    IDLE -> !dtr&rts -> dtr&rts -> dtr&!rts -> !dtr&!rts == RESTART_BOOTLOADER.
#    Proven reliable on this native-USB S3 when the 1200-touch doesn't land;
#    harmless if we're already in the bootloader (the open just fails or no-ops).
try:
    s = serial.Serial(port, 115200)
    for dtr, rts in ((False, True), (True, True), (True, False), (False, False)):
        s.setDTR(dtr); s.setRTS(rts); time.sleep(0.2)
    s.close()
except Exception as e:
    print("dtr-walk note:", e)
PY
    # Wait for the ROM bootloader to re-enumerate the port.
    for _ in $(seq 1 20); do [ -e "$PORT" ] && break; sleep 0.25; done
    sleep 1
}

flash() {  # $1 = --before mode
    "$PYIO" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
        --before "$1" --after hard_reset write_flash "${ARGS[@]}"
}

enter_bootloader
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
