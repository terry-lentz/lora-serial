# Wio Tracker L1 port — WIP log

Running technical log for the L1 (nRF52840) receive-and-display port: what
works, the gotchas we hit (and how they were solved), and what's next. This is
the device-specific counterpart to `docs/CAPABILITIES_JOURNEY.md`.

## Where it stands

- [x] Toolchain: `nordicnrf52` + Adafruit nRF52 Arduino core, builds cleanly.
- [x] Flashing: UF2 bootloader; **autonomous** via the 1200-baud touch.
- [x] OLED: 1.3″ **SH1106** @ I²C **0x3D**, full panel under control.
- [x] Brightness: SH1106 contrast, cycled by the User button (D13).
- [x] Radio: SX1262 up on the L1 pins; **receives our PHY** (bench beacon,
      RSSI ≈ −25 dBm / SNR ≈ 13 dB, no drops).
- [ ] Speak the real link protocol (`lib/linklayer`) — currently raw-PHY RX only.
- [ ] The display application (e.g. stock ticker).
- [ ] Promote to a first-class `[env:…]` in the root `platformio.ini`.

## Gotchas & how they were solved

**SoftDevice offset — the boot-time reset loop (the big one).** The generic
`adafruit_feather_nrf52840` board def targets SoftDevice **S140 v6.1.1**
(app @ `0x26000`), but the L1 ships **S140 7.3.0** (app @ `0x27000`). Linking at
`0x26000` puts the app *inside* the resident SoftDevice: the SD forwards
execution to `0x27000` (our `.text`, not the vector table) → instant hard fault
→ USB connect/disconnect reset loop, no serial, OLED frozen on the old image.
Fixed with a custom board def (`boards/seeed_wio_tracker_L1.json`) + the v7
linker script (`linker/nrf52840_s140_v7.ld`, app @ `0x27000`), installed into the
framework by `scripts/install_ldscript.py`. **Any nRF52 target must match its
resident SoftDevice's flash offset.**

**Recovering a crash-loop.** Only the app region was ever overwritten, so the
bootloader + SoftDevice stay intact and a **double-click RST** always returns to
DFU (solid LED, `TRACKER L1` drive). Restore Meshtastic by copying the backed-up
`CURRENT.UF2` back onto the drive.

**Port permissions / autonomy.** A fresh Adafruit app enumerates as VID `0x239A`,
which no udev rule covers → port comes up `0660` → can't open it (no `dialout`
membership, no passwordless `sudo`) → can't even send the 1200-baud touch.
Fixed by setting the board def's USB VID to **`0x2886`** ("LoRa-Serial"), which
the repo's existing `99-lora-tinyusb.rules` (`idVendor==2886 → 0666`) covers.
After that, DFU entry + flashing + serial are fully hands-off. (One physical DFU
entry was needed to flash the first 2886 build — a chicken/egg bootstrap.)

**Display is an SH1106, not SSD1306.** The Seeed variant says `USE_SSD1306`, but
the raw Adafruit SSD1306 driver paints only a single shifted ~8-px band (SH1106
ignores the SSD1306 horizontal-addressing command, so the whole buffer lands in
one page). Meshtastic's ThingPulse driver hides this. Fixed with the **SH110X**
(`Adafruit_SH1106G`) driver. The panel answers at **0x3D** (not the usual 0x3C);
the app probes both.

**Buzzer vs LED pin.** `LED_BLUE` (PIN_LED2) shares **P1.00 with the buzzer**, so
a heartbeat there *clicks* once a second instead of blinking. Use `LED_GREEN`
(PIN_LED1) for a visible heartbeat.

**RF switch.** RX-only for now: the module's DIO2 drives the T/R switch
(`setDio2AsRfSwitch(true)`) and the board's `SX126X_RXEN` (D5) is held high to
arm the RX path. A bidirectional node will need a proper RadioLib RF-switch
table (RXEN toggled per TX/RX), like Meshtastic's SX126xInterface.

## TODO

- [ ] **Brightness: make LOW darker** (and possibly MED). Current contrast values
      FULL=0xFF / MED=0x30 / LOW=0x02 — LOW still reads too bright.
- [ ] Wire in `lib/linklayer` so the L1 is a real link peer (ACKs, ARQ, session/
      pairing, mode-switch) and interops with a `node_raw` node, not just a raw
      beacon. This is the bulk of the remaining port (a small nRF52 platform
      layer: settings storage, watchdog, crash breadcrumbs).
- [ ] Add the display application (stock-ticker rendering from the received
      transparent stream); AT commands for display settings over USB.
- [ ] Bidirectional RF-switch handling (RXEN per TX/RX).
- [ ] Promote to a root `[env:wio_l1]` once stable (move boards/variants to the
      repo root, share `build_src_filter`).

## References

- Pin map / variant: Meshtastic `variants/nrf52840/seeed_wio_tracker_L1/`.
- Bootloader / SoftDevice: `INFO_UF2.TXT` reports UF2 Bootloader 0.9.2,
  Board-ID `TRACKER L1`, SoftDevice **S140 7.3.0**.
- Feasibility discussion: GitHub issue #7 (sibling idea).
