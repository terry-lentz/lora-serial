#!/usr/bin/env python3
"""raw_verify.py — byte-exact integrity + throughput test for the transparent
(USB_RAW) LoRa serial transport.

Streams a known pattern TX_PORT -> RX_PORT and verifies every byte (md5), at
full host speed (non-blocking writes that deliberately OVERRUN the link) to
exercise the board's PSRAM ingest buffering. Single-threaded write+always-drain
so the host
reader can never starve — host-side artifacts can't masquerade as device loss.

Usage:
    python3 host/raw_verify.py --tx /dev/ttyACM1 --rx /dev/ttyACM0 --size 131072
    python3 host/raw_verify.py --size 262144 --pattern random   # incompressible

Tip: query the board mid/post-run with the +++ AT mode (AT+LINK?) to see
hin/hout/idrop counters — idrop>0 means the stream exceeded the ingest ring.
"""
import argparse, hashlib, os, sys, time
import serial


def build(size, pattern):
    if pattern == "random":
        return os.urandom(size)
    lines, i = [], 0
    while sum(len(x) for x in lines) < size:
        lines.append(
            b"%06d the quick brown fox jumps over the lazy dog 0123456789\n"
            % i)
        i += 1
    return b"".join(lines)[:size]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tx", default="/dev/ttyACM1")
    ap.add_argument("--rx", default="/dev/ttyACM0")
    ap.add_argument("--size", type=int, default=131072)
    ap.add_argument("--pattern", choices=["text", "random"], default="text")
    ap.add_argument("--timeout", type=float, default=300.0)
    a = ap.parse_args()

    tx = serial.Serial(a.tx, 115200, timeout=0, write_timeout=0)
    tx.dtr = True; tx.rts = True
    rx = serial.Serial(a.rx, 115200, timeout=0); rx.dtr = True; rx.rts = True
    time.sleep(0.6); tx.reset_input_buffer(); rx.reset_input_buffer()

    payload = build(a.size, a.pattern)
    print(
        f">> {a.tx} -> {a.rx}: {len(payload)} bytes ({a.pattern}), "
        f"md5={hashlib.md5(payload).hexdigest()}")

    got = bytearray(); off = 0; t0 = time.time(); last = 0
    while len(got) < len(payload) and time.time() - t0 < a.timeout:
        if off < len(payload):
            try:
                w = tx.write(payload[off:off + 512]); off += (w or 0)
            except serial.SerialTimeoutException:
                pass
        c = rx.read(16384)
        if c: got.extend(c)
        else: time.sleep(0.0005)
        if len(got) - last >= 16384:
            last = len(got)
            print(
                f"   rx {len(got):>8}/{len(payload)}  "
                f"{len(got)/(time.time()-t0):.0f} B/s")

    dt = time.time() - t0
    ok = bytes(got) == payload
    print(
        f">> rx {len(got)}/{len(payload)} in {dt:.1f}s "
        f"({len(got)/dt:.0f} B/s) -> "
        f"{'PASS (byte-exact)' if ok else 'FAIL'}")
    if not ok:
        for j in range(min(len(got), len(payload))):
            if got[j] != payload[j]:
                print(
                    f"   first diff @ {j}: exp {payload[j-4:j+18]!r} "
                    f"got {bytes(got[j-4:j+18])!r}")
                break
        if len(got) < len(payload):
            print(
                f"   short by {len(payload)-len(got)} bytes "
                f"(stalled or dropped)")
    tx.close(); rx.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
