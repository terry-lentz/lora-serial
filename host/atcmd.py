#!/usr/bin/env python3
"""atcmd.py — send +++/AT commands to a LoRa modem board over its serial port.

Handles the Hayes "+++" escape guard timing for you (≥1 s idle before and
after), runs each AT command, prints the reply, then returns to data mode with
ATO.

Examples:
    # one-time setup: switch to turbo and SAVE to flash (persists across
    # reboots)
    python3 host/atcmd.py /dev/ttyACM0 AT+MODE=turbo AT\\&W

    # just query state
    python3 host/atcmd.py /dev/ttyACM0 AT+MODE? AT+LINK?

Set the SAME mode on BOTH boards. After AT&W the mode is remembered, so tools
like `agetty`/`tio`/`screen` can use the port normally with no AT traffic per
session.
"""
import sys, time, serial


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    port, cmds = sys.argv[1], sys.argv[2:]
    s = serial.Serial(port, 115200, timeout=0.3); s.dtr = True; s.rts = True
    time.sleep(0.5); s.reset_input_buffer()

    # enter command mode: 1s idle, "+++", 1s idle -> "OK"
    time.sleep(1.1); s.write(b"+++"); s.flush(); time.sleep(1.3)
    if b"OK" not in s.read(200):
        print(
            "!! did not enter command mode (is it a *_raw / USB_RAW board?)")
        sys.exit(1)

    for c in cmds:
        s.reset_input_buffer()
        s.write(c.encode() + b"\r"); s.flush(); time.sleep(0.8)
        reply = s.read(300).decode(errors="replace").strip()
        print(f"{c}\n  {reply.replace(chr(13),'').strip()}")

    # back to transparent data mode
    s.write(b"ATO\r"); s.flush(); time.sleep(0.2)
    s.close()


if __name__ == "__main__":
    main()
