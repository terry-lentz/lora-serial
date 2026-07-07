#!/usr/bin/env bash
#
# make_uf2.sh — convert a built nRF52 firmware.hex into a flashable .uf2 for the
# Adafruit UF2 bootloader (used by the Wio Tracker L1, env `wio_l1`).
#
# The nRF52 boards flash by copying a .uf2 onto the bootloader's USB drive, but
# `pio run` only emits a .hex — this wraps the Adafruit `uf2conv.py` (shipped
# inside the installed framework package) to make the .uf2, resolving its path so
# there is nothing to hard-code.
#
# Usage:  tools/make_uf2.sh <env> [out.uf2]
#   e.g.  tools/make_uf2.sh wio_l1
#         -> .pio/build/wio_l1/firmware.uf2
#
set -euo pipefail

env="${1:?usage: make_uf2.sh <env> [out.uf2]}"
hex=".pio/build/${env}/firmware.hex"
out="${2:-.pio/build/${env}/firmware.uf2}"

if [ ! -f "$hex" ]; then
  echo "make_uf2: $hex not found — build it first:  pio run -e ${env}" >&2
  exit 1
fi

# uf2conv.py ships inside the Adafruit nRF52 framework, which PlatformIO installs
# on the first `pio run` for the env — so resolve it at run time, not up front.
uf2conv="$(find "${HOME}/.platformio/packages/framework-arduinoadafruitnrf52" \
  -name uf2conv.py 2>/dev/null | head -1)"
if [ -z "$uf2conv" ]; then
  echo "make_uf2: uf2conv.py not found; build the nRF52 env once so the" \
       "framework is installed." >&2
  exit 1
fi

# 0xADA52840 is the UF2 family id for the nRF52840; -c writes a .uf2 (not .bin).
python3 "$uf2conv" "$hex" -c -f 0xADA52840 -o "$out"
echo "make_uf2: wrote $out"
