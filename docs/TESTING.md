# Testing & simulation

How this project is tested **without radios** — what the tests cover, how the
three simulators work, and how to run and extend them. The whole suite — **91
tests** across three simulators — runs on a plain PC in ~20 seconds:
`pio test -e native`.

## Why simulate?

Flashing two boards, putting them in RF range, and feeding bytes is slow and
non-deterministic — packet loss, timing, and interference change every run. So the
**portable link layer** (`lib/linklayer/`) is written with **no Arduino or RadioLib
dependencies**: it talks to an *interface*, not to real hardware. That lets it
compile and run natively on the host PC, where a simulated radio channel can inject
loss, reordering, and turn-taking hazards **deterministically and instantly**.

The rule that makes this possible: **all protocol logic lives in `lib/linklayer/`
and is platform-free.** The firmware (`src/`) is a thin shell that wires that logic
to the real SX1262 (via RadioLib) and the USB port. Bugs are caught in the sim, in
seconds, before any flashing — the workflow this project deliberately follows.

## The PlatformIO `native` environment

`platformio.ini` defines an `env:native` that builds for the host PC instead of the
ESP32:

```ini
[env:native]
platform = native
build_src_filter = -<*>      ; don't build the Arduino firmware natively
build_flags =
    -std=gnu++17
    -pthread                 ; threads for the faithful half-duplex sim
    -I lib/linklayer -I lib/third_party/heatshrink -I lib/third_party/x25519
```

It compiles only the `test/` sources plus the header-only libraries — never the
firmware — so there is no hardware dependency. Tests use the **Unity** framework
(bundled with PlatformIO).

Run everything:

```bash
pio test -e native
```

Run one suite:

```bash
pio test -e native -f test_link     # the link-layer tests
pio test -e native -f test_modem    # the faithful half-duplex sim
```

## Three complementary simulators

### 1. `test/test_link/` — deterministic in-memory channel (59 tests)

A fast, **single-threaded** harness. Frames are streamed A→B through an in-memory
channel that can **drop** frames (both directions) using a seeded PRNG, so a "20 %
loss" run is bit-for-bit reproducible. Each test streams a payload and asserts the
received bytes are **byte-exact**, also reporting how many round-trips it took (so a
regression in efficiency shows up, not just correctness).

What it covers:

- **Transport:** lossless transfer, 20 % loss recovery, Selective-Repeat reordering,
  window-16 efficiency and loss, a large encrypted+compressed transfer, reconnect /
  epoch resync.
- **Compression:** round-trip, compressible vs. **incompressible** payloads (the
  latter caught a real length-truncation bug the compressible-only tests missed).
- **Crypto (Ascon-128 AEAD):** official **known-answer vectors (KAT)**, tamper
  detection (single frame and across a stream), the **sliding replay window**, and
  **nonce-skip-across-reboot** (the counter must never reuse a nonce after a reset).
- **Pairing (X25519):** RFC 7748 KAT, and a full ECDH + KDF **key-agreement** test
  (both ends derive the same link key).
- **Identity / role election:** MAC-based initiator election and the **two-board
  discovery outcome** — exactly one initiator, mutually-agreed and distinct
  (nonce-safe) addresses — swept over many MAC pairs, so "both boards became
  initiator" can never originate in the election logic.
- **Interrupt-RX frame ring:** the lock-free **SPSC ring** (`frame_ring.h`) that
  carries frames from the radio ISR/task to the main loop — FIFO order +
  byte-exactness, overflow drops without corruption, index wrap, oversize
  rejection. (See [INTERRUPT_RX.md](./INTERRUPT_RX.md).)

### 2. `test/test_modem/` — faithful threaded half-duplex sim (5 tests)

A slower but **far more realistic** harness that runs the **real**
`initiatorStep`/`responderStep` turn-taking loop in **two threads** against a shared
`SimChannel`. The channel models what a single half-duplex radio actually does:

- A frame is delivered **only if the peer is currently blocked in `rx()`** (i.e.
  actually listening) — otherwise it's *missed*, exactly like a real radio that
  wasn't in receive mode when the preamble arrived.
- **Time-on-air scales with frame size** (`sleep` proportional to length), plus an
  **RX re-arm latency**, so back-to-back burst frames the receiver misses while
  re-arming are reproduced.
- Optional loss injection on top.

This is the simulator that catches **turn-taking / phase / missed-frame** bugs the
deterministic alternating sim cannot — for example a sender that bursts faster than
the receiver can re-arm. It covers a slow consumer (back-pressure), small and bulk
echoes, a compressed+encrypted echo, and an echo under loss.

### 3. `test/test_sim/` — integration sim: ADR + mode-switch + radio behaviours (27 tests)

Builds two full `SimNode`s (real `LinkLayer` + `ModeSwitch` + `AdrController` +
rendezvous) over a channel that models the **inherent hardware behaviours** we hit
on the bench, so the software is forced to be resilient to them in sim before
hardware. The channel models:

- **Mode-deaf delivery** — a frame is heard only if the peer is on the sender's
  mode, so a half-completed mode switch really deafens the link.
- **SNR-driven loss vs each mode's demod floor**, swept up/down over a run to drive
  ADR through the modes; plus per-mode **time-on-air** in the physics layer.
- **In-memory ingest backlog** under backpressure (the firmware's PSRAM ring).
- **A radio that goes deaf mid-run** (`radio_stuck`) — the radio-stuck watchdog
  re-inits at the current mode to recover it.
- **The auto-power control loop** over an asymmetric path — reproduces the
  starvation equilibrium the symmetric-link assumption causes.
- **RX re-arm deaf window** in the physics layer — frames arriving while the
  receiver re-arms are missed; a *smaller* window (re-arm before processing the
  frame) catches more back-to-back frames than a larger one (re-arm after).
- **SX1262 AGC lockup** (`g_agc_lockup`) — a *continuous* receiver that never
  returns to standby goes permanently deaf after N frames (the errata that drove
  the responder to snr -27). A per-frame standby resets the AGC and avoids it.

Representative tests (teeth in **bold** — they fail if the fix regresses):
mode-switch byte-exactness across SNR sweeps; an exhaustive switch model-checker
(4096 loss patterns × up/down/gfsk/reboot); **`reset_on_switch_loses_data`**
(proves a full re-init on switch drops data); **`fixed_mode_holds_on_loss`** (a
pinned mode must never auto-change); `radio_stuck_recovers` +
**`radio_stuck_no_watchdog_stays_dead`**; `autopower_own_rssi_starves_asym`
(matches the hardware numbers) + `autopower_fixed_holds_asym`;
`phys_goodput_ordering` and `phys_rx_rearm_miss_collapses`.

Together: the deterministic sim gives **broad, fast, reproducible** coverage of the
protocol's correctness; the threaded sim gives **timing-faithful** coverage of the
half-duplex hazards; the integration sim gives **behaviour-faithful** coverage of
ADR, mode switching, and the radio failure modes we actually observed.

## What the sim does *not* cover (verify on hardware)

The sim abstracts the radio, so a few things still require a final hardware pass
(intentionally, after the sim is green):

- Real **RF behaviour** — actual SF/BW timing, sensitivity, interference, range.
  The sim *models* SNR loss and a re-arm deaf window, but cm-range bench effects
  (front-end saturation, local EMI making the link bimodal) only show on hardware.
- **RadioLib / SX1262 driver** specifics (DIO1 IRQ, RF switch, TCXO) — e.g. the
  continuous-RX re-arm and whether a given `readData()` sequence keeps receiving
  correctly is a driver detail only hardware confirms. Likewise the **SX1262
  sleep/AGC wedge** (a warm `sleep()` can leave the chip `CHIP_NOT_FOUND` until a
  power-cycle — see docs/RADIO_ERRATA.md): the sim models the *symptom* (deafness)
  but the chip-level recovery is hardware-only.
- **USB-CDC** behaviour and the PSRAM ingest ring under a true 1 MB/s `cat` burst.
- **NVS** persistence across real reboots/reflashes.

The project's workflow: **make the sim green first, then do one batched hardware
validation** — never flash to debug something the sim could have caught.

## Adding a test

1. Write the payload/scenario and assert byte-exactness (and, where relevant, the
   round-trip count) — copy the shape of an existing test in the suite.
2. Register it with `RUN_TEST(your_test)` in that file's `main()`.
3. `pio test -e native` — it must pass before any hardware work.
4. For any change to `lib/linklayer/`, **add a test** that would have caught the bug.
   The sim has repeatedly caught real bugs (compression edges, windowing, nonce
   handling) before hardware — keep that bar (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

## Host-side byte-exact check (with hardware)

Once flashed, [`tools/raw_verify.py`](../tools/raw_verify.py) sends a known stream
through the real link and verifies the bytes returned are identical — the on-hardware
analogue of the sim's byte-exact assertion, and the artifact to attach to any
reliability bug report.
