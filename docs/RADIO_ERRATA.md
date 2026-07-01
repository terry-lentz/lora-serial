# SX1262 known limitations (errata) & how we handle them

The SX1262 is fixed-function silicon — there is **no field firmware update** for the
radio chip. Its documented issues are addressed by **host-side workarounds** (register
writes, calibration, resets), most of which **RadioLib already applies** inside
`begin()` / `setFrequency()`. This file catalogs each known issue, the Semtech
reference, and where the workaround lives in our stack, so a future maintainer can
see what's covered.

**Primary reference:** Semtech *SX1261/2 Data Sheet*, Rev. 2.2, `DS.SX1261-2.W.APP`
(Dec 2024) — **§15 "Known Limitations"** (earlier revs: §15 too). Plus the
[RadioLib Radio Errata Notes](https://github.com/jgromes/RadioLib/wiki/Radio-Errata-Notes)
for the AGC item, which isn't in the main datasheet.

| # | Known limitation | Semtech ref | Workaround | Status in our stack |
|---|------------------|-------------|------------|---------------------|
| 1 | **Modulation quality, 500 kHz LoRa BW** — degraded modulation at BW=500 kHz | DS §15.1 | clear bit 2 of reg `0x0889` (TxModulation) | **RadioLib auto** — `fixSensitivity()` in `begin()`. Affects our `turbo`/`fast` (BW500). |
| 2 | **Image calibration on band change** — sensitivity drops if image isn't recalibrated for the band | DS §9.2.1 | `CalibrateImage` for the freq band | **RadioLib auto** — `calibrateImage()` in `begin()`/`setFrequency()`. |
| 3 | **PA over-clamping (OCP)** — overly-eager PA clamp can cut output | DS (PA/OCP) | adjust OCP / clamp reg | **RadioLib auto** — `fixPaClamping()`. |
| 4 | **Implicit-header RX timeout** — in implicit-header + RX, the timeout can misbehave / not stop on preamble | DS §15.3 | reg fix (`StopTimerOnPreamble` / `0x0902` area) | **RadioLib auto** — `fixImplicitTimeout()` in `begin()`. We use **explicit** headers, so largely N/A, but it's handled. |
| 5 | **Better TX resistance to antenna mismatch** — up to ~5–6 dB TX-power loss into a mismatched antenna | DS §15.2 | write reg `0x08D8` before each TX | **NOT applied.** It's a TX-power optimization, not a stability fix; we're not power-constrained and the Wio-SX1262 antenna is matched. Revisit if range-limited. |
| 6 | **AGC desense / RSSI lock-up** — a **strong signal within ~2.5 MHz** of the RX frequency desensitizes the receiver and **inflates reported RSSI by up to ~35 dB**, blocking weak-signal RX (the receiver "goes deaf"). Also the "interference avoidance / RSSI detection hang." | RadioLib errata wiki (not in DS) | periodic **`resetAGC()`** (warm sleep → recalibrate → standby), guarded so it never runs mid-packet | **Mitigated by power + standby; `resetAGC()` NOT used (yet).** Our close-range "deafness" is this exact desense from a +22 dBm peer cm away — **lowering TX power / separating the boards fixes it** (CAPABILITIES_JOURNEY 21). A per-frame `standby()` also resets the AGC. We do **NOT** use RadioLib's `resetAGC()`: its warm `sleep()` step **wedged our SX1262** (`begin()` → err -2 CHIP_NOT_FOUND, power-cycle to recover — a known RadioLib failure, #740/#683). Meshtastic *does* ship a guarded `resetAGC()` as the proper recovery — a future option if we add the guards (see note below). See CAPABILITIES_JOURNEY 20 & 22. |
| 7 | **CAD returns to STBY_RC** — after a CAD op the radio always lands in standby | RadioLib errata wiki | (no clean workaround) | **N/A** — we don't use CAD (CAD turn-around was explored and deprioritized, see FUTURE_MODES.md). |
| 8 | **IQ-polarity silent RX-deaf (errata 15.4)** — after `SetPacketParams`, reg `0x0736` bit 2 must be set (standard IQ) / cleared (inverted), else **LoRa RX demodulation fails silently while TX still works** | DS §15.4 | re-apply reg `0x0736` bit 2 after every `SetPacketParams` | **RadioLib auto** — applied inside `setPacketParams()` (`invertIQ` handling). RNode applies it by hand; for us it comes via RadioLib. A candidate to check first if RX ever goes silently deaf while TX works. |

## Notes
- Items 1–4 come "for free" because we drive the chip through RadioLib's `begin()`,
  which applies them. If we ever bypass `begin()` (e.g., an incremental reconfigure),
  re-check that these still apply.
- Item 6 (AGC) is the one that bit us as a **wedged/deaf radio**. We recover rather
  than prevent: the radio-stuck watchdog (`MaybeReinitRadio` → `ReinitRadio`) does an
  NRST reset + `begin()`; if that still doesn't restore RX, the link escalates to a
  rendezvous and finally a reboot (see the recovery layering in `fw_device.cpp`
  and the
  sim tests `test_sim_radio_stuck_*` / `test_sim_hard_wedge_reboot_recovers`).
- **Continuous RX and the AGC (corrected 2026-06-30).** Continuous RX appeared to
  trigger the AGC lockup (responder snr ≈ -27 deaf vs per-frame snr ≈ 12). The fix is
  to **return to standby between listens** (the `RX_CONTINUOUS` path standbys+re-arms
  before processing). The detour through RadioLib's `resetAGC()` was a mistake here:
  it **wedged the chip** (warm `sleep()` → CHIP_NOT_FOUND, VBUS power-cycle) and was
  removed. **No RadioLib upgrade is needed** — 7.7.1 is the latest. Continuous RX is
  implemented but **UNVALIDATED on hardware**; per-frame is shipped. See journey 20/22.
- **If we revisit `resetAGC()` (the proper desense recovery), copy Meshtastic's
  guarded recipe — don't call it blind like we did.** Meshtastic ships a working
  `resetAGC()` (`src/mesh/SX126xInterface.cpp`) that recovers item-6 desense; the
  guards are what make it safe:
  - **Never run it mid-packet:** `if (sendingPacket || (isReceiving && isActivelyReceiving())) return;` — call only when idle. Our blind every-2 s call (possibly mid-RX) is likely why it wedged.
  - Sequence: `sleep(true)` (warm) → `standby(STDBY_RC)` → `CALIBRATE_ALL` → wait BUSY low (≤50 ms) → `calibrateImage(freq)` → re-apply `setDio2AsRfSwitch` + `setRxBoostedGainMode` → `startReceive()`.
  - **Re-apply reg `0x08B5` bit 0 after the calibrate** — `CALIBRATE_ALL` (0x7F) silently clears it, which removes the RX-sensitivity boost; without the re-apply every SX1262 loses RX sensitivity ~60 s after boot (Meshtastic #9571/#9777).
  - Keep the existing **reboot watchdog** as the last-resort fallback (a `sleep()` can still wedge the chip; only a power-cycle/reboot recovers that).
- **RNode's false-preamble unlatch** (a runtime recovery for the RSSI/CAD hang):
  if a preamble is detected but no header arrives within the preamble+header window,
  re-issue `startReceive()` to unlatch the stuck RSSI/carrier-detect logic. A cheaper
  in-place recovery than a full re-init for that specific hang.
