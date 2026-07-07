# tools/ — reusable utilities

A catalog of the helper tools for developing and operating lora-serial. **Prefer
extending these over writing a new one-off script** (see the repo-root
`CLAUDE.md`). Each is meant to be reused and improved over time — if you need a
capability one of these almost has, add it here rather than starting fresh.

| Tool | Purpose |
|------|---------|
| `upload_flash.sh <env> <port>` | Flash an ESP32 build over native USB without the BOOT button (1200-baud touch → esptool). |
| `make_uf2.sh <env> [out.uf2]` | Wrap a built nRF52 `firmware.hex` into a flashable `.uf2` (Adafruit UF2 bootloader, e.g. `wio_l1`); resolves `uf2conv.py` from the installed framework. |
| `coredump.sh <env> <port>` | Pull and decode a crash core dump from a board (needs the matching firmware.elf). |
| `install_nrf52_ldscript.py` | PlatformIO **pre-build hook** (not run by hand): installs the S140 v7 linker script into the nRF52 framework so `wio_l1` links at the right SoftDevice offset. |
| `at.py <port> <cmd> ...` | Run AT commands (auto `+++` escape) and print the replies. `--until SUBSTR [--timeout SEC]` waits for a delayed result (e.g. a slow-mode `AT+SPEEDTEST`). |
| `lora_xfer.py <tx> <rx> <n>` | Throughput + byte-exactness test over the serial cable; can pin a mode first. |
| `loraserial.py` | Shared Python lib (`Board` class) the two `.py` tools build on. |
| `version.py` | PlatformIO **pre-build hook** (not run by hand): stamps the firmware with its version from `git describe` as `-D FW_VERSION`. Reported by `ATI` / `AT+VER` / the boot banner. |

## Examples

```sh
# Identify a board and read its link/health state
tools/at.py /dev/ttyACM0 ATI "AT+LINK?" AT+DIAG

# Pin both ends to a mode and measure throughput A->B (16 KB)
tools/lora_xfer.py /dev/ttyACM0 /dev/ttyACM1 16384 --mode turbo

# Flash both boards (one env, same image on each — they auto-elect roles)
tools/upload_flash.sh node_raw /dev/ttyACM0
tools/upload_flash.sh node_raw /dev/ttyACM1

# Decode a crash dump (don't rebuild between flashing and decoding)
tools/coredump.sh node_raw /dev/ttyACM1
```

## Speed & sweep testing

Two ways to measure throughput, plus the sweep loops we run all the time.

**On-device speed test (no host data needed).** The initiator generates
incompressible data internally; the peer just drains+discards it. This isolates
the *radio* throughput from any host/USB effects:

```sh
tools/at.py /dev/ttyACM1 AT+SINK=1          # peer: drain & discard the stream
# The result prints when the test FINISHES (seconds to minutes), so wait for it
# with --until; bump --timeout for slow modes (far/SF12 can take a few minutes).
tools/at.py /dev/ttyACM0 AT+SPEEDTEST=8 --until KB/s --timeout 300
# -> speedtest: 8192B in 21634ms = 0.37 KB/s | retx=42% snr=11.5 rssi=-56 pwr=10
tools/at.py /dev/ttyACM1 AT+SINK=0          # peer: back to normal pass-through
```

**Through-the-cable test (end-to-end byte-exactness).** `lora_xfer.py` feeds
bytes in one USB port and verifies them byte-exact out the other:

```sh
tools/lora_xfer.py /dev/ttyACM0 /dev/ttyACM1 16384 --mode medium --pattern random
# --pattern seq|zeros|random : zeros shows the compression ceiling, random the
# PHY floor; default seq. --mode pins AT+FMODE on both ends first.
```

**Mode sweep** — compare all speed presets at the current range/power:

```sh
for m in turbo fast medium slow far; do
  tools/at.py /dev/ttyACM0 "AT+FMODE=$m" >/dev/null
  tools/at.py /dev/ttyACM1 "AT+FMODE=$m" >/dev/null
  printf '%-7s ' "$m"
  tools/at.py /dev/ttyACM0 AT+SPEEDTEST=8 --until KB/s --timeout 300 | grep KB/s
done
```

**Power sweep** — find the best TX power for the current range. Counter-intuitive
but real: at close (cm–metre) range the *highest* power desenses the front-ends
and *raises* retx, so a lower power is often faster (see docs/RADIO_ERRATA.md and
CAPABILITIES_JOURNEY entry 21):

```sh
for p in 22 14 10 6 2; do
  tools/at.py /dev/ttyACM0 "AT+PWR=$p" >/dev/null
  tools/at.py /dev/ttyACM1 "AT+PWR=$p" >/dev/null
  printf 'pwr=%-2s ' "$p"
  tools/at.py /dev/ttyACM0 AT+SPEEDTEST=8 --until KB/s --timeout 300 | grep KB/s
done
```

The `retx=` / `snr=` / `rssi=` fields in the result tell you *why* a run was
slow: high retx at high snr = re-arm/loss (not range); low snr = too little
signal (raise power or step to a more robust mode). A future `lora_sweep.py`
could wrap these loops once the bench is stable.

## Notes

- The Python tools need `pyserial` (`pip install pyserial`).
- `/dev/ttyACM*` numbers shuffle across reflash/replug — identify a board by
  `ATI` (the elected initiator reports `initiator=1`), never by port number.
- AT mode auto-exits after 60 s idle; the tools re-enter it via `+++` each run.
