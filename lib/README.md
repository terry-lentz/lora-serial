# Libraries

This project's libraries are split by provenance so it's obvious what we wrote
versus what we vendored in.

## `linklayer/` — our own code (MIT, this repo)

The portable, hardware-independent transport. Built and unit-tested on the host
(`pio test -e native`), then compiled into the firmware unchanged.

| File | What it is |
|------|------------|
| `linklayer.h` | Selective-Repeat ARQ + SACK + BDP window framing (a template on the ring size, so it's header-only). |
| `modem.h` | Half-duplex initiator/responder turn loop (template on the link type — header-only). |
| `modeswitch.h` / `.cpp` | Coordinated runtime PHY mode-switch handshake. |
| `adr.h` / `.cpp` | Adaptive Data Rate (`auto`) decision logic. |
| `ll_aead.h` / `.cpp` | Ascon-128 AEAD (authenticated encryption). |
| `ll_transform.h` / `.cpp` | Compression + AES-CTR helpers over the vendored libs. |

## `third_party/` — vendored dependencies (their own licenses)

Dropped in verbatim; **not** covered by this repo's style guide. Each keeps its
upstream license file.

| Dir | Upstream | Purpose | License |
|-----|----------|---------|---------|
| `heatshrink/` | [atomicobject/heatshrink](https://github.com/atomicobject/heatshrink) | LZSS compression (tiny, no malloc) | ISC (`LICENSE`) |
| `x25519/` | TweetNaCl Curve25519 (Bernstein et al.) | ECDH key agreement for `AT+TRAIN` / proximity pairing | public domain (header) |

`platformio.ini` puts `lib/third_party` on the library path via `lib_extra_dirs`,
so includes by basename (e.g. `#include "aes.h"`) resolve unchanged.
