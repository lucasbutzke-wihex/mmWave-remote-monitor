# Pi Dashboard

A tiny, single-purpose web dashboard for a Raspberry Pi. Shows:

- **Serial ports** — only external ones (USB-serial adapters like
  `/dev/ttyUSB0`, `/dev/ttyACM0`) and any virtual ports created by `socat`.
  Internal UARTs (`/dev/ttyAMA*`, `/dev/ttyS*`, `/dev/serial0`, etc.) are
  hidden.
- **Storage** — usage per mounted disk/partition.
- **System** — CPU %, per-core, temperature, RAM, swap, and network
  throughput (up/down speed, btop-style).
- **Logs** — every file in `/tmp`, newest first, with a tap-to-view tail
  (last N lines) in a modal.

The page auto-refreshes every 3 seconds and is designed mobile-first, so it
works well from your phone on the same network.

## 1. Install

```bash
# on the Raspberry Pi
sudo apt update
sudo apt install -y python3-venv

cd ~
# copy this folder (rpi-dashboard/) onto the Pi, e.g. via scp or git, then:
cd rpi-dashboard
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

## 2. Run

```bash
source venv/bin/activate
python3 app.py
```

Then, from your phone or laptop on the same Wi-Fi/LAN, open:

```
http://<pi-ip-address>:8000
```

Find the Pi's IP with `hostname -I`.

## 3. (Optional) Run it permanently as a service

So it starts on boot and restarts if it crashes:

```bash
sudo cp raspi-dashboard.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now raspi-dashboard
```

Check status / logs with:

```bash
sudo systemctl status raspi-dashboard
journalctl -u raspi-dashboard -f
```

Edit `raspi-dashboard.service` first if your project folder or username
differs from the placeholders (`/home/pi/rpi-dashboard`, user `pi`).

## Notes on serial port permissions

Listing ports never needs root. If you later want the dashboard (or other
tools) to *open* a serial device, the user running the app needs to be in
the `dialout` group:

```bash
sudo usermod -aG dialout $USER
# log out/in (or reboot) for it to take effect
```

## Notes on socat detection

The app looks at every running `socat` process, inspects its open file
descriptors (`/proc/<pid>/fd`) for anything pointing at `/dev/pts/N`, and
lists those as serial ports of type `socat`. It also tries to show any
`link=/path` argument from the command line for context (e.g. a friendly
symlink name you gave it). This means:

- It only picks up sockets already running when the page loads (it polls
  live state each refresh, so new ones appear automatically).
- If you run the dashboard as a different user than the one running
  `socat`, it may not have permission to read `/proc/<pid>/fd` — run both
  as the same user, or via sudo, if that happens.

## Security note

There is **no authentication** on this app, and log contents can be viewed
by anyone who can reach port 8000. That's fine on a trusted home LAN, but:

- Don't port-forward this to the public internet as-is.
- If you need remote phone access, put it behind a VPN (e.g. Tailscale,
  WireGuard) rather than exposing it directly.
- If you want basic protection, put it behind a reverse proxy (nginx/Caddy)
  with HTTP basic auth — ask if you'd like a config for that.

## Customizing

- `LOG_DIR` / `LOG_EXTENSIONS` in `app.py` — change the folder watched for
  logs, or restrict to certain extensions (e.g. `{".log"}`).
- `REFRESH_MS` in `static/app.js` — change the auto-refresh interval.
- `INTERNAL_SERIAL_PATTERNS` in `app.py` — adjust which serial devices are
  treated as "internal" and hidden.
