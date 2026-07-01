# Security — current state, weaknesses, and the upgrade path

Plain-language assessment of our link crypto, the concrete weaknesses (you spotted
the big ones), and the research-backed fixes. Companion to
[RESEARCH.md](./RESEARCH.md).

> **Status (implemented):** Fixes **A + B + C** are **done and tested** —
> the link uses **Ascon-128 AEAD** (authenticated, tamper-evident), a **64-bit
> monotonic counter persisted in NVS** (no keystream reuse across reboots), and a
> **sliding-window anti-replay** check, with the **frame header authenticated** as
> associated data. **Secure pairing** is also implemented: `AT+TRAIN` derives a unique
> per-pair key via **X25519 ECDH** over the air (no secret transmitted), stored in NVS
> — so the built-in key is no longer the only secret (weakness #4).
>
> **Forward secrecy (Fix D)** is **built but OPT-IN / experimental, OFF by
> default** (`AT+FS=1`). The crypto core (per-session ephemeral X25519 + KDF) is
> sound and unit-tested (see `test/test_link`), but the *on-air* handshake as
> currently integrated runs outside the proven turn rhythm — it can fail to
> establish and interfere with runtime mode switching — so it's gated off until
> that integration is hardened (carry the ephemeral keys as link-layer payload
> rather than ad-hoc raw frames). Until then the link runs on the **static**
> AT+TRAIN/built-in key (still authenticated, replay-safe — A+B+C). The other
> caveat is that the initial *pairing* is *unauthenticated* ECDH (pair in
> proximity, compare fingerprints).

## What we had before (the old AES-CTR design — superseded)
- **AES-128-CTR** with a **hard-coded pre-shared key** in firmware.
- IV = `src ‖ 4-byte counter`; counter = `++txCtr`, and **`txCtr` reset to 0 on
  reboot**. This had all the weaknesses below; they are now fixed (A–C).

## The weaknesses (all real)
1. **No integrity / authenticity.** CTR is a raw keystream XOR with **no MAC**. An
   attacker who can't read the traffic can still **flip bits** (tamper) and we won't
   detect it. Our CRC is not cryptographic — it can be recomputed.
2. **Nonce/keystream reuse on reboot.** `txCtr=0` at boot + the same key ⇒ the first
   frames after every reboot reuse the **same keystream**. XOR two same-keystream
   ciphertexts and the keystream cancels → plaintext leaks. This is the *exact*
   textbook bug LoRaWAN 1.0 had: *"counter reset while the key remains ⇒ the cipher
   recreates the same key material — classic key-stream reuse."*
3. **No replay protection.** A recorded ciphertext frame can be **re-injected** later
   and we'd accept it.
4. **Key is the only secret, and it's extractable.** One static key in flash, same on
   both units; anyone with a device or the firmware can read it. No forward secrecy —
   if the key leaks, all past *and* future traffic is exposed.

## The fixes (research-backed)

> ✅ **A, B, C are implemented** (see `lib/linklayer/ll_aead.h` for the Ascon-128
> AEAD, validated against the official LWC known-answer vectors in
> `test/test_link`; counter/replay/nonce logic in `linklayer.h`; NVS persistence
> in `src/fw_host.cpp` — `Host::LoadSettings`/`Host::SaveSettings`, with the
> frame-counter checkpoint in `Host::PersistTxCtr`). The chosen cipher is
> **Ascon-128** (the well-specified v1.2 variant
> underlying NIST SP 800-232). Tag length is configurable (`-D LINK_TAGLEN=8|16`,
> default 8). **D is still future work.**

### What's implemented, concretely
- **Every frame is sealed** with Ascon-128 AEAD — including empty ACK frames — so the
  9-byte header (seq/ack/SACK/flags/epoch) **and** the 8-byte counter are
  authenticated as associated data. A bit-flip anywhere fails the tag and the frame
  is dropped (then ARQ-resent). *Tested:* an attacker flipping 25 % of in-flight
  frames still yields a byte-exact stream.
- **64-bit monotonic counter → nonce**, persisted to NVS every 256 frames and
  **resumed at `saved + 256` on boot**, so counters used-but-not-yet-saved are
  skipped and a nonce is never reused (the LoRaWAN-1.1 fix). *Tested:* the first
  post-reboot counter always exceeds every pre-reboot one.
- **Sliding-window anti-replay** (64-frame window): a counter is accepted once;
  replays and too-old frames are rejected; in-window reordering is tolerated.
- **Plaintext frames are rejected** on an encrypted link (no unauthenticated injection).

### Fix A — switch to an AEAD (kills #1 and, with B, #2)
Use **Authenticated Encryption with Associated Data**: encrypt + authenticate in one
pass, producing a **tag** that detects any tampering. Candidates:
- **Ascon-AEAD128** — the **NIST lightweight-crypto standard** (SP 800-232, finalized
  2025) purpose-built for constrained devices like ours, and easier to make
  side-channel-resistant. On-trend, tiny reference C. ([NIST SP 800-232](https://csrc.nist.gov/pubs/sp/800/232/final))
- **ChaCha20-Poly1305** — fast in pure software, battle-tested (WireGuard, TLS),
  already in ESP32 mbedTLS. Pragmatic, zero vendoring.
- **AES-GCM** — hardware-accelerated on ESP32-S3, but GCM nonce reuse is *even more*
  catastrophic than CTR, so only with rock-solid nonce management (Fix B).

Verdict: **Ascon-AEAD128** as the principled choice (matches "lightweight crypto
standard"), or **ChaCha20-Poly1305** as the no-vendoring pragmatic one. Both replace
AES-CTR and give us integrity for free.

### Fix B — never reuse a nonce (kills #2)
A 64-bit **monotonic frame counter** as the nonce, that **never** repeats:
- **Persist it in NVS** across reboots, and bump past the last-saved value at boot —
  this is precisely how **LoRaWAN 1.1 fixed** the 1.0 reset bug.
- Or sidestep it entirely with a fresh **random per-session nonce/key** from a
  handshake (Fix D), so the keystream space is new every session.

### Fix C — replay window (kills #3)
Carry the monotonic counter in each frame (authenticated by the AEAD tag) and have
the receiver **reject any counter ≤ the highest already seen**, with a small sliding
**replay window** to tolerate reordering — the standard IPsec/LoRaWAN scheme.

### Fix D — handshake for forward secrecy + per-session keys (kills #4)  ⚙️ built, opt-in
At every link bring-up the two ends run an automatic **ephemeral X25519**
Diffie-Hellman handshake and derive a **fresh per-session key**:

```
session_key = AsconKdf16( static_key ‖ ephemeral_DH ‖ init_pub ‖ resp_pub )
```

- **Forward secrecy:** the ephemeral private keys are never stored, so a later
  leak of the static (paired) key can't rederive a past session key. In fact a
  passive attacker who *holds* the static key still can't decrypt a recorded
  session — they'd also need an ephemeral private, which never leaves RAM.
- **Mutual auth (implicit):** the static key is mixed into the KDF, so a party
  without it derives a different key and the first AEAD frame simply fails its
  tag — no data flows to an impostor. No separate auth tag is sent in the
  handshake.
- **Per-session nonces:** a fresh key per session also means the AEAD counter
  space is new every time (this is the "random per-session key" route under
  Fix B).

This is a **minimal construction from the primitives we already ship** (X25519 +
the Ascon KDF), **not** the literal Noise Protocol Framework — same goals
(forward secrecy + per-session keys + mutual auth via the paired key), far less
code on the MCU. The **crypto core is sound and unit-tested** in `test/test_link`
(key agreement, forward-secrecy property, wrong-key rejection, tamper detection);
see [`lib/linklayer/session.h`](../lib/linklayer/session.h).

> ⚠️ **Opt-in / experimental (`AT+FS=1`, OFF by default).** The current radio
> integration (`src/fw_session.cpp`) drives the handshake with ad-hoc raw frames
> and a blocking listen *outside* the proven half-duplex turn rhythm. On hardware
> that made it fail to establish reliably and, worse, **interfere with runtime
> mode switching** (it consumes turns the mode-switch handshake needs). So it's
> gated off by default while the integration is reworked to carry the ephemeral
> keys as ordinary link-layer payload (encrypted under the static key) on the
> normal turn schedule. Query state with `AT+SESSION?`.
> ([Noise, for reference](https://noiseprotocol.org/noise.html))

## Chosen approach & order
1. **AEAD + monotonic-counter-in-NVS + replay window** (Fixes A+B+C). This eliminates
   the three concrete bugs — tampering, nonce reuse, replay — while keeping the
   pre-shared key. Biggest security gain for the effort; unit-testable in the sim.
2. **Associated data:** authenticate the frame header (seq/flags/counter) too, so an
   attacker can't rewrite headers.
3. ⚙️ **Per-session X25519 handshake** (Fix D) for forward secrecy + per-session keys,
   so a key leak isn't catastrophic — crypto built + tested, but **opt-in** until the
   on-air integration is hardened (see Fix D).
4. **Later — per-device keys / key provisioning** instead of one shared firmware key,
   and an *authenticated* pairing (today's `AT+TRAIN` ECDH is proximity + fingerprint).

## Honest notes
- This is a **private point-to-point** link, not a high-assurance system — but Fixes
  A–C (now implemented) remove the embarrassing bugs (bit-flipping, keystream reuse,
  in-session replay).
- All of it is unit-tested in the native sim (KAT conformance, tamper detection,
  replay rejection, counter monotonicity across a simulated reboot) **and** verified
  on hardware (byte-exact encrypted transfer; AT-mode link query).
- **In-session replay** is fully blocked by the RAM sliding window. **Cross-session**
  replay (capture frames, force a reboot, replay) is open in the default config because
  the RX counter isn't persisted to NVS (flash wear). Fix D (forward secrecy) closes it
  — each session derives a fresh key so old frames fail the new key's AEAD tag — but Fix
  D is **opt-in** today (`AT+FS=1`), so by default this cross-session window remains.
- The built-in/paired key is **static and extractable from firmware**. In the default
  config it's the *only* confidentiality secret, so treat it as "keeps casual
  eavesdroppers out," not "resists a determined attacker with a device." Enabling Fix D
  (once hardened) makes a static-key leak non-catastrophic — forward secrecy would keep
  past/recorded sessions safe even then (a passive attacker also needs an ephemeral
  private, which never leaves RAM). Per-device provisioning + authenticated pairing
  (item 4) closes the rest.
