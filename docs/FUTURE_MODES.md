# Advanced modes: `auto` (ADR), GFSK `ludicrous`, and CAD

The status of three advanced features, with the design notes and the engineering
journey behind each. Companion to [ROADMAP.md](./ROADMAP.md) and
[THROUGHPUT.md](./THROUGHPUT.md).

| Feature | Status |
|---|---|
| **`auto` mode (ADR)** | ✅ **Done** — implemented, sim-tested, hardware-validated |
| **GFSK `ludicrous`** | ✅ **Done** — implemented, hardware-validated; the fastest mode |
| **CAD turn-around** | ⏸ Explored, **deliberately deprioritized** (see below) |

**The shared foundation (built):** both ends must always agree on the PHY, so any
runtime mode change (ADR's rate steps, or switching LoRa↔GFSK) needs an in-band,
reliable **mode-switch handshake**. That's implemented: the link-layer header
carries a 4-bit **control nibble** in its spare flag bits (`SetCtrlTx()`/`CtrlRx()`
in `linklayer.h`, authenticated as part of the AEAD header), and the portable
`ll::ModeSwitch` state machine drives REQ → ACK → both-apply → probation
auto-revert on top of it. If a switch fails, both ends fall back to the robust
control config (`kCtrlSf`/`kCtrlBwCode`/`kCtrlCr` = SF12/BW125/CR4-8).

---

## 1. `auto` mode — Adaptive Data Rate (ADR)  ✅ IMPLEMENTED

**Status.** Implemented, unit-tested in the native sim, and hardware-tested. The
decision logic is the portable, unit-tested **ADR controller**
(`lib/linklayer/adr.h`), driven over the **mode-switch handshake**
(`lib/linklayer/modeswitch.h`: REQ → ACK → both apply → probation auto-revert).
Enable with `AT+MODE=auto` on the initiator; it maps measured **SNR + the real
retransmit rate** → fastest safe preset, steps **down eagerly on loss** / **up
with hysteresis**, and (opt-in) gates GFSK behind a strong **RSSI** (not SNR —
LoRa SNR saturates ~+11 dB, so it can't tell a 5 cm link from a marginal one).
With `auto` off, the hot path is byte-identical to before.

> **The mode-coordination bug hunt (a real journey).** Getting runtime mode
> switching reliable was the hardest part of the project, and it taught us that
> *every individual mode works* — the bugs were all in *coordinating a switch*
> between the two ends. In order of discovery and fix:
>
> 1. **`AT+MODE` didn't coordinate the peer.** Setting a mode on the initiator
>    only changed its own radio (`ApplyModeByName` locally), leaving the
>    responder on the old PHY → a deaf link. This masqueraded as "turbo is
>    broken" (we even briefly excluded turbo from `auto`!) — but it was just the
>    initiator on turbo talking to a responder still on another mode. **Fix:**
>    `AT+MODE` now drives the make-before-break handshake so both ends move
>    together. With that, **turbo carries data byte-exact** (64 KB verified) —
>    turbo was never the problem.
> 2. **Every mode switch reset the link + bumped the epoch.** `ApplyModeByIndex`
>    called the full `ApplyLinkConfig` (a `LinkLayer::Init` with a fresh random
>    epoch). The peer saw the epoch change, resynced, and re-ran the session
>    handshake; that churn could revert the switch and dropped in-flight data.
>    **Fix:** a mode change now calls `LinkLayer::SetTiming` (window +
>    retransmit only) — no reset, no epoch bump; in-flight frames just
>    retransmit on the new PHY.
> 3. **The forward-secrecy handshake broke coordination.** The per-session FS
>    handshake (new — see SECURITY.md Fix D) ran with ad-hoc blocking RX
>    *outside* the proven turn rhythm; it often failed to establish and consumed
>    the turns the mode-switch handshake needed, so switches (especially
>    LoRa→GFSK `ludicrous`) reverted. **Fix:** FS is now **opt-in, off by
>    default** (`AT+FS`) until its integration is reworked. With FS off,
>    `ludicrous` coordinates cleanly from an idle link and carries data exact.
> 4. **Mismatch recovery.** If a switch still half-completes under heavy load
>    (an ACK lost → one end moves, the other doesn't), the link could be left
>    mismatched. **Fix:** a **dead-link rendezvous** — if a node hears no
>    *valid* (decoded) frame for ~9 s, both independently fall back to a known-
>    good mode (`medium`) and re-converge. The "valid frame only" gate keeps RF
>    noise (a strong nearby TX on another PHY) from masking a dead link.
>
> Net: **fixed modes are rock-solid** (turbo/medium verified byte-exact, both
> directions). The mode-switch mechanism is now **formally specified**
> ([MODE_SWITCH_SPEC.md](./MODE_SWITCH_SPEC.md)) and **simulation-validated**:
> an exhaustive adversarial model-checker (`test/test_link` `test_msm_*`) proves
> every loss/reboot interleaving re-converges, and an integration + physics sim
> (`test/test_sim`) drives SNR-triggered switches mid-stream byte-exact and
> reproduces per-mode time-on-air. Remaining work is on **hardware**: the
> individual fast modes' real-world throughput/timing (the sim shows turbo *should*
> be ~4.5× medium; hardware currently doesn't match — a radio/timing issue, not the
> link code) and re-tuning the rendezvous so it doesn't pull a pinned fast mode
> back to medium on a lossy link.

**Initiator-driven, responder-follows (and how to read it).** ADR lives entirely
on the **initiator** (the lower-address board — see the README "Roles" section).
Enable it there with `AT+MODE=auto`; on the **responder** it's a no-op (the
responder has no decisions — it just adopts the mode the initiator coordinates over
the air). One consequence trips people up: **only the initiator's `AT+MODE?` shows
the `(auto)` tag.** The responder reports the raw mode it was switched to
(`mode=turbo sf=5 …`, no `(auto)`), because it isn't running the controller. So to
confirm auto is working, check that the **SF/BW matches on both ends**, not that
both say "auto". (A possible nicety: have the handshake carry an "ADR-driven" bit so
the responder could display `(auto:follow)`.)

Remaining work:
- Tune `kAdrMarginDb` / probation / cadence against real RSSI/SNR, and confirm
  the make-before-break timing on hardware.
- **A mode switch currently re-inits the link** (via `ApplyLinkConfig()`), which
  bumps the epoch and **drops any buffered/in-flight bytes** — same as a manual
  `AT+MODE` today. For ADR mid-transfer this is lossy; the refinement is to
  *drain* the send/recv rings (or quiesce) before applying the new PHY.

**Goal.** A mode named `auto` (`AT+MODE=auto`) that picks the **fastest preset
the current link can sustain**, stepping down for range/robustness and up when
there's margin — instead of the user guessing turbo vs. far.

**What we already have.** Everything needed to *measure* the link:
`g_radio.rssi()` (smoothed RSSI) and `radio.getSNR()`, both surfaced via
`AT+LINK?`. The named presets (`kRfModes`) are the rungs of the ladder. Per-mode
timing already auto-derives from time-on-air in `Radio::ApplyMode()`, so switching
rungs "just works" mechanically.

**The decision rule.** Each LoRa SF has a known demodulator floor (the SNR below
which it can't decode); higher SF tolerates lower SNR:

| Mode  | SF | approx. demod SNR floor |
|-------|----|-------------------------|
| turbo | 5  | ~ −5 dB |
| fast  | 7  | ~ −7.5 dB |
| medium| 7  | ~ −7.5 dB |
| slow  | 9  | ~ −12.5 dB |
| far   | 12 | ~ −20 dB |

ADR keeps the measured SNR a **margin** (say 5–6 dB) above the current rung's
floor:
- **Step up** (faster) when SNR has sat comfortably above the *next-faster*
  rung's floor + margin for N consecutive turns.
- **Step down** (more robust) immediately on a burst of CRC failures / ACK
  timeouts, or when SNR drops under the current floor + margin.
- **Hysteresis**: require sustained evidence before stepping up; step down
  eagerly. This avoids oscillation (the classic ADR failure mode).

**Who decides & how it's applied.** The **initiator** owns the decision (it
already drives turns), computes the target rung, and sends a "switch to rung R"
control message that the responder acks before both apply it on the agreed turn —
the handshake described above. The decision can also be advisory-only first
(print the recommendation via `AT+LINK?`) so it's testable without changing PHY.

**Implementation sketch.**
- Add an `auto` entry the AT layer recognizes specially (not a fixed SF/BW row):
  it sets a `g_adr_enabled` flag and seeds from `medium`.
- Add an `AdrTick()` called from `MaybeStatus()` (already on a ~1.5 s cadence)
  that reads SNR/error counters and proposes a rung.
- Reuse the mode-switch handshake to apply changes on both ends.
- Persist "auto" as the chosen mode (the resolved rung is runtime-only).

**Risks.** Mode desync is the big one (mismatched PHY = deaf link) — the
handshake must be robust and fail safe (fall back to the control config on
repeated ack loss). Needs real field iteration to tune the margins/hysteresis;
that's why it's bench-deferred. This is the same controller sketched as the
"ADR safety half / full ADR" steps in the roadmap.

---

## 2. CAD turn-around (Channel Activity Detection)

**Goal.** Replace part of the fixed RX/TX timing with the SX126x's hardware
**CAD** (Channel Activity Detection), and optionally drop the LoRa explicit
header on this fixed-format link — saving airtime and power.

**How it helps.** Today the responder blocks in `RxWithTimeout()` for a listen
window sized to a full frame's ToA. CAD lets the radio cheaply sniff for a LoRa
preamble in a few symbols and only commit to a full receive when activity is
detected — less time with the receiver fully on (power), and a tighter, more
deterministic RX→TX hand-off. Dropping the explicit header (implicit-header mode)
saves a handful of symbols per frame, which matters most at high SF where every
symbol is expensive.

**RadioLib hooks.** `radio.scanChannel()` (one-shot CAD) or
`startChannelScan()` + a CAD-done DIO1 action (`setDio1Action`) for the
non-blocking style we already use for TX/RX-done. CAD parameters
(`setCadParams`: symbol count, det-peak/min) need tuning per SF. Implicit header
is `radio.implicitHeader(len)` with a fixed payload length (our frames aren't
fixed-length, so this needs a length convention or per-mode fixed sizing).

**Tradeoffs / why deferred (and now deprioritized).** Modest ROI for real
complexity: CAD parameters are finicky to tune (false negatives drop frames;
false positives waste time), and implicit header forces a fixed frame length that
fights our variable-length SACK/AEAD frames. The current serviced RX wait is
simple and already byte-exact. Three reasons it's the *lowest* priority of the
three after the GFSK work:
1. **It's LoRa-only.** CAD detects LoRa preambles/chirps — it does nothing for
   GFSK, so it can't help the `ludicrous` overhead problem (the one place we most
   want to cut turn-around cost).
2. **Its payoff is mainly power** (sniff-and-sleep instead of a long blocking
   listen), but our nodes are **USB-powered** (bench + the NanoPi deployment), so
   that benefit is largely moot here.
3. **It risks the link** — a mis-tuned `setCadParams` drops frames and can wedge a
   turn, for a marginal latency/power gain.
So CAD stays designed-but-unbuilt: revisit it only for a **battery-powered**
node, where the sleep-between-sniffs power saving actually matters.

---

## 3. GFSK "ludicrous" modulation  ✅ IMPLEMENTED & HARDWARE-VALIDATED (fastest mode)

`AT+MODE=ludicrous` switches the SX1262 into **GFSK** via `radio.beginFSK()`; the
portable link layer rides on top unchanged. It's **hardware-validated** byte-exact,
dynamically switchable to/from LoRa at runtime, and — after the journey below — it
is now **the fastest mode**, ~**12 KB/s** at 200 kbps vs turbo's ~8 KB/s.

### The journey (why the first cut was *slower* than turbo, and the fix)

1. **First bring-up: byte-exact but only ~4 KB/s — *slower* than turbo.** Despite
   GFSK's 3× higher raw bitrate (200 kbps vs turbo's ~62.5), end-to-end was worse.
   A first (misleading) run showed 10 KB/s, but that was an artifact — it measured
   against data left buffered in the PSRAM ingest ring from a previous stalled run.
   The honest re-measure was ~4 KB/s.
2. **Measure, don't guess.** Added `tx`/`retx` counters to the link layer (exposed
   via `AT+LINK?`). The data: **~15% of frames were being retransmitted.** GFSK's
   frames are tiny (~10 ms), so the receiver was **missing back-to-back frames
   while re-arming** its receiver between them — real, recoverable loss.
3. **The real culprit was the *cost* of each loss, not the loss itself.** A lost
   frame was only resent after `retransmit_ms` (≈ **520 ms** for GFSK) — ~50× a
   frame's airtime. So every dropped frame stalled the whole window for half a
   second.
4. **Fix — fast retransmit on SACK.** The SACK bitmap already tells the sender
   *exactly* which frames the peer skipped. So instead of waiting out the timeout,
   resend a gap **immediately** on the next turn when a later frame is already
   acked (standard TCP-style fast retransmit). Implemented in `AckProcess()` +
   `NextTx()`, unit-tested (`test_fast_retransmit` proves recovery with the
   timeout set to never fire). **This is a win for every lossy mode, not just GFSK.**
5. **Result: ~4 → ~12 KB/s, byte-exact, sustained** (verified to 64 KB). GFSK now
   beats turbo and earns the name. The ~15% loss is still there but each gap costs
   one turn instead of half a second.

### Things tried, and what's left

- **TX pacing — tried, didn't pan out (reverted, kept as an off-by-default knob).**
  The theory: a few-ms gap after each GFSK frame lets the peer re-arm its receiver,
  cutting the ~15% re-arm loss. In practice, at cm-range bench it *destabilized*
  the link (tipped it into loss/retransmit storms) instead of helping, and
  fast-retransmit already makes the residual loss cheap. So `kFskTxPaceMs` defaults
  to **0**; it's left as a tunable for future field experiments at realistic range.
- **Auto-power vs. GFSK — fixed.** The RSSI-based auto-power control was the main
  source of GFSK flakiness at close range: GFSK has no processing gain, so the
  control nudging power around could tip it below margin (or into saturation) and
  start a loss storm. Auto-power now **skips GFSK entirely**
  (`g_radio.phy_fsk()` guard in
  `AdjustTxPower`) — GFSK is a short-range, max-speed mode and holds a fixed power.
  (A separate close-range gotcha remains for *LoRa*: switching to turbo/SF5 after
  auto-power has floored the power can dip below SF5's demod threshold — a future
  refinement is SNR-margin-aware auto-power.)
- **MTU is *not* a lever** (course-correction from an earlier note): LoRa is
  hard-capped at 255 B and we're already at `MAXPAY=222`; RadioLib's FSK packet
  length is also 8-bit (≤255), so a bigger GFSK MTU would need an infinite-length
  PHY mode RadioLib doesn't cleanly expose *plus* a rework of the ≤255-assuming
  link layer. Not worth it.
- **Still open:** the ~15% re-arm loss is *recovered* (cheap via fast-retransmit)
  but not *prevented*; a faster RX re-arm path could reduce it. And GFSK should be
  field-tested at realistic short range rather than cm-range bench (where it's RF-
  sensitive). Net today: stable ~12 KB/s, the fastest mode.

It ships as an **experimental, opt-in** mode (set manually on both ends; not in the
ADR ladder, since GFSK has no spreading gain and is short-range only).

**Name.** The GFSK rung is called **`ludicrous`** (`AT+MODE=ludicrous`) — it only
works when the two ends are practically on top of each other, and it goes as fast
as the radio physically can. (Spaceballs "ludicrous speed" / Tesla "Ludicrous
mode".) It sits one step *above* `turbo` in the ladder:
`far → slow → medium → fast → turbo → ludicrous`.

**Goal.** Expose the SX126x's **(G)FSK** modulation for a much higher *raw*
bitrate at short range — beyond even the LoRa `turbo` (SF5/BW500) rung.

**The physics.** LoRa trades data rate for processing gain (chirp spread
spectrum) → great sensitivity/range at low rates. FSK has **no** spreading gain,
so it's far less sensitive (shorter range) but can run **tens to hundreds of
kbit/s**. For two boards close together (bench, same room, short hop) that's a
big speed win the LoRa modes can't reach.

**RadioLib hooks.** The SX1262 driver has a full FSK API: `radio.beginFSK(freq,
br, freqDev, rxBw, power, preambleLen, tcxo)`, `setBitRate()`, `setFrequencyDeviation()`,
`setRxBandwidth()`, `setDataShaping()` (the "G" Gaussian filter). The
packet/IRQ model (`startTransmit`/`startReceive`/DIO1-done) is the same as LoRa,
so our `TxFrame`/`RxWithTimeout` structure and the whole link layer
(ARQ/SACK/AEAD/compression) sit on top **unchanged** — only the PHY setup differs.

**How it'd slot in.** A new branch in the radio setup (the `ludicrous` mode) that
calls `beginFSK(...)` instead of the LoRa `begin(...)`, plus GFSK-appropriate
timing. Because the link layer is PHY-agnostic, the byte-pipe semantics are
identical.

**Dynamic LoRa ↔ GFSK switching.** Yes — switching *modulation* at runtime works
through exactly the same mode-switch handshake as ADR (the control nibble already
added to the link-layer header). GFSK is just another entry in the mode list; the
handshake says "both switch to mode X" without caring whether X is a LoRa preset or
GFSK. So you could `AT+MODE=ludicrous` manually, or have `auto` drop into it — same
machinery.

**Where GFSK sits in the ladder (and why not the lower modes).** GFSK has **no
spreading gain**, so it is *only* sensible as a **short-range, high-speed top rung**
— effectively a step *above* `turbo`. It makes **no sense for the lower modes**
(`medium`/`slow`/`far`): those exist to buy range via LoRa's processing gain,
which GFSK simply doesn't have. So the model is: LoRa presets cover the whole
range/speed curve; GFSK is an optional extra rung at the very top for "we're close,
go as fast as possible."

**Opt-in, including inside `auto`.** GFSK must be **off by default** and explicitly
enabled, for three reasons: it has the shortest range, its regulatory treatment
differs from LoRa, and a user may simply not want modulation changing under them.
Concretely:
- **Manual:** `AT+MODE=ludicrous` selects it directly (short range only).
- **In `auto`:** the ADR ladder is LoRa-only **unless** the user opts in (e.g.
  `AT+ADR=gfsk` / a `use_gfsk` flag persisted in NVS). When enabled, GFSK becomes
  the top rung the controller can step up into when the link is very strong; when
  disabled, `auto` never leaves LoRa. The auto controller already works on a
  configurable *set of candidate rungs*, so GFSK is just an optional member of that
  set.

**Tradeoffs.** Short range is the cost — GFSK is a "fast when close" option, not a
general improvement, and the LoRa modes already span the whole speed/range curve.
It's a second PHY to maintain, and (see the journey above) it's RF-sensitive at
extreme close range. That's why it ships opt-in and isn't in the ADR ladder.

---

## What's left

Of the three, only **CAD** remains unbuilt, and it's deliberately deprioritized
(LoRa-only, mainly a power win, our nodes are USB-powered — see its section above).
The optional follow-ups on the two shipped features:
- **`auto`/ADR:** tune the SNR margins/hysteresis in the field; make auto-power
  SNR-margin-aware per mode (the close-range gotcha above); optionally let `auto`
  opt into GFSK as a top rung.
- **GFSK `ludicrous`:** reduce the residual re-arm frame loss (TX pacing — under
  evaluation), and field-test at realistic short range rather than cm-range bench.
