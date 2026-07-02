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
| **Training mode** | Button press on both → random network ID + shared AES key, no manual config | **Secure pairing done** — `AT+TRAIN` (X25519 ECDH) + first-boot proximity auto-pair derive a unique per-pair key; the built-in key is only a fallback |
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
robustness on cheap boards. The main functional gaps are now **FHSS** and
**field maturity** (secure pairing has since landed — see below).

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
- [x] **Coordinated mode switching** — `AT+MODE=<name>` drives a make-before-break
  handshake so both ends move together (with probation auto-revert + a dead-link
  rendezvous). Sim-tested and hardware-validated, byte-exact across switches.
  This is the solid foundation the adaptive features build on.
- [~] **Adaptive Data Rate (ADR)** — `AT+MODE=auto` layers SNR/loss-driven mode
  *picking* on top of that handshake. Built + sim-tested + bench-tested, but
  **opt-in and off by default**: field testing found it can wedge in the slowest
  mode on a marginal link (the switch-instant power transient — journey entry
  34), so it needs more field validation before it can ship on. See
  [FUTURE_MODES.md](./FUTURE_MODES.md).
- [~] **Auto TX-power** (`AT+APWR=1`) — a peer-SNR closed-loop that trims each
  side's power to the link's headroom; built + sim-tested, also **opt-in /
  experimental** (off by default) pending the same field validation. Novel for
  point-to-point LoRa (LoRaWAN power control is network-server-driven).
- [x] **Per-mode BDP window + ToA-derived timing** — done.
- [x] **Host-overrun safety (PSRAM ingest ring)** — done.

### Security
- [x] **Secure pairing / training mode — DONE.** `AT+TRAIN` on both ends runs an
  **X25519 ECDH** over the air: they exchange *public* keys in the clear and each
  derives the same link key (the secret never crosses the air), shown as a matching
  fingerprint, stored in NVS (survives reflash). Replaces the built-in key. Validated
  on hardware (matching fingerprint + byte-exact transfer on the derived key). Caveat:
  *unauthenticated* ECDH — pair in proximity, compare the fingerprint (MITM check).
- [~] **Forward secrecy** — crypto built + unit-tested (per-session **ephemeral
  X25519** + Ascon KDF; a minimal construction, not the literal Noise framework), but
  **opt-in / experimental** (`AT+FS=1`, off by default). The on-air handshake currently
  runs outside the proven turn rhythm and interferes with runtime mode switching;
  hardening it (carry the ephemeral keys as link-layer payload) is the remaining work.
  See `lib/linklayer/session.h`, `AT+FS`/`AT+SESSION?`, and SECURITY.md "Fix D".
- [~] **Per-device key provisioning** — first-boot **proximity pairing** now
  auto-derives and persists a unique **per-pair** key (the shared firmware key is
  only a fallback until then), so a pair no longer runs on one shared firmware
  key. A true per-*device* factory-provisioned identity key remains possible as a
  flash-time step.
- [x] **AEAD + NVS nonce counter + replay window** — done.

### Code quality & packaging
- [x] **Modularize `main.cpp`** — split into per-subsystem static-singleton
  classes: `fw_config.h` (shared contract), `fw_radio` (radio/ISR/modes),
  `fw_host` (USB↔link, AT, pairing, NVS), `fw_diag` (watchdogs/crash), `fw_device`
  (orchestration: setup/loop, roles, recovery, ADR), and `fw_session` (forward
  secrecy), with `main.cpp` the slim composition root; headers declare, `.cpp`
  files implement.
- [x] **Documented coding standard** — follows the Google C++ Style Guide
  (PascalCase functions/types, snake_case vars, `kConstant`, `ALL_CAPS` macros), with
  embedded carve-outs; ≤80-column rule enforced via `make format-check`. See
  [CODING_STANDARDS.md](./CODING_STANDARDS.md). Magic numbers extracted to named
  constants; AT commands commented.
- [x] **Standard build** — top-level `Makefile` wrapping PlatformIO.
- [ ] **Raise the language standard to C++20 + adopt the zero-cost modern subset**
  (`enum class`, `std::span`/`string_view` for buffer params, `[[nodiscard]]`).
  - **Where we are, and why.** The project began as an Arduino/PlatformIO ESP32
    sketch, and the arduino-esp32 core compiles sketches at its long-standing
    default of **`-std=gnu++11`** — so the firmware, and therefore everything in
    `lib/linklayer/` it pulls in, is held to **C++11** today. The native test
    env is separately pinned to **C++17** (`-std=gnu++17`), so the portable code
    targets the *intersection*: C++11-clean for anything the firmware compiles,
    C++17-isms only in native-only test code. C++11 + templates + `constexpr`
    already covers the static-allocation, no-heap design (rule 5), so nothing has
    forced a bump — the modern-C++ features above simply aren't reachable yet.
  - **The plan to reach C++20.** (1) Override the firmware standard in
    `platformio.ini` (`build_unflags = -std=gnu++11`, add `-std=gnu++2a`) and
    confirm the arduino-esp32 / ESP-IDF headers still build; (2) raise the native
    env to `-std=gnu++20` so tests and firmware agree on one standard; (3) adopt
    the zero-cost subset in `lib/linklayer/` one construct at a time, native
    suite green at each step. Heap-heavy STL stays out of the frame/ring hot
    paths regardless (rule 5).
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

The modularization and secure pairing are **done** (see the checklist above);
what remains, roughly in order:

1. **Field range testing** — validate the estimated per-mode ranges, and the two
   experimental adaptive features (ADR + auto-power), at real line-of-sight range;
   log RSSI/SNR vs distance. This is the gate to shipping the adaptive modes on by
   default.
2. **Harden forward secrecy** — carry the ephemeral X25519 keys as link-layer
   payload so the handshake runs inside the proven turn rhythm (SECURITY.md
   "Fix D"), so it can move from opt-in to default.
3. **C++ modernization + hardware guide** — the remaining code-quality and
   packaging items above (raise the language standard to C++20 and adopt the
   zero-cost subset; a BOM / wiring / antenna guide).
4. **FHSS** — the largest RF feature gap; a bigger effort, and see the note above
   on why it is low priority for our narrow band.
