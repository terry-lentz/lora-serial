# How It Works — LoRa link concepts, the PHY, and our benchmark results

A from-scratch explainer for this project: what the radio knobs mean, how LoRa
pulls a signal out of the noise and guarantees clean data, and what we actually
measured. Pairs with [`README.md`](../README.md) (the build/flash/operational doc).

---

## Part 1 — The core knobs

### SF — Spreading Factor

LoRa doesn't send bits as plain on/off pulses. It spreads each bit across a
**chirp** — a tone that sweeps up across the channel. SF is *how much* each
symbol is spread out.

- **Higher SF (12)** — spread very wide, sent slowly. The receiver averages over
  a long, redundant signal and can dig the message *out from under the noise*.
  Long range, low speed.
- **Lower SF (7)** — short, tightly packed symbols. Much faster, but needs a
  cleaner (stronger) signal.

> Each step down (12→11→10…) ≈ **2× the data rate** but costs ≈ **2.5 dB** of
> sensitivity. This is the main speed-vs-range knob. Range: SF7 (fast/short) to
> SF12 (slow/long).

Analogy: SF12 is saying a word *very… slowly… and… repeating… it* so someone
across a noisy room still catches it. SF7 is talking at normal speed — fine up
close, useless across a stadium.

### ToA — Time on Air (per frame)

How many milliseconds the radio is actually transmitting one packet. It matters
three ways:

1. **It is your speed** — long airtime = low throughput.
2. **Latency** — at SF12 a round-trip is inherently multi-second (the "slow
   modem" feel).
3. **Legal duty cycle** — Taiwan's 920–925 MHz band caps the fraction of time
   you may transmit (often ~1%). At ~4.5 s per SF12 packet, a 1% cap forces
   ~440 s of silence after each — which is *why* the design insists on terse,
   compressed, no-keepalive traffic.

### Link budget (and dB / dBm)

Think of it as an **energy bank account** for the signal, measured in **dB**
(a ratio scale: +3 dB ≈ 2× power, +10 dB ≈ 10×).

- **Deposit:** TX power + antenna gains.
- **Withdrawal:** path loss — signal weakens with distance, walls, foliage.
- **Minimum balance:** the receiver's *sensitivity* — the faintest signal it can
  still decode.

> **Link budget = how much loss you can tolerate before the link dies.** More
> budget = more range (or more margin through obstacles).

Lowering SF *raises* the receiver's minimum balance (worse sensitivity), so
you're spending range budget to buy speed. Handy conversions:

- **~6 dB ≈ 2× distance** (free space).
- **dBm** = the same scale as an absolute power (e.g. RSSI −40 dBm). More
  negative = weaker.
- SF12 decodes down to ~**−137 dBm**; SF7 needs ~**−124 dBm**. That ~13 dB gap is
  the full SF7↔SF12 range difference.

---

## Part 2 — How LoRa decodes below the noise and rejects garbage

Four distinct mechanisms, doing four different jobs.

### (a) Pulling signal out of noise — the chirp

The chirp is a known, deterministic sweep. The receiver **de-chirps**: it
multiplies the incoming signal by an inverted copy of the expected chirp.

- The **real signal** matches the pattern, so its energy collapses into one sharp
  peak (a single FFT bin).
- **Random noise** doesn't match, so its energy stays smeared flat.

The peak piles up while the noise stays thin — so it sticks out even if, before
de-chirping, the signal was *below* the noise floor. That gain is **processing
gain ≈ 2^SF**. At SF12, 2¹² = 4096 ≈ **+36 dB** — the reason it decodes ~36 dB
under the noise. (The data itself is encoded in *where each chirp starts* — 1 of
2^SF frequency positions. De-chirp → FFT → "which bin?" → those bits.)

### (b) "Is this one of ours?" — preamble + sync word

- **Preamble** (`PREAMBLE = 12`): a run of plain chirps at the start. When the
  receiver sees a consistent, repeating peak line up, it knows a packet is
  beginning and locks timing/frequency. Noise never produces that aligned
  repetition.
- **Sync word** (`SYNC_WORD = 0x12`): special symbols after the preamble. If it
  doesn't match, the whole packet is dropped. Coarse "is this ours?" filter — why
  our nodes hear each other but ignore LoRaWAN devices (which use `0x34`). Must
  match on both ends.
- **Start-of-frame**: a couple of *downchirps* mark the boundary to the payload.

⚠️ The sync word is **not** addressing or security — it's a 1-byte filter,
trivially spoofable. Real device addressing + encryption are higher layers we
have not built yet.

### (c) Repairing errors — FEC (the Coding Rate)

`CR4/5` … `CR4/8` adds redundant **parity bits** (a Hamming code) so the receiver
*repairs* bit errors with no retransmission:

- `CR4/5` = 1 parity bit per 4 data bits (light, faster).
- `CR4/8` = 4 parity bits per 4 (heavy, survives a rougher link).

Two supporting tricks:
- **Interleaving** — bits are shuffled across many symbols, so one clobbered
  symbol scatters as single-bit errors across many codewords (each easily fixed)
  instead of destroying one codeword.
- **Gray coding** — symbol values arranged so mistaking a peak for its *neighbor*
  bin is only a single bit error.

### (d) The final verdict — CRC

After FEC repairs, a **CRC** checksum over the payload is verified
(`radio.setCRC(true)`). One bit still wrong → CRC fails → packet discarded.

> **FEC corrects** what it can → **CRC catches** what it couldn't → corrupt
> packets are dropped before they ever reach our code. That's why milestone-1
> framing adds no checksum of its own, and why dropped packets simply trigger a
> stop-and-wait retransmit one layer up.

### The whole pipeline

**TX:** bytes → whiten (scramble) → append CRC → add FEC parity → interleave →
Gray-code → map to chirps → prepend preamble + sync word → antenna.

**RX:** detect preamble → check sync word (else drop) → de-chirp → FFT peak →
un-Gray/de-interleave → FEC repair → check CRC (pass → deliver, fail → drop).

Each knob lives in this chain: **SF** = chirp spread (processing gain), **CR** =
FEC parity, **sync word** = filter, **CRC** = final gate.

---

## Part 3 — Benchmark results

Measured with the `BENCH` firmware (see README): NODE_A negotiates each config
with NODE_B over a robust SF12 control channel, runs a time-boxed throughput
burst of 64-byte frames at that config, then reports results. **Two radios
point-blank on a desk, ~10 dBm TX.**

Goodput is stop-and-wait *effective* throughput (payload only, including
turnaround) — roughly half the raw PHY bitrate.

| Config | ToA / 67 B frame | Goodput | vs SF12 speed | Link budget vs SF12 |
|---|---|---|---|---|
| **SF12** BW125 CR4/8 | 4464 ms | 0.09 kbps | 1× (baseline) | 0 dB (max range) |
| SF11 BW125 CR4/5 | 1626 ms | 0.24 kbps | ~3× | ~−2.5 dB |
| SF10 BW125 CR4/5 | 772 ms | 0.49 kbps | ~5× | ~−5 dB |
| **SF9** BW125 CR4/5 | 427 ms | 0.90 kbps | ~10× | ~−7.5 dB |
| **SF8** BW125 CR4/5 | 233 ms | 1.64 kbps | ~18× | ~−10 dB |
| SF7 BW125 CR4/5 | 127 ms | 2.86 kbps | ~32× | ~−12.5 dB |
| SF7 BW250 CR4/5 | 63 ms | 5.28 kbps | ~58× | ~−15.5 dB |
| SF7 BW500 CR4/5 | 31 ms | 9.0 kbps | ~100× | ~−18.5 dB |

**Every config delivered 100%** (`ok N/N`) at point-blank range.

### Interpretation

- **Sweet spot: `SF8/BW125/CR4/5`** — ~18× the throughput of SF12 (1.6 kbps, a
  genuinely usable command/response shell) for only ~10 dB of link budget. By the
  ~6 dB = 2× rule, that's ~⅓ the range of SF12 — but SF12's extra reach is the
  balloon/mountaintop record regime anyway, so SF8 keeps the bulk of LoRa's
  practical range.
- **More margin/range** → `SF9/BW125` (~0.9 kbps, −7.5 dB).
- **More speed, range less critical** → `SF7/BW250` (~5.3 kbps, −15.5 dB).

### ⚠️ Caveat: SNR here is not a range predictor

The SNR readings in the run (4–14 dB) are meaningless for range, because two
radios inches apart partially desensitize each other (near-field). Treat this run
as a pure **throughput / decodability** characterization. Real link margin only
shows up once the radios are properly separated — which is exactly what the ADR
controller will key off later (picking the fastest SF a given distance supports).

---

## Where these live in the code

- Radio params: `kFreqMhz`, `kSyncWord`, `kPreamble`, `kTcxoV`, `kTxPowerDbm` and
  the control/fixed configs in [`src/fw_config.h`](../src/fw_config.h).
- `radio.setCRC(true)` / `radio.setDio2AsRfSwitch(true)` (radio bring-up) live in
  `Device::Setup()` ([`src/fw_device.cpp`](../src/fw_device.cpp)); `g_radio.ApplyMode()`
  (live SF/BW/CR switching) is in [`src/fw_radio.cpp`](../src/fw_radio.cpp).
- The reliable transport (framing, Selective-Repeat ARQ + SACK, AEAD, compression)
  — [`lib/linklayer/`](../lib/linklayer/), unit-tested natively in [`test/`](../test/).
