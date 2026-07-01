# LoRa Serial Link — a reliable point-to-point "wireless serial cable"

<img src="docs/img/hero.jpg" width="480" alt="Two XIAO ESP32S3 + Wio-SX1262 boards wired to a NanoPi NEO, with a phone logged in over the LoRa link">

*The whole thing in the wild: the two LoRa boards (left) bridging a NanoPi NEO's
serial console to a phone (right) logged in over the air — a plain terminal app,
no custom software.*

<img src="docs/img/demo.gif" width="800" alt="A LoRa Serial Link session: driving the LoRa link from a terminal — AT command interface, live RSSI/SNR, and a logged-in console over the air">

*A live session between two radios (sped up): configuring the radios and logging in over the LoRa link from an ordinary terminal.*

Turn two LoRa boards into a **wireless serial cable.** Plug a terminal or serial
device into either end; **whatever you send comes out the other side, byte-exact,
kilometers away — with no special software on either host.** It's a transparent
serial port that happens to be a long-range radio.

## Get it — flash a prebuilt release (no build needed)

[![Latest release](https://img.shields.io/github/v/release/terry-lentz/lora-serial?sort=semver&label=latest)](https://github.com/terry-lentz/lora-serial/releases/latest)

Don't want to build from source? Grab the latest release and flash both boards —
no toolchain required. Both boards run the **same image** and auto-elect their
roles at boot.

| Step | What to do |
|---|---|
| **1 · Download** | From the **[latest release](https://github.com/terry-lentz/lora-serial/releases/latest)**, download **`lora-serial-<version>.firmware.bin`** (the complete factory image). |
| **2 · Flash each board** | Open the **[ESP web flasher](https://espressif.github.io/esptool-js/)**, connect a board, and flash that file at offset `0x0`. Repeat for the second board. |
| **3 · Power on** | That's it — power both boards. They auto-pair, encrypt, and auto-tune; open a serial terminal on each end and start sending. |

**Defaults, out of the box** — the pair **auto-pairs** (deriving a unique
per-pair key on first boot) and is **encrypted by default**, and it runs in
**auto speed** (ADR) and **auto power**, adapting the mode and TX power to the
link on its own. You don't have to touch anything unless you want to override a
setting:

- **Encryption key** — already encrypted on a built-in key; run **`AT+TRAIN`**
  once on both ends for a unique per-pair key. See
  [SECURITY.md](./docs/SECURITY.md).
- **Speed** — check the live mode with **`AT+MODE?`**; pin a fixed one with
  **`AT+MODE=<name>`** (e.g. `medium`), then **`AT&W`** to persist, instead of
  auto. (Pin a band-compliant mode for your region — see **Regulatory** below.)
- **TX power** — auto by default; **`AT+APWR=0`** then **`AT+PWR=<dBm>`** to fix
  it.

(`firmware.bin` is the full image that boots a blank chip; the release also ships
an `app.bin` for OTA — see the release notes.) **Prefer to build from source?**
See **[Build from source](#build-from-source)** below.

## What you get

Not a toy demo — a full transparent serial transport with its own
Selective-Repeat link layer, a runtime-tunable radio, real authenticated
encryption, and on-device diagnostics. The headlines, and the depth under each:

- 🔌 **Plug-and-play transparent serial** — each end is a plain USB serial port;
  whatever you write comes out the other side byte-exact, with no host software.
  - Works with `tio`, PuTTY, `screen`, a phone serial app, or point `agetty` at
    it for a **remote login shell over LoRa**.
  - Presents a friendly USB identity (**`LoRa-Serial-XXXX`**, suffix from the
    chip MAC) so two boards plugged into one host are easy to tell apart.
- 🆔 **Zero-effort setup — no addresses, no pairing required** — flash the
  *same* firmware to both boards and power them on; there's nothing to number or
  configure to get a working link.
  - They **auto-discover each other and elect their roles + addresses from their
    chip MACs** at boot (lower MAC becomes the initiator) — identical firmware,
    no per-board build, no manual addressing.
  - Encryption works immediately on a built-in key; the optional one-time
    `AT+TRAIN` upgrade to a unique per-pair key is the only setup step, and even
    that is interactive, not required.
  - **First-boot proximity pairing:** a fresh board (empty NVS) pairs *once* at
    low power with an adjacent board — auto-discovering, electing roles, deriving
    a **unique per-pair key**, and persisting it — then runs at full power and
    skips discovery on every later boot. See "Pairing & keys" below.
- ✅ **Reliable & byte-exact** — a custom Selective-Repeat ARQ link layer, not
  fire-and-forget packets.
  - **SACK** (selective acknowledgement) retransmits only the frames actually
    lost; verified byte-exact through 30% packet loss and multi-MB transfers.
  - A **BDP-sized sliding window** that auto-sizes to the current mode's speed,
    plus automatic fragmentation/reassembly.
  - Optional **on-the-wire compression** (`AT+COMP`, heatshrink LZSS) — large
    wins on text/logs (see the throughput table below).
  - Half-duplex turn-taking driven by a **formally model-checked**
    mode-switch / recovery state machine
    ([MODE_SWITCH_SPEC](./docs/MODE_SWITCH_SPEC.md)).
- 🔒 **Encrypted & authenticated by default** — Ascon-128 AEAD (a NIST
  lightweight-crypto standard) with replay protection, on out of the box.
  - **Encrypted with zero setup** using a built-in fallback key; run
    **`AT+TRAIN`** once on both ends for a **unique per-pair key** derived over
    the air via X25519 ECDH (the secret is never transmitted), stored in NVS so
    it survives reflashes.
  - Optional per-session **forward secrecy** (`AT+FS=1`, experimental).
  - **`AT&F`** factory-resets the device and wipes the paired key (back to the
    fallback) — that's your hard-reset / re-pair. Full write-up in
    [SECURITY.md](./docs/SECURITY.md).
- ⚡ **Speed vs. range on the fly** — retune the radio at runtime, no reflash.
  - Six modes from **turbo** (SF5) to **far** (SF12), plus an experimental
    **ludicrous** GFSK mode; `AT+MODE=<name>` switches both ends together.
  - **`AT+MODE=auto` is ADR** (adaptive data rate), **on by default**: a
    loss-aware engine that picks the fastest mode the link's SNR *and*
    retransmit rate will carry, and steps down for range on its own. It now
    carries sustained bulk byte-exact too — an earlier bulk-transfer wedge
    turned out to be a software throttle in the RX task, since fixed
    ([journey entry 29](./docs/CAPABILITIES_JOURNEY.md)). Pin a fixed mode with
    `AT+MODE=<name>` to opt out.
  - **Auto-power** (`AT+APWR`) trims each side's TX power to what the peer's
    signal report says the link needs. So out of the box a board runs
    **compression + encryption + auto-power + ADR** — a self-tuning, encrypted
    link with zero setup. Only the GFSK **`ludicrous`** rung stays opt-in
    (`AT+ADRGFSK=1`): the fastest rung, but short-range and pending field-range
    validation.
  - Tune frequency (`AT+FREQ`), sync word (`AT+SYNC`), TX power (`AT+PWR`), and
    inter-frame gap (`AT+TXGAP`) live — set the carrier for your region
    (see the **Regulatory** section below).
- 🩺 **Diagnostics & self-healing built in** — it tells you what it's doing and
  why it last broke.
  - Live link stats (`AT+LINK?`: RSSI/SNR, retransmits, per-mode counters) and
    session/crypto status (`AT+SESSION?`).
  - Crash forensics (`AT+DIAG`: last-reset reason, free/min RAM, coredump flag)
    with on-device core dumps you can decode to a backtrace, plus deliberate
    crash/hang injection (`AT+CRASH`) to exercise the tooling.
  - A built-in throughput tester (`AT+SPEEDTEST` / `AT+SINK`).
  - Recovers itself: a software loop watchdog, a radio-stuck watchdog, and an
    auto-recovery state machine (re-init → rendezvous → reboot) with timeouts
    scaled to the current mode's time-on-air.
  - **Host-side tools** ([`tools/`](./tools/README.md)) that drive all of the
    above: `at.py` (run AT commands / speed tests), `lora_xfer.py` (throughput +
    byte-exactness over the cable), `coredump.sh` (pull & decode a crash), and
    `upload_flash.sh` (flash over the XIAO's native USB).
- 🧩 **Cheap, generic hardware + a real test bench** — runs on any LoRa radio
  [RadioLib](https://github.com/jgromes/RadioLib) supports (developed on XIAO
  ESP32S3 + Wio-SX1262). A native simulation (`pio test -e native`) models real
  hardware behaviors — mode-deaf delivery, SNR-driven loss, time-on-air,
  backpressure, a radio going deaf — so changes are proven before they ever
  reach a board.

**Speeds at a glance** (measured, byte-exact, encrypted; compressed = best case
on text/logs, uncompressed = worst case on random binary):

| Mode | ≈ vintage link | Uncompressed | Compressed | Rough range\* |
|---|---|---|---|---|
| **ludicrous** (GFSK)‡ | ≈ ISDN | ~7.5 KB/s | **~12 KB/s** | very short |
| **turbo** | ≈ 56K–ISDN | ~4.0 KB/s | ~9.0 KB/s | ~0.5–1 km |
| **medium** (default) | ≈ 28.8K | ~1.0 KB/s | ~3.2 KB/s | ~2–4 km |
| **far** | sub-300-baud | ~0.014 KB/s | ~0.04 KB/s | ~10–15+ km |

\* Range is a line-of-sight estimate; see [docs/THROUGHPUT.md](./docs/THROUGHPUT.md)
for all six modes, both compression cases, and the measurement method.

‡ `ludicrous` is an **experimental, opt-in** GFSK mode for very short range — the
fastest mode, but no spreading gain. See the modes table below.

> **New to radio?** Start with **[docs/RADIO_BASICS.md](./docs/RADIO_BASICS.md)** — a
> plain-English primer (spreading factor, dBm, what LoRa actually is) built on a
> "talking across a field" analogy. No jargon, no math.

### Other radios (LoRa vs SiK vs HaLow)

This project uses **LoRa** for its decode-below-the-noise range at very low
power. If you need more raw speed, a different radio might fit better — a swap is
a **throughput/feature upgrade, not a reliability fix** (the flakiness in this
project's history was software, since fixed, not the radio). The short version:

| Radio | Modulation | Throughput | Range | Best for |
|---|---|---|---|---|
| **LoRa** (this project) | Chirp spread spectrum | ~1–12 KB/s | km-scale (up to ~10–15+ km) | Max range at min power; obstructed links; battery |
| **SiK / FSK** (RFD900x) | (G)FSK | ~2–250 kbps | few km (40+ km best-case) | Higher-rate P2P with a power budget; FHSS telemetry |
| **Wi-Fi HaLow** (802.11ah) | OFDM, sub-GHz | ~1–15 Mbps | ~1 km+ | Mbps sub-GHz at ~km, with an IP stack |

Full comparison — the trade-offs, licensing, honorable mentions (CC1200, BLE
Coded PHY), and the low-power future — is in
[docs/HW_ALTERNATIVES.md](./docs/HW_ALTERNATIVES.md).

## Background Story

This project started as a fun experiment to see if I could get two SX1262
LoRa boards to talk to each other using custom firmware. I had played with
[Meshtastic](https://meshtastic.org/) quite a bit but in my area (Taiwan) there
wasn't much traffic and it was not too interesting (see
[SW_ALTERNATIVES.md](./docs/SW_ALTERNATIVES.md) for how this project compares).
The 'more interesting' application (to me) was to remotely connect
to a machine (tty) long distances to remotely administer or run applications at
modem speeds. I even thought about running a BBS on it for giggles.

In this project, I wanted  to do some 'vibe coding' with Claude and gave it a lot
of guidance along the way. Fairly quickly (in a couple hours) I had the basic 'toy'
functionality working. I could easily use Termius on my phone connected to the
hardware over serial to remotely access a machine. It was slow and not too
reliable. The connection quickly would hang. Any changes required a reflash.

Initially there was no concept of 'modes' to be able to configure spreading factor,
bandwidth, and signal strength on the fly. I made these configurable and since I
had two devices near each other connected to the same machine, I could let Claude
continuously iterate at improving the performance while I sat back and watched.

Eventually I thought it would be a good idea to make these configurable with AT
commands like a real modem. And so the AT commands were born allowing the changing
of mode on the fly and various other settings that I thought would also be useful
to configure (the full AT command set is documented in the AT-mode section below).

Ultimately, I then thought it would be nice if we could use the SNR
(signal-to-noise-ratio) data to figure out how to dynamically adjust the SF/BW and
signal strength. With Claude's help we did a bunch of research (see
[RESEARCH.md](./docs/RESEARCH.md)) to discover best approaches, landing on the
[ADR (adaptive data rate)](./docs/FUTURE_MODES.md) approach you currently see
implemented as `AT+MODE=auto`. That research also led to building a custom link
layer (see the "Our own link layer" section below, and
[DESIGN.md](./docs/DESIGN.md) for the full transport spec).

I also wanted encryption and specifically the ability to prevent replay attacks.
Initially it started out with just a static shared key baked into the firmware but
later on we added support for a 'training' mode (`AT+TRAIN`) where the devices
automatically can on their own agree upon a shared key over the air — using an
X25519 key exchange, so the secret itself is never transmitted. This is persisted
across firmware reflashes by being stored in NVS (non-volatile storage). The
crypto design is written up in [SECURITY.md](./docs/SECURITY.md).

Another important part was to be able to run simulation locally to test code changes
without needing to redeploy firmware every time. There are a fair number of tests
that are available to be run with the PlatformIO development environment (`pio test
-e native`). Some of these tests came from real bugs that were encountered along the
way. How the simulation and tests work is documented in
[TESTING.md](./docs/TESTING.md).

A few times I hit problems with the board crashing but no real way to debug the
issues easily. So I instructed Claude to help me build out some crash-dump analysis,
metrics, and diagnostic tools — written up in
[DIAGNOSTICS.md](./docs/DIAGNOSTICS.md) (what each tool/field is) and
[DEBUGGING.md](./docs/DEBUGGING.md) (a step-by-step worked walkthrough). I thought it
would be good to have an ability to purposefully crash or hang the device (the
`AT+CRASH` command) in order to test those diagnostic tools, so this was also added.

Overall, this is just a fun personal project to see how far I could get these devices
to work from and connect to a terminal session. It really does work. I also wanted to
see how far I could push Claude to build something that works and learn more about
LoRa along the way. I am not a radio expert and know just enough to be
'dangerous.' I learned a lot and decided to make this available to the public for
anyone that finds it interesting or useful. If you are interested in how I got Claude
to write code the way it did, take a look at the [CLAUDE.md](./CLAUDE.md) file. This
is a fairly lightweight set of rules compared to other projects I've built internally
(not yet made public) but maybe it is useful. An important 'tip' is to let Claude
help you build up this file (and it can also reference other files if you want to split
it up).

## Docs

*Start here*
- [RADIO_BASICS](./docs/RADIO_BASICS.md) (newcomer primer — dBm, SF, what LoRa is)
- [HOW_IT_WORKS](./docs/HOW_IT_WORKS.md) (technical deep-dive)
- [CAPABILITIES_JOURNEY](./docs/CAPABILITIES_JOURNEY.md) — what it can do **and the journey of problems we hit and solved** getting there

*Design & protocol*
- [DESIGN](./docs/DESIGN.md) (transport spec)
- [MODE_SWITCH_SPEC](./docs/MODE_SWITCH_SPEC.md) (the runtime mode-switch state machine, formally model-checked)
- [INTERRUPT_RX](./docs/INTERRUPT_RX.md) (the interrupt-driven RX design)
- [RADIO_ERRATA](./docs/RADIO_ERRATA.md) (SX126x errata + workarounds)
- [FUTURE_MODES](./docs/FUTURE_MODES.md) (auto/ADR, GFSK ludicrous, CAD — advanced modes)
- [SECURITY](./docs/SECURITY.md) (crypto design — Ascon AEAD, X25519, forward secrecy)
- [RESEARCH](./docs/RESEARCH.md) (design reasoning + references)

*Performance & operations*
- [THROUGHPUT](./docs/THROUGHPUT.md) (measured speeds + range per mode)
- [DIAGNOSTICS](./docs/DIAGNOSTICS.md) (AT+DIAG fields, metrics, crash tools)
- [DEBUGGING](./docs/DEBUGGING.md) (crash tools + worked walkthrough)
- [TESTING](./docs/TESTING.md) (how the sim models hardware; test coverage)

*Comparisons, roadmap & standards*
- [HW_ALTERNATIVES](./docs/HW_ALTERNATIVES.md) (other radios: LoRa vs SiK vs Wi-Fi HaLow)
- [SW_ALTERNATIVES](./docs/SW_ALTERNATIVES.md) (vs Reticulum/Meshtastic/SparkFun)
- [ROADMAP](./docs/ROADMAP.md) (maturity plan)
- [CODING_STANDARDS](./docs/CODING_STANDARDS.md) (the full coding-standards rationale)

## Build from source

Two boards, ~10 minutes. You need [PlatformIO](https://platformio.org/) and two
XIAO ESP32S3 + Wio-SX1262 boards. (Don't want to build? Grab prebuilt
`*.factory.bin` from the GitHub **Releases** page and skip to step 3.)

```bash
# 1. Get the tools and verify the build (no hardware needed for the tests)
pipx install platformio          # or: pip install platformio
make test                        # runs the sim/unit suite — should pass

# 2. Build + flash the two boards (ONE image, identical on each — they pick the
#    half-duplex role at runtime). Plug in BOTH boards and find their ports
#    with:  ls /dev/ttyACM*   (macOS: /dev/tty.usbmodem*)
make build
make flash PORT=/dev/ttyACM0   # one board
make flash PORT=/dev/ttyACM1   # the other (same image — they're identical)
#  ^ both boards run IDENTICAL firmware and need NO per-board setup: at boot they
#    auto-elect the half-duplex initiator/responder role from their chip MACs
#    (lower MAC initiates). See "Roles: initiator & responder" below.
#  ^ flashing uses tools/upload_flash.sh — needed for the XIAO's native-USB
#    quirk; a plain `pio run -t upload` fails to reset. See "Build & flash".

# 3. (Recommended) Pair them once for encryption: open each board's port in a
#    terminal (tio /dev/ttyACM0), type +++ to enter AT mode, run AT+TRAIN on
#    BOTH within a few seconds, confirm the fingerprints match, then AT&W to save.

# 4. Use it. The boards are now a transparent serial cable. To get a login shell
#    over the link, run a getty on the "server" board's port and connect from the
#    other end with any terminal:
sudo agetty -L 115200 ttyACM0 vt100     # server end (the machine you log into)
tio /dev/ttyACM0                        # client end (roaming) -> login prompt
```

That's the whole path. Optional next steps: pick a speed/range preset with
`AT+MODE=<name>` (or `AT+MODE=auto` to let the link adapt), set up a **permanent
respawning console** with the systemd unit in [`deploy/`](./deploy/README.md), and
read the regulatory/**[Before transmitting](#️-before-transmitting)** notes before
going on-air. Full detail for every step is in
[Build & flash](#build--flash), [Use it as a login terminal](#use-it-as-a-login-terminal-no-custom-software),
and [Configure / query the link](#configure--query-the-link-from-a-plain-terminal--at-mode).

## Built with — and why these choices

- **[RadioLib](https://github.com/jgromes/RadioLib) drives the radio.** It speaks a
  *single* API across nearly every LoRa chip (SX126x / SX127x / SX128x, LR11xx,
  STM32WL, RFM9x…), so our transport stays **radio-agnostic and portable** — this is
  what lets the project run on cheap, varied hardware instead of one board. We chose it
  over the **chip-specific Semtech driver** (lots of boilerplate, single part), the
  **Arduino-LoRa** library (SX127x-only, lightly maintained), and **RadioHead** (broad
  but dated): RadioLib is actively maintained, gives **raw-PHY access** (not
  LoRaWAN-locked), and exposes `getTimeOnAir()` — which we use to **auto-tune the turn
  timing for every mode**.
- **[PlatformIO](https://platformio.org/) builds and tests it.** One `platformio.ini`
  **pins exact platform/library versions** (reproducible builds), defines every
  firmware variant, *and* a **native test environment** — so the portable link layer is
  unit-tested on your PC (`pio test -e native`) with no hardware. We chose it over the
  **Arduino IDE** (no reproducible deps, no native tests, no CI) and **raw
  ESP-IDF/CMake** (far more boilerplate) — while still getting the Arduino core's
  USB-CDC and RadioLib convenience underneath.

## What this is (and how it differs from Reticulum / Meshtastic)

A **dead-simple, rock-solid, fast point-to-point "LoRa serial cable."** Both ends
present a plain serial port; bytes in one come out the other, reliably and
byte-exact. It's just the pipe — because it looks like an ordinary serial port,
existing tools ride on top unchanged: point your OS `agetty` at it for a remote
shell, run a file transfer, attach a remote console for any serial device, etc.

We are **not** building a mesh. That's the deliberate edge: a dedicated two-node link
deletes the contention/routing overhead a general stack must carry, so we get closer
to the LoRa PHY ceiling for the 2-node case and stay far simpler.

- **Meshtastic** → LoRa messaging/mesh with screens. Different job.
- **Reticulum + `rnsh`** → a full secure *mesh networking* stack that can also do a
  LoRa shell. More capable in general; heavier for pure point-to-point. If you want a
  secure mesh, use it.
- **This** → the lean, owned, P2P reliable serial link. See
  [SW_ALTERNATIVES.md](./docs/SW_ALTERNATIVES.md) (prior art + head-to-head),
  [DESIGN.md](./docs/DESIGN.md) (transport spec), and [RESEARCH.md](./docs/RESEARCH.md)
  (techniques surveyed and chosen).

## Architecture: a smart radio modem

Both ends are **identical** — **XIAO ESP32S3 + Wio-SX1262 (B2B)** on USB-C, running
the same firmware (role decided at runtime from the address, so the boards are
interchangeable). Each presents a plain serial port; whatever you plug in is the app.

```
  any serial program            any serial program
  (terminal, getty, scp,        (terminal, getty,
   a device's console, …)        a device's console, …)
       │ USB-CDC (plain serial)        │ USB-CDC (plain serial)
  [XIAO ESP32 + SX1262] ~~~~ LoRa ~~~~ [XIAO ESP32 + SX1262]
   firmware = the smart modem:  RadioLib PHY + Selective-Repeat ARQ +
   flow control + auto TX power + per-mode timing  (all on-device)
```

The **entire reliable link lives on the ESP32** (PHY, framing+CRC, Selective-Repeat
ARQ, half-duplex turn-taking, flow control, optional auto power) — the radio timing is too
tight to push across USB latency, and keeping it on-device is what makes each end a
**plain reliable serial port with no host software**. The host just reads/writes
bytes; reliability is guaranteed underneath (USB NAK back-pressure + radio ARQ).

## Firmware

Custom firmware (not Meshtastic / not any stock image). Both boards run the **same
firmware**; the role (initiator / responder of the half-duplex turn-taking) is
auto-elected at boot from each chip's MAC (lower MAC initiates), so the two boards
are interchangeable and need no per-board configuration.

- **Framework:** ESP32 Arduino core, built with the PlatformIO `espressif32` platform
- **Radio driver:** [RadioLib](https://github.com/jgromes/RadioLib) 7.7.1 (SX1262)
- **Sources:** [`src/`](./src/) — `fw_config.h` (shared config/types), `fw_radio.{h,cpp}`
  (RadioLib glue, ISR, modes, TX/RX), `fw_host.{h,cpp}` (USB I/O, PSRAM ingest, AT mode,
  pairing, NVS), `fw_diag.{h,cpp}` (crash/health diagnostics), `fw_session.{h,cpp}`
  (forward-secrecy handshake), `fw_device.{h,cpp}` (the `Device` orchestrator: turn
  engine, role discovery, recovery, ADR), `main.cpp` (shared globals + `setup()`/`loop()`
  forwarding to `g_device`) — and
  [`lib/linklayer/`](./lib/linklayer/) (the portable, unit-tested data-link layer below)

### Roles: initiator & responder

The link is **half-duplex** — only one radio transmits at a time — so the two ends
take **turns**, and one of them has to drive that rhythm. That's the **initiator**;
the other is the **responder**:

- **Roles are auto-elected from the chip's MAC** — at boot the two boards briefly
  beacon their factory MAC and the **numerically lower MAC becomes the
  initiator** (and takes link address 1; the other takes 2). **Identical firmware
  on both boards, nothing to configure** — flash the same image to both and they
  sort it out. Check which is which with `ATI` (it prints `initiator=1` on the
  initiator). **Don't rely on the `/dev/ttyACM*` number** — it can change across
  reboots/replugs; the `/dev/serial/by-id` name carries each board's MAC.
- **You never choose "initiator" yourself** — there's no role switch and no
  address to assign. The MAC is a unique, permanent tie-breaker, so the roles
  fall out automatically and deterministically.
- **The initiator drives each turn** (sends its burst or a poll, then listens) and
  **owns the decisions** that must stay in sync — most notably **`auto`/ADR**: only
  the initiator measures SNR and *decides* when to change mode. The **responder
  follows**, adopting whatever mode the initiator coordinates over the air.
- **Consequence for `AT+MODE=auto`:** set it on the **initiator**. On the responder
  it does nothing (it has no decisions to make). And when you query `AT+MODE?`, only
  the **initiator** shows the `(auto)` tag — the responder just reports the raw
  mode it was switched to. So to confirm auto is working, check that the **SF/BW
  matches on both ends**, not that both say "auto". (More in
  [FUTURE_MODES.md](./docs/FUTURE_MODES.md).)

Everything else — the byte pipe, encryption, compression — is fully symmetric; the
initiator/responder split is purely about who keeps the turn-taking clock.

> **Could the boards just auto-negotiate who initiates, with no addresses at all?**
> In effect they already do: you never declare a role — it's derived from the
> address, and the A/B builds assign those for you, so there's nothing for you to
> pick. Going fully address-free (two blank boards electing a leader over the air)
> is possible but buys little and adds risk: the link needs addresses anyway for
> framing (`src`/`dst`) and as part of the encryption nonce, and a *deterministic*
> rule ("lower address initiates") can't hit the failure mode a live election can —
> both sides briefly deciding they're the initiator (two talkers, collisions) or
> both the responder (two listeners, deadlock). A fixed tie-break from a value the
> boards already must have is simpler and can't split-brain. See
> [DESIGN.md](./docs/DESIGN.md) §5.

## Our own link layer (custom on-air protocol)

A genuinely distinctive part of this project: the reliability isn't borrowed from
LoRaWAN or any stack — it's a **small custom data-link layer we designed**
([`lib/linklayer/linklayer.h`](./lib/linklayer/linklayer.h)), written to be portable
(no Arduino/RadioLib deps) so it runs on the ESP32 **and** natively on a PC, where
it's unit-tested against simulated loss/reordering. It gives us Selective-Repeat ARQ
with a SACK bitmap, per-frame compression, and Ascon-128 AEAD — in a frame that fits
the LoRa 255-byte limit exactly.

```
 LoRa payload (≤ 255 bytes) — our frame:

 byte:  0     1     2      3     4     5      6      7     8     9 .. 16   17 ............... N      N+1 .. end
       ┌─────┬─────┬──────┬─────┬─────┬──────┬──────┬─────┬─────┬──────────┬───────────────────┬──────────────┐
       │ src │ dst │flags │ seq │ ack │epoch │sackHi│sackLo│ len │  ctr64   │  payload          │  AEAD tag    │
       └─────┴─────┴──────┴─────┴─────┴──────┴──────┴─────┴─────┴──────────┴───────────────────┴──────────────┘
        └──────────────── 9-byte header (HDR) ─────────────────┘ └ 8 (enc) ┘ └ len bytes ──────┘ └ 8 or 16 ───┘

   flags byte:  bit0 F_DATA   bit1 F_COMP   bit2 F_ENC   bit3 F_MORE
                bits4-7 = a 4-bit control nibble (mode-switch / ADR handshake)
   ctr64    : present only when F_ENC — 64-bit monotonic frame counter (nonce + replay id)
   payload  : the data bytes, compressed first if F_COMP shrank them, then encrypted if F_ENC
   AEAD tag : present only when F_ENC — authenticates the header + counter + payload
              (so seq/ack/sack/flags can't be forged or tampered)
```

- **Selective-Repeat ARQ + SACK + fast retransmit** — the 2-byte SACK bitmap reports
  exactly which frames arrived, so the sender resends only the gaps; a per-mode window
  (sized to the bandwidth-delay product) keeps the pipe full instead of stop-and-wait.
  A gap the SACK exposes is resent **immediately** (TCP-style fast retransmit) rather
  than waiting out the retransmit timer — which is what makes the high-rate `ludicrous`
  (GFSK) mode actually fast.
- **`epoch`** — a boot id; if it changes, the peer rebooted and both sides resync.
- **`F_MORE`** — "more frames coming this turn," so a burst of any length is received
  without a fixed whole-burst timer.
- **AEAD** — when encryption is on, *every* frame (even empty ACKs) is sealed and the
  whole header is authenticated. See [DESIGN.md](./docs/DESIGN.md) and
  [SECURITY.md](./docs/SECURITY.md).

## Radio support (tested vs. portable)

LoRa is Semtech-proprietary, so every "LoRa chip" is Semtech silicon or licensed
Semtech IP — there is no third-party clone. We **drive the radio entirely through
[RadioLib](https://github.com/jgromes/RadioLib)**, whose common `PhysicalLayer` API
(`begin` / `getTimeOnAir` / `startTransmit` / `readData`) we use; our transport, USB
and app layers are **radio-agnostic**. So supporting another chip is mostly a
radio-init swap, not a rewrite.

- **Tested:** **SX1262** (Semtech SX126x family) on the Seeed Wio-SX1262 + XIAO ESP32S3.
- **Should work via RadioLib (untested here)** — the other Semtech LoRa families RadioLib supports:
  - **SX126x** — SX1261 / **SX1262** / SX1268 (our family; sub-GHz).
  - **SX127x** — SX1272 / SX1276 / SX1278 / SX1279 (older; the common **RFM95/96** modules).
  - **SX128x** — 2.4 GHz LoRa (worldwide-license-free band).
  - **LR11xx** — LR1110 / LR1120 / LR1121 (LoRa **+ GNSS / WiFi geolocation**).
  - **STM32WL** — ST MCU with a licensed Semtech sub-GHz radio on-die (`WLR89x`).
- **Not LoRaWAN-locked:** we use raw LoRa PHY, so any of the above works as a P2P link.

Porting to more radios = add the RadioLib class + `begin()` params for that
chip behind our existing `Radio::ApplyMode()`; the timing auto-derives from
`getTimeOnAir()`.

## Use it as a login terminal — no custom software

The board is a **plain USB serial device**. On Linux it enumerates as
**`/dev/ttyACMx`** (macOS: `/dev/tty.usbmodem*`; Windows: `COMx`) — "the board's
port" just means that device node. Because the link is byte-transparent, the OS's
own serial-console tools turn it into a remote login with **zero custom code**:

- **Server end (the machine being logged into)** — point `agetty` at the board's port:
  ```bash
  sudo agetty -L 115200 ttyACM0 vt100      # -L = no carrier-detect/auto-baud (a plain link)
  # or the managed equivalent:
  sudo systemctl enable --now serial-getty@ttyACM0.service
  ```
  This spawns the real **`login` + PAM + your shell** over LoRa. Plain `agetty` is
  *one-shot* (it exits when you log out). For a **permanent console that respawns
  after every logout** — e.g. on an Armbian SBC — use the ready-made systemd unit and
  udev rule in [`deploy/`](./deploy/README.md):
  ```bash
  sudo cp deploy/99-lora.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
  sudo cp deploy/lora-getty@.service /etc/systemd/system/ && sudo systemctl daemon-reload
  sudo systemctl enable --now lora-getty@lora        # respawns on logout
  ```
- **Client end (roaming)** — any terminal: `tio /dev/ttyACM0`, PuTTY, `screen`,
  `minicom`. You get the login prompt.

getty handles the login; our device handles the reliable byte pipe. The same
applies to anything that speaks a serial port (PPP/SLIP for IP-over-LoRa, a
device's serial console, etc.) — no host software required.

## Configure / query the link from a plain terminal (`+++` / AT mode)

Like a Hayes dial-up modem, the transparent serial build (`*_raw`) understands an
**out-of-band command mode** — so you can change radio settings or check the link
**without any host software**, from the same terminal you're already using:

1. Stay idle ~1 second, type **`+++`**, stay idle ~1 second → the modem replies `OK`.
   (The 1-second guard windows are why `+++` *inside* your data stream passes
   straight through as data and never trips command mode.)
2. Now issue AT commands (each ended with Enter):

   | Command | Effect |
   |---|---|
   | `AT` | `OK` (link check) |
   | `ATI` | identity: name, address, peer, initiator role, and `fw=` firmware version |
   | `AT+VER` | firmware version (`fw=…`), stamped from the git tag at build time (`v0.2.0`, or `v0.2.0-3-gabc1234` on a dev build) |
   | `AT+LINK?` | live link state — `rssi`, `snr`, `pwr` (TX dBm), `txq` (queued), `hin`/`hout` (host bytes in/out), `ibuf` (ingest ring KB), `idrop` (overrun bytes), `tx`/`retx` (frames sent/resent), `heap` (free internal SRAM) |
   | `AT+SESSION?` | forward-secrecy status — `session` (1 = a per-session key is active), and a 2-byte fingerprint of the `static` vs `active` key (they differ once the handshake completes). |
   | `AT+DIAG` | crash & health report — boots, **why it last reset** (panic/brownout/watchdog/clean), uptime before that reset, free/min internal SRAM, core-dump presence. See [DIAGNOSTICS.md](./docs/DIAGNOSTICS.md). |
   | `AT+CRASH=<panic\|hang>` | **deliberately crash this board** (recoverable) to verify the diagnostics catch it — `panic` → core dump + `lastreset=PANIC`; `hang` → software-watchdog reboot. See [DEBUGGING.md](./docs/DEBUGGING.md). |
   | `AT+MODE?` | show current range/speed mode + the available presets |
   | `AT+MODE=<name>` | pin a fixed mode and **coordinate the peer**: `turbo` (SF5/500) · `fast` (SF7/500) · `medium` (SF7/250) · `slow` (SF9/125) · `far` (SF12/125) · `ludicrous` (GFSK). Run on the **initiator**; it switches both ends via a make-before-break handshake (the responder follows). Timing auto-derives from the mode. |
   | `AT+FMODE=<name>` | **force** this mode locally only (no peer coordination). Use to set both ends manually (run it on each) or to recover a mismatched pair. |
   | `AT+MODE=auto` | **adaptive data rate** — the initiator measures link SNR **and the live retransmit rate** and *coordinates both ends* to the fastest mode the link sustains (responder follows; handshake with auto-revert, plus a dead-link rendezvous to recover any mismatch). Climbs `far`…`turbo`; steps up to GFSK `ludicrous` on a strong, close-range link (`AT+ADRGFSK=1`). Run on the initiator. **On by default**; carries sustained bulk byte-exact (the old heavy-load wedge was an RX-task throttle, fixed — [journey 29](./docs/CAPABILITIES_JOURNEY.md)). Pin a fixed mode with `AT+MODE=<name>` to opt out. |
   | `AT+PWR=n` | set TX power to `n` dBm (fixed unless auto-power is on) |
   | `AT+APWR=0\|1` / `AT+APWR?` | toggle **auto TX-power** control. **On by default** — a peer-SNR feedback loop holds each side's TX power a margin above the mode's demod floor; set `AT+APWR=0` to pin fixed power. See [THROUGHPUT.md](./docs/THROUGHPUT.md). |
   | `AT+ADDR=n` / `AT+PEER=n` | *(advanced/legacy)* override the link address — normally **auto-elected from the MAC** at boot, so you don't set these. |
   | `AT+NAME=s` | set this node's name (≤15 chars) |
   | `AT+FREQ=mhz` / `AT+FREQ?` | set/show carrier frequency (e.g. `923.2`); accepts ~150–960 MHz. **Both ends must match.** Verify it's legal for your region. |
   | `AT+SYNC=0xNN` / `AT+SYNC?` | set/show the private-link sync word. **Both ends must match.** |
   | `AT+ENC=0\|1` / `AT+COMP=0\|1` | toggle encryption / compression |
   | `AT+FS=0\|1` / `AT+FS?` | toggle **forward secrecy** (per-session ephemeral-key handshake). **Experimental, off by default** — see [SECURITY.md](./docs/SECURITY.md). |
   | `AT+TRAIN` | **secure pairing**: run on *both* ends (same mode) — they agree on a unique key over X25519 ECDH (no secret sent over the air) and print a fingerprint that must match. Stored in NVS, survives reflash. Replaces the built-in key. |
   | `AT+PAIR` | **proximity re-pair**: bring the two boards close and run on *both* — they re-discover at low power, re-elect roles, and re-train a fresh key. Same result as `AT+TRAIN` plus role election. See "Pairing & keys" above. |
   | `AT&W` | persist current settings to flash (NVS) — survives reboot **and reflash** |
   | `AT&F` | factory reset: clear NVS to build defaults (also **wipes the paired key**, so the next boot re-pairs / reverts to the built-in key) |
   | `AT?` | list all commands |
   | `ATO` | leave command mode, back to the transparent data pipe |

This is also the answer to "is the link up?" without a carrier-detect (DCD) wire:
the ESP32 TinyUSB CDC stack can't drive DCD device→host, but `AT+LINK?` gives you
RSSI/SNR/power on demand over the same port.

### Pairing & keys — three levels

The link is **encrypted by default** (Ascon-128 AEAD); how the *key* is
established is up to you, in increasing order of security:

1. **Out of the box — built-in key.** With no setup the link uses a fallback key
   compiled into the firmware. It works immediately, but it's the *same key on
   every board running this firmware*, so it's privacy-by-obscurity, not real
   per-pair security.
2. **`AT+TRAIN` — a unique per-pair key (recommended).** Run it on **both** ends
   (boards on the same mode); they derive a **unique** key over an X25519 ECDH
   exchange — the secret itself is never transmitted — print a fingerprint that
   must match on both ends, and store the key in NVS (it survives reflash). This
   is the recommended one-time step; after it, the pair is cryptographically
   distinct from every other.
3. **Proximity pairing — first-boot auto-pair (default).** A board with **no key
   in NVS** boots straight into pairing: it drops to **low TX power** (so it
   reaches a nearby board — proximity = intent, like NFC/BLE), discovers + elects
   roles, auto-runs the `AT+TRAIN` exchange to derive a **unique per-pair key**,
   **persists role + key**, then jumps to full power. Every later boot loads that
   identity and skips discovery. Place the two fresh boards close, power both, and
   they pair themselves. While pairing they blink the LED and print
   `[LoRa-Serial] PAIRING …` on the USB port (`ATI` reports `state=pairing`).
   Build `-D PROX_PAIR=0` to disable (re-elect from the MAC every boot instead).
   *Caveat: low power alone doesn't strictly enforce adjacency (~0 dBm still
   reaches a metre-plus); an RSSI gate to require true proximity is a planned
   follow-up — for guaranteed isolation, pair away from other powered boards.*

**Re-pair** any time with **`AT+PAIR`** (re-runs proximity pairing on both ends,
keeping other settings) or **`AT&F`** (factory-reset — wipes the key so the next
boot re-pairs). The full crypto design is in [SECURITY.md](./docs/SECURITY.md).

### Pick a range/speed mode — a ONE-TIME setup (works with agetty, tio, anything)

The mode is **saved in flash**, so you set it **once** and every later session — including
`agetty` — just uses the port with **no AT traffic per session**. Use the helper
(`host/atcmd.py`) which handles the `+++` escape timing for you. Do this on **both** boards:

```bash
# one-time: switch to turbo and persist it
python3 host/atcmd.py /dev/ttyACM0 AT+MODE=turbo AT\&W
# check it
python3 host/atcmd.py /dev/ttyACM0 AT+MODE? AT+LINK?
```

After that, `sudo agetty -L 115200 ttyACM0 vt100` (or your terminal) just works at the
saved mode — the AT layer is invisible to it. To change modes later, re-run the helper on
both ends. (You can also do it by hand in any terminal: type `+++`, wait ~1 s, then
`AT+MODE=turbo`, `AT&W`, `ATO`.)

### Modes: speed vs. range

Every mode is one point on the same speed↔range trade-off. The ladder, slowest/
farthest to fastest/closest:

```
   far  ◄─────────────── the speed ↔ range trade-off ───────────────►  ludicrous
   ┌──────┬──────┬────────┬──────┬───────┬───────────────────────────────────┐
   │ far  │ slow │ medium │ fast │ turbo │ ludicrous (GFSK, opt-in)          │
   │SF12  │ SF9  │ SF7    │ SF7  │ SF5   │ FSK — "we're basically touching"  │
   │/125  │/125  │ /250   │/500  │/500   │ ~12 KB/s — the fastest mode       │
   └──────┴──────┴────────┴──────┴───────┴───────────────────────────────────┘
   ◄── more range, less speed         more speed, less range ──►
        ~10-15 km                              ~0.5 km            (LOS estimates)
```

| Mode | SF / BW | Uncompressed* | Compressed* | ≈ vintage link | RX sens (≈) | LOS range** |
|------|---------|---------------|-------------|----------------|-------------|-------------|
| **ludicrous** | GFSK 200 kbps | ~7.5 KB/s | **~12 KB/s**† | ≈ ISDN B‑channel (~64–96 kbit/s) | lowest | very short |
| **turbo**  | SF5 / 500 | ~4.0 KB/s | ~9.0 KB/s | ≈ 56K–ISDN | ~−111 dBm | ~0.5–1 km |
| **fast**   | SF7 / 500 | ~1.7 KB/s | ~5.0 KB/s | ≈ 33.6–56K modem | ~−117 dBm | ~1–2 km |
| **medium** | SF7 / 250 | ~1.0 KB/s | ~3.2 KB/s | ≈ 28.8K modem | ~−120 dBm | ~2–4 km |
| **slow**   | SF9 / 125 | ~0.13 KB/s | ~0.43 KB/s | ≈ 2400–4800 baud | ~−129 dBm | ~5–10 km |
| **far**    | SF12 / 125 | ~0.014 KB/s‡ | ~0.04 KB/s‡ | sub-300 baud | ~−137 dBm | ~10–15+ km |

\* **Two real measurements, not one blended number.** *Uncompressed* feeds random
binary (the worst case — heatshrink can't shrink it); *Compressed* feeds all-zeros
(the best case). Real text/logs/shell land **between** the two (heatshrink ~halves
typical text). Both are end-to-end **byte-exact** payload rates with **AEAD
encryption on** and the interrupt-driven RX path, measured 2026-06-30 on the bench
(boards ~1.5 m apart, +22 dBm) with [`tools/lora_xfer.py`](./tools/lora_xfer.py)
over a **64 KB** transfer for turbo/fast/medium/ludicrous and **8 KB** for slow —
large enough to amortize the half-duplex turn-around. See
[THROUGHPUT.md](./docs/THROUGHPUT.md) for the full method + raw runs.

\*\* **Estimates only** — line-of-sight, +22 dBm, a decent antenna. Real range is
dominated by antenna, height, and obstacles; validate in the field. Each step roughly
doubles+ range while roughly halving speed.

\‡ `far` (SF12) is now **measured and byte-exact** (after the TX-safety fix that
made far work at all — see [CAPABILITIES_JOURNEY](./docs/CAPABILITIES_JOURNEY.md)
entry 26), but by a different method than the rows above: its ~13 s/frame airtime
makes a 64 KB run impractical, so *uncompressed* is from the on-device
`AT+SPEEDTEST` (1–2 KB, 0 % retx) and *compressed* from a small all-zeros
transfer, measured at `pwr=10`. Throughput is dominated by per-frame overhead at
SF12, so compression helps less here than on faster modes.

\† **`ludicrous` (GFSK) is experimental but it's the fastest mode** —
hardware-validated byte-exact at **~12 KB/s** (sustained to 64 KB), switchable
to/from LoRa at runtime. Its first cut was *slower* than turbo until **fast retransmit
on SACK** removed the per-loss stall; that journey is written up in
[FUTURE_MODES.md](./docs/FUTURE_MODES.md). Opt-in, very short range (no spreading
gain); set it manually on **both** ends (`AT+MODE=ludicrous`).

## Regulatory — pick a frequency that's legal where you are

The SX1262 can tune roughly **150–960 MHz**, so the *band is a configuration
choice* — and which sub-GHz ISM band is legal is **your responsibility**, set by
your country's regulator. The firmware ships defaulted to the author's region
(**Taiwan**), so **change it for yours before transmitting.**

| Region | Typical band | Carrier (`AT+FREQ`) | Notes |
|--------|--------------|---------------------|-------|
| **Taiwan** (author) | **920–925 MHz** (NCC LP0002) | `923.2` (build default) | Only ~5 MHz usable — tight. |
| **US / Canada** | 902–928 MHz (FCC Part 15 / ISED) | `915.0` | Widest band; FHSS or ≤ −20 dBc rules for some uses. |
| **EU / UK** | 863–870 MHz (ETSI EN 300 220); 868 common | `868.0` | **Duty-cycle limited** (often 1 %); also a 869.4–869.65 sub-band at higher power. |
| **Australia / NZ** | 915–928 MHz (AS/NZS 4268) | `919.0` | — |
| **India** | 865–867 MHz | `866.0` | — |

These are **starting points, not legal advice** — check your local band edges,
**max EIRP**, and **duty-cycle** limits, and that both ends use the same value.
The carrier is a plain frequency (`AT+FREQ=915.0`); set it identically on both
boards. The radio accepts roughly **150–960 MHz** — pick a value legal where you
operate.

**Set it** (same on both boards; the lower-MAC board coordinates nothing here —
frequency is local, so set each):

```sh
tools/at.py /dev/ttyACM0 "AT+FREQ=915.0" "AT&W"   # set carrier + persist to NVS
tools/at.py /dev/ttyACM1 "AT+FREQ=915.0" "AT&W"
```

`AT+FREQ?` shows the current carrier. To bake a different default into the
firmware, change `kFreqMhz` in `src/fw_config.h`. TX power is `AT+PWR`
(≤ +22 dBm); keep total EIRP within your region's limit.

### Bandwidth is regulated too — not just the frequency

Picking a legal *frequency* is only half of it: regulators also cap the
**occupied bandwidth (OBW)** of the signal, and the wide LoRa bandwidths are
**not legal everywhere**. Each speed mode uses a different bandwidth:

| Mode | PHY | Bandwidth |
|------|-----|-----------|
| `turbo` | SF5 LoRa | **500 kHz** |
| `fast` | SF7 LoRa | **500 kHz** |
| `medium` (default) | SF7 LoRa | 250 kHz |
| `slow` | SF9 LoRa | 125 kHz |
| `far` | SF12 LoRa | 125 kHz |
| `ludicrous` | GFSK 200 kbps | ~400 kHz occupied |

- **Taiwan (NCC LP0002 / AS923):** the 920–925 MHz plan is channelized for
  **125 kHz and 250 kHz only — there is no 500 kHz channel.** So `medium`,
  `slow`, and `far` fit the plan; **`turbo`/`fast` (500 kHz) and `ludicrous`
  (wide GFSK) do not**, and should be treated as **non-compliant in Taiwan
  until verified.** NCC *does* test OBW, so a hard limit applies — but the
  official LP0002 PDF blocks automated access and we could not pin the exact OBW
  figure from public sources, so **confirm it against the official spec before
  any 500 kHz use / certification.** (AS923 channel bandwidths confirmed via the
  LoRa Alliance regional parameters.)
- **US / Canada (FCC Part 15.247):** 500 kHz is fine — the digital-modulation
  rule actually favours ≥ 500 kHz. All modes are OK on bandwidth.
- **EU / UK (ETSI EN 300 220):** 868 MHz is narrow-channelized — **500 kHz is
  not permitted**; stay ≤ 250 kHz.

> ⚠️ **Auto mode + Taiwan.** With `AT+MODE=auto` (ADR, **on by default**) the
> link *automatically* climbs into `turbo`/`ludicrous` on a strong (close-range)
> link — i.e. into the wide bandwidths that are **outside** Taiwan's plan. For a
> Taiwan deployment, either pin a compliant mode (`AT+MODE=medium` then `AT&W`)
> or disable the GFSK rung (`AT+ADRGFSK=0`); a built-in region/bandwidth cap
> (keep ADR ≤ 250 kHz) is planned. On a controlled bench it's fine — for field
> use, mind the OBW.

## Hardware notes (Wio-SX1262 on XIAO ESP32S3, B2B connector)

- RadioLib `Module(NSS=41, DIO1=39, RST=42, BUSY=40)`
- SX1262 **DIO2 → RF switch** → `radio.setDio2AsRfSwitch(true)` (skip this and
  RX is deaf / TX dead).
- SX1262 **DIO3 → TCXO @ 1.8 V** → pass `tcxoVoltage = 1.8` to `begin()`.
  The TCXO is required for the narrow BW125 / SF12 link.

## Build & flash

Built with [PlatformIO](https://platformio.org/). The **transparent serial
transport** is the single `node_raw` env. **Flash the same image to both
boards** — the firmware is identical on each; they auto-elect the half-duplex
role from their MACs at boot, so there's nothing per-board to build or set.

There's a standard **Makefile** wrapping the PlatformIO commands:

```bash
# Install PlatformIO if needed:  pipx install platformio   (or: pip install platformio)

make test                     # run the unit/sim test suite (no hardware needed)
make build                    # compile the firmware image
make flash PORT=/dev/ttyACM0  # flash a board (repeat for each board's port)
make flash PORT=/dev/ttyACM1  # the other board — same image
make help                     # list all targets
```

Or call PlatformIO directly:

```bash
pio test -e native          # run the unit/sim test suite (no hardware needed)

# Flash each board. Use the helper, NOT `pio ... -t upload` — see below for why.
./tools/upload_flash.sh node_raw /dev/ttyACM0
./tools/upload_flash.sh node_raw /dev/ttyACM1   # same image, other board
```

Prefer not to build? Grab **prebuilt firmware from the GitHub Releases page** and
flash the `*.factory.bin` at offset `0x0` (web flasher or esptool — see the release
notes).

### Flashing over native USB (`tools/upload_flash.sh`)

The XIAO ESP32S3 flashes over the chip's **native USB**, and esptool's normal
auto-reset toggles DTR/RTS in a way that makes the port re-enumerate mid-connect
— `pio run -t upload` fails with `[Errno 19] No such device`. The BOOT/RESET
buttons that would let you enter the bootloader by hand are **covered by the
Wio-SX1262 shield** on this kit, so they're not an option either.

[`tools/upload_flash.sh`](./tools/upload_flash.sh) works around this with no buttons:

1. **1200-baud touch** — opening the USB-CDC port at 1200 bps makes the Arduino
   USB stack reboot into the ROM USB-Serial/JTAG bootloader.
2. **Flash with esptool** at the ESP32-S3 offsets (`0x0` bootloader, `0x8000`
   partitions, `0xe000` boot_app0, `0x10000` app), `--after hard_reset` to run.
   It tries `default_reset` first (clean once in the JTAG bootloader) and falls
   back to `no_reset`.

Port permissions: the reset cycling re-creates the `/dev/ttyACM*` nodes, which
reverts any `chmod`. Install a udev rule so every XIAO enumerates world-RW. XIAO
boards show up under **two** USB vendor IDs — `303a` (Espressif-native USB) and
`2886` (Seeed, e.g. the newer XIAO-S3 / HaLow variants) — so allow both:

```bash
printf 'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"\nSUBSYSTEM=="tty", ATTRS{idVendor}=="2886", MODE="0666"\n' \
  | sudo tee /etc/udev/rules.d/99-xiao.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### ⚠️ Before transmitting

- **Regulatory (Taiwan NCC LP0002):** band is 920–925 MHz. `FREQ_MHZ` defaults
  to 923.2. **Verify legal TX power and duty cycle before any outdoor/field
  use.** `TX_POWER_DBM` defaults to a low bench value.
- **Duty cycle:** `TX_INTERVAL_MS` (5 s) is *bench-only* and ignores duty cycle.
  At SF12 a small frame is ~1.5 s time-on-air; a 1% duty cap implies ~150 s
  between sends. Fix the cadence before going on-air.
- **Bench safety:** keep the two radios a few cm–metres apart at low power. A hot
  TX into a nearby RX can desensitize or damage the front end.

## Activity LED

The firmware toggles the green LED **on the Wio-SX1262 board** (D1, GPIO48,
active-high) on every TX/RX-complete event — it flickers during traffic, steady
when idle. (Note: it's a plain green LED, not the addressable RGB that GPIO48
usually drives on bare ESP32-S3 devkits.)

## Identity, compression, flow control

The modem has three configurable features, with build-flag defaults that are
overridden by runtime settings stored in NVS (flash). A future WiFi UI would
edit the same settings.

- **Identity / addressing.** Each device has an `addr` (1–254), a paired `peer`
  (0 = accept any), and a `name`. Air frames carry `src`/`dst`; a node ignores
  frames not addressed to it or not from its peer — so two pairs nearby on the
  same frequency/sync word don't confuse each other. Defaults: laptop=`addr 1`,
  device=`addr 2`, paired to each other.
- **Compression.** Per-air-frame heatshrink (LZSS), with a "stored" fallback
  when a block doesn't shrink. Independent per frame, so it's robust to loss.
  Decoding always honors the per-frame flag, so mismatched ends still interoperate.
- **Flow control / overrun safety.** The host can't outrun the link: the board
  absorbs fast input in a multi-MB **PSRAM ingest ring** (plus USB NAK
  back-pressure), so a fast burst (e.g. `cat bigfile`) stays byte-exact even though
  USB delivers ~1 MB/s into a ~KB/s link.

### Config console (runtime settings)

Runtime config is the AT/`+++` command mode described above (`AT+MODE`, `AT+ADDR`,
`AT+PEER`, `AT+NAME`, `AT+FREQ`, `AT+SYNC`, `AT+PWR`, `AT+ENC`, `AT+COMP`; `AT&W` to
persist, `AT&F` to factory-reset, `AT?` for the full list). Settings persist in NVS,
so they **survive reboot and reflash** — set identity/mode once without reflashing.

## Roadmap

1. ✅ Reliable link: Selective-Repeat ARQ + SACK, per-mode BDP window, ToA-tuned timing.
2. ✅ Transparent USB↔LoRa byte pipe (the serial transport) + PSRAM overrun safety.
2c. ✅ Identity/addressing, per-frame compression, flow control, NVS config, AT mode.
3. ✅ **Authenticated encryption** on the air payload — Ascon-128 AEAD + NVS-persisted
   counter (reboot-safe nonce) + anti-replay window. Next crypto step: Noise/X25519
   handshake for forward secrecy (see [SECURITY.md](./docs/SECURITY.md)).
4. ✅ **`auto` mode (ADR)** — `AT+MODE=auto`: SNR-driven mode pick + a coordinated
   make-before-break mode-switch handshake. Sim-tested and hardware-validated.
5. ✅ **GFSK `ludicrous` mode** — opt-in non-LoRa PHY, hardware-validated (the fastest
   mode; dynamically switchable to/from LoRa at runtime).
6. WiFi config UI (edits the same NVS settings).

## Next steps (deferred — the core transport is done)

The reliable, encrypted, mode-flexible transparent serial transport is complete and
hardware-validated — including `auto`/ADR and the GFSK `ludicrous` mode (both done;
see [docs/FUTURE_MODES.md](./docs/FUTURE_MODES.md)). These are the
intentionally-deferred enhancements that remain, in rough priority:

- **Phase 5 — rateless / fountain-coded bulk mode.** Encode bulk transfers as a stream
  of fountain symbols (e.g. LT/RaptorQ) so the receiver reconstructs from *any* sufficient
  subset — no per-frame ACKs. Biggest win on **lossy, long-range, or one-to-many/broadcast**
  links. *Lower priority for our case:* the Selective-Repeat + SACK ARQ is already byte-exact
  through 30 % loss point-to-point, so the marginal gain is modest vs. the added complexity.
  Worth doing if very-marginal links or multicast become a goal.
- **Phase 2 — CAD turn-around + implicit header.** Use Channel-Activity-Detection for the
  RX→TX hand-off and drop the LoRa explicit header on a fixed-format link. Saves a little
  airtime/power; **low ROI** and explored-but-deprioritized (it's LoRa-only and mainly a
  power win — our nodes are USB-powered). ([analysis](./docs/FUTURE_MODES.md))
- **Forward secrecy (crypto).** A Noise/X25519 handshake for per-session keys, so a leaked
  long-term key doesn't expose past/future sessions — and closes the cross-reboot replay
  residual. See [SECURITY.md](./docs/SECURITY.md) (Fix D).
