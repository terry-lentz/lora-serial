# Radio, explained for newcomers

New to radio? Start here. By the end of this page you'll understand what every
knob in this project does — spreading factor, bandwidth, dBm, sensitivity, SNR —
what **LoRa** actually is and why it reaches so far, and how it compares to the
other wireless tech you already know. No math, no jargon you haven't met yet.

The whole thing rests on one picture, so let's build it.

---

## The big picture: two people talking across a field

Forget radios for a second. Picture **two people trying to talk across an open
field.** Everything about radio is some version of this conversation:

- If they're **close** and it's **quiet**, they talk normally and fast.
- If they're **far apart**, or it's **windy and noisy**, they have to talk
  **slower**, **louder**, and **repeat themselves** to get through.

That's the entire trade-off radio lives inside: **speed vs. distance vs.
reliability — pick two.** You cannot have a fast, long-range, rock-solid link all
at once; pushing one corner costs you another. Every "knob" below is just a way of
choosing where you sit in that triangle.

---

## The knobs (as ways of talking across that field)

### Loudness — power, measured in dBm
How hard you shout. In radio this is **transmit power**, measured in **dBm**.

dBm is just a loudness number where **more negative = fainter**: −30 dBm is a
shout, −120 dBm is the faintest whisper. It's a *logarithmic* scale, which sounds
scary but only means: **every +10 dBm is "10× the power," every +3 dBm is "2×."**
So going from 19 dBm to 22 dBm is doubling your shout. Louder = more range, but
costs battery and runs into legal limits (each country caps how loud you may be).

### Good ears — receiver sensitivity
How well the *listener* hears. **Sensitivity** is the faintest signal a receiver
can still understand, also in dBm. A radio rated **−120 dBm sensitivity** can make
out an *extremely* faint whisper. **The fainter it can hear, the farther the link
reaches.** Different settings change this number — that's the core of range.

### Talking over the wind — SNR (signal-to-noise ratio)
A field is never silent — there's wind, rustling, distant traffic. **SNR** is
**how far your voice rises above that background hiss.** Normally you need your
voice *louder* than the noise to be understood (positive SNR). LoRa's superpower,
which we'll get to, is understanding a voice that's **quieter than the hiss
itself** (negative SNR) — like picking out a whisper under a noisy crowd.

### How wide a lane — bandwidth (BW)
Radio lives on frequencies, like lanes on a highway. **Bandwidth** is **how wide a
lane you use** (measured in kHz — e.g. 125, 250, 500). A **wider lane carries more
data per second (faster)** but also **scoops up more background noise (less
sensitive, shorter range)**. A narrow lane is slower but more focused, so faint
signals survive farther. Wider = faster but shorter; narrower = slower but farther.

### Spelling things out — coding rate (CR), a.k.a. error correction
When it's noisy, you might **spell important words in the NATO alphabet**
("Alpha-Bravo-Charlie") so even if a letter gets lost, the listener reconstructs
the word. Radios do the same with **error-correcting codes**: they send a few
**extra "parity" bits** so the receiver can *repair* small errors **without asking
you to repeat.** More redundancy (e.g. `4/8`) = tougher but slower; less (`4/5`) =
leaner but more fragile. (This is **FEC**, forward error correction.)

### Link budget — the loudness allowance
Tie it together: **link budget** is your total "allowance" of loudness between the
two ends. It's *your shout (power) + cupping your hands (antenna gain) − how much
the distance and obstacles muffle you (path loss)*, and it has to stay **above the
listener's faintest-whisper threshold (sensitivity).** As long as you're above it,
you connect. Drop below it and the link dies. Handy rule of thumb: **every ~6 dB of
extra budget roughly doubles your distance** (in open space).

> **That's it — that's radio.** Power, sensitivity, noise, bandwidth, error
> correction, and the budget that ties them together. Everything else is detail.

---

## So what is LoRa?

**LoRa** = **Lo**ng **Ra**nge. It's a way of *encoding* the signal — a
**modulation** — invented by a company called Semtech, and it's freakishly good at
one thing: **being understood from very far away, even when the signal is weaker
than the background noise.** That's the "negative SNR" magic from above. It does it
with a trick called **chirp spread spectrum.**

### What's a "chirp"?
Instead of sending data as plain on/off blips at one frequency, LoRa sends
**chirps** — tones that **sweep** smoothly across the lane, like a slide whistle
going *wheeeoop*. (You've heard a chirp: it's the sound a bird makes, or the
"pew" sweep in sci-fi.) Data is encoded in **where each chirp starts** its sweep.

### Why that reaches so far — "spreading" and processing gain
Here's the clever part. The receiver knows *exactly* what shape the chirp should
be (a known, steady sweep). So it does something called **de-chirping**: it lines
the incoming signal up against the expected sweep and adds up all that energy.

- The **real chirp** matches the expected pattern perfectly, so all its energy
  **piles up into one sharp spike.**
- **Random noise** doesn't match the sweep, so its energy **stays smeared out flat.**

Spike vs. flat: the message pops out **even if, before de-chirping, it was buried
under the noise.** Spreading the signal out over time and then "collapsing" it back
is what buys the range — engineers call the bonus **processing gain.** The more you
spread it (higher spreading factor), the bigger the spike, the deeper under the
noise you can dig — at the cost of taking longer to say each symbol.

Which brings us to the LoRa-specific knob:

### Spreading factor (SF) — *how slowly and redundantly you speak*
**SF** (5 to 12 on our chip) is how much each chirp is spread out:
- **Low SF (5–7):** short, quick chirps. **Fast, but needs a cleaner/closer
  signal** — normal-speed talking, fine up close.
- **High SF (11–12):** long, drawn-out chirps the receiver averages over. **Slow,
  but reaches astonishingly far** — saying one… word… *very… slowly* so someone
  across a stadium still catches it in the wind.

Each step up the SF ladder roughly **doubles the range potential and halves the
speed.** It's the main speed-vs-distance dial — and it's why this project has named
*modes* (turbo at SF5, far at SF12, and the rest in between).

---

## LoRa vs. the wireless you already know

At a high level, every wireless tech picks a different corner of that
speed/distance/power triangle:

| Tech | Range | Speed | Power | The "field" analogy |
|---|---|---|---|---|
| **Wi-Fi** | a room / house | very fast (Mbps–Gbps) | high | shouting a firehose of words across a room |
| **Bluetooth** | a few meters | fast | low | chatting quietly with someone next to you |
| **Cellular (LTE/5G)** | km, via towers | fast | high + infrastructure | a megaphone relay network you rent |
| **Plain FSK radio** (most cheap modules) | hundreds of m | medium | low | talking normally outdoors — fine line-of-sight, no magic |
| **LoRa** | **km, even tens of km** | **slow (bps–kbps)** | **very low** | a slow, deliberate voice that carries impossibly far |

Two distinctions worth knowing:

- **LoRa vs. plain FSK.** FSK (frequency-shift keying) is the simple, classic way
  most low-cost radios work: shift between two tones for 0 and 1. It's perfectly
  good *line-of-sight*, but it has **no processing gain** — it can't dig below the
  noise, so its range tops out far short of LoRa's. (Our SX1262 chip can actually
  do *both* LoRa and FSK — FSK is the lever for a short-range "go fast" mode.)
- **LoRa vs. LoRaWAN.** People conflate these. **LoRa** is just the radio link —
  two devices talking. **LoRaWAN** is a whole *network* built on top of LoRa: many
  devices reporting to gateways that bridge to the internet, with addressing,
  servers, the works. This project uses **raw LoRa, point-to-point** — just two
  radios, no network, no gateway, no internet. A private wireless wire.

The price LoRa pays for its range is **speed**: we're talking *bytes* per second to
*kilobytes* per second, not megabytes. That's the deal — and it's a great deal when
what you need is to reach a sensor on a far hill, or get a login shell to a cabin
with no cell coverage, on a coin cell.

---

## Why this project has "modes"

Because you can't escape the triangle, this link lets you **pick your corner** at
runtime — each mode is just a preset of the knobs above:

| Mode | SF / BW | Feels like | Best for |
|---|---|---|---|
| **turbo** | SF5 / 500 kHz | ~56K dial-up modem | speed, short range |
| **medium** | SF7 / 250 kHz | ~14.4K modem | balanced everyday use |
| **ultra** | SF12 / 125 kHz | slower than 300-baud teletype | maximum range |

Closer and clear? Use turbo. Far away or through obstacles? Step toward far and
trade speed for reach. See [throughput.md](./THROUGHPUT.md) for the measured
numbers and ranges.

---

## Where to go next

- **[how-it-works.md](./HOW_IT_WORKS.md)** — the technical deep-dive: exactly how
  LoRa de-chirps, the preamble/sync/CRC pipeline, and how our link layer guarantees
  every byte arrives.
- **[../README.md](../README.md)** — build it, flash it, use it.
- **[throughput.md](./THROUGHPUT.md)** — measured speeds and range per mode.

One last reassurance: you now know more about radio than you think. *Loudness,
ears, noise, lane-width, spelling-it-out, and a budget that ties them together* —
that vocabulary covers ninety percent of every radio conversation you'll ever have.
