# Custom Arduino variants

PlatformIO looks here (via `board_build.variants_dir = variants` in a board's
env) for custom Arduino **pin-map variants**. A variant defines the board's pin
numbering, peripheral pins, and — on the nRF52 — its linker script.

```
variants/
  seeed_wio_tracker_L1/   pin map + nrf52840_s140_v7.ld  (used by [env:wio_l1])
```

## Why only the Wio Tracker L1 has an entry (and the XIAO doesn't)

This is intentional, not an omission. A board only needs a custom variant when
its core doesn't already ship one:

- **XIAO ESP32-S3** — the arduino-esp32 core already ships a `XIAO_ESP32S3`
  variant, so `boards/xiao_esp32s3_lora.json` just points at it. Nothing to
  vendor here.
- **Wio Tracker L1** — the Adafruit nRF52 core has no matching variant, so we
  vendor one (the pin map from Meshtastic plus the S140 **v7** linker script the
  board's resident SoftDevice requires). Hence the single entry above.

So `boards/` has a JSON for *both* boards (the consistent, canonical part), while
`variants/` carries only what the cores don't already provide. See
`docs/WIO_TRACKER_L1.md` for the L1's build/flash details.
