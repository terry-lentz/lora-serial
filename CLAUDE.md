# CLAUDE.md — working rules for this repo

This file is loaded every session. **Read it before writing code.** The full
rationale lives in [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md); this is
the short, must-follow version. Write code that already passes — don't write it
loose and reformat afterward.

## The bar: PRODUCTION-QUALITY, teaching-grade code

This project is meant to be a **pinnacle of code quality** — something a stranger
can pick up, *read*, understand, and use as the base for their own project. The
owner wants the highest standard, not "good enough." Hold every change to that
bar:

- **Readable to a newcomer.** Favor clarity over cleverness. Explain *why*, not
  just *what*. The link layer (`lib/linklayer/`) is the heart of the project and
  deserves the most thorough explanation — comment it as if teaching the reader
  the protocol.
- **No unexplained constants** (see rule 3). Every literal is named + commented,
  and pulled to a header where it can be found and tuned.
- **No cruft.** Remove dead code, stale comments, and debugging scaffolds once
  they've served their purpose. Names say what they mean.
- **Modern, allocation-free C++** (see rule 5) and a clean **OOP** structure:
  the firmware glue is organized as static-singleton classes (see "Architecture
  & C++ structure"). Maintain that shape — keep new behavior in the right class
  and the native sim green at every step.
- When in doubt, put in the extra effort. "Someone will read this and learn from
  it" is the test every file should pass.

## Hard rules (get these right the FIRST time)

1. **≤ 80 columns, every line of our own source.** This is enforced by
   `make format-check` and it is the #1 thing to not get wrong after the fact.
   While writing, keep lines short:
   - Wrap at commas or *before* a binary operator; indent the continuation.
   - Split long string literals into adjacent literals: `"part one "` `"part two"`.
   - Reflow long `//` comments onto multiple lines rather than running over.
   - Display width: a multibyte char (`—`, `≤`, `µ`) counts as **one** column,
     not its byte count. (The check measures characters, so trust it over `awk`.)
   - Rule of thumb: if a line of code passes ~72 chars, plan the wrap as you type.

2. **Headers declare, `.cpp` defines.** Logic goes in `.cpp`. Headers hold
   declarations, types, constants, and short inline accessors only. The sole
   exception is `lib/linklayer/` — it's a template library, so logic must be
   header-visible there.

3. **No bare magic numbers in logic — explain AND name them.** Pull every
   literal into a named `constexpr`/`const` (or a `#define` for a build knob),
   declared in the relevant header where practical so it can be found and
   adjusted, with a comment saying what it means and why. This includes
   "incidental" constants: a PRNG seed like `0xC0DE` or an LCG's `1103515245`/
   `12345` must say *what they are* (e.g. "ANSI-C LCG multiplier/increment;
   deterministic test vector") — never leave an unexplained hex/number in the
   code. The only exception is a value whose meaning is unmistakable from
   context (e.g. `* 2`, `+ 1`, index `0`). If you find an unexplained literal,
   fixing it is part of the job, not someone else's later cleanup.

4. **Comment thoroughly, for a beginner — and state the PRESENT, not the past.**
   This is a teaching-quality, open-source project — under-commented code is a
   defect to fix, not a style to copy.
   - **Every function** gets a **Doxygen `/** … */` doc block** at its
     declaration, in a form that can generate API docs:
     ```
     /**
      * @brief One-line descriptive summary ("Returns the count").
      *
      * @param[in]  dividend  the number to be divided.
      * @param[out] remainder where the remainder is written.
      * @return the quotient.
      */
     ```
     Use `@brief`, the `@param[in]`/`@param[out]` direction tags, and `@return`
     (omit `@return` for `void`; omit `@param` when there are none). Note
     ownership / null-ability where relevant. **EVERY function, method, getter,
     setter, operator, and constructor gets this block — NO exceptions, no
     matter how trivial.** Even a one-line inline getter gets a full
     `/** @brief … @return … */` block; a bare `//` is not enough. Consistent,
     doc-generatable coverage on every declaration is the standard. (The
     `/* */` form is permitted by Google — it allows either `//` or `/* */`;
     we use it for every declaration so Doxygen can extract them all.)
   - **Implementation / inline comments use `//`** (Google's norm). Reserve the
     `/* */` block form for the Doxygen declaration docs above.
   - **Every constant, variable, struct/class data member, and enum value** gets
     a **Doxygen** comment too — a trailing `///< …` one-liner, or a `/** … */`
     block above for multi-line — so it appears in generated docs. Never a bare
     `//`, and never an undocumented `static const X = 7;`. (Comments INSIDE a
     function body — implementation notes — stay plain `//`.)
   - Explicitly call out any choice made for embedded reasons: determinism,
     RAM/footprint, no-heap-in-hot-path, ISR/IRAM safety, real-time radio timing.
   - Each AT command gets a comment with its on-the-wire/runtime effect.
   - **Matter-of-fact, not a changelog.** Comments describe what the code does
     *now*, so a reader understands it standalone. Do NOT narrate the iteration
     history ("the old version did X, then we found Y, now we do Z", "fix on top
     of fix"). That story belongs in `docs/CAPABILITIES_JOURNEY.md`. When you
     touch code that has accreted such narration, **clean it up** — distill it to
     the current truth and move any worth-keeping history to the JOURNEY.

5. **Memory: static/stack only in the hot paths — no heap, no smart pointers.**
   This is deliberate, for determinism and zero fragmentation on a long-running
   radio. Use **static/global** objects instantiated once (`static
   LinkLayer<16384> g_link`), **stack** buffers (`uint8_t fr[MAXFRAME]`), and
   **fixed pre-allocated rings** (the 2 MB PSRAM ingest buffer). Do NOT introduce
   `new`/`delete`, `std::vector`/`std::string`/`std::function`, or
   `unique_ptr`/`shared_ptr` in the frame/ring/turn paths — smart pointers imply
   the heap (atomic refcount / allocation) and are a *regression* here. "Modern
   C++" in this project means RAII-where-it-helps, `constexpr`, templates,
   `enum class`, references — not heap. (Even Meshtastic uses a packet pool, not
   `shared_ptr`, in its radio path.)

6. **Never reformat vendored code** under `lib/third_party/` (heatshrink,
   x25519). It keeps upstream style so it can be re-synced.

7. **Order `#include`s the Google way.** In a `.cpp`, the file's OWN header
   first (on its own line), then a blank line, then each group separated by a
   blank line in this order: C system headers (`<...>`), C++ standard headers,
   other libraries' headers, then this project's headers. **Within every group,
   sort alphabetically.** Headers (`.h`) have no "own header" — just the grouped,
   alphabetized sections. Use `#define` include guards (not `#pragma once`),
   formatted `LINK_LAYER_<FILE>_H_` for `lib/linklayer/`, `LORA_SERIAL_<FILE>_H_`
   for `src/`.

## Naming (Google C++ baseline)

| Kind | Convention | Example |
|------|-----------|---------|
| Functions / methods | `PascalCase` | `ApplyMode`, `NextTx` |
| Types | `PascalCase` | `ModemSettings`, `LinkLayer` |
| Variables, params | `snake_case` | `out_len`, `freq_mhz` |
| Globals | `g_` + `snake_case` | `g_link`, `g_static_key` |
| Private members | trailing `_` | `base_seq_`, `ack_pending_` |
| Constants | `kCamelCase` | `kFreqMhz`, `kRfModes` |
| Macros / wire-protocol fields | `ALL_CAPS` | `FEAT_ENC`, `MAXFRAME` |

## Architecture & C++ structure

The portable `lib/linklayer/` is class-based and natively unit-tested. The
firmware glue is organized as **static-singleton classes**, one per subsystem,
each instantiated once (no heap — rule 5; the instance is a `static`, not
`new`'d, cf. Meshtastic's `RadioInterface → SX126xInterface`):

- **`Radio`** (`g_radio`, `fw_radio.{h,cpp}`) — the SX1262: LED, DIO1 ISR, the
  interrupt RX task, the mode table, `Tx`/`Rx`, and the per-mode derived
  timing/power/PHY/RSSI behind accessors (`toa_ms()`, `SetTxPower()`, …).
- **`Host`** (`g_host`, `fw_host.{h,cpp}`) — the transparent USB↔link data
  plane: the host-TX ring, the PSRAM ingest ring, the `+++` AT parser,
  speed-test/sink, X25519 pairing, auto-power, and NVS settings.
- **`Diag`** (`g_diag`, `fw_diag.{h,cpp}`) — the software + IDF watchdogs, the
  RTC crash breadcrumbs, and the `AT+DIAG` report.
- **`Device`** (`g_device`, `fw_device.{h,cpp}`) — orchestration: `Setup()`/
  `Loop()`, MAC role discovery, proximity pairing, the initiator/responder
  turn engine, layered recovery (radio-reinit < rendezvous < reboot), and ADR.

`main.cpp` is the slim composition root: it defines the few genuinely shared
globals (`cfg`, `g_link`, `g_modesw`, `g_adr`, the `radio` object, the keys,
`g_rx_idle_hook`) and forwards `setup()`/`loop()` to `g_device`. C-style
callbacks (the DIO1 ISR, the idle hook, the TX-counter persist) reach their
singleton through `static` thunk methods; the `RTC_NOINIT_ATTR` crash
breadcrumbs stay file-local statics in `fw_diag.cpp` (that attribute needs
static storage). Keep this shape — subsystem behavior in its class, the shared
contract in `main.cpp`, and **no heap / smart pointers** in the
frame/ring/turn paths.

## Before saying a code change is done

Run both and make sure they pass — don't ask the user to discover a break:

```sh
make format-check        # ≤80 cols on our sources
pio test -e native       # the full native unit + sim suite
```

## Development workflow — simulation first, hardware second

This is how work gets done on this project. It is not optional polish; it is the
method that has repeatedly caught real bugs before they reached hardware.

1. **Validate every change in the native sim before flashing.** `pio test -e
   native` runs the unit tests (`test/test_link`) and the integration sim
   (`test/test_sim`). A logic or protocol change is not "done" until the sim is
   green. **Do not flash firmware to validate a fix** — flash only after the sim
   confirms it.

2. **Reproduce hardware issues in the sim *first*, then fix.** When the hardware
   misbehaves (turbo collapse, a mode-switch dropping data, a deaf radio), the
   move is: model that behavior in the sim until the sim reproduces the symptom,
   *then* iterate on the fix in the fast sim loop. Hardware loops are slow and
   noisy; the sim is deterministic and instant. Only return to hardware to
   confirm the sim-validated fix.

3. **Make the sim model real hardware behaviors**, so the software is forced to
   be resilient to them. The sim already models: MODE-DEAF delivery (a frame is
   heard only if the peer is on the sender's mode), SNR-driven loss vs each
   mode's demod floor, per-mode time-on-air, an in-memory ingest backlog under
   backpressure, and a radio that goes deaf mid-run (radio-stuck watchdog). When
   you discover a new inherent hardware behavior, add it to the sim.

4. **Write tests with teeth.** For each fix, also add the *negative* test that
   fails without the fix (e.g. "without the watchdog the deaf radio stays dead",
   "reset-on-switch drops data"). A test that can't fail proves nothing. Add a
   test for any behavior a change touches.

5. **Use the device's own diagnostics to investigate**, not guesswork:
   `AT+DIAG`, `AT+LINK?`, per-side RSSI/SNR/retx counters, and the crash core
   dump (`tools/coredump.sh <env> <port>`). The coredump decode needs the EXACT
   firmware.elf that was running at the crash — don't rebuild between flashing
   and decoding or the app-SHA won't match.

6. **The two-board bench — test real data flow on one host.** The fastest way
   to reproduce and understand a *hardware* issue (mode switches, GFSK, a deaf
   or rebooting radio) is two boards flashed with the same image, both plugged
   into the same machine. They auto-elect their half-duplex roles from their
   MACs, so each simply shows up as its own USB serial port and they form a
   link with no per-board setup. This is the bench that found most of the hard
   bugs — reach for it whenever the native sim can't reach the behavior (nothing
   in `src/`, the radio/PHY glue, is built natively).

   - **Find the boards.** `ls -l /dev/serial/by-id/` — each is
     `usb-Open_LoRa-Serial_<MAC>-if00 -> ../../ttyACMn`. `ATI` on a port shows
     its role (`initiator=0|1`), address, peer, and link state.
   - **Drive each board** over its port with `tools/at.py <port> <cmd>…` (it
     auto-enters AT mode via `+++`). Baseline a pair with
     `tools/at.py <port> ATI "AT+MODE?" "AT+LINK?" AT+DIAG`.
   - **Send data / measure throughput.** Run `AT+SPEEDTEST=<kb>` on the
     **initiator**: the firmware generates incompressible data internally and
     reports KB/s + retx, so no second host program is needed —
     `tools/at.py <initiator> AT+SPEEDTEST=64 --until KB/s --timeout 120`. For a
     byte-exact end-to-end check over the transparent transport, stream between
     the two ports with `host/raw_verify.py` / `tools/lora_xfer.py`.
   - **Exercise a scenario.** e.g. a mode switch: `AT+MODE=<name>` on the
     **initiator** coordinates BOTH ends (the responder follows), then send
     data. A `medium`→`ludicrous` switch followed by a speedtest is the GFSK
     repro.
   - **Read what broke.** `AT+DIAG` reports `boots`, the last reset reason, and
     the `wedgeop`/`rxop` breadcrumbs — the radio op each task last entered
     before a wedge (see the `RMARK` traces in `fw_radio.cpp` and the
     `g_dbg_stage` marks in `fw_device.cpp`). `coredump=YES` means a crash dump
     is saved; `tools/coredump.sh <env> <port>` pulls and decodes it.
   - **Recover a wedged board.** A hard wedge won't answer AT (silent on
     serial) — power-cycle it (unplug/replug USB, or the reset button); runtime
     recovery only clears the softer, self-detected deaf-locks.

## Keep the docs in sync — every change updates the docs

Docs are part of "done," not a later chore. **Every time we change behavior,
update the relevant docs in the same pass** so they never drift from the code.
The user expects this by default — don't wait to be asked.

- `README.md` — quick start, feature list, throughput numbers.
- `docs/DESIGN.md` — architecture and how the pieces fit.
- `docs/RESEARCH.md` — the design reasoning and references behind a mechanism.
- `docs/TESTING.md` — how the sim models hardware and what the tests cover.
- `docs/CAPABILITIES_JOURNEY.md` — each real problem hit and how it was solved
  (add an entry once a problem is actually resolved).
- `docs/MODE_SWITCH_SPEC.md` — the formal mode-switch / recovery state machine.
- Plus the focused docs: `SECURITY.md`, `ROADMAP.md`, `THROUGHPUT.md`,
  `DIAGNOSTICS.md`, `FUTURE_MODES.md`, `CODING_STANDARDS.md`, etc. — update
  whichever a change touches.

When a change adds a recovery mechanism, mode, AT command, or protocol field,
find every doc that mentions the area and update it; don't leave a stale claim.

**Docs state the PRESENT, not the journey.** A standalone doc (`SECURITY.md`,
`DESIGN.md`, `THROUGHPUT.md`, …) describes what the system does *now*,
matter-of-fact — not the story of how it got there. An old design, the bugs hit,
and the sequence of fixes belong in `docs/CAPABILITIES_JOURNEY.md`; reference it,
don't retell it. So: no "Fix A / Fix B", no "the old design did X, now we do Y",
and never a present-tense description of a bug that's already fixed — a reader
must not mistake resolved history for the current state. This is rule 4's "state
the PRESENT, not the past" applied to whole documents.

## Reusable tools — no one-off helpers

When you need a helper (an AT runner, a throughput test, a flasher, a log
parser), **make it a maintained tool in `tools/`, not a throwaway script.** The
only exception is something genuinely niche we'll never repeat. The rule:

- Before writing a helper, check `tools/README.md` — extend an existing tool if
  one almost fits, rather than starting from scratch.
- New reusable helper → add it to `tools/` AND list it in `tools/README.md`
  (the catalog). Share common logic via `tools/loraserial.py`.
- Improve the tools over time so they keep working well; treat a flaky tool as a
  bug to fix, not a reason to rewrite a one-off.

Current catalog: `upload_flash.sh`, `coredump.sh`, `at.py`, `lora_xfer.py`,
`loraserial.py`, `version.py` (build-time version stamp) — see
`tools/README.md`.

## Hardware guardrails

- **Reflash the boards freely, without asking** — iterating on flash/test loops
  on your own speeds up development and is wanted. The sim-first rule still
  applies (validate in the native sim before flashing), but once a change is
  sim-green you do NOT need permission to flash and re-flash repeatedly.
- **The one exception:** do not reflash when the user has explicitly asked you
  to hold off because *they* are testing on the boards. Honor that until they
  say testing is done.
- One firmware env (`node_raw`), flashed to BOTH boards — identical image. The
  half-duplex role + 1-byte address auto-elect from the chip MAC at runtime, so
  there is no per-board build. Identify which board is which by `ATI`
  (`initiator=1` is the elected initiator), never by port number.
- **ALWAYS flash with `make flash PORT=/dev/ttyACMx` (→ `tools/upload_flash.sh`),
  NEVER `pio run -t upload` or plain `esptool`.** The XIAO's native USB
  re-enumerates when the firmware reboots into the bootloader, so esptool's
  default DTR/RTS auto-reset fails mid-connect (`No serial data received` /
  `No such device`) — this cost a very long debugging detour once. The tool
  resets into the ROM bootloader itself (a 1200-baud touch **and** a DTR/RTS
  reboot-sequence walk) and flashes the stable download port, so it works
  **button-free** — which matters because the SX1262 hat physically covers the
  BOOT button. Flash one board per port; repeat for the other. If a flash leaves
  a board in download mode (it enumerates as `USB JTAG/serial debug unit`), a
  plain unplug/replug boots the freshly-flashed firmware.
- Taiwan band: 920–925 MHz.
