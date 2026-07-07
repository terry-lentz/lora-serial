# Debugging a misbehaving board — a worked walkthrough

A hands-on companion to [DIAGNOSTICS.md](./DIAGNOSTICS.md) (which is the
field-by-field reference). This page walks the actual workflow — *"a board did
something weird, now what"* — with real output and the real-world gotchas.

The short version: **`AT+DIAG` first** (it tells you *why* it last reset), then
pull a **core dump** if it panicked, and use **`AT+CRASH`** to reproduce faults
on demand and confirm the whole chain works.

---

## 0. Self-test: prove the diagnostics work (`AT+CRASH`)

You don't have to wait for a real crash — `AT+CRASH` deliberately crashes the
board (recoverably; NVS survives) so you can verify the tooling end-to-end.

```bash
# panic path: illegal write -> ESP panic -> core dump to flash -> reboot
tools/at.py /dev/ttyACM0 AT+CRASH=panic

# hang path: stop returning to loop() -> software watchdog reboots after ~25 s
tools/at.py /dev/ttyACM0 AT+CRASH=hang
```

### Worked example — the panic path (real output)

1. Baseline, then crash:
   ```
   $ tools/at.py /dev/ttyACM0 AT+DIAG
   boots=5 ... coredump=no
   $ tools/at.py /dev/ttyACM0 AT+CRASH=panic
   crashing now (reconnect after reboot, then AT+DIAG)
   ```
2. The board panics and **reboots** — note its USB port **renumbers** (see
   gotchas): `/dev/ttyACM0` came back as `/dev/ttyACM2`.
3. Ask the rebooted board what happened:
   ```
   $ tools/at.py /dev/ttyACM2 AT+DIAG
   boots=6 lastreset=PANIC (crash) ranbefore=34s uptime=25s \
     iram=325K miniram=319K coredump=YES (tools/coredump.sh)
   ```
   ✅ It correctly reports **`lastreset=PANIC`**, that it ran **34 s** before
   dying, and that a **core dump is stored**.

### The hang path
`AT+CRASH=hang` makes the loop stop running; ~25 s later the software watchdog
reboots the board, and afterwards `AT+DIAG` shows
`lastreset=SW-WATCHDOG (loop hang)` with the pre-hang `ranbefore` uptime. This is
the safety net for the silent-hang failure mode (the loop wedging on, say, a
radio driver stall).

---

## 1. Triage a real incident

```bash
tools/at.py /dev/ttyACMx AT+DIAG
```
Read `lastreset`:

| It says | Means | Do |
|---|---|---|
| `PANIC (crash)` | a real fault | pull the core dump (§2) |
| `SW-WATCHDOG (loop hang)` | loop stopped for 25 s | what was it doing? `ranbefore` = how long it ran; no dump (clean reboot) |
| `BROWNOUT (power dip)` | supply/USB power problem | better cable/supply, not firmware |
| `interrupt/task watchdog` | IDF watchdog fired | usually also a coredump — pull it |
| `software (reflash/restart)` / `power-on` | normal | nothing |

Also watch `iram`/`miniram` (internal SRAM, free / min-ever) over a few polls —
a steadily falling `miniram` is a leak heading for a crash. `tools/lora_status.py`
graphs these live.

---

## 2. Pull and decode a core dump

On a panic the IDF writes a full ELF core dump to the flash `coredump`
partition. To turn it into a backtrace:

```bash
pip install esp-coredump          # one-time: the decoder isn't bundled
./tools/coredump.sh node_raw /dev/ttyACM0
```
The script does the 1200-baud touch into the bootloader (auto-detecting the
renumbered port), reads the partition over USB, and decodes it against
`firmware.elf` into a function/line backtrace. The raw dump is also saved to
`.pio/build/<env>/coredump.bin`.

**Critical:** the decode matches the dump against the **exact build that
crashed** (an app-SHA check). If you see
`coredump SHA256(...) != app SHA256(...)`, your `firmware.elf` isn't the binary
that was running. So:
- **Don't rebuild between flashing a board and decoding its crash** (even a
  comment change makes a new ELF with a new SHA).
- The core dump partition is **not reliably overwritten** by a later crash —
  **erase it between tests** so you decode the dump you mean:
  ```bash
  # in the bootloader (after a 1200-touch), erase just the coredump partition:
  esptool.py --chip esp32s3 --port <bootloader-port> erase_region 0x7F0000 0x10000
  ```

### Live backtrace without the flash read (UART0)
A panic also prints registers + a backtrace to **UART0** (GPIO43=TX), separate
from the USB data port. Wire a 3.3 V USB-serial adapter to GPIO43 and watch at
115200 to see it at the moment of the crash; decode a `Backtrace: 0x… 0x…` line
with `xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <addrs>`.

---

## 3. Reproduce → fix → confirm loop
1. `AT+CRASH=panic` (or trigger the real bug).
2. `AT+DIAG` on the rebooted board → confirm `PANIC` + `coredump=YES`.
3. `./tools/coredump.sh <env> <port>` → backtrace → find the offending line.
4. Fix, `make build && make flash-* ...`, then re-run §0 to confirm it's gone.
5. **Erase the coredump partition** before the next test (above).

---

## Gotchas (all hit while building these tools)

- **USB ports renumber on every reboot/flash/touch.** `/dev/ttyACM0` after a
  crash may be `/dev/ttyACM2`. **Identify a board by `ATI` (`initiator=1` is the
  initiator) or by its MAC in `/dev/serial/by-id`, never by its port number.**
  Re-`ls /dev/ttyACM*` after any reboot. (Roles are MAC-elected, so "initiator"
  isn't tied to a particular board — check `ATI`.)
- **A killed AT tool can strand a board in AT command mode** (it entered with
  `+++` but died before `ATO`). Symptoms: `+++` no longer returns `OK`, data
  doesn't pass. Fix: send a clean `\r` then `ATO` (a leading CR clears any
  half-typed line), or just **wait 60 s** — AT mode now idle-times-out on its
  own. `tools/lora_status.py` self-heals this.
- **A getty leaves the tty as `root:tty 0620`** (permission-denied for you) even
  after it's killed: `sudo chmod a+rw /dev/ttyACMx` or re-trigger udev.
- **A hard hang / USB zombie may not be recoverable over USB** — no `AT`, no
  1200-touch (it needs the app's USB stack alive), no esptool. **Power-cycle it**
  (unplug/replug USB). The BOOT button is the IDF fallback but it's under the
  Wio shield on this kit. (The software watchdog recovers *loop* hangs; a fault
  that takes down USB enumeration needs the power cycle.)
- **Can't run `AT` while `tio`/`agetty` hold the port** — they keep the line
  busy so `+++` never trips. Free the port first.
