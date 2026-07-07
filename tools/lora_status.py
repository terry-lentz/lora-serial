#!/usr/bin/env python3
"""lora_status.py — live link/health status monitor for a LoRa board.

Polls a board's AT console (AT+MODE? / AT+LINK? / AT+DIAG) on an interval and
prints a compact one-line status — handy in a tmux/terminal split for watching
ADR adapt as you move the boards apart, or watching RSSI/SNR/retransmits during
a transfer.

    tools/lora_status.py /dev/ttyACM0            # 5 s interval
    tools/lora_status.py /dev/ttyACM0 --interval 3

IMPORTANT: there is ONE shared radio channel and no out-of-band telemetry, so
each poll briefly drops this board into AT command mode (~2 s) — which diverts
*its* host I/O. That's fine for observing the link, but DON'T run it against a
board that's mid-interactive-shell (it'll stutter the session). Use it between
sessions, during a file transfer, or on a bench board you're just observing.
"""
import sys, time, re, argparse, serial


def already_at(s):
    """Is the board already in AT mode? A bare 'AT' returns OK if so."""
    s.reset_input_buffer()
    s.write(b"\rAT\r"); s.flush(); time.sleep(0.4)   # leading \r clears junk
    return b"OK" in s.read(200)


def enter_at(s, tries=3):
    """Get into AT mode. Try the Hayes +++ escape a few times (the 1 s idle
    guards are timing-sensitive, so one attempt can miss); if that fails, the
    board may already BE in AT mode (e.g. a previous run was killed before ATO)
    — detect and reuse that. Returns True if we end up in command mode."""
    for _ in range(tries):
        s.reset_input_buffer()
        time.sleep(1.15); s.write(b"+++"); s.flush(); time.sleep(1.35)
        if b"OK" in s.read(200):
            return True
        if already_at(s):
            return True
    return False


def exit_at(s):
    s.write(b"\rATO\r"); s.flush(); time.sleep(0.3)   # back to the data pipe


def at_cycle(s, cmds):
    """Enter AT mode, run cmds, return concatenated replies, exit to data."""
    if not enter_at(s):
        return None                      # couldn't enter (busy / not USB_RAW)
    out = b""
    for c in cmds:
        s.reset_input_buffer()
        s.write(c.encode() + b"\r"); s.flush(); time.sleep(0.7)
        out += s.read(400)
    exit_at(s)
    return out.decode(errors="replace")


def field(text, pat, default="?"):
    m = re.search(pat, text)
    return m.group(1) if m else default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--interval", type=float, default=5.0)
    a = ap.parse_args()
    s = serial.Serial(a.port, 115200, timeout=0.3)
    s.dtr = True; s.rts = True; time.sleep(0.5)
    print(f"# polling {a.port} every {a.interval}s (Ctrl-C to stop)")
    try:
        while True:
            t = time.strftime("%H:%M:%S")
            try:
                r = at_cycle(s, ["AT+MODE?", "AT+LINK?", "AT+DIAG"])
            except Exception as e:
                print(f"{t}  !! {e}"); time.sleep(a.interval); continue
            if r is None:
                print(f"{t}  (busy — could not enter AT; board in use?)")
                time.sleep(a.interval); continue
            mode = field(r, r"mode=(\S+)")
            auto = " auto" if "(auto)" in r else ""
            rssi = field(r, r"rssi=(-?\d+)"); snr = field(r, r"snr=(-?[\d.]+)")
            pwr = field(r, r"pwr=(-?\d+)"); txq = field(r, r"txq=(\d+)")
            tx = field(r, r"\btx=(\d+)"); retx = field(r, r"retx=(\d+)")
            heap = field(r, r"heap=(\d+K)"); rst = field(r, r"lastreset=(\S+)")
            loss = ""
            if tx.isdigit() and retx.isdigit() and int(tx) > 0:
                loss = f" retx={100*int(retx)//int(tx)}%"
            print(f"{t}  {mode}{auto}  rssi={rssi} snr={snr} pwr={pwr} "
                  f"txq={txq}{loss} heap={heap} lastreset={rst}")
            time.sleep(a.interval)
    finally:
        # Always leave the board in its data pipe, not stuck in AT mode.
        try:
            exit_at(s)
        except Exception:
            pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
