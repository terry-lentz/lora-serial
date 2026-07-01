# Maturity roadmap & competitive position

Where this project stands today, how it compares to the closest existing product
([SparkFun LoRaSerial](https://github.com/sparkfun/SparkFun_LoRaSerial)), and the
concrete work left to make it a *mature*, publishable project. Companion to
[SW_ALTERNATIVES.md](./SW_ALTERNATIVES.md) (the broader landscape: Reticulum, Meshtastic).

## Closest sibling: SparkFun LoRaSerial

SparkFun LoRaSerial is the same idea as this project — "a pair of serial radio
modems that transparently pass data" — shipped as mature hardware + firmware
(MIT/open, Arduino C++). It is the most direct point of comparison.

### Where SparkFun LoRaSerial leads
| Their strength | Detail | This project |
|---|---|---|
| **Dedicated 1 W hardware** | Purpose-built modem: **30 dBm** PA, enclosure, screw terminals, antenna | Runs on generic dev boards at **22 dBm** → less range, no enclosure |
| **Frequency hopping (FHSS)** | Automatic channel hopping: collision avoidance, regulatory dwell-time, coexistence, harder to intercept/jam | Single fixed channel (see FHSS note below for why) |
| **Multipoint broadcast** | One-to-many, not just P2P | Strictly point-to-point (by design) |
| **Training mode** | Button press on both → random network ID + shared AES key, no manual config | Hardcoded key today; secure pairing planned (below) |
| **Field maturity** | Years of use, polished manual, product support | Young; bench + early field tested |
| **Turnkey product** | Buy a finished unit; download/flash | Open firmware + CI release binaries; no hardware product |

### Where this project leads
| Strength | Detail |
|---|---|
| **Generic / any LoRa chip** | RadioLib-driven → SX126x/127x/128x/LR11xx/STM32WL and any board, not one product. Cheap COTS hardware. |
| **Modern AEAD crypto** | Ascon-128 (NIST lightweight standard): **authenticated** encryption + **replay protection** + header authentication. SparkFun uses AES (encryption only, no documented AEAD/replay). |
| **Higher peak throughput** | ~**8 KB/s** at turbo (SF5/500) vs ~0.4–2 KB/s — the fast end of the curve is exposed. |
| **Host-overrun reliability** | PSRAM ingest ring + bulk reads → a fast `cat bigfile` is byte-exact (works around a real arduino-esp32 USB-CDC drop bug). |
| **Newer radio** | SX1262 (better sensitivity, lower power) vs an SX127x-class part. |
| **OS-login-ready** | Plug `agetty` straight on; systemd + udev deploy units included. |
| **Selective-Repeat + SACK + per-mode BDP window** | A more sophisticated ARQ than simple stop-and-wait. |
| **Documentation depth** | A radio primer + the full engineering journey + security plan. |

**Summary:** SparkFun LoRaSerial leads on range and turnkey hardware; this project
leads on modern security, raw speed, hardware flexibility, and host-overrun
robustness on cheap boards. The main functional gaps are **FHSS**, **secure
pairing**, and **field maturity**.

---

## What's left to be "mature"

Roughly in priority order. Checked items are done.

### Reliability & RF
- [ ] **Frequency hopping (FHSS).** The biggest functional gap vs SparkFun — but
  **deliberately not a priority for us**, for two reasons:
  1. **Regulatory motivation doesn't apply to our band.** SparkFun targets the US
     902–928 MHz ISM band at **1 W**, where regulators *reward* frequency hoppers
     (≥50 channels, short dwell) with high power and relief from dwell limits — so
     FHSS is largely regulation-driven for them. We operate in **Taiwan's 920–925 MHz**
     band, which is only **~5 MHz wide** — too narrow to spread across many channels
     (especially at BW250/500) — and we're not chasing 1 W, so that incentive is absent.
  2. **High complexity for modest P2P benefit.** FHSS needs both ends hopping in
     lockstep (a shared schedule seeded from the network key + tight time sync) plus a
     re-acquisition/scan protocol after any desync (e.g. a reboot) — significant
     machinery on top of the half-duplex ARQ, with real risk to the reliability we've
     built. For a private 2-node link in a quiet band, the payoff (coexistence /
     anti-jam) is modest.
  - **Cheaper 80/20 alternative (preferred first step):** a configurable channel
    (`AT+FREQ`) to manually dodge interference, optionally with listen-before-talk —
    most of the coexistence benefit at a fraction of the complexity. Full FHSS only
    becomes worthwhile for the US ISM band at high power, or genuinely congested spectrum.
- [ ] **Duty-cycle / dwell-time enforcement** for regulatory compliance (ties into
  FHSS). Today the cadence is bench-oriented.
- [x] **Adaptive Data Rate (ADR)** — `AT+MODE=auto`: the initiator picks the
  fastest mode the link supports from measured SNR and coordinates both ends via
  a make-before-break handshake. Sim-tested **and hardware-validated** (climbed to
  turbo, responder followed, byte-exact). See [FUTURE_MODES.md](./FUTURE_MODES.md).
- [x] **Per-mode BDP window + ToA-derived timing** — done.
- [x] **Host-overrun safety (PSRAM ingest ring)** — done.

### Security
- [x] **Secure pairing / training mode — DONE.** `AT+TRAIN` on both ends runs an
  **X25519 ECDH** over the air: they exchange *public* keys in the clear and each
  derives the same link key (the secret never crosses the air), shown as a matching
  fingerprint, stored in NVS (survives reflash). Replaces the built-in key. Validated
  on hardware (matching fingerprint + byte-exact transfer on the derived key). Caveat:
  *unauthenticated* ECDH — pair in proximity, compare the fingerprint (MITM check).
- [ ] (superseded plan, kept for context) Original training-mode note: an
  `AT+TRAIN` (and/or button) on both ends runs an **X25519 ECDH** key agreement
  over the air, deriving a shared key that is **never transmitted** — strictly
  better than sharing a key over the air, since an eavesdropper can't derive the
  secret from the exchanged public keys. Persist the derived key in NVS. (Tracked;
  ties to SECURITY.md "Fix D".)
- [~] **Forward secrecy** — crypto built + unit-tested (per-session **ephemeral
  X25519** + Ascon KDF; a minimal construction, not the literal Noise framework), but
  **opt-in / experimental** (`AT+FS=1`, off by default). The on-air handshake currently
  runs outside the proven turn rhythm and interferes with runtime mode switching;
  hardening it (carry the ephemeral keys as link-layer payload) is the remaining work.
  See `lib/linklayer/session.h`, `AT+FS`/`AT+SESSION?`, and SECURITY.md "Fix D".
- [ ] **Per-device key provisioning** — flashed/provisioned secret instead of one
  firmware key. Could be a flash-time step or the training mode output.
- [x] **AEAD + NVS nonce counter + replay window** — done.

### Code quality & packaging
- [x] **Modularize `main.cpp`** — split into `fw_config.h` (shared contract),
  `fw_radio.{h,cpp}`, `fw_host.{h,cpp}`, and `main.cpp`; headers declare, `.cpp` files
  implement.
- [x] **Documented coding standard** — follows the Google C++ Style Guide
  (PascalCase functions/types, snake_case vars, `kConstant`, `ALL_CAPS` macros), with
  embedded carve-outs; ≤80-column rule enforced via `make format-check`. See
  [CODING_STANDARDS.md](./CODING_STANDARDS.md). Magic numbers extracted to named
  constants; AT commands commented.
- [x] **Standard build** — top-level `Makefile` wrapping PlatformIO.
- [ ] **Modernize the C++ further** (where zero-cost on embedded): `enum class`,
  `std::span`/`string_view` for buffer params, `[[nodiscard]]`; bump to C++20.
  Deliberately keep heap-heavy STL out of the frame/ring hot paths.
- [x] **CI** — native tests + build all variants on every push/PR.
- [x] **Release binaries** — tagged releases publish flashable factory images.
- [x] **LICENSE (MIT) + CONTRIBUTING + docs reorg + radio primer** — done.
- [ ] **Hardware guide** — a recommended bill of materials, wiring, and antenna
  notes; optionally a reference enclosure.

### Features
- [x] **More AT config** — frequency (`AT+FREQ`), sync word, name, factory-reset
  (`AT&F`), `AT+TRAIN`, and a help (`AT$`/`AT?`) listing — all done. (A global
  numbered-channel scheme was prototyped and removed — see the journey entry
  "Channels: investigated and removed"; region-aware channel plans may return
  with FHSS.)
- [ ] **Multipoint / broadcast** (optional, scope-expanding) — a one-to-many mode.
  Only if there's demand; it dilutes the lean-P2P focus.
- [ ] **Field range testing** — validate the estimated per-mode ranges with real
  line-of-sight runs and log RSSI/SNR vs distance.

---

## Suggested order of work

1. **Modularize + modernize the firmware** — split `main.cpp` into focused modules and
   adopt the zero-cost modern-C++ subset; the native test suite guards the refactor.
2. **Secure pairing (`AT+TRAIN`)** — removes the hardcoded-key weakness (the most-cited
   maturity gap) and is a stepping stone to forward secrecy.
3. **FHSS** — the largest RF feature gap; a bigger effort, best done after the refactor
   so it lands in clean modules (and see the note above on why it is low priority here).
