# Deploying a permanent LoRa login console

These files turn a board into an **always-on remote login terminal** on a Linux host
(e.g. an Armbian SBC) — a `login:` prompt that respawns after every logout, reachable
over the LoRa link with no custom software on either end.

## Quick start (systemd — Armbian, Debian, Ubuntu, etc.)

On the host the board is plugged into:

```bash
# 1. (recommended) stable device name + keep ModemManager off the port
sudo cp deploy/99-lora.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
ls -l /dev/lora                      # confirm the symlink appeared

# 2. install the respawning console service
sudo cp deploy/lora-getty@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now lora-getty@lora     # or @ttyACM0 if you skipped step 1
```

That's it. From the other board, open any terminal (`tio /dev/ttyACM0`, `screen`,
PuTTY, a phone serial app) and you'll get a fresh `login:` every time — it respawns
on logout because of `Restart=always`.

Check / stop:
```bash
systemctl status lora-getty@lora
sudo systemctl disable --now lora-getty@lora
```

## Notes

- **`-L` is required** — the unit runs `agetty -L` (local line, no carrier-detect /
  auto-baud), because our link has no DCD wire. The stock `serial-getty@` template
  does *not* pass `-L`, which is why this dedicated unit exists.
- **ModemManager** is the most common gotcha: if it's installed it may grab
  `/dev/ttyACM*` and break the console. The udev rule sets `ID_MM_DEVICE_IGNORE=1`;
  alternatively `sudo systemctl mask ModemManager`.
- **Verify USB IDs** before relying on the udev rule (see comments in `99-lora.rules`):
  `udevadm info -a -n /dev/ttyACM0 | grep -m2 -E 'idVendor|idProduct'`.
- **Two boards on one host?** Match a unique attribute (serial number / USB path) in
  the udev rule so each gets its own symlink.
- **Set the radio mode once** before deploying (it persists in NVS):
  `python3 host/atcmd.py /dev/ttyACM0 AT+MODE=turbo AT\&W` — see the main README.

## Without systemd (sysvinit / busybox init)

Add a respawn line to `/etc/inittab` and `telinit q`:
```
T0:23:respawn:/sbin/agetty -L 115200 ttyACM0 vt100
```
