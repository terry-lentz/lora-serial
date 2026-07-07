# Wio Tracker L1 port — WIP log

Running technical log for the L1 (nRF52840) receive-and-display port: what
works, the gotchas we hit (and how they were solved), and what's next. This is
the device-specific counterpart to `docs/CAPABILITIES_JOURNEY.md`.

## Where it stands

- [x] Toolchain: `nordicnrf52` + Adafruit nRF52 Arduino core, builds cleanly.
- [x] Flashing: UF2 bootloader; **autonomous** via the 1200-baud touch.
- [x] OLED: 1.3″ **SH1106** @ I²C **0x3D**, full panel under control (spike).
- [x] Brightness: SH1106 contrast, cycled by the User button (D13) (spike).
- [x] Radio RX: SX1262 up on the L1 pins; **received our PHY** (bench beacon,
      RSSI ≈ −25 dBm / SNR ≈ 13 dB, no drops) (spike).
- [x] **Platform abstraction**: the shared firmware (`src/`) is platform-agnostic
      behind `src/platform/`; `node_raw` + the native sim stay green.
- [x] **Root `[env:wio_l1]`**: the real node firmware **compiles for the L1**
      (nRF52 impls + board/variant at repo root). Minimal interop config.
- [ ] **Flash + link test**: confirm the L1 links to a XIAO and passes serial
      data each way (hardware — next).
- [ ] Full parity: re-enable encryption → pairing/roles → mode-switch/ADR.
- [ ] The display application (stock ticker) + dual host (USB serial + OLED).

## Structure

The L1 is now a first-class PlatformIO target, not a side project:

- `boards/seeed_wio_tracker_L1.json` — board def (S140 7.3.0, app @ 0x27000).
- `variants/seeed_wio_tracker_L1/` — pin map + `nrf52840_s140_v7.ld`.
- `tools/install_nrf52_ldscript.py` — pre-build hook that installs the v7 script
  into the framework (it only ships v6).
- `[env:wio_l1]` in the root `platformio.ini` — builds `src/` with the nRF52
  platform files; `build_src_filter` selects esp32-vs-nrf52 per env.
- `src/platform/` — the abstraction: `platform.h` + `platform_{esp32,nrf52}.cpp`,
  `prefs.h` (NVS vs stub), `rtos.h` (FreeRTOS/IRAM/pinned-create), `board.h`
  (pins/LED/RF-switch). Diag is split: `fw_diag_{esp32,nrf52}.cpp`.
- `devices/wio_tracker_l1/` — this folder: docs + `beacon_xiao/` bench tx. The
  standalone hardware-bring-up spike is retired (it lives in git history; the
  real firmware supersedes it).

Build: `pio run -e wio_l1` → `.pio/build/wio_l1/firmware.hex`; convert to UF2
with the Adafruit `uf2conv.py` (family `0xADA52840`) and flash via DFU.

## Gotchas & how they were solved

**SoftDevice offset — the boot-time reset loop (the big one).** The generic
`adafruit_feather_nrf52840` board def targets SoftDevice **S140 v6.1.1**
(app @ `0x26000`), but the L1 ships **S140 7.3.0** (app @ `0x27000`). Linking at
`0x26000` puts the app *inside* the resident SoftDevice → instant hard fault →
USB connect/disconnect reset loop, no serial, OLED frozen. Fixed with a custom
board def + the v7 linker script (`variants/seeed_wio_tracker_L1/`), installed
into the framework by `tools/install_nrf52_ldscript.py`. **Any nRF52 target must
match its resident SoftDevice's flash offset.**

**Recovering a crash-loop.** Only the app region is overwritten, so the
bootloader + SoftDevice stay intact and a **double-click RST** always returns to
DFU (solid LED, `TRACKER L1` drive). Restore Meshtastic by copying its
`CURRENT.UF2` back onto the drive.

**Port permissions / autonomy.** A fresh Adafruit app enumerates as VID `0x239A`,
which no udev rule covers → port comes up `0660` → can't open it → can't send
the 1200-baud touch. Fixed by setting the board def's USB VID to **`0x2886`**
("LoRa-Serial"), which the repo's `99-lora-tinyusb.rules` (`2886 → 0666`) covers.
After that, DFU entry + flashing + serial are hands-off.

**Display is an SH1106, not SSD1306.** The variant says `USE_SSD1306`, but the
raw Adafruit SSD1306 driver paints only a single shifted ~8-px band. Fixed with
the **SH110X** (`Adafruit_SH1106G`) driver; the panel answers at **0x3D**.

**Buzzer vs LED pin.** `LED_BLUE` (PIN_LED2) shares **P1.00 with the buzzer** —
a heartbeat there *clicks*. Use `LED_GREEN` (PIN_LED1) (now `BOARD_LED_PIN`).

**ESP calls hiding in shared code.** Porting surfaced several ESP-only calls
beyond the obvious: `Serial.setRxBufferSize`, `ESP.restart`, and `feedLoopWDT`
(sprinkled through the radio/host loops). All now go through `platform::`
(`HostSetRxBufferSize`, `Reboot`, `WatchdogFeed`) — no-ops/equivalents on nRF52.

**Display libs pull in Wire → TinyUSB.** Adding `Adafruit SH110X` to the L1 env
made the framework's `Wire_nRF52.cpp` need `Adafruit_TinyUSB.h`, which the LDF
didn't resolve → build fail. The *link* firmware needs no I²C, so SH110X/GFX are
omitted for now; they come back with the display code (and that include gets
solved then).

## TODO

- [ ] **Flash + link test (hardware)**: flash `wio_l1` to the L1; flash a XIAO
      with matching minimal flags (no-enc/no-pair/static-role/medium/921 MHz);
      confirm they link and pass a few bytes of serial each way. (`node_test`
      env for the XIAO not yet added — see below.)
- [ ] **Brightness: make LOW darker** (and possibly MED). Was FULL=0xFF /
      MED=0x30 / LOW=0x02 in the spike — LOW still read too bright. Fold into the
      display code when it returns.
- [ ] Add a `node_test` (XIAO) env matching the L1's minimal interop flags, so
      the bench pair is one command each.
- [ ] Climb to full parity: re-enable encryption → pairing/roles → mode-switch.
- [ ] Display application (stock-ticker rendering from the received transparent
      stream) + dual host (USB serial *and* OLED); re-add SH110X/GFX and resolve
      the Wire/TinyUSB include.
- [ ] nRF52 diag hardening: software watchdog + reset-surviving crash breadcrumb
      (currently `fw_diag_nrf52.cpp` reports boots/reset-cause/heap only).

## Future ideas

- **BLE keyboard → standalone LoRa terminal.** The nRF52840's BLE is resident
  (S140 SoftDevice already flashed), so the L1 can act as a BLE HID *central* and
  connect to a Bluetooth keyboard; keystrokes feed the same host-input path as
  the USB CDC → the display node becomes a pocket terminal (screen + keyboard, no
  host computer). Requires enabling the SoftDevice (we currently run without BLE
  — which is why direct RNG/register access is fine). Tabled for now.

## References

- Pin map / variant: Meshtastic `variants/nrf52840/seeed_wio_tracker_L1/`.
- Bootloader / SoftDevice: `INFO_UF2.TXT` — UF2 Bootloader 0.9.2, Board-ID
  `TRACKER L1`, SoftDevice **S140 7.3.0**.
- Feasibility discussion: GitHub issue #7.
