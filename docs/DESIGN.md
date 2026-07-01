# Transport design — a reliable 2-node serial link over LoRa

This is the design for the core of the project: a **rock-solid, transparent, point-to-point
serial pipe over LoRa**. Everything else (an OS login via `agetty`, file transfer,
remote consoles for serial devices) is just an existing tool riding on top of the
pipe — we don't ship those, the plain serial port lets you use the ones you have.

It is deliberately **not** a mesh. That narrowness is the whole advantage — see
[ALTERNATIVES.md](./ALTERNATIVES.md) for how this lets us beat general-purpose
stacks (Reticulum/Meshtastic) at the specific 2-node job.

> **Status: this design is implemented and hardware-validated.** Selective-Repeat
> ARQ + aggregated SACK + BDP-sized window, signaled (F_MORE) hand-off, per-mode
> ToA-derived timing, Ascon-128 AEAD, named modes, and — most recently — a
> coordinated runtime **mode-switch handshake** with **loss-aware ADR** (`auto`).
> The sections below are the design rationale; ✅ marks what shipped. The live
> frame format and code are in [`lib/linklayer/linklayer.h`](../lib/linklayer/linklayer.h).

## Layers (kept clean and separate)
```
  App      | login/shell, file transfer, anything that speaks a serial port
  ---------|--------------------------------------------------------------
  Data link| THIS doc: reliable byte stream, framing+CRC, ARQ, half-duplex MAC
  ---------|--------------------------------------------------------------
  PHY      | SX1262 LoRa: packets only (modulation, hardware CRC)
```
The data-link layer is **byte-transparent** — no CR/NL or terminal semantics ever
leak into it. Those live in the app layer (the pty).

## Background: the first cut (now replaced)
The first transport was a rigid **initiator-polls-responder** turn-taking +
**Go-Back-N**, window 4, cumulative ack. It had three failure modes, all since
fixed by the design below:
1. **Window ≪ bandwidth-delay product (BDP)** → the sender drains its window and
   *stalls* waiting for an ack every round-trip. (This is the multi-window stall on
   full-size frames.)
2. **Go-Back-N** → one lost frame forces resending the whole window — wasted airtime.
3. **Fixed turn timeouts** → the initiator can abandon a long burst mid-way, or
   deadlock on a missed timer.

## Design: Pipelined TDD with aggregated Selective-Repeat
A lean, deterministic transport specialized for two nodes. Six pieces (1–4 are the
reliable byte pipe; 5–6 are runtime rate adaptation):

### 1. Asymmetric, traffic-sized turns
Not equal initiator/responder turns. The **data-heavy** side gets a long transmit burst;
the reverse side gets a tiny turn for an ack only. A download = sender pushes a big
burst, receiver replies with one compact SACK. No full turns wasted on the idle
direction. (Internally one node still owns "who starts," but turns are sized by who
has data, not by role.)

### 2. Window = bandwidth-delay product
Burst length `N = ceil(BDP / frame_payload)`, where `BDP = goodput × RTT` and
`RTT ≈ burst_airtime + turnaround + sack_airtime`. Large enough that the sender
transmits **continuously** through a whole burst without waiting. **This is the
direct fix for the stall** — the pipe never empties. `N` is derived per radio mode
from `getTimeOnAir()`, so it's correct at every SF/BW automatically.

### 3. Selective-Repeat + aggregated SACK bitmap
The receiver acks an entire burst with a **cumulative ack + a 16-bit SACK bitmap**
of which frames above the ack base arrived. These ride in the *header* of the
reverse-direction frame (no dedicated SACK frame), so the sender resends **only**
the gaps and then continues with new frames. One loss ≠ resend the window, and ack
airtime is amortized over the whole burst. A clean run carries the ack with zero
extra bytes. **Fast retransmit:** if a later frame is SACKed while an earlier one
isn't, the gap is resent immediately rather than waiting for the retransmit timer —
this was the key fix that made GFSK the fastest mode.

### 4. Signaled turn-around (not timed)
Every frame but the last in a burst sets `F_MORE`; the frame that clears `F_MORE`
ends the burst, so the receiver fires its SACK **immediately** (pre-armed RX→TX)
and the sender is already listening. This removes both the fixed-timeout dead time
and the "abandon the burst halfway" failure. A safety timeout still exists, but the
*normal* path is event-driven.

### 5. Adaptive data rate (`auto` / ADR) — ✅ implemented
The `auto` mode lets the link pick its own PHY at runtime instead of being pinned
to one named mode. Two mechanisms make this safe:

**a) The coordinated mode-switch handshake** (`lib/linklayer/modeswitch.h`).
Both ends must always agree on the PHY or the link goes deaf, so changing it is a
*make-before-break* protocol carried on the 4-bit control nibble:
- the **initiator** proposes a target mode (REQ) on every frame until answered;
- the **responder** sees the REQ, sends an ACK **on the old PHY**, then switches;
- the **initiator** switches the moment it receives that ACK;
- both arm a **probation timer** on the new PHY — if no frame is heard before it
  elapses (e.g. the ACK was lost so only one side moved), they **revert to the
  previous mode**. Because both revert to the *same* previous mode, they always
  re-converge. A persistent failure (ACK never gets through on a lossy link)
  triggers **`Abort()`**, which parks the link on the working-but-lossy mode
  rather than dead-flapping. A final **dead-link rendezvous** backstop (firmware
  `MaybeRendezvous`): if either end hears no *valid* frame for ~9 s, both fall
  back to a fixed known-good mode and re-converge — the guaranteed recovery from
  any half-completed switch or reboot.

  This is a distributed atomic-reconfiguration problem (not a LoRa-ADR one), so
  it is **formally specified** in [MODE_SWITCH_SPEC.md](./MODE_SWITCH_SPEC.md) and
  **exhaustively model-checked** in `test/test_link` (`test_msm_*`): all 4096 loss
  patterns × {up, down, GFSK, double-switch, reboot} re-converge to a single
  agreed mode, with a teeth test proving the rendezvous is load-bearing. An
  integration sim (`test/test_sim`) then drives SNR up/down to switch modes
  mid-stream and proves the byte stream survives every switch byte-exact.

**b) The loss-aware ADR decision** (initiator-only; the portable, unit-tested
`lib/linklayer/adr.h`, wired up by `Device::DriveAdr()` glue in `fw_device.cpp`).
The initiator owns the decision; the responder just follows the handshake. It is
**loss-aware, not SNR-only** — bench testing showed SNR can look fine while a mode
is structurally lossy (re-arm misses on short frames), so the controller leans on
the measured **retransmit rate**:
- **step down** to a more robust mode when retx% crosses a high-water mark;
- **step up** only when SNR clears the target mode's demod floor *and* recent retx
  is low, and only after the reading is stable for several cycles (hysteresis, so
  it doesn't oscillate);
- a **cooldown** after each switch, and a timeout that aborts a switch that can't
  complete. See [FUTURE_MODES.md](./FUTURE_MODES.md) for the tuning constants.

**Roles:** "initiator" vs "responder" is purely *who drives ADR and starts turns*
— it is **not** client/server and not visible to the app. Either board can be
either; the role is **auto-elected at boot from each chip's factory MAC** (the
numerically lower MAC initiates and takes link address 1, the other takes 2 — so
the two addresses are always distinct, which the AEAD nonce requires). Identical
firmware runs on both boards with no per-board configuration; a short cleartext
MAC-beacon exchange at startup breaks the half-duplex symmetry. Query the live
PHY on *either* end with `AT+MODE?`; the responder reports the concrete mode it
has been switched to (it does not echo `auto`, since only the initiator runs the
controller).

### 6. GFSK `ludicrous` — a second PHY under the same link layer
`ludicrous` is not another LoRa preset — it switches the SX1262 to **GFSK**
modulation entirely. It is the fastest rung (~12 KB/s on the bench, beating
`turbo`) but the least robust, so in `auto` it is gated behind a high SNR and low
retx and is only entered from `turbo`. Because GFSK and LoRa are different PHYs,
entering/leaving it is just another target of the same mode-switch handshake — the
link layer above (ARQ, SACK, AEAD, framing) is identical on both PHYs. GFSK in
`auto` is opt-in via `AT+ADRGFSK=1` (off by default in the shipped firmware). The
throughput journey that got GFSK from below-`turbo` to fastest-rung (fast
retransmit on SACK gaps was the key fix) is written up in
[FUTURE_MODES.md](./FUTURE_MODES.md).

## Frame format (as implemented — see `lib/linklayer/linklayer.h`)
The fixed header is a few bytes; SACK info rides in the header (not a separate
frame), so a data frame and its acknowledgement of the reverse direction are the
same frame:
```
[src][dst][flags][seq][ack][epoch][sackHi][sackLo][len][payload...]
  flags (low nibble): F_DATA 0x01, F_COMP 0x02, F_ENC 0x04, F_MORE 0x08
                      F_MORE = more frames follow in this burst (last frame clears it)
  flags (high nibble = CTRL_MASK 0xF0): the mode-switch / ADR control channel
                      bit 0x10 = ACK-of-request; bits 0x20-0x80 = (mode index + 1)
  seq/ack : 8-bit data sequence + cumulative ack (next seq expected) — Selective-Repeat
  sackHi/sackLo : 16-bit SACK bitmap of frames buffered above the ack base
  epoch   : boot id; changes on restart/(re)pairing so the peer resyncs and stale
            frames from a previous link are dropped
```
When `F_ENC` is set, **Ascon-128 AEAD** wraps the payload: an 8-byte monotonic
counter (big-endian) follows the 9-byte header as both nonce and replay id, then
the ciphertext, then the auth tag (8 or 16 bytes). The 9-byte header **and** the
counter are the AEAD associated data, so seq/ack/flags/counter can't be rewritten
without failing the tag check. Optional compression (`F_COMP`) sits between the
byte stream and the framer (compress-then-encrypt). The control nibble lives in
the header either way, so the mode-switch handshake is authenticated whether or
not encryption is on, and unset control bits leave frames byte-identical to before
the feature existed.

## Why this is better for *our* niche
- **Faster:** pipe stays full (BDP window) + amortized acks + zero contention
  overhead (no CSMA backoff needed for 2 nodes) → closer to the PHY ceiling than a
  general mesh stack gets for a 2-node link.
- **More robust:** Selective-Repeat survives loss without airtime blowup; signaled
  hand-off can't deadlock on a missed timer.
- **Lean:** no addressing/routing/contention machinery — the entire MAC is the
  `clear-F_MORE → SACK` hand-off.

We do **not** beat the LoRa PHY ceiling (nobody does). We get *closer* to it for the
2-node case than general stacks, and we stay simple.

## Config (mirroring the clean RNode breakdown)
A mode is just a named preset over the standard radio knobs:
```
frequency      Hz
bandwidth      125000 / 250000 / 500000
spreadingfactor 5..12   (5 fastest, 12 longest range)
codingrate     5..8     (4/5 fastest, 4/8 most robust)
txpower        dBm
```
Named LoRa modes (each with timing/window derived from its ToA, bench-tested):
`turbo` (SF5/500), `fast` (SF7/500), `medium` (SF7/250), `slow` (SF9/125),
`far` (SF12/125), plus the GFSK `ludicrous` PHY. See [THROUGHPUT.md](./THROUGHPUT.md).

## Firmware structure (the glue around the link layer)
The portable link layer (`lib/linklayer/`) knows nothing about the radio or the
host; the ESP32 firmware wires it to real hardware. That glue is split into four
small classes, each instantiated **exactly once as a static singleton** (`g_*`)
— no heap, no `new`, in keeping with the project's determinism/no-fragmentation
rule. Each owns its former free-floating globals as private members:

- **`Radio` (`g_radio`, `fw_radio.{h,cpp}`)** — everything SX1262: LED, the DIO1
  interrupt service routine, the interrupt-driven RX task, the mode table and
  live `ApplyMode` (SF/BW/CR switching), `Tx`/`Rx`, and the per-mode derived
  timing exposed through accessors (`toa_ms()`, `interframe_ms()`,
  `turn_rx_ms()`, `retransmit_ms()`, `listen_ms()`, `window()`, `tx_power()`,
  `phy_fsk()`, `rssi()`).
- **`Host` (`g_host`, `fw_host.{h,cpp}`)** — the transparent USB↔link data
  plane: the non-blocking host-TX ring, the PSRAM ingest ring, the `+++` AT
  command parser (`AT+SPEEDTEST`/`SINK`, X25519 `AT+TRAIN` pairing, auto-power),
  and **all NVS persistence** (`LoadSettings`/`SaveSettings`, frame-counter
  checkpoint `PersistTxCtr`).
- **`Diag` (`g_diag`, `fw_diag.{h,cpp}`)** — the software watchdog + IDF task
  WDT, RTC crash breadcrumbs, and the `AT+DIAG` report (`Init`/`Pet`/`Report`/
  `RebootNoProgress`).
- **`Device` (`g_device`, `fw_device.{h,cpp}`)** — the orchestrator that owns
  `Setup()`/`Loop()`: MAC-based role discovery, first-boot proximity pairing,
  the initiator/responder turn engine, the layered recovery (radio-reinit <
  rendezvous < reboot), and initiator-driven ADR.

`main.cpp` is then just the **composition root** (~54 lines): it defines the
shared globals once and provides `setup()`/`loop()`, which forward straight to
`g_device.Setup()`/`g_device.Loop()`.

## Implementation status
All of the original plan has shipped; what's left is tuning and stretch goals.
1. ✅ **SACK + BDP window + signaled (`F_MORE`) hand-off** — Selective-Repeat
   replaced Go-Back-N; killed the stall and made bulk transfer reliable.
2. ✅ **Aggregated SACK bitmap** in-header + retransmit-only-gaps, plus
   fast-retransmit on a SACKed-past gap.
3. ✅ **Per-mode window** sized from `getTimeOnAir()` (BDP); turn timing
   auto-derives from each mode's time-on-air.
4. ✅ **Named modes** + per-mode timing — `turbo/fast/medium/slow/far` over SF/BW/CR
   plus the GFSK `ludicrous` PHY, switchable at runtime via `AT+MODE=<name>`
   (persist with `AT&W`).
5. ✅ **Transparent symmetric modem** — unified firmware (role chosen from address
   at runtime; interchangeable boards) + **AT/`+++` command mode** for out-of-band
   config and link-state query. *DCD link-state was found infeasible:* the
   Arduino/TinyUSB CDC stack can read host line-state (DTR/RTS) but exposes no API
   to drive carrier-detect device→host. `AT+LINK?` (RSSI/SNR/power/tx-queue) covers
   the same need better — works over any plain terminal, no extra wires.
6. ✅ **Link adaptation / ADR** — the `auto` mode: coordinated mode-switch handshake
   + loss-aware initiator-driven ADR + GFSK opt-in (§5–6 above). Crash/heap/watchdog
   diagnostics also shipped — see [DIAGNOSTICS.md](./DIAGNOSTICS.md) and
   [DEBUGGING.md](./DEBUGGING.md).
7. (Later) ADR tuning at real range; CAD-based turn-around; optional CSMA mode
   *iff* we ever go >2 nodes. See [FUTURE_MODES.md](./FUTURE_MODES.md).

## Honest caveats
- The protocol is unit-tested in the native sim (loss injection, reordering, BDP
  windowing, and the mode-switch/abort choreography) before every hardware flash —
  but **the sim can't model real RF loss** (re-arm misses, interference,
  saturation); those only show up on hardware, which is where the loss-aware ADR
  tuning came from.
- The PHY still caps absolute throughput.
- All of this is point-to-point. Multi-node is explicitly out of scope (that's where
  you'd use Reticulum/Meshtastic instead).
