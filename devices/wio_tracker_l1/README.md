# Wio Tracker L1 Pro — receive-and-display node (WORK IN PROGRESS)

A second hardware target for this project: the **Seeed Wio Tracker L1 Pro**
(nRF52840 + SX1262 + 1.3″ SH1106 OLED), running a firmware that **receives over
LoRa and shows the data on its screen**. The long-term goal is a display node for
the transparent link (e.g. rendering a stock-ticker feed sent from a node).

> **Status:** the hardware bring-up is proven end-to-end (the board builds,
> flashes, drives its OLED, and **received our PHY over the air** — bench beacon,
> RSSI ≈ −25 dBm, no drops), and the **real node firmware now compiles for the
> L1** as a first-class env (`wio_l1`). Not yet flashed/link-tested on hardware,
> and the display isn't wired into the node firmware yet. See [WIP.md](WIP.md)
> for the running technical log and TODOs.

The L1 runs the **same `src/` firmware as `node_raw`**, chosen at build time via
the platform abstraction in `src/platform/` — it does **not** affect the ESP32
build. This folder holds the L1's docs and a bench transmitter.

## Why a shared codebase (not a fork)

The L1 is an **nRF52840** (ARM Cortex-M4F) — a different MCU family from the XIAO
ESP32-S3 (different core, USB stack, flashing, SoftDevice). The portable
`lib/linklayer/` ports for free; the ~handful of MCU-specific calls sit behind
`src/platform/` (`platform_{esp32,nrf52}.cpp`, `prefs.h`, `rtos.h`, `board.h`),
so the *same* `src/fw_*.cpp` compiles for both — guaranteeing the two ends speak
an identical protocol. The diag layer is split (`fw_diag_{esp32,nrf52}.cpp`).

## Layout (repo root)

```
platformio.ini                 [env:wio_l1] — the L1 build (S140 7.3.0)
boards/seeed_wio_tracker_L1.json   custom board def
variants/seeed_wio_tracker_L1/     pin map + nrf52840_s140_v7.ld
tools/install_nrf52_ldscript.py    pre-build hook (installs the v7 script)
src/ + src/platform/               shared firmware + platform abstraction
devices/wio_tracker_l1/
  beacon_xiao/          bench transmitter (XIAO ESP32S3) for RF testing
  README.md  WIP.md
```

## Hardware map (nRF52840)

| Function | Pin | Notes |
|---|---|---|
| SX1262 NSS / DIO1 / BUSY / RESET | D4 / D1 / D3 / D2 | via variant defines |
| SPI SCK / MISO / MOSI | 8 / 9 / 10 | |
| RF switch | RXEN = D5 (RadioLib toggles per TX/RX); DIO2 = module T/R | |
| TCXO | DIO3 @ 1.8 V | same Wio-SX1262 module as the XIAO |
| OLED (SH1106) | I²C SDA=D14 / SCL=D15, **addr 0x3D** | 1.3″ 128×64 |
| User button | D13 (active-low) | cycles OLED brightness |
| PHY | 921.0 MHz, SF7/BW250/CR4:5, sync 0x12, preamble 8, CRC | 2 MHz clear of a 923 pair |

## Prerequisites

```sh
pio pkg install -g -p nordicnrf52     # the nRF52 platform (one-time)
```

## Build

```sh
pio run -e wio_l1
# -> .pio/build/wio_l1/firmware.hex  (convert to firmware.uf2 to flash)
```
A pre-build hook (`tools/install_nrf52_ldscript.py`) installs the S140 v7 linker
script into the framework, so a fresh checkout builds cleanly. First bring-up
uses a **minimal interop config** (no encryption/pairing, static roles, fixed
`medium` mode, 921 MHz) set in the `[env:wio_l1]` build flags.

Make the UF2:
```sh
UF2CONV=~/.platformio/packages/framework-arduinoadafruitnrf52/tools/uf2conv/uf2conv.py
python3 "$UF2CONV" .pio/build/wio_l1/firmware.hex -c -f 0xADA52840 \
    -o .pio/build/wio_l1/firmware.uf2
```

## Flash (UF2 bootloader — no esptool)

The board uses the Adafruit UF2 bootloader. **Enter DFU, then drop the `.uf2`**
onto the `TRACKER L1` drive that mounts; it writes flash and reboots.

- **Autonomous (no buttons):** send a **1200-baud touch** to the running app's
  serial port — it reboots into DFU:
  ```sh
  python3 -c "import serial,time;s=serial.Serial('/dev/ttyACM1',1200);s.setDTR(False);time.sleep(0.2);s.close()"
  cp .pio/build/wio_l1/firmware.uf2 "/media/$USER/TRACKER L1/"
  ```
- **Physical (first time / recovery from a bad flash):** double-**click** RST →
  the LED goes **solid** and the `TRACKER L1` drive mounts → copy the `.uf2`.

> The app enumerates as USB VID **0x2886** ("LoRa-Serial") so the repo's existing
> `99-lora-tinyusb.rules` (`idVendor==2886 → MODE=0666`) makes the port
> accessible without `sudo` — that's what keeps flashing autonomous. See WIP.md.

## Test the radio path (bench beacon)

Flash a XIAO as a raw-LoRa transmitter on the same PHY, then watch the L1 receive:
```sh
pio run -d devices/wio_tracker_l1/beacon_xiao
mkdir -p .pio/build/xiao_beacon && cp devices/wio_tracker_l1/beacon_xiao/.pio/build/xiao_beacon/{firmware,bootloader,partitions}.bin .pio/build/xiao_beacon/
tools/upload_flash.sh xiao_beacon /dev/ttyACM0     # XIAO port
# L1 serial (/dev/ttyACM1) then prints:  [L1 1b] RX #N rssi=-25 snr=13 "L1-SPIKE N"
```
Restore the XIAO to the normal firmware afterward: `make flash PORT=/dev/ttyACM0`.

## Back up / restore the stock Meshtastic

The board ships with Meshtastic. To preserve and restore it:
- **Firmware image:** in DFU, copy `CURRENT.UF2` off the drive (a best-effort
  full image) and keep it somewhere safe; restore by copying it back.
- **Config/identity:** `meshtastic --export-config > l1_config.yaml`; restore with
  `meshtastic --configure l1_config.yaml`.
- **Clean re-flash:** the official Meshtastic `.uf2` for `seeed_wio_tracker_L1`.
