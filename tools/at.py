#!/usr/bin/env python3
# @file at.py
# @brief Run AT commands on a LoRa-Serial board over USB serial.
#
# Enters AT command mode automatically (Hayes "+++" escape), runs each
# command in order, and prints the reply. Works whether the board is
# currently in transparent data mode or already in AT mode.
#
# Usage:
#   tools/at.py <port> <cmd> [cmd ...] [--until SUBSTR] [--timeout SEC]
#
# Examples:
#   tools/at.py /dev/ttyACM0 ATI
#   tools/at.py /dev/ttyACM0 "AT+LINK?" AT+DIAG
#   tools/at.py /dev/ttyACM1 AT+MODE=auto AT&W
#   # wait for a DELAYED result (e.g. a slow-mode speed test that finishes
#   # minutes later): read the LAST command's reply until "KB/s" appears,
#   # up to 300 s.
#   tools/at.py /dev/ttyACM0 AT+SPEEDTEST=8 --until KB/s --timeout 300
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loraserial import Board


def main():
    ap = argparse.ArgumentParser(
        description="Run AT commands on a LoRa-Serial board.",
        usage="at.py <port> <cmd> [cmd ...] [--until SUBSTR] [--timeout SEC]")
    ap.add_argument("port")
    ap.add_argument("cmds", nargs="+", metavar="cmd")
    ap.add_argument("--until", metavar="SUBSTR",
                    help="read the LAST command's reply until SUBSTR appears "
                         "(for delayed results like AT+SPEEDTEST)")
    ap.add_argument("--timeout", type=float, default=300.0, metavar="SEC",
                    help="max seconds to wait for --until (default 300)")
    a = ap.parse_args()
    b = Board(a.port)
    try:
        b.enter_at()
        last = len(a.cmds) - 1
        for i, c in enumerate(a.cmds):
            if a.until and i == last:
                reply = b.at_until(c, a.until, a.timeout)
            else:
                reply = b.at(c)
            print(f"--- {a.port} {c} ---\n{reply}")
    finally:
        b.close()


if __name__ == "__main__":
    main()
