# Mode-switch mechanism — formal spec

Runtime ADR has two separable parts. This doc specifies the **mechanism** (how
two ends atomically change PHY); the **policy** (when/what to switch — SNR +
retransmit + hysteresis) lives in [`lib/linklayer/adr.h`](../lib/linklayer/adr.h)
and just feeds the mechanism a *desired mode*. The mechanism is the part that was
historically buggy, so it is specified here and **exhaustively model-checked** in
`test/test_link` (see `test_msm_*`).

## The problem

Change the active PHY on **both** ends from mode `M` to mode `N`, over a link
that is:

- **half-duplex** — only one end transmits per turn;
- **mode-deaf** — a frame sent on mode `X` is received *only* if the receiver is
  currently on `X` (LoRa SF/BW mismatch, or LoRa↔GFSK, = total deafness);
- **lossy** — any frame may be dropped;
- **restartable** — either end may reboot at any time;
- subject to a **make-before-break window** — the interval during a switch where
  the two ends are on different modes and therefore cannot hear each other.

Disagreement on the active mode = a dead link. So the mechanism must be correct
*despite* the window, loss, and reboots.

## Carrier

The switch rides the link header's 4-bit **control nibble** (`ll::CTRL_MASK`),
authenticated as AEAD associated data when encryption is on:

- bit `0x10` = ACK flag (1 = ACK of a request, 0 = a request);
- bits `0x20..0x80` = `(mode index + 1)`; `0` = no control this frame.

Only frames that **decode validly** (pass CRC *and*, if encrypted, the AEAD tag,
*and* addressing) are allowed to drive the state machine. RF noise or a
cross-PHY false-detect that passes PHY CRC but fails AEAD must be ignored — it
must NOT clear a probation timer or be read as a control message. (This is the
"valid-RX only" rule; violating it was a real bug.)

## State (per node)

```
current   : the active mode index
prev      : the mode to fall back to if a just-applied switch goes silent
target    : (initiator) the mode we're requesting, or none
armed     : (responder) a mode we've agreed to and will apply after we TX
owe_ack   : (responder) a mode whose ACK we still must send
probation : armed right after applying a switch; cleared by a VALID rx
```

A node is **Busy** while `target`/`armed`/`apply`/`probation` is set.

## Protocol (make-before-break)

1. **Initiator** `Request(N)` → `target=N` (no-op if `N==current` or Busy). It
   stamps `REQ(N)` on every outgoing frame until answered.
2. **Responder** sees a valid `REQ(N)` (N≠current) → `owe_ack=N, armed=N`. It
   stamps `ACK(N)` on its reply **on the OLD mode**, then (after TX) applies `N`
   and arms probation (`prev=M`).
3. **Initiator** sees the valid `ACK(N)` → applies `N`, arms probation
   (`prev=M`).
4. Both are now on `N`. The first valid frame each receives on `N` clears
   probation → **converged**.

## Failure handling

- **REQ lost:** initiator keeps re-sending `REQ(N)`; responder never armed.
  Harmless retry.
- **ACK lost:** responder applied `N` and now hears nothing (initiator still on
  `M`) → **probation reverts** it to `M`. Both back on `M`; initiator retries.
- **Asymmetric apply** (initiator → `N`, responder reverted to `M`, or vice
  versa): the end that is alone on `N` hears nothing → **its probation reverts**
  it. Converges to `M`.
- **Persistent loss** (the handshake can't complete on a too-lossy link): the
  initiator's policy layer **aborts** the request after a timeout and stays on
  the working mode rather than dead-flapping.
- **Reboot / any unhandled desync:** the **dead-link rendezvous** backstop — if
  a node receives no *valid* frame for `T_rz`, both ends independently fall back
  to a fixed known-good mode (`medium`) and re-converge. This is the liveness
  guarantee that holds *even if the handshake has a hole*: the worst case is
  always **bounded recovery**, never permanent deadlock.

## Invariants (what the model-checker asserts)

- **SAFETY:** whenever both ends are *quiescent* (no switch in flight, no
  probation pending), they are on the **same** mode. The ends never durably
  disagree.
- **LIVENESS (bounded recovery):** from *any* reachable state, once the channel
  stops dropping frames, within a bounded number of turns both ends are
  quiescent on the **same** mode and the link delivers data. No interleaving of
  loss / reorder / reboot / deaf-window timing leads to permanent disagreement.

## Test strategy

1. **Model-checker** (`test_msm_*`): two `ModeSwitch` instances over a
   *mode-deaf* channel, with an adversary that may drop each direction's control
   message every round, inject reboots, deliver garbage (valid-CRC but
   AEAD-failing) frames, and issue up/down switch requests. Enumerate the
   interleavings (bounded depth) and assert SAFETY every round + LIVENESS after
   the adversary quiesces. Pure logic, deterministic, offline.
2. **Integration sim** (`test_sim_*`): full nodes (link + mode-switch + ADR +
   rendezvous) over a channel modeling mode-deaf delivery, **SNR-driven loss**,
   **per-mode time-on-air delays**, and the **in-memory ingest backlog**. Vary
   SNR to drive switches up and down on the fly; assert the byte stream is
   delivered **byte-exact** through every switch and that a large backlog drains
   exactly.
