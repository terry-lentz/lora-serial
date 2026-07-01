# Research log — making a 2-node LoRa link faster & more reliable

Survey of current techniques (with sources), what each does in plain language,
whether it fits our **point-to-point reliable serial** goal, and what we chose to
implement. Companion to [DESIGN.md](./DESIGN.md) (the transport spec) and
[SW_ALTERNATIVES.md](./SW_ALTERNATIVES.md) (prior art / positioning).

Ground truth: **nothing beats the PHY ceiling.** Same SX1262 → same physics. These
techniques are about (a) getting *closer* to the ceiling, (b) surviving loss without
airtime blowup, and (c) picking the *right* ceiling for the conditions.

---

## The big synthesis first
Two insights shaped our choices:

1. **Interactive and bulk are different problems.** A shell wants **low latency**
   (small messages, fast round-trips). A file transfer wants **high sustained
   throughput** (one-way, loss-tolerant). The optimal protocol differs — so we plan
   **two operating modes** over the same link: *interactive* (tight ARQ) and *bulk*
   (rateless / big-window).
2. **Modulation is a lever, not just SF/BW.** The SX1262 also does **GFSK up to 300
   kbps** — ~5× LoRa's max. For short range, that's a different (higher) ceiling
   entirely. So the fastest rung isn't just low-SF LoRa; it can be GFSK (the
   planned `ludicrous` mode).

---

## Techniques surveyed

### 1. Selective-Repeat ARQ + SACK bitmap + BDP-sized window  ✅ ADOPT (core)
- **Idea:** resend only the frames that were actually lost (not the whole window),
  ack a whole burst with one compact bitmap, and size the in-flight window to the
  **bandwidth-delay product** so the pipe never empties.
- **Why:** this is the textbook fix for exactly our stall (window ≪ BDP → stop-and-
  wait) and for Go-Back-N's wasteful whole-window resends. SACK/bitmap acks are the
  standard high-delay-link efficiency tool. The LoRa **MPLR** image protocol uses
  bit-vector acks specifically to cut ack count/wait.
- **Fit:** perfect, 2-node. **This is the heart of [DESIGN.md](./DESIGN.md).**
- Sources: SACK/BDP (TCP long-fat-network literature); selective-repeat ARQ analyses
  (IEEE); MPLR bit-vector acks.

### 2. GFSK / (G)FSK modulation mode  ✅ ADOPT (the `ludicrous` mode)
- **Idea:** the SX1262 supports **(G)FSK up to 300 kbps**, vs **62.5 kbps** max for
  LoRa. RadioLib supports FSK on the SX126x.
- **Why:** for short range (boards close, strong SNR), GFSK is a *completely higher
  ceiling* — potentially ~5× the fastest LoRa mode. Loses LoRa's below-noise-floor
  range, so it's a short-range option only.
- **Fit:** add as the top `ludicrous` rung (short range, max speed). Orthogonal to the
  ARQ work — same transport, different PHY.
- Source: [SX1262 product page / datasheet](https://www.semtech.com/products/wireless-rf/lora-connect/sx1262) (62.5 kbps LoRa / 300 kbps FSK).

### 3. Rateless / fountain codes (LT, Raptor) — RALoRa  ✅ ADOPT (bulk mode)
- **Idea:** the sender generates an *endless* stream of coded packets from the data;
  the receiver recovers the whole file once it has received **any K** of them. **No
  per-packet ACKs, no retransmission round-trips** — you just keep sending until the
  receiver says "got it."
- **Why:** this *eliminates* the ARQ round-trip stall for one-way bulk transfer and
  naturally adapts to loss (lossy link just needs a few more packets). **RALoRa**
  (Yang, Liu & Du, IEEE/ACM ToN 2024) [R2] used rateless coding for LoRa link
  adaptation and extended node lifetime ~66%. There's even rateless FUOTA for LoRaWAN.
- **Fit:** ideal for our **bulk transfer mode** (files), where there's no need for
  interactive round-trips. Medium effort (implement an LT/fountain codec).
- Sources: [RALoRa, IEEE/ACM ToN 2024](https://dl.acm.org/doi/abs/10.1109/TNET.2024.3392342); Raptor/rateless coding literature.

### 4. Channel Activity Detection (CAD)  ✅ ADOPT (turn-around + listen)
- **Idea:** a low-power radio op (`Tsym + 32/BW`) that detects a preamble without
  full receive. Semtech's intended CSMA primitive for LoRa.
- **Why:** lets the receiver cheaply tell "is the peer transmitting?" → cleaner,
  faster, lower-power **turn-around detection** and a safety net against talking over
  each other. Cheaper than continuous RX between turns.
- **Fit:** use it for the `F_END`→SACK hand-off and idle listening. Low effort, real
  power/robustness win.
- Source: [Semtech AN1200.85 — CAD](https://www.semtech.com/uploads/technology/LoRa/cad-ensuring-lora-packets.pdf).

### 5. Implicit header mode  ✅ ADOPT (free airtime)
- **Idea:** if both ends agree on a fixed frame length, drop LoRa's explicit header →
  fewer symbols on air per frame.
- **Why:** small, free throughput win for fixed-size data frames (our data frames are
  a fixed payload size).
- **Fit:** easy; enable for the bulk data frames once frame sizing is fixed.

### 6. HARQ with Incremental Redundancy (IR-HARQ)  ⏳ LATER
- **Idea:** combine FEC with ARQ — on a decode failure, send *additional* redundancy
  (incremental) and combine with the first copy, instead of resending the whole
  packet. LDPC/Raptor based; "AMC + HARQ" adapts the rate too.
- **Why:** better throughput on *marginal* links (recovers without full resends).
- **Verdict:** powerful but heavy (needs rate-compatible FEC codec). Defer until the
  core ARQ + modes are solid; rateless coding (#3) already gives much of the benefit
  for bulk.
- Sources: IR-HARQ with LDPC/Raptor (IEEE/arXiv); image-over-LoRa-with-HARQ (Elsevier 2025).

### 7. Adaptive Modulation & Coding / ADR / ML rate adaptation  ⚙️ IN PROGRESS
- **Idea:** continuously pick SF/BW/CR/power (and modulation) from measured link
  quality. **ILoRa** (Qi, Li, Zhang & Wang, 2025) [R3] even uses a neural net for
  sub-symbol rate adaptation.
- **Key realisation (the decomposition):** ADR is really *two* problems, and the
  literature only covers one of them. (1) The **policy** — when/what to switch —
  is the classic LoRaWAN-ADR work: SNR margin + **hysteresis** to avoid the famous
  SF7↔SF12 oscillation. Ours (`lib/linklayer/adr.h`) does exactly that, and is
  loss-aware (uses retransmit rate, not just SNR). (2) The **mechanism** — how two
  ends *atomically* change PHY on a half-duplex link where a mismatch = mutual
  deafness — is **not a LoRa-ADR problem at all**: LoRaWAN sidesteps it (the
  network server commands SF with confirmed downlinks; endpoints never go mutually
  deaf). It is a **distributed atomic-reconfiguration** problem, better served by
  the distributed-systems / radio-handover literature (TLA+-verified
  reconfiguration [R14]; the "coordinate-to-switch at an agreed instant" pattern of
  802.11 CSA / Bluetooth AFH).
- **What we built:** a **formally specified** mode-switch state machine
  ([MODE_SWITCH_SPEC.md](./MODE_SWITCH_SPEC.md)) — make-before-break handshake +
  probation revert + a dead-link **rendezvous** backstop — validated by an
  **exhaustive adversarial model-checker** in `test/test_link` (`test_msm_*`): all
  4096 loss patterns × {up, down, GFSK, double-switch, reboot} re-converge, and a
  teeth test proves the rendezvous is load-bearing for reboots. An **integration
  sim** (`test/test_sim`) then drives SNR up/down to trigger switches mid-stream
  and proves the byte stream stays exact, and a **physics layer** charges real
  per-mode time-on-air so timing-induced throughput faults can be reproduced
  deterministically (with correct timing turbo ≈ 4.5× medium; a too-tight
  inter-frame timeout collapses it — the symptom hardware showed).
- **Verdict:** manual named modes are solid; `auto`/ADR mechanism is sim-validated;
  remaining work is the hardware throughput/timing of the individual fast modes
  before relying on auto.
- Sources: ILoRa (Elsevier 2025); LoRaWAN ADR survey [R15]; "How Agile is the ADR
  Mechanism of LoRaWAN?" [R16]; ADR-Lite [R17]; formal reconfiguration [R14].

### 8. LR-FHSS (Long Range FHSS)  ❌ SKIP (for now)
- Semtech's newer modulation on the SX1262, aimed at *network capacity/robustness*
  (many uplinks), not point-to-point throughput. Not useful for our 2-node speed goal.

---

## Chosen approach & order
Layered so each step is independently testable in the native sim before hardware:

1. **Core transport** (DESIGN.md): Selective-Repeat + aggregated SACK bitmap +
   BDP-sized window + signaled `F_END` hand-off. *Fixes the stall; makes bulk
   reliable.*
2. **CAD-based turn-around** + **implicit header** for the data frames. *Cheap
   efficiency + robustness.*
3. **Named modes** over `freq/bw/sf/cr/txpower`, timing/window derived per mode from
   `getTimeOnAir()`, bench-tested (THROUGHPUT.md). Add a **GFSK `ludicrous`** mode.
4. **Bulk transfer mode** using **rateless/fountain coding** (no ARQ round-trips) for
   files; keep tight ARQ for interactive.
5. **Later:** IR-HARQ for marginal links; full ADR / ML rate adaptation.

## What other firmware actually does — Meshtastic, RNode/Reticulum, LoRaWAN (2026-06-30)

Surveyed the two production SX126x firmwares we respect, plus the ADR/power-control
literature, to ground our RX/recovery/power/ADR choices in what's proven. Sources
in the **References** block ([R20]–[R27]).

### RX path — continuous + interrupt-driven is the proven shape
- **Both** Meshtastic (`SX126xInterface`/`RadioLibInterface`) and RNode (`sx126x.cpp`)
  run **continuous RX** (`RX_TIMEOUT_INF` / opcode timeout `0xFFFFFF`), **interrupt-
  driven** off DIO1/DIO0. The ISR does only a notify; the worker reads the FIFO.
  Meshtastic **re-arms** RX in its radio task after each RX/TX; RNode just stays in
  continuous RX and reads on the IRQ. [R20][R22]
- This validates our `INTR_RX` design (docs/INTERRUPT_RX.md): the responder "hang" we
  fought was a **wedged chip** (from `resetAGC`'s `sleep()`), *not* the task. Our
  simpler `RX_CONTINUOUS` (main-loop, re-arm before processing) is a lower-risk first
  step toward the same win.
- **Missed-edge poll:** Meshtastic treats DIO1 as edge-triggered and polls
  `RADIOLIB_IRQ_RX_DONE` as a backup (`pollMissedIrqs`/`checkRxDoneIrqFlag`) — once
  right after arming and periodically. We already do this in the per-frame path
  (`kIrqPollMs`); keep it when going continuous. [R20]

### Radio recovery / the "goes deaf" errata — what it really is
- The SX126x "deafness" is a **strong signal within ~2.5 MHz desensitizing RX and
  inflating reported RSSI by up to ~35 dB** (RadioLib errata wiki) — exactly our
  close-range desense. Best primary fix: **lower TX power / separate the radios**
  (CAPABILITIES_JOURNEY 21). [R21]
- Recovery, if needed, is a **guarded `resetAGC()`** (Meshtastic ships one): warm
  `sleep(true)` → `standby(RC)` → `CALIBRATE_ALL` → `calibrateImage` → re-apply RF
  switch + RX-boost, **never run mid-packet**, and **re-apply reg `0x08B5` bit 0**
  after the calibrate (else RX sensitivity silently dies ~60 s later). We learned the
  hard way that calling it **un**guarded **wedges the chip** (`CHIP_NOT_FOUND`, power-
  cycle to recover — RadioLib #740/#683). Full recipe in docs/RADIO_ERRATA.md. [R20]
- RNode applies **errata 15.4** (reg `0x0736` bit 2 after every `SetPacketParams`, or
  LoRa RX fails *silently* while TX works) and an **RSSI/CAD-hang unlatch** (re-issue
  receive on a false preamble). RadioLib applies 15.4 for us inside `setPacketParams`.
  For a stuck TX, RNode simply **reboots the MCU** after a 20 s TX-done timeout — our
  layered radio-stuck watchdog is finer-grained than that. [R22]

### TX power & ADR — nobody adapts; this is *our* niche
- Meshtastic and RNode/Reticulum both use **fixed, manually-configured TX power and
  fixed SF/BW/CR** that must match on both ends; **no ADR, no auto-power.** Meshtastic
  *requires* identical presets mesh-wide. Link quality (RSSI/SNR) is a *local* readout
  reported to the host, never fed back over the air to the peer. [R20][R22][R23]
- A symmetric 2-node link is the **ideal** place for closed-loop control (no shared-
  preset constraint, no many-to-one fairness). The theory is settled: **Foschini–
  Miljanic 1993** [R24] — `p(t+1) = (γ_target/γ_measured)·p(t)` provably converges to
  the minimum power for a target SINR, and **collapses to a trivially stable scalar
  loop with no co-channel interference** (our case). This is exactly our peer-SNR
  design (sim `test_autopower_peer_snr_holds_asym`): each side reports the SNR it
  hears the peer at; the peer drives **its own** power so that reported SNR holds a
  target margin above the demod floor. Closest published P2P loops: Philip & Singh
  2021 [R25], Silva 2023 [R26] ("SlidingChange": decide on a *window*, not per-packet).

### LoRaWAN ADR algorithm — confirms our `lib/linklayer/adr.h`
- Canonical math is **Semtech "simple rate adaptation recommended algorithm," Rev 1.0,
  Oct 2016** [R27] (NOT the LoRa Alliance spec, which defines only the MAC commands):
  `SNRmargin = SNRm − RequiredSNR(SF) − margin_db`, `Nstep = trunc(SNRmargin/3)`.
  `SNRm` = **max over the last 20 frames**. Required-SNR table = **SX1276 datasheet
  Table 13**: SF7 −7.5 dB, −2.5 dB per SF up to SF12 −20 dB — **matches our
  `kModeSnrFloor`**. `margin_db` is 10 dB (Semtech-typical/ChirpStack) — ours is 6 dB
  (`kAdrMarginDb`), a touch more aggressive; revisit on hardware.
- **Asymmetry is the key stability rule:** spend surplus on **rate first, then lower
  power**; on a deficit **raise power, never auto-lower rate** ("leads to constantly
  oscillating values"). Step **up slowly** (needs N stable frames), **fall back
  eagerly**. Loss drives **retransmits**, not rate. Our ADR already does loss-aware,
  hysteretic, step-down-eager decisions — consistent with this. Device-side back-off
  (`ADR_ACK_LIMIT=64`/`ADR_ACK_DELAY=32`, power→max then DR→robust) is a clean
  small-state-machine pattern worth mirroring as a dead-link fallback. [R27]
- **Damping (why ADR/power loops oscillate, and the fixes):** filter SNR (EMA/window
  of 20), a static margin dead-band, ~2.5–3 dB quantized steps, and gate on frame
  counts — all things to honor when we turn auto-power/ADR on. ADR is for **stable**
  links only (Semtech: not for mobile). [R27]

**Net for us:** RX path and recovery should follow the Meshtastic/RNode proven shape
(continuous + interrupt + missed-edge poll + guarded recovery, *no blind sleep*);
auto-power and ADR are our own value-add (no one else adapts a P2P link), and our
existing designs already match the literature — they just need hardware validation
once the bench is back and continuous RX is confirmed. Order stays: **stability
first, then auto-power, then ADR** (the user's sequencing).

## Honest notes
- Every step must be proven in the sim (loss, reordering, BDP windowing, rateless
  decode) before flashing — that's how we caught real bugs before.
- The PHY ceiling is fixed; these get us closer to it and keep us reliable under
  loss. The biggest *absolute* speed jump available is the **GFSK `ludicrous` mode** for
  short range; the biggest *reliability/bulk* jump is **rateless coding**.
- All point-to-point. Multi-node is out of scope (→ Reticulum/Meshtastic).

---

## References
Links and authors for the work cited above. Where an author list is long or I could
not verify it from an open page, it's marked "see link" rather than guessed.

**Reliable transport · ARQ · coding**
- **[R1]** M. Mathis, J. Mahdavi, S. Floyd, A. Romanow. *TCP Selective Acknowledgment
  Options.* IETF RFC 2018, 1996. — basis for our aggregated-bitmap (SACK) ack.
  <https://www.rfc-editor.org/rfc/rfc2018>
- **[R2]** Kang Yang, Miaomiao Liu, Wan Du. *RALoRa: Rateless-Enabled Link Adaptation
  for LoRa Networking.* IEEE/ACM Transactions on Networking, vol. 32, no. 4, Aug 2024.
  DOI 10.1109/TNET.2024.3392342. <https://ieeexplore.ieee.org/document/10507855/>
- **[R3]** Xiaoke Qi, Haiyang Li, Dian Zhang, Lu Wang. *ILoRa: Interleaving-driven
  neural network for rate adaptation in LoRa communications.* Computer Communications,
  2025. <https://www.sciencedirect.com/science/article/abs/pii/S0140366425002440>
- **[R4]** *Deep Learning based Payload Optimization for Image Transmission over LoRa
  with HARQ.* Elsevier, 2025 (authors: see link).
  <https://www.sciencedirect.com/science/article/pii/S254266052500215X>
- **[R5]** *AMC and HARQ: How to Increase the Throughput.* arXiv:1802.06933 (authors:
  see link). <https://arxiv.org/abs/1802.06933>
- **[R6]** *Variable-rate Retransmissions for Incremental Redundancy Hybrid ARQ.*
  arXiv:1207.0229 (authors: see link). <https://arxiv.org/abs/1207.0229>
- **[R7]** Selective-Repeat ARQ for high-speed point-to-(multi)point links — survey of
  IEEE analyses (e.g. SIGCOMM/IEEE Xplore). <https://dl.acm.org/doi/10.1145/18172.18205>

**LoRa PHY · radio techniques**
- **[R8]** Semtech. *SX1261/2 sub-GHz RF Transceiver — Data Sheet* (LoRa ≤ 62.5 kbps;
  (G)FSK ≤ 300 kbps). <https://www.semtech.com/products/wireless-rf/lora-connect/sx1262>
- **[R9]** Semtech. *Channel Activity Detection: Ensuring Your LoRa Packets Are Sent.*
  Application Note AN1200.85.
  <https://www.semtech.com/uploads/technology/LoRa/cad-ensuring-lora-packets.pdf>
- **[R10]** Stuart Robinson. *Large Data Transfers with LoRa.* StuartsProjects, 2021.
  <https://stuartsprojects.github.io/2021/02/26/Large-Data-Transfers-with-LoRa-Part1.html>

**Cryptography** (detailed in [SECURITY.md](./SECURITY.md))
- **[R11]** NIST. *SP 800-232: Ascon-Based Lightweight Cryptography Standards for
  Constrained Devices*, 2025. Ascon by C. Dobraunig, M. Eichlseder, F. Mendel,
  M. Schläffer. <https://csrc.nist.gov/pubs/sp/800/232/final>
- **[R12]** Trevor Perrin. *The Noise Protocol Framework*, rev. 34, 2018.
  <https://noiseprotocol.org/noise.html>
- **[R13]** *LoRaWAN Security: An Evolvable Survey on Vulnerabilities, Attacks and
  their Systematic Mitigation.* ACM Trans. Sensor Networks, 2022 (authors: see link)
  — source for replay-prevention via monotonic frame counters + MIC, and the
  counter-reset key-stream-reuse bug. <https://dl.acm.org/doi/10.1145/3561973>

**ADR · mode-switch coordination · formal methods**
- **[R14]** W. Schultz et al. *Formal Verification of a Distributed Dynamic
  Reconfiguration Protocol.* ACM CPP 2022 — the atomic-reconfiguration model our
  mode-switch state machine is checked against.
  <https://dl.acm.org/doi/10.1145/3497775.3503688>
- **[R15]** *A Survey of LoRaWAN Adaptive Data Rate Algorithms* (CSIR) — oscillation
  and hysteresis as the policy-level lesson.
  <https://researchspace.csir.co.za/server/api/core/bitstreams/a2f1901a-c9e7-4294-99f8-7ccd5bd32c2e/content>
- **[R16]** *How Agile is the Adaptive Data Rate Mechanism of LoRaWAN?* arXiv:1808.09286.
  <https://arxiv.org/pdf/1808.09286>
- **[R17]** *ADR-Lite: A Low-Complexity Adaptive Data Rate Scheme for LoRa.*
  arXiv:2210.14583. <https://arxiv.org/pdf/2210.14583>

**Production firmware RX/recovery/power patterns (surveyed 2026-06-30)**
- **[R20]** Meshtastic firmware — `src/mesh/SX126xInterface.cpp`,
  `src/mesh/RadioLibInterface.cpp`: continuous interrupt-driven RX + per-packet
  re-arm, `pollMissedIrqs`/`checkRxDoneIrqFlag`, the guarded `resetAGC()` (warm
  sleep→recalibrate, reg `0x08B5` bit-0 re-apply), fixed TX power, 60 s busyTx
  reboot watchdog. <https://github.com/meshtastic/firmware>
- **[R21]** RadioLib *Radio Errata Notes* (SX126x AGC desense / RSSI inflation;
  `resetAGC`/`restartAGC`). <https://github.com/jgromes/RadioLib/wiki/Radio-Errata-Notes>
  — plus the `sleep()`-wedge / `CHIP_NOT_FOUND` reports RadioLib #740, #683.
- **[R22]** RNode firmware — `sx126x.cpp` (continuous RX, errata 15.4 reg `0x0736`
  bit 2 after `SetPacketParams`, false-preamble RSSI/CAD unlatch, TX-timeout
  `hard_reset()`=MCU reboot), `Utilities.h`, `RNode_Firmware.ino`.
  <https://github.com/markqvist/RNode_Firmware>
- **[R23]** Reticulum `RNS/Interfaces/RNodeInterface.py` — fixed SF/BW/CR/power,
  validated to match both ends; RSSI/SNR are a local readout to the host, not an
  over-air peer report (peer feedback only at the app layer via `rnprobe`).
  <https://github.com/markqvist/Reticulum>

**TX-power control · ADR algorithm**
- **[R24]** G. J. Foschini, Z. Miljanic. *A simple distributed autonomous power
  control algorithm and its convergence.* IEEE Trans. Veh. Tech. 42(4):641, 1993.
  DOI 10.1109/25.260745 — the convergence proof our peer-SNR loop rests on.
- **[R25]** A. M. Philip, R. Singh. *Adaptive Transmit Power Control Algorithm for
  Dynamic LoRa Nodes.* Sustainable Computing 32:100613, 2021.
  DOI 10.1016/j.suscom.2021.100613 — closest published P2P LoRa TX-power loop.
- **[R26]** F. Silva et al. *Adaptive Parameters for LoRa-Based Networks
  Physical-Layer.* Sensors 23(10):4597, 2023. DOI 10.3390/s23104597 —
  "SlidingChange": decide on a window, not per-packet (the stability lesson).
- **[R27]** Semtech. *LoRaWAN — simple rate adaptation recommended algorithm*, Rev
  1.0, Oct 2016 — the canonical ADR math (`Nstep=trunc(margin/3)`, max-of-20). Plus
  SX1276 datasheet Rev 7 Table 13 (required-SNR-per-SF) and LoRaWAN L2 1.0.4
  §4.3.1.1 / RP002 (`ADR_ACK_LIMIT`/`DELAY` back-off).
