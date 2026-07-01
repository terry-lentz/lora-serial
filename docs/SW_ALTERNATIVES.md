# Software alternatives & prior art

*For radio-hardware alternatives (LoRa vs SiK vs Wi-Fi HaLow), see
[HW_ALTERNATIVES.md](./HW_ALTERNATIVES.md).*

Before/while building this, we evaluated what already exists for "data over LoRa,"
so we know what we're duplicating, what we're not, and why we chose to build a
lean point-to-point reliable serial link ourselves. Short version: the *concept*
exists (notably Reticulum), but the cheap turnkey modules skip the hard part
(reliability) and the mature stack is heavier than we need. We continue ours for
**control, understanding, and a tailored lean project** — not because nothing else
does it.

## What the LoRa chip gives you (and doesn't)
The SX1262 is a **packet** radio: "send ≤255 bytes" / "a packet arrived." It has
**no** reliable byte-stream, retransmission, flow control, or "serial port." Every
reliability feature we have (windowed ARQ, flow control, half-duplex turn-taking,
byte-stream framing, encryption) sits on top and is **not** in the chip.

## The landscape

### Cheap "transparent serial" modules — Ebyte E22 / E220, RYLR, etc.
- Feed a UART in one module, bytes come out the other's UART. The transparent-serial
  category **exists** off the shelf. ([Ebyte E22](https://www.cdebyte.com/products/E22-230T22D))
- Have FEC and even multi-hop relay.
- **But fire-and-forget: no ACK/ARQ.** A lost packet is just lost; reliable delivery
  "must be implemented at the application level." So they solve *transparency* and
  punt on *reliability* — which is exactly the hard part we built.

### SparkFun LoRaSerial (the closest open sibling)
- **The same product idea, done as mature hardware + firmware:** "a pair of serial
  radio modems that transparently pass data." Open-source (Arduino C++), with
  prebuilt binaries and a polished manual. ([github.com/sparkfun/SparkFun_LoRaSerial](https://github.com/sparkfun/SparkFun_LoRaSerial))
- **Where it's ahead of us:** dedicated **1 W (30 dBm)** hardware (more range,
  enclosure, ~15 km LOS demoed), **frequency hopping (FHSS)**, **multipoint
  broadcast**, a button **training mode** for key pairing, and years of field maturity.
- **Where we're ahead:** **generic** — runs on any RadioLib LoRa chip and cheap COTS
  boards, not one product; **modern AEAD** (Ascon-128: authenticated + replay-safe vs
  their AES); **higher peak throughput** (~8 KB/s turbo); **host-overrun robustness**
  (PSRAM ingest); newer SX1262 radio; OS-login-ready.
- **Net:** they win on range + turnkey hardware + FHSS; we win on flexibility, modern
  security, and raw speed on cheap parts. Full side-by-side and the gaps we're closing
  are in [ROADMAP.md](./ROADMAP.md).

### LoRaWAN
- The famous standard, but the **wrong tool**: sensor→gateway *star* network, heavy
  duty-cycle limits, tiny asymmetric uplink-biased messages. Not a bidirectional
  interactive serial pipe. Useless for a shell.

### Reticulum + RNode + rnsh  (the serious match)
- [Reticulum](https://github.com/markqvist/Reticulum) is a crypto-based networking
  stack that runs over LoRa (via **RNode** firmware), serial, WiFi/TCP, BLE, packet
  radio, etc. End-to-end encrypted with forward secrecy.
- **`rnsh`** is a fully interactive **remote shell over Reticulum** — literally our
  end goal — designed for low-bandwidth links (LoRa, packet radio), with a
  line-interactive mode for links down to a few hundred bits/sec.
  ([software](https://reticulum.network/manual/software.html))
- **Connectivity:** RNode connects to a host over **USB, BLE, or WiFi/TCP**.
  Android: **Sideband** app connects to an RNode over USB/BLE (but Sideband is
  *messaging*, not a shell). `rnsh` on a phone = Termux, and **Termux can't reach
  USB/BLE** (Android sandbox), so phone-side shell needs WiFi/TCP — the same Android
  wall we hit.
- **Tuning:** the `RNodeInterface` config exposes exactly the knobs we have —
  `frequency`, `bandwidth`, `spreadingfactor` (5–12), `codingrate` (5–8),
  `txpower`. ([interfaces](https://reticulum.network/manual/interfaces.html))
- **Channel access:** a custom **CSMA** MAC (carrier-sense + `persistence`/`slottime`
  backoff) — *no initiator/responder polling*. Any node transmits when the channel is clear.
- **Throughput:** same SX1262 PHY → same ceiling. Reticulum's docs say LoRa
  throughput is "hundreds of bytes per second, not kilobits"; a real RNode example
  measured ~3.12 kbps. We're in the same ballpark (and can be leaner for pure P2P).

### Meshtastic
- LoRa **messaging / mesh** app, the champion of off-the-shelf boards with **OLED/
  E-Ink screens showing live stats** (e.g. the Seeed **Wio Tracker L1**, nRF52840 +
  SX1262 + GPS + display + battery). BLE/WiFi to a phone app.
- But it's messaging/mesh, **not** a transparent serial pipe or shell.

## How we position (and what's genuinely ours)
| | Cheap transparent modules | Reticulum + rnsh | Meshtastic | **This project** |
|---|---|---|---|---|
| Transparent serial | ✅ | ~ (networking) | ❌ | ✅ |
| Reliable (ARQ) | ❌ | ✅ | ✅ (msgs) | ✅ |
| Interactive shell | ❌ | ✅ (rnsh) | ❌ | ✅ |
| Encryption | ~ | ✅ (FS) | ✅ (PSK) | ✅ (Ascon AEAD) |
| USB / BLE / WiFi | USB | ✅ all | ✅ | USB (WiFi planned) |
| Screen + stats device | ❌ | ~ | ✅ | planned |
| SF/BW/CR/power tuning | ✅ | ✅ | ✅ | ✅ |
| Lean point-to-point | ✅ | heavier | heavier | ✅ |
| Fully owned / hackable | ❌ | ✅ | ✅ | ✅ |

**Why we still build ours:** leanness/latency for a dedicated two-peer link, total
control and understanding of every layer, and a tailored "reliable LoRa serial
cable + terminal" project. We will not beat the PHY ceiling, and Reticulum is more
capable in general — if you want turnkey capability, adopt it. We want the lean,
owned thing.

## Competing head-on at point-to-point (we are NOT solving mesh)
We don't try to out-Reticulum Reticulum at networking. We pick a **narrower fight
and win it**: the best *two-node reliable serial link* over LoRa. The narrowness is
the edge — a general mesh stack must carry machinery that a dedicated 2-node link
can simply delete:

| Axis | Reticulum (general/mesh) | **Us (dedicated P2P)** | Why we can win P2P |
|---|---|---|---|
| Channel access | CSMA: carrier-sense + persistence/slottime backoff | deterministic `F_END`→SACK hand-off | with 2 nodes there's ~no contention, so CSMA's listen/backoff is pure overhead we skip |
| Framing | addressing + routing + networking headers | minimal 2-node header | no addresses/routes to carry |
| Reliability | windowed Resource transfer (general) | SACK + **BDP-sized** window tuned to the exact link | window sized per radio mode from `getTimeOnAir()` keeps the pipe full |
| Crypto | per-packet forward secrecy (heavier) | Ascon-128 AEAD + replay window (per-session forward secrecy built but opt-in) | authenticated + replay-safe at a light per-frame cost; ephemeral-key FS is `AT+FS`, still experimental |
| Speed ceiling | LoRa PHY | LoRa PHY **+ optional GFSK "turbo" (≈5×)** | we expose the chip's GFSK mode for short range |
| Shape | a network you build apps *into* | a **serial cable** you plug anything into | matches "remote console for any serial device" |

**The pitch:** Reticulum is the answer if you want a *secure mesh network*. We're the
answer if you want a **dead-simple, rock-solid, fast "LoRa serial cable"** between two
points — lower overhead, higher P2P efficiency, and a transparent serial port on each
end that any program (or a terminal) can use. Same PHY ceiling; we get closer to it
for the 2-node case and stay far simpler.

Concretely measurable head-to-head claim (to validate): **at the same SF/BW/CR, our
goodput/airtime efficiency for a sustained 2-node transfer should meet or beat
Reticulum's**, because we spend no airtime on contention or routing. See
[THROUGHPUT.md](./THROUGHPUT.md) for our bench numbers.

## Inspiration we're taking (NOT copying)
- **Config breakdown** by `frequency / bandwidth / spreadingfactor / codingrate /
  txpower` — clean and standard; worth mirroring in our config/mode presets.
- **CSMA-style channel access** as a *concept*: don't gate throughput on rigid
  initiator/responder per-window ack round-trips (our current stall lives there). Let the
  channel be used more continuously.
- **Windowed reliable transfer** sized to the bandwidth-delay product, with the
  receiver acking cumulatively/selectively rather than stop-and-wait-per-window.

## When to adopt instead of continue
If you reach a point where you want: a mature secure mesh, phone messaging that
"just works" (Sideband), or to stop maintaining a radio stack — **flash RNode +
run Reticulum/rnsh**, and build screen/stats on the Meshtastic-class ecosystem.
For a lean, owned, P2P reliable serial link + terminal, continue here.
