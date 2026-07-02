# Interrupt-driven RX — design

Interrupt-driven receive is the firmware's **shipping RX path**: the SX1262's
DIO1 interrupt drives a high-priority task that drains the RX FIFO into a
lock-free ring, and the main loop consumes it. It is **unconditional** (no build
flag) and hardware-validated **byte-exact across every mode**, GFSK `ludicrous`
included. This document is the design as it ships; the long road to it — a
"responder goes deaf" chase that turned out to be a chip errata plus a loose
board-to-board connector, *not* this architecture — is in
[CAPABILITIES_JOURNEY.md](./CAPABILITIES_JOURNEY.md) entries 20, 22–23, and
28–29.

## Why interrupt-driven

- The SX1262 RX FIFO is **256 bytes** and our `MAXFRAME` is **255**, so only
  **one** near-MTU frame fits at a time — each frame must be read out before the
  next arrives.
- The **responder** is the heavy-RX side (it receives the data burst; the
  initiator gets only sparse ACKs), so under a back-to-back burst it must drain
  each frame promptly or the next overruns the FIFO.
- Draining on the **DIO1 interrupt** — rather than polling the FIFO from the
  main loop, which can lag a frame under load — keeps up with the burst and lets
  the radio stay in **continuous receive** with no per-frame turn-around wait,
  so there is no re-arm "deaf window" between frames. This is the same
  interrupt-driven, continuously-receiving shape Meshtastic (`SX126xInterface`)
  and RNode (`sx126x.cpp`) use.

## Architecture

```
 DIO1 ISR (IRAM, minimal)      radio task (high prio)        main loop (loopTask)
 -----------------------       ----------------------        --------------------
 RX/TX-done edge:              ulTaskNotifyTake (block)      turn engine, link,
   operation_done_ = true      Lock(radio_mutex_)            host I/O (unchanged)
   vTaskNotifyGiveFromISR -->    if RX_DONE:                 Rx() pops rx_ring_ -->
   portYIELD_FROM_ISR              readData() -> rx_ring_       g_link.OnRx() ...
                                   standby()+startReceive()
                                Unlock(radio_mutex_)
```

- **DIO1 ISR** (`Radio::OnDio1`, IRAM): the one edge fires for both RX-done and
  TX-done. It stays minimal and IRAM-safe — it sets `operation_done_` (which
  `Tx()` watches for TX-done) and wakes the radio task with
  `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR`. **No SPI and no
  flash-resident calls** (e.g. `digitalWrite`) run here: under heavy interrupt
  load that crashes the chip, which is why the activity LED is toggled from the
  main loop, not the ISR.
- **Radio task** (`Radio::RadioTask`, high priority): blocks on the
  notification; on wake it takes `radio_mutex_`, and if the IRQ flags show
  `RX_DONE` (a TX-done edge just wakes it to a no-op) it `readData()`s the frame
  into the **SPSC frame ring** (`rx_ring_`), caches the packet RSSI (EMA) and
  SNR under the lock, then **re-arms** receive — a quick `standby()` →
  `startReceive()` that also resets the AGC between frames (cheap insurance
  against the SX1262 close-range desense errata). The re-arm runs **after** a
  completed `RX_DONE`, so it never aborts an in-flight frame, and it carries
  **no delay** (see the rule below). The task never touches `g_link` — link
  state stays single-threaded in the main loop.
- **Main loop** (`Radio::Rx`): where it needs a frame it **pops** `rx_ring_`
  (the same timeout semantics the old polling `Rx` had) and feeds it to
  `g_link.OnRx()`. TX is unchanged and stays in the main loop (`Tx`).
- **`radio_mutex_`** (recursive): the one shared hazard is concurrent SPI to the
  SX1262. The radio task holds it for `readData` + re-arm; the main loop holds
  it for `Tx`/`startReceive`/`standby`/`reset`. Mutual exclusion means no
  concurrent SPI, so no half-driven transaction can wedge the chip.

## The load-bearing rule: keep the FIFO-drain hot path delay-free

**Never put a per-frame delay or yield in the radio task's drain path.** The
fast back-to-back-burst PHYs — GFSK `ludicrous` and LoRa `turbo` — fire
`RX_DONE` on almost every wake, so a `vTaskDelay(1)` after each processed frame
inserts a scheduling gap *between* burst frames; the task can no longer drain +
re-arm ahead of the next frame, and real frames are dropped or misframed → the
link goes deaf. (This exact throttle, added while chasing an unrelated bug,
silently broke `ludicrous` — see JOURNEY entry 29.)

Loop-starvation protection is therefore confined to a **genuine deaf-radio
backstop**, not a per-frame throttle: a successful frame resets a
consecutive-failure counter, so ordinary GFSK noise (which mostly passes CRC)
never trips it. Only after **many consecutive failed reads** (`kRxStormFails`)
does the task briefly halt RX (`kRxStormBackoffMs`) to give the main loop an
uncontested window for its stuck-radio recovery, then re-arm. The loop stays fed
through its own `Rx()` pop path (which pets the watchdog), never a yield in the
task.

## Thread-safety summary

| Shared thing | Producer | Consumer | Protection |
|--------------|----------|----------|------------|
| radio (SPI)  | radio task (read/re-arm) + main loop (tx/arm) | — | `radio_mutex_` (recursive) |
| `rx_ring_`   | radio task | main loop (`Rx`) | lock-free **SPSC ring** (`frame_ring.h`) |
| `g_link`     | main loop only | main loop only | none needed (single-threaded) |

The SPSC ring is single-producer (the radio task) / single-consumer (the main
loop), so it needs no lock of its own; a full ring simply **drops** the frame
and the ARQ layer retransmits it. `Radio::rssi_ema_` / `last_snr_` are written
by the task and read via `g_radio.rssi()` / `snr()` in the loop — plain word
writes, benign races like `operation_done_`.

## Testing

- **Native (host PC):** the SPSC ring logic is unit-tested in `test/test_link/`
  — `test_frame_ring_basic` (FIFO order + byte-exact),
  `test_frame_ring_full_drops` (overflow drops, no corruption),
  `test_frame_ring_wrap_byte_exact` (index wrap), and
  `test_frame_ring_oversize_rejected`. The integration sim (`test/test_sim/`)
  also models the **RX re-arm deaf window** and the **SX1262 AGC lockup** a
  never-standby receiver would hit, so the "re-arm each frame" choice is
  exercised in sim.
- **Hardware:** the FreeRTOS task + ISR timing, real FIFO behaviour, and the
  TX/RX SPI mutex are validated with `AT+SPEEDTEST` per mode plus a sustained
  flood soak — byte-exact with no hang or deafness in **every** mode, GFSK
  included.
- **Backstop:** the loop task watchdog and the no-progress reboot remain the
  last-resort recovery, so any regression self-recovers rather than needing a
  manual reflash.
