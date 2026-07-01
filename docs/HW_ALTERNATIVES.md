# Hardware alternatives: other radios for point-to-point

This project is a reliable point-to-point serial link built on **LoRa** (the
Semtech SX1262). If you're weighing a different radio for a similar job — a
transparent, two-node "wireless serial cable" — this page lays out the real
contenders and where each one wins.

**An honest framing first.** Most of the "unreliability" this project fought
during development was **software**, not the radio: an RX-task throttle that
starved the receiver and made it look deaf, and a pinned-GFSK path with no
recovery when a switch went wrong. Both are fixed, and both are covered in
[CAPABILITIES_JOURNEY.md](./CAPABILITIES_JOURNEY.md). So treat everything below
as a **throughput/feature upgrade**, not a reliability rescue: LoRa itself was
never the flaky part. If you want more speed, a power budget for a 1 W radio, or
an IP stack, a hardware swap can get you there — but you'd be trading away LoRa's
genuine strengths (decode-below-the-noise range at very low power), not escaping
a broken foundation.

New to the vocabulary here (dBm, SNR, spreading factor, sensitivity)? Read
[RADIO_BASICS.md](./RADIO_BASICS.md) first — it explains every term below with a
"talking across a field" analogy. For *software/project* alternatives on this
same hardware (Reticulum, Meshtastic, SparkFun LoRaSerial), see
[SW_ALTERNATIVES.md](./SW_ALTERNATIVES.md).

---

## At a glance

| Technology | Modulation | Throughput | Range | TX power | Ecosystem / stack | Best for |
|---|---|---|---|---|---|---|
| **LoRa** (SX1262, this project) | Chirp spread spectrum (CSS) | ~0.3–37.5 kbps raw; ~1–12 KB/s app-layer | km-scale (~2–4 km medium, ~10–15+ km far) | ≤ ~22 dBm | LoRaWAN — TTN, Helium, ChirpStack | Max range at min power; low rate; obstructed/noisy links; battery IoT |
| **SiK / FSK** (RFD900x, Si446x) | (G)FSK — no spreading gain | ~2–250 kbps air rate | few km typical; 40+ km best-case | ≤ 1 W (30 dBm) | Open GPL SiK firmware; MAVLink | Higher-rate P2P with a power budget; turnkey telemetry; FHSS |
| **Wi-Fi HaLow** (802.11ah) | OFDM, sub-GHz | 150 kbps – ~78 Mbps; ~1–15 Mbps realistic | ~1 km+ (up to ~1–3 km) | region-dependent | Full 802.11 + IP stack | Mbps sub-GHz at ~km, with TCP/IP |

Each row gets a full section below, followed by honorable mentions, a decision
guide, and a look at the low-power future.

---

## LoRa — Semtech SX1262 (what this project uses)

LoRa is **chirp spread spectrum (CSS)**: every symbol is a tone that sweeps
across the channel, and the receiver "de-chirps" it by correlating against the
known sweep. Because a real chirp piles its energy into a sharp spike while noise
stays smeared flat, the receiver enjoys **processing gain** — it can decode a
signal **~15–20 dB *below* the noise floor**. On the SX1262 that means
sensitivity down to about **−137 dBm** (SF12 / BW125). That single trick is
LoRa's whole reason to exist, and nothing else in this comparison can match it.
(For the "why" behind the spike-vs-flat picture, see
[RADIO_BASICS.md](./RADIO_BASICS.md).)

- **Range.** Km-scale. This project measures roughly ~2–4 km on `medium` and
  ~10–15+ km line-of-sight on `far` — see [THROUGHPUT.md](./THROUGHPUT.md).
- **Throughput.** Low, by design. Raw PHY runs from ~0.3 kbps (SF12) up to
  ~37.5 kbps (SF5 / BW500); after framing, ARQ, and encryption, the app-layer
  rate here lands around ~1–12 KB/s depending on mode.
- **TX power.** Up to ~22 dBm — but the deep sensitivity means you rarely need
  it. Low TX power still reaches far, which is exactly what makes LoRa
  battery-friendly.
- **Ecosystem.** Huge, via **LoRaWAN** — The Things Network, Helium, ChirpStack.
  (This project uses *raw* point-to-point LoRa, not LoRaWAN — see
  [SW_ALTERNATIVES.md](./SW_ALTERNATIVES.md) for why.)
- **Licensing — the catch.** LoRa operates in the **unlicensed sub-GHz ISM
  bands**, but the *modulation itself is patented and proprietary to Semtech*.
  You must buy Semtech silicon (or a licensed part, e.g. ST's STM32WL, which
  embeds a Semtech radio on-die); you **cannot legally roll your own LoRa PHY**.
  The band is open; the modulation is not.

**Best for:** maximum range at minimum power, low data rate, obstructed or noisy
links, battery sensors, and networked IoT.

---

## SiK / FSK — RFD900x, Silicon Labs Si446x, open-source SiK firmware

SiK radios (the RFD900x / RFD868x family, built on Silicon Labs Si446x
transceivers running the open-source **SiK** firmware) are the classic drone- and
robotics-telemetry link. Crucially, they use **(G)FSK** — the *same modulation
family* as this project's GFSK `ludicrous` rung, **not** LoRa/CSS. FSK has **no
processing gain**, so it needs a *positive* SNR to decode; sensitivity sits
around **−110 to −121 dBm** depending on the data rate. That's the fundamental
trade against LoRa: faster and simpler, but it can't dig below the noise.

- **Configuration feels familiar.** Instead of LoRa's spreading factor /
  bandwidth / coding rate (those are CSS-only knobs), you set the **air data
  rate** (~2–250 kbps) directly, plus hop-channel count, TX power, Golay FEC, and
  MAVLink framing — all over **Hayes AT commands / S-registers** (`AT&V` to dump
  the config, `ATSn=` to set a register). If you like this project's `+++`/AT
  console, you'll feel at home.
- **FHSS built in.** SiK radios **frequency-hop** across many channels, which
  buys interference and regulatory resilience that this project's *single-channel*
  link doesn't have.
- **TX power.** Up to **1 W (30 dBm)** on the RFD900x — far above the SX1262's
  ~22 dBm. That's a telemetry radio for drones with a real power budget, not a
  coin-cell part.
- **Range.** The RFD900x advertises **40+ km line-of-sight** best-case (lowest
  air rate, directional antennas, full 1 W). Realistically it's a few km on whip
  antennas, and at 250 kbps the range collapses toward hundreds of metres.
- **Licensing — refreshingly open.** Generic FSK silicon plus **open-source GPL
  firmware**, in the unlicensed ISM bands. Ironically **more open than LoRa**:
  there's no proprietary modulation to license.

**Best for:** higher-throughput point-to-point when you have a power budget,
turnkey bidirectional telemetry, and FHSS resilience. In effect, a SiK radio is
**this project's GFSK `ludicrous` rung, productized** — FSK speed plus FEC,
hopping, up to 1 W, and mature firmware. What it *cannot* do is LoRa's
decode-below-the-noise trick, so at the very weakest signals **LoRa still reaches
further**.

---

## Wi-Fi HaLow — IEEE 802.11ah

Wi-Fi HaLow is **802.11 dragged down to sub-GHz**. It uses **OFDM** around
~900 MHz in **1 / 2 / 4 / 8 / 16 MHz** channels — trading Wi-Fi's short range for
much better reach while keeping real Wi-Fi throughput.

- **Throughput.** From **150 kbps** (MCS10, 1 MHz channel) up to **~78 Mbps** on
  the widest channels; realistically **1–15 Mbps** at range. This is a different
  league from LoRa or FSK.
- **Range.** ~1 km and up — roughly 1–3 km in good conditions.
- **Chips & modules.** Morse Micro **MM6108**, Newracom **NRC7292 / NRC7394**;
  modules from Morse Micro, Silex, Alfa, and Heltec.
- **It's real Wi-Fi.** A full **IP stack (TCP/IP)** comes for free. It can run
  raw peer-to-peer/IBSS, **802.11s mesh**, or act as an AP/relay — and it has a
  power-save mode (**TWT**, Target Wake Time) for battery IoT, though at a higher
  duty cycle than a LoRa sensor.
- **Regulatory caveat (matters here).** This project targets **Taiwan's
  920–925 MHz** allocation, which is only **5 MHz wide** — so you're limited to a
  1 / 2 / 4 MHz channel with **no aggregation**. Always check the local channel
  plan before counting on the wide-channel Mbps numbers.

**Best for:** "**HaLow speeds but just point-to-point**" — Mbps sub-GHz at ~km
range, *with* an IP stack, when LoRa's kbps simply isn't enough.

---

## Honorable mentions

- **TI CC1200** — a fast long-range **FSK / 4-GFSK** transceiver: **500 kbps –
  1 Mbps** at 868/915 MHz with good sensitivity. The closest part to this
  project's spirit and *much* faster than the SX1262's GFSK rung — a natural pick
  if you want "our GFSK mode, but quicker."
- **2.4 GHz long-range** (ESP-NOW / Wi-Fi Long Range / Ubiquiti) — Mbps at km
  ranges with clear line-of-sight, but **no sub-GHz penetration** through
  obstacles and foliage.
- **BLE Coded PHY** (LE Long Range) — **125 / 500 kbps**, ~1 km, and *ubiquitous
  in phones*. But it's **2.4 GHz** and shorter-range than sub-GHz options.

---

## How to choose

- **Max range / min power / low rate / battery / obstructed → LoRa.** Nothing
  else digs below the noise floor.
- **Higher-throughput P2P with a power budget, turnkey telemetry, hopping →
  SiK/FSK** (or **CC1200** for more speed).
- **Mbps sub-GHz with an IP stack → Wi-Fi HaLow.**

One nuance specific to *this* project: it deliberately carries **both LoRa modes
*and* a GFSK rung**, and its ADR (`AT+MODE=auto`) picks **LoRa when the link is
far or obstructed** (range via processing gain) and **GFSK when the peers are
close** (raw speed). See [FUTURE_MODES.md](./FUTURE_MODES.md). A SiK radio could
only ever be the FSK half of that spectrum — it can't fall back to CSS when the
signal gets weak.

---

## The low-power future (weeks on a battery)

If the goal is a radio link that runs for **weeks on a battery**, LoRa is the
natural path: its sensitivity lets you transmit at low power, and its low duty
cycle lets you sleep hard between transmissions.

The tension is that **this project today keeps the radio in continuous receive.**
That's the right call for a transparent "serial cable" — a terminal session wants
low latency, so the receiver has to be listening all the time — but continuous RX
is power-hungry. Running for weeks would mean shifting from *always-on* to
*scheduled-wake*, layering in:

- **RX duty-cycling** — LoRa RX-duty-cycle mode or Channel-Activity-Detection
  (CAD) wake, so the receiver sleeps and samples the channel instead of listening
  nonstop.
- **MCU deep-sleep between turns** — the ESP32-S3's light/deep sleep, powering
  the radio and CPU down between scheduled exchanges.
- **Lower TX power** — the existing **auto-power** loop (`AT+APWR`) already trims
  TX power to what the link needs; a battery profile would lean on it harder.
- **A store-and-forward / scheduled-wake profile** — "wake → burst → sleep"
  instead of an always-on transparent serial port.

The trade is **latency vs. battery life**: an always-on serial cable will never
run for weeks, but a "wake → burst → sleep" telemetry profile could. This is a
future *operating mode/profile*, not a hardware change — see
[ROADMAP.md](./ROADMAP.md) and [FUTURE_MODES.md](./FUTURE_MODES.md) for where it
fits.
