# Security

What the link does to protect your data — and, just as important, what it
doesn't. For the reasoning behind these choices see [RESEARCH.md](./RESEARCH.md);
for how the crypto got here (the AES-CTR design it replaced and the bugs that
drove the change) see [CAPABILITIES_JOURNEY.md](./CAPABILITIES_JOURNEY.md).

## What the link protects

- **Authenticated encryption — Ascon-128 AEAD.** Every frame, down to empty ACKs,
  is sealed with Ascon-128 (the NIST lightweight-crypto standard, SP 800-232;
  validated against the official known-answer vectors in `test/test_link`). The
  9-byte header (seq / ack / SACK / flags / epoch) **and** the 8-byte counter are
  authenticated as associated data, so a bit-flip anywhere fails the tag and the
  frame is dropped and re-sent by ARQ — tampering is caught, never silently
  delivered. Tag length is a build knob (`-D LINK_TAGLEN=8|16`, default 8), and
  plaintext frames are rejected on an encrypted link.
- **Nonces never repeat — even across reboots.** The AEAD nonce is a 64-bit
  monotonic counter, persisted to NVS every 256 frames and resumed at *saved+256*
  on boot so any counters used-but-not-yet-saved are skipped. It never rewinds, so
  a nonce is never reused (the failure that would leak keystream). See
  `Host::PersistTxCtr` and `LINK_CTR_STRIDE`.
- **Replay is rejected.** A 64-frame sliding window accepts each counter exactly
  once; a replayed or too-old frame is dropped, while in-window reordering is
  still tolerated.
- **A key unique to your two boards.** `AT+TRAIN` derives a per-pair key over the
  air via X25519 ECDH — the shared secret is never transmitted — and stores it in
  NVS so it survives reflashes. First-boot **proximity pairing** runs this
  automatically at low power; until a pair is trained the link uses a shared
  built-in fallback key (still authenticated and replay-safe). `AT&F` wipes the
  paired key.
- **Forward secrecy (opt-in).** `AT+FS=1` adds a per-session ephemeral X25519
  handshake that derives a fresh session key
  (`session_key = AsconKdf16(static_key ‖ ephemeral_DH ‖ init_pub ‖ resp_pub)`),
  so a later leak of the static key can't rederive a past session; the static key
  is mixed in, so an impostor without it fails the very first tag. The crypto core
  is unit-tested — but it's **off by default** (see Limitations).

All of the above is exercised in the native sim (KAT conformance, tamper
detection, replay rejection, counter monotonicity across a simulated reboot) and
verified on hardware with byte-exact encrypted transfers.

## Limitations

This is a **private point-to-point** link, not a high-assurance system — its
current edges:

- **Cross-session replay (default config).** The replay window lives in RAM and
  isn't persisted (to spare flash wear), so an attacker who records frames, forces
  a reboot, and replays them is *not* blocked by default. Forward secrecy
  (`AT+FS=1`) closes this — a fresh per-session key makes old frames fail the new
  tag — but it's opt-in.
- **The static key is extractable.** The built-in and paired keys are static and
  readable from firmware / flash; in the default config the paired key is the only
  confidentiality secret. Treat it as "keeps casual eavesdroppers out," not
  "resists a determined attacker who has a device." Per-device key provisioning is
  future work.
- **Pairing is unauthenticated.** `AT+TRAIN`'s ECDH has no authentication of its
  own — pair in physical proximity and compare fingerprints to rule out a
  man-in-the-middle. To check any two units share the same key at a glance, run
  **`AT+KEY?`** on each: it prints a short one-way fingerprint (`AsconKdf16` of
  the static key, 8 hex chars) that matches iff the keys match, without ever
  exposing the key. (The L1 shows the same fingerprint on its INFO screen.)
- **Forward secrecy is experimental.** Its on-air handshake currently runs outside
  the proven half-duplex turn rhythm, so it can fail to establish and can interfere
  with runtime mode switching — which is why it stays off by default. Check its
  state with `AT+SESSION?`.

## The road ahead

- Harden the forward-secrecy handshake (carry the ephemeral keys as ordinary
  link-layer payload on the normal turn schedule) and enable it by default.
- Per-device key provisioning and an authenticated pairing step, so a shared
  firmware key and proximity-only trust aren't the floor.
