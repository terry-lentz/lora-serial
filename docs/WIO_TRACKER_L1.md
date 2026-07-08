# Wio Tracker L1 — receive-and-display node

A second hardware target for this project: the **Seeed Wio Tracker L1**
(nRF52840 + SX1262 + 1.3″ SH1106 OLED). It runs the **same firmware as the XIAO
node** and adds an OLED front-end, so besides being an ordinary USB serial port
it **shows the received stream on its screen** — a pocket display for a
transparent LoRa feed (e.g. a stock ticker sent from the other end).

The L1 builds from the same `src/` as `node_raw`; the MCU differences sit behind
`src/platform/`, so the ESP32 build is unaffected and the two ends are guaranteed
to speak an identical protocol. Build it with `pio run -e wio_l1`.

## Why a shared codebase (not a fork)

The L1 is an **nRF52840** (ARM Cortex-M4F) — a different MCU family from the XIAO
ESP32-S3 (different core, USB stack, flashing, resident SoftDevice). The portable
`lib/linklayer/` ports for free; the handful of MCU-specific calls sit behind
`src/platform/` (`platform_{esp32,nrf52}.cpp`, `prefs.h`, `rtos.h`, `board.h`),
so the *same* `src/fw_*.cpp` compiles for both. The diag layer is split
(`fw_diag_{esp32,nrf52}.cpp`); the OLED front-end (`fw_display.cpp`) builds only
on boards that declare `HAS_DISPLAY`.

## Where the pieces live (repo root)

```
platformio.ini                     [env:wio_l1] — the L1 build (S140 7.3.0)
boards/seeed_wio_tracker_L1.json   custom board def (pins, LED, HAS_DISPLAY)
variants/seeed_wio_tracker_L1/     pin map + nrf52840_s140_v7.ld
tools/install_nrf52_ldscript.py    pre-build hook (installs the v7 ld script)
src/ + src/platform/               shared firmware + platform abstraction
src/fw_display.cpp                 the OLED front-end (this board only)
```

## Hardware map (nRF52840)

| Function | Pin | Notes |
|---|---|---|
| SX1262 NSS / DIO1 / BUSY / RESET | D4 / D1 / D3 / D2 | via variant defines |
| SPI SCK / MISO / MOSI | 8 / 9 / 10 | |
| RF switch | RXEN = D5 (RadioLib toggles per TX/RX); DIO2 = module T/R | |
| TCXO | DIO3 @ 1.8 V | same Wio-SX1262 module as the XIAO |
| OLED (SH1106) | I²C SDA=D14 / SCL=D15, **addr 0x3D** | 1.3″ 128×64 |
| User button | D13 (active-low) | cycles screens |
| Trackball | up/down/left/right + press (D25–D29) | menu navigation |

## The display and its controls

The 128×64 OLED shows three screens; the **user button** (D13) cycles between
them, and the **trackball** navigates. A settings change made on-device is
applied and persisted exactly like the equivalent AT command.

**Top status bar (all screens, drawn identically):** carrier frequency, an
encryption padlock (crossed out when off), the mode name, a 5-bar signal meter
(the measured **SNR margin** above the mode's demod floor — the real predictor of
decode, not raw RSSI; RSSI-based on GFSK), a single half-duplex **direction**
arrow (up = transmitting, down = receiving; hollow when idle, filled on
activity), a **battery** gauge (read from VBAT; blank on USB power), and a
heartbeat that pulses on each frame *received* from the peer — so if the heart
stops and the bars fall to zero, the link has gone quiet.

**MAIN** — a 5-line teletype of the received stream over a 64-line history.
- **Trackball up/down**: scroll back through history; **hold** to auto-scroll
  (it accelerates). A scrollbar on the right shows the position.
- At the bottom the view *follows* new input (live); scrolled up it *pauses* so
  incoming data doesn't yank you back down. Roll down to catch up.
- Renders the full **CP437** glyph set (box-drawing, arrows, `±°µ`, `▲`=0x1E /
  `▼`=0x1F, …). It is a transparent byte stream: send plain text; `\n` starts a
  line; only CR/LF/NUL are treated as control.

**INFO** — read-only diagnostics (trackball up/down scrolls the list): frequency,
mode, signal (RSSI/SNR), battery, power (static dBm or `AUTO`), link up/down,
TX/reTX counts, uptime, encryption, compression, forward-secrecy, ADR-GFSK, a
**key fingerprint** (matches `AT+KEY?` — the same on two units means their keys
match), and the device name.

**CONFIG** — an editable settings menu (scroll with up/down): Brightness, Region,
Frequency, Mode, TX power, Auto-power, Encryption, Compression, Forward-secrecy,
ADR-GFSK, **Buffer** (send-queue retention), **Keep** (keeplatest window).
- **Trackball up/down**: move the selection (the list scrolls; a scrollbar shows
  the position).
- **Press**: enter edit (the value is bracketed with `◄ ►`); **left/right**
  change it; **press** again to confirm (apply + save to flash).
- **User button**: cancel the edit (or, when not editing, go to the next screen).
- Brightness previews live as you change it. A **Mode** change *coordinates the
  peer* (the initiator drives both ends; a spinner shows next to the mode until
  the switch lands). Frequency/encryption/compression/forward-secrecy/ADR-GFSK
  are **local** — the peer must be set to match (the title shows *“match peer”*
  while editing them). **Region** (TW/US/EU) sets the frequency range and snaps
  the carrier into the new band. TX power and auto-power are local radio
  settings.
- **Buffer** picks the outbound send-queue retention policy: `KeepAll` (the
  default — byte-exact; a full buffer back-pressures the host) or `KeepLast`
  (freshness-first — drop the oldest queued bytes, keep only the most recent
  **Keep** KiB). This is a **local** setting; each board is configured on its
  own. See [THROUGHPUT.md](./THROUGHPUT.md#buffering--backlog--the-send-queue-and-its-retention-policy)
  for the full model and the `AT+BUFMODE` / `AT+BUFKEEP` equivalents.

Brightness has three steps (FULL / MED / LOW); LOW drops to the contrast floor
plus a shortened pre-charge period for a genuinely dim night level.

## Settings persistence

The nRF52 has no NVS, so `src/platform/prefs.h` provides a `Preferences`-
compatible store backed by the Adafruit core's internal **LittleFS**
(`src/platform/prefs_nrf52.cpp`): one small file per key under a namespace
directory, so settings survive a reboot exactly like the ESP32's NVS. `AT&W`
saves and `AT&F` clears, same as the XIAO.

## Build

```sh
pio pkg install -g -p nordicnrf52      # the nRF52 platform (one-time)
pio run -e wio_l1                       # -> .pio/build/wio_l1/firmware.hex
```
A pre-build hook (`tools/install_nrf52_ldscript.py`) installs the S140 v7 linker
script into the framework, so a fresh checkout builds cleanly. Convert the hex to
a UF2 to flash:
```sh
UF2CONV=~/.platformio/packages/framework-arduinoadafruitnrf52/tools/uf2conv/uf2conv.py
python3 "$UF2CONV" .pio/build/wio_l1/firmware.hex -c -f 0xADA52840 \
    -o .pio/build/wio_l1/firmware.uf2
```

## Flash (UF2 bootloader — no esptool)

The board uses the Adafruit UF2 bootloader: **enter DFU, then drop the `.uf2`**
onto the `TRACKER L1` drive that mounts; it writes flash and reboots.

- **Autonomous (no buttons):** a **1200-baud touch** on the running app's serial
  port reboots it into DFU:
  ```sh
  python3 -c "import serial,time;s=serial.Serial('/dev/ttyACM1',1200);s.setDTR(False);time.sleep(0.2);s.close()"
  cp .pio/build/wio_l1/firmware.uf2 "/media/$USER/TRACKER L1/"
  ```
- **Physical (first time / recovery):** double-**click** RST → the LED goes
  **solid** and the `TRACKER L1` drive mounts → copy the `.uf2`.

The app enumerates as USB VID **0x2886** (“LoRa-Serial”), which the repo's
`99-lora-tinyusb.rules` maps to `MODE=0666`, so the port is usable without
`sudo` — that's what keeps flashing hands-off.

## Pairing with a peer

The L1 uses the **same production defaults as the XIAO**: compression +
encryption on, MAC-based role auto-election, and first-boot **proximity
pairing**. Give it a peer (a XIAO flashed with `node_test`, or another node on
the same frequency), power both close together, and they pair automatically;
`AT+TRAIN` on both is the manual alternative for a per-pair key. Until paired, the
OLED shows the status bar and pairing banners (not a blank screen).

## Backing up / restoring the stock Meshtastic

The board ships with Meshtastic. To preserve and restore it:
- **Firmware image:** in DFU, copy `CURRENT.UF2` off the drive and keep it safe;
  restore by copying it back. Or re-flash the official Meshtastic `.uf2` for
  `seeed_wio_tracker_L1`.
- **Config/identity:** `meshtastic --export-config > l1_config.yaml`; restore with
  `meshtastic --configure l1_config.yaml`.

## Port notes (things specific to this board)

- **SoftDevice offset.** The L1 ships **S140 7.3.0**, so the app must link at
  `0x27000` (v7), not the Feather's `0x26000` (v6.1.1) — the wrong offset puts
  the app inside the resident SoftDevice and boot-loops. The custom board def +
  the v7 linker script (installed by the pre-build hook) handle this.
- **The panel is an SH1106, not SSD1306** — driven by `Adafruit_SH1106G` at I²C
  `0x3D`. Its pre-charge/VCOM are tuned by the driver init; only *contrast* is a
  safe brightness knob (a wrong VCOM blanks the glass).
- **`-DUSE_TINYUSB` breaks host→device input** (it reconfigures the CDC). The fix
  is to put the TinyUSB header dir on the include path *without* the flag, so the
  default CDC that reads input stays intact.
- **The OLED runs on its own low-priority FreeRTOS task** so its blocking I²C
  never stalls the half-duplex turn loop; the link loop only enqueues received
  bytes into a lock-free ring.
- **newlib-nano** disables floating-point `scanf` by default, so `-Wl,-u,
  _scanf_float` is needed for `AT+FREQ=` to parse; and the core's LittleFS driver
  references `LED_BUILTIN`, mapped to `PIN_LED1` via a build flag.
