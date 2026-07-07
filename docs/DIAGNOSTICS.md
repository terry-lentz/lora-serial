# Diagnostics & crash debugging

This link is normally a *silent* black box: the USB port is the data pipe, not a
log console (we set `CORE_DEBUG_LEVEL=0` and run TinyUSB CDC), so a board that
misbehaves gives you nothing by default. This page covers the instrumentation
that's built in to change that — health metrics, crash breadcrumbs, a watchdog,
and full core dumps. For a **step-by-step worked example** (including
`AT+CRASH`, the self-test that deliberately crashes a board to prove the tools
work), see **[DEBUGGING.md](./DEBUGGING.md)**.

## TL;DR — a board just misbehaved, what do I do?

1. **`AT+DIAG`** — the first stop. Tells you *why it last reset* and its health:
   ```
   boots=7 lastreset=PANIC (crash) ranbefore=312s uptime=20s \
     iram=298K miniram=271K coredump=YES (tools/coredump.sh)
   ```
2. Read `lastreset`:
   - **`PANIC (crash)`** → a real fault. If `coredump=YES`, run
     `./tools/coredump.sh <env> <port>` for a backtrace (below).
   - **`BROWNOUT (power dip)`** → power/USB supply problem, not firmware.
   - **`SW-WATCHDOG (loop hang)`** → the loop stopped running for 25 s (e.g. a
     radio driver stalled). No coredump (it's a clean reboot), but `ranbefore`
     tells you how long it ran first.
   - **`software (reflash/restart)`** / **`power-on`** → normal.
3. Watch `iram` / `miniram` over time (via `AT+DIAG` or `AT+LINK?`): a steadily
   falling `miniram` is a memory leak closing in on a crash.

## What's built in

### `AT+DIAG` — crash & health report (read-only)
| Field | Meaning |
|---|---|
| `boots` | total boots, counted in NVS (survives reflash) |
| `lastreset` | why the board last reset (see table below) |
| `ranbefore` | uptime (s) the firmware reached *before* that reset — from an RTC breadcrumb that survives a crash (but not a power cycle) |
| `uptime` | current uptime (s) |
| `iram` / `miniram` | free / min-ever-free **internal SRAM** (KB) — the scarce, crash-relevant pool (PSRAM is reported separately and rarely matters) |
| `coredump` | whether a crash core dump is stored in flash |
| `wedgeop` | radio-op breadcrumb — which blocking op the **loop** was in at the last wedge reboot: `a` ApplyMode, `f` ApplyRadioFsk, `p` setOutputPower, `t` transmit, `r` receive, `i` Reinit; `P`/`M`/`H` are the Poll / recovery+ADR / host-pump loop sections. Stamped into RTC so it survives the reboot. Meaningful only when `lastreset` is a wedge. |
| `rxop` | same, for the **RX task**: `d` inside `readData`, `.` idle. With `wedgeop` it tells you *which task hung where* — e.g. `wedgeop=r rxop=.` is the loop waiting on a deaf radio; `wedgeop=i` is a recovery re-init (see journey entry 28). |

**Reset reasons:** `power-on`, `software (reflash/restart)`, `PANIC (crash)`,
`interrupt watchdog`, `task watchdog`, `SW-WATCHDOG (loop hang)` (our watchdog),
`NO-PROGRESS (radio wedge reboot)` (a deaf-but-alive link that recovery couldn't
restore), `BROWNOUT (power dip)`, `external reset`, `unknown`.

### `AT+LINK?` — also carries live `heap=` (internal SRAM) for monitoring.

### `AT+CRASH=<panic|hang>` — deliberately crash the board (self-test)
Recoverable (NVS survives). `panic` triggers an illegal write → ESP panic →
core dump + `lastreset=PANIC`; `hang` stops the loop → the software watchdog
reboots it → `lastreset=SW-WATCHDOG`. Use it to verify the whole capture chain
end-to-end — see the walkthrough in [DEBUGGING.md](./DEBUGGING.md).

### Software watchdog (hang → reboot, not silent death)
A 1 Hz timer reboots the board if the main loop stops "petting" it for **25 s**
(longer than any legitimate radio wait, so normal operation never trips it). A
true hang — the failure mode we hit during GFSK bring-up, where the board went
silent and unresponsive — now self-recovers and is flagged as
`lastreset=SW-WATCHDOG` with the pre-hang `ranbefore` uptime. (The ESP-IDF
interrupt watchdog also still catches interrupts-disabled hangs, as a panic.)

### Core dumps (full backtrace, persisted across the crash)
The build enables **esp-coredump-to-flash** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`),
and the 8 MB partition table has a 64 KB `coredump` partition. On a panic the IDF
writes a full ELF core dump there; `AT+DIAG` shows `coredump=YES`. Read + decode
it:
```bash
pip install esp-coredump          # one-time (the decoder isn't bundled)
./tools/coredump.sh node_raw /dev/ttyACM0
```
The script reads the partition over USB and decodes it against the matching
`firmware.elf` into a backtrace (function names + line numbers). The raw dump is
also saved to `.pio/build/<env>/coredump.bin` for offline decode. Erase a stored
dump by overwriting it (or it's overwritten by the next crash).

### UART0 panic backtrace (live, no flash read)
On any panic the ROM/IDF handler **also prints registers + a backtrace to UART0**
(the hardware UART on **GPIO43=TX / GPIO44=RX**, separate from the USB data
port), then reboots. To capture it live, wire a 3.3 V USB-serial adapter to
GPIO43 and watch at 115200 baud:
```bash
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200
```
Decode a raw backtrace line (`Backtrace: 0x... 0x...`) with the toolchain's
`xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/<env>/firmware.elf <addrs>`.
(GPIO43/44 may be under the Wio shield on this kit — the flash/AT-DIAG/coredump
paths above need no extra wiring and are usually enough.)

### `AT` idle timeout
AT command mode auto-exits after **60 s** of silence (`ATO` is no longer the only
way out), so a forgotten or crashed AT tool can't strand a board out of its data
pipe.

## Live status monitor — `tools/lora_status.py`
Watch link/ADR state refresh on an interval (great for seeing ADR adapt as you
move the boards, or watching RSSI/SNR/retransmits during a transfer):
```bash
tools/lora_status.py /dev/ttyACM0 --interval 5
# 12:31:07  turbo auto  rssi=-56 snr=8 pwr=-6 txq=0 retx=4% heap=298K lastreset=...
```
**Caveat:** there's a single shared radio channel and no out-of-band telemetry,
so each poll briefly drops *that* board into AT mode (~2 s), diverting its host
I/O. Use it for observation (transfers, ADR, bench) — **not** against a board in
the middle of an interactive shell, which it would stutter. A future framed
side-channel could make this truly parallel; for now this is the trade-off of a
lean single-pipe design.

## What we still can't do
- **Diagnose a crash whose evidence is gone.** A reflash or power cycle clears
  the RTC breadcrumb (and a power cycle the reset reason); `boots`/coredump
  persist. Read `AT+DIAG` / pull the coredump *before* reflashing a board that
  died.
- **Get a coredump from a pure hang** (vs. a panic). A hang is caught by the
  software watchdog as a clean reboot (no dump) — use `ranbefore` + what the
  board was doing. Only true faults/panics produce a coredump.
