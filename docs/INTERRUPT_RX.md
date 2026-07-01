# Interrupt-driven RX — design

**Goal:** drain the SX1262 RX FIFO *promptly* on the DIO1 interrupt so back-to-back
burst frames don't overrun it — which makes **continuous RX** safe, which kills the
per-frame re-arm deaf window, which is the only way to reach the README throughput.

**Status:** implemented behind `INTR_RX` (default OFF); per-frame polling ships.

## Update 2026-06-30 — research-validated, but two paths now exist

Research into Meshtastic (`SX126xInterface`/`RadioLibInterface`) and RNode
(`sx126x.cpp`) **confirms this architecture is the proven one**: both run
**continuous RX (`RX_TIMEOUT_INF`)**, **interrupt-driven**, re-armed in a worker
(Meshtastic) / left continuous (RNode), with the ISR doing only a notify. So the
radio-task design below is correct — the responder "hang" we chased was **not** the
task: it was a **wedged SX1262** (`CHIP_NOT_FOUND`) caused by calling RadioLib
`resetAGC()` (warm `sleep()`) in the RX path. See CAPABILITIES_JOURNEY 22.

Two receive paths now coexist, both flag-gated, both **default OFF** (per-frame
ships):

1. **`RX_CONTINUOUS`** — *main-loop* continuous RX: re-arm `standby()`+`startReceive()`
   **before** the caller processes the frame, so the radio listens during `OnRx`.
   Single-threaded (no task, no mutex), the **simplest** way to shrink the deaf
   window. Test this first — lowest risk.
2. **`INTR_RX`** — the radio-task design in this doc (ISR → task → SPSC ring →
   main loop). Higher performance under burst (prompt FIFO drain), more moving
   parts. The proven Meshtastic/RNode shape.

Both are **UNVALIDATED on hardware** (the wedge invalidated the earlier tests).
Lessons folded in for whoever validates:
- **Do NOT add `resetAGC()` blind.** If desense recovery is needed, use Meshtastic's
  *guarded* recipe (never mid-packet, re-apply reg `0x08B5` bit 0 after calibrate) —
  see docs/RADIO_ERRATA.md. Our close-range desense is better fixed by **lower TX
  power** (CAPABILITIES_JOURNEY 21).
- **Add a missed-IRQ poll backup** (Meshtastic `pollMissedIrqs`/`checkRxDoneIrqFlag`):
  DIO1 is edge-triggered and the ESP32 can drop edges. The per-frame path already
  polls `RADIOLIB_IRQ_RX_DONE` every `kIrqPollMs`; the `INTR_RX` task's `kRxRearmMs`
  wakeup is a coarse equivalent — make it finer if frames are missed.
- Keep the **reboot watchdog** as the last-resort recovery (a `sleep()`-wedged chip
  only recovers on power-cycle/reboot).

## Why the current polling RX wedges (the responder)

- The SX1262 RX FIFO is **256 bytes**; our `MAXFRAME` is **255** → only **one**
  near-MTU frame fits at a time.
- The **responder** is the heavy-RX side (it receives the data burst; the initiator
  only gets sparse ACKs). Under a back-to-back burst it must read each frame out of
  the FIFO before the next arrives.
- Today RX is **polling**: `RxWithTimeout()` checks `operation_done` (set by the DIO1
  ISR) in a `delay(1)` loop, then `readData()`, then the caller decrypts (`OnRx`),
  then loops. Under load the read can lag a frame → the next frame overruns the FIFO
  → the radio/loop wedges. Always the responder, never the initiator (sparse RX).
- Per-frame standby (current default) avoids overrun because the standby+re-arm
  resets the FIFO each frame and the turn cadence gives time — at the cost of the
  re-arm deaf window (the ~20-40% retx we measure at high SNR).

RNode and Meshtastic both read on the **interrupt** (ISR → worker), never polling
the FIFO under load — that's the pattern to adopt.

## Architecture

```
 DIO1 ISR (IRAM, minimal)            radio task (high prio)         main loop (loopTask)
 ----------------------              ----------------------         --------------------
 RxDone -> notifyGive(radioTask) --> wait notify                   turn engine, link,
                                     take radioMutex               host I/O (unchanged)
                                     readData() -> frameQueue   --> drain frameQueue ->
                                     give radioMutex                g_link.OnRx() ...
```

- **DIO1 ISR**: stays minimal/IRAM-safe — `vTaskNotifyGiveFromISR(radioTask,&hpw)`
  + `portYIELD_FROM_ISR(hpw)`. No SPI in the ISR (RadioLib SPI is flash-resident).
- **Radio task**: blocks on the notification; on wake, takes `radioMutex`, checks
  the IRQ flags, and if RX_DONE does `readData()` into a fixed ring of frame buffers,
  pushes (len, bytes) onto a **FreeRTOS queue** (`xQueueSend`), releases the mutex.
  It does **not** touch `g_link` — link state stays single-threaded in the main loop.
- **Main loop**: where it used to call `RxWithTimeout`, it instead `xQueueReceive`s
  frames (with the same timeout semantics) and feeds them to `g_link.OnRx()` exactly
  as today. TX is unchanged (`TxFrame` in the main loop).
- **`radioMutex`**: the one shared hazard is concurrent SPI. The radio task takes it
  for `readData`; the main loop takes it for `TxFrame`/`startReceive`/`standby`/
  `reset`. Mutual exclusion = no concurrent SPI. A TxDone notification (same DIO1)
  is ignored by the task (it checks RX_DONE specifically); TxDone stays main-loop.

## Thread-safety summary

| Shared thing | Producer | Consumer | Protection |
|--------------|----------|----------|------------|
| radio (SPI)  | radio task (read) + main loop (tx/arm) | — | `radioMutex` (FreeRTOS) |
| frame queue  | radio task | main loop | FreeRTOS queue (built-in) |
| `g_link`     | main loop only | main loop only | none needed (single-threaded) |

`Radio::rssi_ema_`/counters written by the radio task, read by the main loop
(via `g_radio.rssi()`): plain word writes (benign races, like
`operation_done_`).

## Incremental plan (de-risk; flag-gated)

1. **Queue + task scaffolding** (`#if INTR_RX`): create `radioMutex`, the frame
   ring + FreeRTOS queue, the radio task, and the ISR-notify variant. Unit-test the
   queue logic natively. No behavior change yet (task drains, main loop still polls).
2. **Switch the receive path** to consume the queue (`xQueueReceive`) instead of
   `RxWithTimeout` polling, keeping per-frame standby first (safe), to prove the
   ISR→task→queue→OnRx path is byte-exact on hardware.
3. **Add the `radioMutex`** around TX/arm/read and confirm no SPI races (no
   `CHIP_NOT_FOUND`, no garbled frames) under a speed-test flood.
4. **Then** enable continuous RX on top (no per-frame standby) — the FIFO now drains
   promptly, so no overrun. Measure throughput per mode vs per-frame.
5. Make it the default only after it's solid (byte-exact + no hangs) in **all** modes.

## Testing

- **Sim-able:** the SPSC frame-queue logic (native unit test); the existing physics
  sim already shows continuous-RX's ~2.3× upside.
- **Hardware-only:** the FreeRTOS task + ISR timing + real FIFO behaviour + TX/RX
  mutex. Validate with the on-device `AT+SPEEDTEST` (per mode) + sustained-flood
  soak (responder must stay alive). The cm-range bench is bimodal — a cleaner /
  separated RF setup makes results trustworthy.
- **Backstop:** the hardware loop TWDT + no-progress reboot remain, so a regression
  self-recovers instead of needing a manual reflash.

## Fallback

`INTR_RX` (and `RX_CONTINUOUS`) default OFF. Per-frame polling is the shipped path
until the new path is proven. Both can coexist behind flags during bring-up.
