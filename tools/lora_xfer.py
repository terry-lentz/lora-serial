#!/usr/bin/env python3
# @file lora_xfer.py
# @brief Throughput + byte-exactness test over the LoRa serial cable.
#
# Streams a deterministic payload from the TX board's USB port to the RX
# board's USB port (the transparent serial transport), verifies it arrives
# byte-exact, and reports the rate. Optionally pins a mode on BOTH ends
# first (AT+FMODE, which forces the local mode with no peer coordination —
# so set the same name). This is the standard way we measure per-mode
# throughput on the bench; prefer extending it over writing a new script
# (see tools/README.md / CLAUDE.md).
#
# Usage:
#   tools/lora_xfer.py <tx_port> <rx_port> <nbytes> [--mode NAME]
#                      [--timeout S] [--idle S]
#
# Examples:
#   tools/lora_xfer.py /dev/ttyACM0 /dev/ttyACM1 8192
#   tools/lora_xfer.py /dev/ttyACM0 /dev/ttyACM1 16384 --mode turbo
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loraserial import Board


def make_payload(n, pattern="seq"):
    """Build the test payload. Patterns differ in compressibility so we can
    measure both the headline (compressible) and worst-case (random) rates:
      seq    - 256-byte repeating permutation (semi-compressible, the default)
      zeros  - all 0x00 (maximally compressible -> the headline ceiling)
      text   - repeated ASCII line (very compressible, like real logs/shell)
      random - LCG pseudo-random bytes (incompressible -> worst case)
    """
    if pattern == "zeros":
        return bytes(n)
    if pattern == "text":
        line = b"the quick brown fox jumps over the lazy dog 0123456789\n"
        return (line * (n // len(line) + 1))[:n]
    if pattern == "random":
        out = bytearray(n)
        x = 0x12345678
        for i in range(n):
            x = (x * 1103515245 + 12345) & 0xFFFFFFFF
            out[i] = (x >> 16) & 0xFF
        return bytes(out)
    return bytes((i * 73 + 13) & 0xFF for i in range(n))   # seq


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("tx_port")
    ap.add_argument("rx_port")
    ap.add_argument("nbytes", type=int)
    ap.add_argument("--mode", help="pin this mode on both ends (AT+FMODE)")
    ap.add_argument("--pattern", default="seq",
                    choices=["seq", "zeros", "text", "random"],
                    help="payload compressibility (default seq)")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="overall wall-clock limit (s); raise for slow runs")
    # --idle must exceed the mode's round-trip or a slow link looks 'stalled':
    # far/SF12 frames are ~13 s of airtime, so 8 s falsely gave up before the
    # first frame even arrived. 60 s covers far's round-trips; a successful
    # transfer still finishes immediately on got==sent, so this only affects how
    # long a genuinely stalled transfer waits before giving up.
    ap.add_argument("--idle", type=float, default=60.0,
                    help="give up after this many seconds with no new bytes "
                         "(must exceed the mode's round-trip; far ~ tens of s)")
    a = ap.parse_args()

    tx, rx = Board(a.tx_port), Board(a.rx_port)
    try:
        tx.enter_at()
        rx.enter_at()
        if a.mode:
            print(f"[tx AT+FMODE={a.mode}: {tx.at('AT+FMODE=' + a.mode)}]")
            print(f"[rx AT+FMODE={a.mode}: {rx.at('AT+FMODE=' + a.mode)}]")
        tx.data_mode()
        rx.data_mode()
        time.sleep(0.3)
        # Drain any residue from the AT->data transition (e.g. the tail of an
        # "OK" reply) so it can't show up as a stray leading "data" byte.
        for _ in range(5):
            if not (tx.read_data(4096) + rx.read_data(4096)):
                break

        data = make_payload(a.nbytes, a.pattern)
        got = bytearray()
        t0 = time.time()
        last = t0
        tx.write_data(data)
        while len(got) < a.nbytes and time.time() - t0 < a.timeout:
            chunk = rx.read_data(4096)
            if chunk:
                got += chunk
                last = time.time()
            elif time.time() - last > a.idle:
                break

        dt = time.time() - t0
        ok = bytes(got) == data
        rate = len(got) / dt / 1024 if dt > 0 else 0.0
        print(f"mode={a.mode or '(current)'} pattern={a.pattern} "
              f"sent={a.nbytes} got={len(got)} exact={ok} time={dt:.2f}s "
              f"rate={rate:.2f} KB/s")
        if not ok and got:
            m = min(len(got), a.nbytes)
            fm = next((i for i in range(m) if got[i] != data[i]), None)
            print(f"  first_mismatch={fm} len={len(got)}/{a.nbytes}")
    finally:
        tx.close()
        rx.close()


if __name__ == "__main__":
    main()
