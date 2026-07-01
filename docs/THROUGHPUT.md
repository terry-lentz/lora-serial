# Throughput & Range — how the levers work

Plain-language guide to what actually controls speed and distance on this link,
plus the numbers measured on the bench.

> **History / status.** Three ceilings were hit in turn and all three are fixed:
> 1. The **USB stack** — the ESP32-S3's Hardware USB-Serial/JTAG dropped bytes on
>    bursts (capped us at ~270 B/s). **Fixed** by switching to **TinyUSB OTG-CDC**
>    (real flow control). See `ARDUINO_USB_MODE=0`.
> 2. The **transport** — Go-Back-N + a tiny window + rigid turn timers stalled on
>    multi-window full-frame transfers. **Fixed** with **Selective-Repeat + SACK +
>    a BDP-sized window + ToA-derived per-mode timing** (see [DESIGN.md](./DESIGN.md)).
> 3. The **host→board USB overrun** — a fast `cat bigfile` floods USB (~1 MB/s;
>    "115200 baud" is cosmetic) far faster than the link, and the arduino-esp32
>    USB-CDC core silently drops on overflow (upstream bug #10836, no NAK).
>    **Fixed** with a **2 MB PSRAM ingest ring + bulk reads** — 128 KB fast-overrun
>    transfers byte-exact, no host-side flow control. Verified end-to-end
>    (`hin`/`hout`/`idrop` counters via `AT+LINK?`).
>
> **Now:** turbo reaches ~**9 KB/s** compressed / ~**4 KB/s** uncompressed, and
> GFSK `ludicrous` ~**12 / 7.5 KB/s**, over the **Ascon-128 AEAD encrypted** link
> (auth + replay protection), byte-exact via the interrupt-driven RX path. The
> radio's airtime — not USB or the transport — is the limit.

## Measured per-mode — uncompressed vs compressed (bench, +22 dBm, AEAD on)

Two honest numbers per mode, not one blended figure: **uncompressed** feeds random
binary (worst case — heatshrink can't shrink it) and **compressed** feeds all-zeros
(best case). Real text/logs/shell land **between** them. Both are end-to-end,
**byte-exact**, AEAD-encrypted payload rates on the **interrupt-driven RX path**,
measured 2026-06-30 (boards ~1.5 m apart) with `tools/lora_xfer.py`:

| Mode | SF / BW / CR | Uncompressed | Compressed | Xfer | RX sens (≈) | LOS range* |
|------|--------------|--------------|------------|------|-------------|------------|
| **ludicrous** | GFSK 200 kbps  | ~7.5 KB/s | **~12 KB/s** | 64 KB | lowest | very short |
| **turbo**  | SF5 / 500 / 4·5  | ~4.0 KB/s | ~9.0 KB/s | 64 KB | ~−111 dBm | ~0.5–1 km |
| **fast**   | SF7 / 500 / 4·5  | ~1.7 KB/s | ~5.0 KB/s | 64 KB | ~−117 dBm | ~1–2 km |
| **medium** | SF7 / 250 / 4·5  | ~1.0 KB/s | ~3.2 KB/s | 64 KB | ~−120 dBm | ~2–4 km |
| **slow**   | SF9 / 125 / 4·6  | ~0.13 KB/s | ~0.43 KB/s | 8 KB | ~−129 dBm | ~5–10 km |
| **far**    | SF12 / 125 / 4·8 | ~0.014 KB/s‡ | ~0.04 KB/s‡ | 1–4 KB | ~−137 dBm | ~10–15+ km |

\* Range = **estimate** (LOS, +22 dBm, decent antenna); field-validate. Sensitivity
is approximate per the SX1262 datasheet.

\‡ `far` (SF12) is now **measured byte-exact** (2026-06-30) — it works after the
TX-safety fix (its ~13 s/frame airtime exceeded the old fixed 8 s TX timeout, so
every far frame was aborted mid-air; see CAPABILITIES_JOURNEY entry 26). Measured
differently from the faster rows because a 64 KB run is impractical at ~14 B/s:
*uncompressed* is the on-device `AT+SPEEDTEST` (1–2 KB, **0 % retx**, `hout`
matched), *compressed* a small all-zeros transfer, both at `pwr=10`. At SF12 the
per-frame overhead dominates, so compression buys less than on faster modes; near
the SF12/CR4·8 ceiling (~222 B / ~13 s ≈ 17 B/s raw).

**Method (reproduce it):** transfers are large enough to amortize the half-duplex
turn-around — **64 KB** for turbo/fast/medium/ludicrous, **8 KB** for slow (a small
transfer reads *low* because fixed per-turn overhead dominates; e.g. turbo
compressed measured 6.5 KB/s at 8 KB but 9.0 KB/s at 64 KB). Uncompressed =
`--pattern random`, compressed = `--pattern zeros`:

```sh
tools/lora_xfer.py /dev/ttyACM0 /dev/ttyACM1 65536 --mode turbo --pattern random
tools/lora_xfer.py /dev/ttyACM0 /dev/ttyACM1 65536 --mode turbo --pattern zeros
```

**`ludicrous` ≈ ISDN:** its compressed ~12 KB/s ≈ **96 kbit/s** and uncompressed
~7.5 KB/s ≈ **60 kbit/s** — straddling a single **ISDN B-channel (64 kbit/s)**, the
fastest the link goes. (turbo ≈ a 56K modem, medium ≈ 28.8K, slow ≈ 2400 baud.)

> **Fast-mode optimization (done):** turbo was ~3.8 KB/s — overhead/turnaround-bound,
> not airtime-bound (only ~24% airtime used). Fixing that ~**doubled it to ~9 KB/s**
> compressed (near the PHY ceiling). Levers: a **BDP-sized per-mode window**
> (turbo → 16 frames/burst so one turn-around is amortized over a long burst; far →
> 2) and **skipping the 30 ms interactive-echo piggyback wait on one-way bulk flows**
> (it was pure dead-time in the ACK path). Both auto-scale from `getTimeOnAir()`.

> **Measure with `tools/lora_xfer.py`** (reusable; supports `--pattern
> zeros|text|random` and `--mode`): `tools/lora_xfer.py /dev/ttyACM0 /dev/ttyACM1
> 16384 --mode turbo`. Remember the table is **compressible** data; test `random`
> for the worst case.

> **Two findings worth remembering:**
> - **Encryption is NOT a throughput cost.** Ascon decrypt is ~tens of µs on a
>   240 MHz S3; toggling `AT+ENC=0` changes throughput by ~nothing.
> - **Most "deafness"/retx was a loose connector, not the link.** A high retx rate
>   (and many "responder went deaf / wedged" episodes) traced to an **intermittent
>   board-to-board connector** on one unit — the SX1262 wasn't answering SPI
>   (`begin()` → CHIP_NOT_FOUND). With a solid radio the per-frame path is already
>   **0 % retx** at fast modes, and the interrupt-driven RX path is byte-exact
>   across every mode. (The full saga is in
>   [CAPABILITIES_JOURNEY.md](./CAPABILITIES_JOURNEY.md).) Lesson: at a cm/metre
>   bench, also lower TX power — +22 dBm point-blank desenses the front end.

---

## The data path (where bytes flow)

```
   host output                                                         host input
        │                                                                  ▲
 ┌──────▼───────┐  raw USB-CDC   ┌────────────┐  LoRa (half-duplex)  ┌────────────┐  raw USB
 │   tio / cat  │ ─────────────▶ │  board A   │ ════════════════════▶│  board B   │ ─────────▶ tio
 │   (host)     │  ❶ ingest ring │ (initiator)│  ❷ ARQ window / turns│(responder) │  ❸ tx ring
 └──────────────┘                └────────────┘                      └────────────┘
```

❶ **host → board (USB host→device): absorbed, not the wall.** USB delivers far
faster than the link (~1 MB/s). The board absorbs the burst into a **2 MB PSRAM
ingest ring** and drains it into the radio at link speed; when the ring fills, the
USB stack NAKs (real back-pressure) so nothing is dropped. This used to be the
hard ceiling on the old Hardware-USB/JTAG path; it no longer is.

❷ **board → board (radio): half-duplex turn-taking + ARQ — the real limit.** One
radio can't transmit and receive at once, so the two ends take turns. A
**BDP-sized Selective-Repeat window** is sent, then the sender waits for a SACK
before sending more. The window is sized so one turn-around is amortized over a
long burst — at turbo this keeps the radio near its airtime ceiling. Speed here is
set by **airtime** (SF/BW/CR) plus turn-around overhead, *not* by USB.

❸ **board → host (USB device→host): not a bottleneck.** A non-blocking ring drains
as fast as the terminal reads.

---

## The levers

### Spreading Factor — SF (5–12)
How many chips encode each symbol. **Higher SF = more range, exponentially slower**
(each +1 SF ≈ doubles airtime and adds ~2.5 dB link budget ≈ ~1.3× range). SX1262
floor is **SF5** (there is no SF4). Lower SF is faster but needs a cleaner signal.

### Bandwidth — BW (125 / 250 / 500 kHz)
Wider = faster (proportional) but ~3 dB *less* sensitive per doubling (≈ shorter
range). 500 kHz is 2× the speed of 250 kHz but a bit less range.

### Coding Rate — CR (4/5 … 4/8)
Forward-error-correction overhead. 4/5 = least overhead/fastest; 4/8 = most
redundancy/most robust/slowest. Higher CR helps a marginal link survive.

### ARQ window (`lc.window`, max `MAXWIN`)
How many frames fly before a SACK is required. Too small for the round-trip time
and the sender stalls waiting for ACKs; big enough and a burst is amortized over a
single turn-around. The firmware sizes it per mode from the bandwidth-delay product
(turbo → 16, far → 2), derived automatically from `getTimeOnAir()`.

### Turn timing (`g_turnRxMs`, `g_interframeMs`, `g_retransmitMs`)
Fixed waits that coordinate whose turn it is and when to resend. They scale with
the per-frame airtime (ToA): tiny at SF5, large at SF12. Set too short, a burst
gets abandoned mid-way and retransmitted; too long, time is wasted waiting. All
auto-derive from `getTimeOnAir()` so each mode just works.

### TX power
+22 dBm (SX1262 max). Legal in Taiwan (920–925 MHz allows 27 dBm EIRP outdoor /
30 dBm indoor). More power = more range, no speed effect.

---

## Modes (range vs speed presets)

Modes are **switchable at runtime** with `AT+MODE=<name>` (turbo/fast/medium/slow/
far) over the `+++` command channel — set the same on both ends, `AT&W` to persist.
Per-mode window and turn timing auto-derive from each mode's time-on-air, so they
just work. A *mode* picks SF/BW/CR for a range/speed trade-off; the measured
end-to-end numbers are in the table at the top of this document.

---

## The experiment that found the real bottleneck (history)

Early on, throughput sat near ~290 B/s and we suspected the radio. So we flashed
**SF5/BW500** — 8× the raw radio rate — and re-measured. Throughput **did not
change**. Profiling the link telemetry under a fast feed showed the send window
**pinned full the entire time** while the send ring backed up — round-trip/ACK
limited, not airtime limited. And pushing the host feed past ~300 B/s **wedged the
USB framing** on the old Hardware-USB/JTAG path.

So two things — *neither the radio* — capped us near 300 B/s: the USB-Serial/JTAG
host→device feed, and the half-duplex ARQ round-trip with a tiny fixed window.
Both are now fixed:

1. **USB:** TinyUSB OTG-CDC (real flow control) + a PSRAM ingest ring with bulk
   reads removed the host→device wall — a fast `cat bigfile` is byte-exact.
2. **Transport:** Selective-Repeat + SACK + a BDP-sized window + ToA-derived turn
   timing lets the radio carry the higher feed, amortizing the turn-around.

With both lifted, the radio's airtime is the limit again — exactly as it should be —
and turbo runs near its ~9 KB/s (compressed) PHY ceiling.
