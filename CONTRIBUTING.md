# Contributing

Thanks for your interest! This is a focused project with a deliberately narrow
scope — please read the scope note before proposing big features.

## Scope (what this is, and isn't)

This is a **reliable, encrypted, transparent point-to-point serial link over LoRa** —
a "wireless serial cable" for two nodes. It is **not** a mesh or a network stack.

- **In scope:** anything that makes the two-node link more reliable, faster, more
  secure, easier to deploy, or better documented; support for more LoRa radios via
  RadioLib; new range/speed modes; host-side tooling.
- **Out of scope:** mesh routing, multi-hop, many-node addressing, LoRaWAN. If you
  want those, [Reticulum](https://reticulum.network/) and
  [Meshtastic](https://meshtastic.org/) already do them well. The closest sibling in
  *this* niche is [SparkFun LoRaSerial](https://github.com/sparkfun/SparkFun_LoRaSerial).

## Building & testing

The portable link layer has a native test suite that runs on your PC (no hardware):

```bash
pipx install platformio        # or: pip install platformio
pio test -e native             # link-layer + modem sim tests (loss, reorder, AEAD, large transfers)
```

Firmware builds (don't need hardware to compile-check):

```bash
pio run -e node_raw                      # build the transparent-serial firmware
```

Flash with the helper (native USB needs a 1200-baud touch, not `pio -t upload`):

```bash
./tools/upload_flash.sh node_raw /dev/ttyACM0
```

**Please run `pio test -e native` before opening a PR**, and add a test for any
link-layer change. The sim has caught real bugs (compression edge cases, windowing,
nonce handling) before they reached hardware — keep that bar. See
[docs/TESTING.md](./docs/TESTING.md) for how the two simulators work (a fast
deterministic in-memory channel + a timing-faithful threaded half-duplex sim) and how
to extend them.

## Sign your work (Developer Certificate of Origin)

We use the [Developer Certificate of Origin](https://developercertificate.org/)
(DCO): a lightweight, one-line-per-commit certification that you wrote the patch
or otherwise have the right to submit it under this project's license. It is
**not** a copyright assignment, and there's no CLA to sign.

Add a `Signed-off-by` line to each commit with `-s`:

```bash
git commit -s -m "Fix nonce handling on mode switch"
```

which appends a line matching your Git author identity:

```
Signed-off-by: Jane Developer <jane@example.com>
```

**Every commit in a PR must be signed off.** An automated DCO check runs on each
pull request and flags any that are missing; if you forget, sign off the whole
branch at once and force-push:

```bash
git rebase --signoff main && git push --force-with-lease
```

Sign-off (`-s`) is the DCO and is required. It is separate from cryptographic
commit signing (`-S`), which we don't require from contributors (though it's
welcome).

## Code layout

- `lib/linklayer/` — the portable, radio-agnostic data-link layer (ARQ, SACK, AEAD,
  compression). No Arduino/RadioLib deps, so it compiles natively for tests.
- `lib/third_party/x25519/`, `lib/third_party/heatshrink/` — **vendored**
  third-party code (left in upstream style — see Style below).
- `src/fw_config.h` — the shared contract: build-time config, constants, the
  settings/mode types, and `extern` declarations of the shared globals.
- `src/fw_radio.{h,cpp}` — `class Radio` (`g_radio`): LED, DIO1 ISR, the
  interrupt RX task, mode table + `ApplyMode`, `Tx`/`Rx`, and per-mode timing.
- `src/fw_host.{h,cpp}` — `class Host` (`g_host`): USB↔link glue (output ring,
  PSRAM ingest ring, AT command mode + `AT+TRAIN` pairing, NVS settings).
- `src/fw_diag.{h,cpp}` — `class Diag` (`g_diag`): software + task watchdogs,
  crash breadcrumbs, the `AT+DIAG` report.
- `src/fw_device.{h,cpp}` — `class Device` (`g_device`): the orchestrator —
  `Setup()`/`Loop()`, role discovery/pairing, the turn engine, recovery, ADR.
- `src/main.cpp` — ~54 lines: defines the shared globals once, plus `setup()`/
  `loop()`, which just forward to the static-singleton `g_device`.
- `host/` — host-side tools: `raw_verify.py` (byte-exact throughput check) and
  `atcmd.py` (AT-command helper). No host daemon is required — the link is a plain
  serial port, so OS `agetty` handles login directly.
- `test/` — native unit/integration tests (Unity); see [docs/TESTING.md](./docs/TESTING.md).
- `docs/` — design, security, throughput, testing, coding standards, research, and
  the radio primer.
- `deploy/` — systemd unit + udev rule for a permanent login console.

## Style

We follow the **[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)**
with a few documented embedded carve-outs — the full policy (naming table,
header/source split, comment rules, carve-outs) is in
[docs/CODING_STANDARDS.md](./docs/CODING_STANDARDS.md). In short: `PascalCase`
functions/types, `snake_case` variables, `kConstant` constants, `ALL_CAPS` macros;
logic in `.cpp`, declarations in `.h`; comment for a beginner and explain *why*.
New radio behavior should auto-derive its timing from `getTimeOnAir()` so it works
at any SF/BW.

**Line length: 80 columns, hard.** Every line in our own source
(`src/`, `lib/linklayer/`, `test/`, `host/`) is wrapped to **at most 80 columns**
(display width — a multibyte UTF-8 char like `—` counts as one column). Keep it that
way: it makes side-by-side diffs and small-terminal review painless. Wrap C/C++ at
commas or before binary operators with the continuation indented to match; split long
string literals into adjacent literals (`"abc" "def"` concatenates) without changing a
byte; reflow long `//` comments across multiple `//` lines. In Python, prefer
parenthesised implicit continuation over backslashes.

Check it before a PR:

```bash
# lists any line wider than 80 columns in our code (ignores vendored libs)
python3 - <<'PY'
import glob
ours = (glob.glob('src/**/*.*', recursive=True)
      + glob.glob('lib/linklayer/**/*.*', recursive=True)
      + glob.glob('test/**/*.*', recursive=True)
      + glob.glob('host/**/*.py', recursive=True))
for f in ours:
    for n, l in enumerate(open(f, encoding='utf-8'), 1):
        if len(l.rstrip('\n')) > 80:
            print(f"{f}:{n}: {len(l.rstrip())} cols")
PY
```

**Vendored code is exempt.** `lib/third_party/x25519/` (TweetNaCl-derived) and
`lib/third_party/heatshrink/` are imported third-party sources — leave them upstream
formatting (incl. long lines) so they can be re-synced from upstream cleanly. Don't
reformat them and don't apply our 80-column rule to them.

## Reporting issues

Include: the mode (`AT+MODE?`), `AT+LINK?` output (rssi/snr/pwr/counters), board +
host OS, and steps to reproduce. For reliability issues, the `host/raw_verify.py`
byte-exact test output is gold.
