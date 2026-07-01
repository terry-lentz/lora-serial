# Coding standards

This project follows the **[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)**
as its baseline, with a small set of explicitly-documented carve-outs for the
embedded/RF domain. The goal is teaching-quality, publishable code: consistent,
heavily commented, and approachable to someone new to radio or embedded C++.

If something here is silent, defer to the Google guide. If the Google guide and an
embedded reality genuinely conflict, the carve-outs below win — and we say why.

## Layout: headers declare, `.cpp` defines

Per Google ("Put longer function bodies in the `.cc` file"), **logic lives in
`.cpp`; headers hold declarations, types, and short inline accessors.** Concretely:

- `src/fw_config.h` — the shared contract: build-time config, constants, the
  settings/mode types, and `extern` declarations of the globals the modules share.
- `src/fw_radio.{h,cpp}`, `src/fw_host.{h,cpp}`, `src/fw_diag.{h,cpp}`,
  `src/fw_session.{h,cpp}`, `src/fw_device.{h,cpp}` — declarations in the header,
  all implementation in the `.cpp` (mirrors RadioLib's own `Module.h`/`Module.cpp`
  layout — a good local reference). `fw_device` is the device orchestrator
  (`class Device`): the turn engine, role discovery/pairing, link recovery, ADR.
- `src/main.cpp` — defines the shared globals once, plus `setup()`/`loop()`,
  which just forward to the static-singleton `g_device`.
- Headers are self-contained (`#pragma once`, include what they use).

The portable link layer (`lib/linklayer/`) is the one place we keep substantial
logic in headers: it is a **template** library (`LinkLayer<RING>`), and templates
must be header-visible. That is consistent with Google's template guidance.

## Naming

| Kind | Convention | Example |
|------|-----------|---------|
| Functions / methods | `PascalCase` | `ApplyMode`, `Poll`, `NextTx` |
| Types (class/struct/enum) | `PascalCase` | `ModemSettings`, `LinkLayer` |
| Variables, parameters | `snake_case` | `out_len`, `freq_mhz` |
| Globals | `snake_case`, `g_` prefix | `g_link`, `g_tx_power` |
| Class private data members | `snake_case_` (trailing `_`) | `base_seq_`, `ack_pending_` |
| Struct public data members | `snake_case` (no `_`) | `cfg.bw_code` |
| Constants (`const`/`constexpr`) | `kCamelCase` | `kFreqMhz`, `kRfModes` |
| Macros | `ALL_CAPS` | `FEAT_ENC`, `CFG_SF` |

## Line length: 80 columns, hard

Every line in our own source is **≤ 80 columns** (display width — a multibyte
char like `—` counts as one). Wrap C/C++ at commas or before binary operators;
split long string literals into adjacent literals (`"a" "b"` concatenates);
reflow long `//` comments. Check before a PR (the snippet is in
[CONTRIBUTING.md](../CONTRIBUTING.md#style)).

## Comments

Comment for a beginner reading the code: explain **what a value is and why**, not
just the mechanics. In particular:

- **No bare magic numbers in logic** — pull a literal out into a named
  `constexpr`/`const` (or a `#define` where it's a build knob) with a comment, so
  the *meaning* lives next to the *value*.
- **Call out every embedded-driven choice**: anything done for determinism,
  RAM/footprint, no-heap-in-hot-path, ISR/IRAM safety, or real-time radio timing
  should say so and why.
- **Each AT command is commented** with what it does and its on-the-wire/runtime
  effect (see `fw_host.cpp`).

## Carve-outs (and why)

These deviate from a pedantic reading of the Google guide on purpose:

1. **Vendored third-party code is exempt** — `lib/third_party/heatshrink/`
   and `lib/third_party/x25519/` (TweetNaCl-derived) keep their upstream formatting and
   naming so they can be re-synced from upstream cleanly. Do not reformat them.
2. **Arduino entry points** `setup()` and `loop()` keep their framework-mandated
   names (not `Setup`/`Loop`).
3. **Wire-protocol constants stay `ALL_CAPS`** — the frame flags (`F_DATA`,
   `F_COMP`, …) and field sizes (`HDR`, `CTRW`, `MAXFRAME`, `MAXWIN`, …) name an
   on-air protocol; ALL_CAPS reads as "protocol field" and matches the format
   table in `linklayer.h`.
4. **The injected radio interface `ll::IRadio`** keeps terse verb methods
   (`tx`/`rx`/`now`) — it's a tiny hardware-abstraction seam mirrored by the real
   radio and the sim; the brevity aids readability at every call site.
5. **Crypto primitives mirror their spec's notation** — inside `ll_aead.h` the
   Ascon implementation keeps short locals like `K0`, `N0`, `S` that match NIST
   SP 800-232 / the Ascon spec, which makes it auditable against the reference.

## Tests are the safety net

Any change to the portable link layer must keep `pio test -e native` green and
should add a test for the behavior it changes. The native sim has repeatedly
caught real bugs before hardware — see [TESTING.md](./TESTING.md).
