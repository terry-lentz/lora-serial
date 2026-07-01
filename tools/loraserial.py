#!/usr/bin/env python3
# @file loraserial.py
# @brief Shared helpers for talking to a lora-serial board over USB.
#
# Used by the CLI tools in this directory (at.py, lora_xfer.py). The
# firmware is a transparent serial cable with a Hayes-style "+++" escape
# into an AT command mode; this wraps that handshake so callers don't
# reimplement it each time. See tools/README.md for the catalog of tools
# built on this.
import time
import serial   # pyserial

# Must be >= the firmware's kEscapeGuardMs (1000 ms): the "+++" escape needs
# that much host silence before AND after the three plus signs to be accepted.
ESCAPE_GUARD_S = 1.2


class Board:
    """One board on a serial port. Knows how to enter AT mode and run commands,
    and how to drop back to transparent data mode for a throughput test."""

    def __init__(self, port, baud=115200, timeout=0.5):
        self.port = port
        self.s = serial.Serial(port, baud, timeout=timeout)
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def enter_at(self):
        """Enter AT command mode via the "+++" escape. Idempotent: if we're
        already in AT mode the plus signs get buffered, so we send a trailing
        newline to flush that partial line before real commands run."""
        time.sleep(ESCAPE_GUARD_S)
        self.s.write(b"+++")
        self.s.flush()
        time.sleep(ESCAPE_GUARD_S + 0.2)
        self.s.read(200)                       # consume "OK" / echoed plus signs
        self.s.write(b"\r\n")                   # flush any buffered "+++" as a line
        self.s.flush()
        time.sleep(0.3)
        self.s.read(200)

    def at(self, cmd, wait=0.7, nbytes=8000):
        """Run one AT command (must already be in AT mode) and return the reply
        text. Strips nothing structural — the caller sees what the board sent."""
        self.s.write((cmd + "\r\n").encode())
        self.s.flush()
        time.sleep(wait)
        return self.s.read(nbytes).decode(errors="replace").strip()

    def at_until(self, cmd, expect, timeout=300.0):
        """Run one AT command and keep reading until `expect` appears in the
        reply or `timeout` seconds elapse, then return all text read. For
        commands whose result is DELAYED — e.g. AT+SPEEDTEST finishes minutes
        later on a slow mode (far/SF12), long after at()'s fixed 0.7 s wait."""
        self.s.write((cmd + "\r\n").encode())
        self.s.flush()
        deadline = time.time() + timeout
        buf = ""
        while time.time() < deadline:
            chunk = self.s.read(4096).decode(errors="replace")  # blocks <=0.5s
            if chunk:
                buf += chunk
                if expect in buf:
                    break
        return buf.strip()

    def data_mode(self):
        """Return to transparent data mode (ATO). Call enter_at() first so the
        state is known: ATO from AT mode is a clean exit; from data mode the
        bytes would just go over the air."""
        self.s.write(b"\r\nATO\r\n")
        self.s.flush()
        time.sleep(0.4)
        self.s.read(400)
        self.s.reset_input_buffer()

    def write_data(self, b):
        self.s.write(b)
        self.s.flush()

    def read_data(self, n):
        return self.s.read(n)
