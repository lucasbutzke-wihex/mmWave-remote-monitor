#!/usr/bin/env python3
"""
Raspberry Pi Dashboard
----------------------
Small, dependency-light web dashboard showing:
  - Connected serial ports (USB/ACM adapters + socat virtual ports),
    excluding the Pi's internal UARTs.
  - Disk usage per mounted partition.
  - CPU / RAM / temperature.
  - Network throughput (like btop's up/down speed).
  - Log files found in /tmp (list + tail viewer).

Run:
    python3 app.py

Then open http://<pi-ip>:8000 from your phone/laptop on the same network.

No authentication is built in on purpose (kept simple) - see README.md
for notes on restricting access if you expose this beyond your LAN.
"""

import os
import re
import time
import threading
import subprocess
from datetime import datetime

import psutil
from flask import Flask, jsonify, render_template, abort, request

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

app = Flask(__name__)

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

LOG_DIR = "/tmp"
LOG_EXTENSIONS = None  # e.g. {".log"} to filter, None = show every regular file
MAX_LOG_LIST = 50
DEFAULT_TAIL_LINES = 200
MAX_TAIL_LINES = 2000

# Internal / on-board serial devices to hide (Raspberry Pi UARTs, consoles, etc.)
INTERNAL_SERIAL_PATTERNS = [
    r"^/dev/ttyAMA\d+$",
    r"^/dev/ttyS\d+$",
    r"^/dev/serial\d*$",
    r"^/dev/ttyprintk$",
    r"^/dev/console$",
]
_internal_re = re.compile("|".join(INTERNAL_SERIAL_PATTERNS))

# ---------------------------------------------------------------------------
# Network rate tracking (need two samples to compute a speed)
# ---------------------------------------------------------------------------

_net_lock = threading.Lock()
_net_prev = {"t": None, "sent": 0, "recv": 0}


def get_network_stats():
    counters = psutil.net_io_counters()
    now = time.time()

    global _net_prev
    with _net_lock:
        prev = _net_prev
        sent_rate = recv_rate = 0.0
        if prev["t"] is not None:
            dt = max(now - prev["t"], 1e-6)
            sent_rate = max((counters.bytes_sent - prev["sent"]) / dt, 0)
            recv_rate = max((counters.bytes_recv - prev["recv"]) / dt, 0)
        _net_prev = {"t": now, "sent": counters.bytes_sent, "recv": counters.bytes_recv}

    return {
        "sent_rate_bps": sent_rate,
        "recv_rate_bps": recv_rate,
        "total_sent": counters.bytes_sent,
        "total_recv": counters.bytes_recv,
    }


# ---------------------------------------------------------------------------
# Serial ports
# ---------------------------------------------------------------------------

def is_internal_port(device: str) -> bool:
    return bool(_internal_re.match(device))


def get_physical_serial_ports():
    ports = []
    if list_ports is None:
        return ports
    for p in list_ports.comports():
        if is_internal_port(p.device):
            continue
        ports.append({
            "device": p.device,
            "description": p.description or "Serial device",
            "hwid": p.hwid or "",
            "type": "usb",
        })
    return ports


def get_socat_serial_ports():
    """Find PTYs owned by running socat processes."""
    results = []
    for proc in psutil.process_iter(["pid", "name", "cmdline"]):
        try:
            name = (proc.info.get("name") or "")
            cmdline = proc.info.get("cmdline") or []
            if "socat" not in name and not any("socat" in c for c in cmdline):
                continue

            pid = proc.info["pid"]
            cmd_str = " ".join(cmdline) if cmdline else name

            # Try to find any friendly "link=" path(s) in the command for context
            links = re.findall(r"link=([^\s,]+)", cmd_str)

            pty_devices = set()
            fd_dir = f"/proc/{pid}/fd"
            try:
                for fd in os.listdir(fd_dir):
                    try:
                        target = os.readlink(os.path.join(fd_dir, fd))
                    except OSError:
                        continue
                    if target.startswith("/dev/pts/"):
                        pty_devices.add(target)
            except (FileNotFoundError, PermissionError):
                pass

            if not pty_devices:
                # Fall back: nothing readable via /proc, still report the process
                # if its cmdline references a pty explicitly.
                for m in re.findall(r"/dev/pts/\d+", cmd_str):
                    pty_devices.add(m)

            if not pty_devices:
                continue

            desc = cmd_str
            if links:
                desc = f"socat link -> {', '.join(links)}"
            if len(desc) > 90:
                desc = desc[:87] + "..."

            for dev in sorted(pty_devices):
                results.append({
                    "device": dev,
                    "description": desc,
                    "hwid": f"pid {pid}",
                    "type": "socat",
                    "pid": pid,
                })
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return results


def get_all_serial_ports():
    ports = get_physical_serial_ports() + get_socat_serial_ports()
    # de-dupe by device path, keep first occurrence
    seen = set()
    unique = []
    for p in ports:
        if p["device"] in seen:
            continue
        seen.add(p["device"])
        unique.append(p)
    unique.sort(key=lambda p: p["device"])
    return unique


# ---------------------------------------------------------------------------
# System stats
# ---------------------------------------------------------------------------

def get_cpu_temp():
    # Raspberry Pi / most Linux SBCs
    thermal_path = "/sys/class/thermal/thermal_zone0/temp"
    try:
        with open(thermal_path) as f:
            return round(int(f.read().strip()) / 1000.0, 1)
    except (FileNotFoundError, ValueError, PermissionError):
        pass
    # Fallback to vcgencmd if available (Raspberry Pi OS)
    try:
        out = subprocess.run(
            ["vcgencmd", "measure_temp"], capture_output=True, text=True, timeout=2
        )
        m = re.search(r"[-\d.]+", out.stdout)
        if m:
            return round(float(m.group()), 1)
    except (FileNotFoundError, subprocess.SubprocessError):
        pass
    return None


def get_cpu_stats():
    return {
        "percent": psutil.cpu_percent(interval=None),
        "per_core": psutil.cpu_percent(interval=None, percpu=True),
        "temp_c": get_cpu_temp(),
        "load_avg": os.getloadavg() if hasattr(os, "getloadavg") else None,
    }


def get_memory_stats():
    vm = psutil.virtual_memory()
    sm = psutil.swap_memory()
    return {
        "total": vm.total,
        "used": vm.used,
        "available": vm.available,
        "percent": vm.percent,
        "swap_total": sm.total,
        "swap_used": sm.used,
        "swap_percent": sm.percent,
    }


SKIP_FS_TYPES = {"tmpfs", "devtmpfs", "squashfs", "overlay", "proc", "sysfs", "cgroup", "cgroup2", "devpts"}


def get_disk_stats():
    disks = []
    seen_mounts = set()
    for part in psutil.disk_partitions(all=False):
        if part.fstype in SKIP_FS_TYPES:
            continue
        if part.mountpoint in seen_mounts:
            continue
        try:
            usage = psutil.disk_usage(part.mountpoint)
        except (PermissionError, FileNotFoundError):
            continue
        seen_mounts.add(part.mountpoint)
        disks.append({
            "mount": part.mountpoint,
            "device": part.device,
            "fstype": part.fstype,
            "total": usage.total,
            "used": usage.used,
            "free": usage.free,
            "percent": usage.percent,
        })
    disks.sort(key=lambda d: d["mount"])
    return disks


# ---------------------------------------------------------------------------
# Logs (/tmp)
# ---------------------------------------------------------------------------

def get_log_files():
    entries = []
    try:
        with os.scandir(LOG_DIR) as it:
            for entry in it:
                if not entry.is_file(follow_symlinks=False):
                    continue
                if LOG_EXTENSIONS and not any(entry.name.endswith(ext) for ext in LOG_EXTENSIONS):
                    continue
                try:
                    stat = entry.stat()
                except OSError:
                    continue
                entries.append({
                    "name": entry.name,
                    "size": stat.st_size,
                    "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="seconds"),
                    "mtime_ts": stat.st_mtime,
                })
    except FileNotFoundError:
        pass
    entries.sort(key=lambda e: e["mtime_ts"], reverse=True)
    return entries[:MAX_LOG_LIST]


def safe_log_path(name: str) -> str:
    """Resolve a log filename to a real path strictly inside LOG_DIR."""
    if not name or "/" in name or "\\" in name or name in (".", ".."):
        abort(400, "Invalid file name")
    full = os.path.realpath(os.path.join(LOG_DIR, name))
    real_dir = os.path.realpath(LOG_DIR)
    if os.path.dirname(full) != real_dir or not os.path.isfile(full):
        abort(404, "File not found")
    return full


def tail_file(path: str, n_lines: int) -> str:
    # Simple, dependency-free tail: read from the end in chunks.
    n_lines = max(1, min(n_lines, MAX_TAIL_LINES))
    chunk_size = 4096
    data = b""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            remaining = f.tell()
            while remaining > 0 and data.count(b"\n") <= n_lines:
                read_size = min(chunk_size, remaining)
                remaining -= read_size
                f.seek(remaining)
                data = f.read(read_size) + data
    except OSError as e:
        return f"(could not read file: {e})"

    text = data.decode("utf-8", errors="replace")
    lines = text.splitlines()
    return "\n".join(lines[-n_lines:])


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/data")
def api_data():
    return jsonify({
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "serial_ports": get_all_serial_ports(),
        "cpu": get_cpu_stats(),
        "memory": get_memory_stats(),
        "disks": get_disk_stats(),
        "network": get_network_stats(),
        "logs": get_log_files(),
    })


@app.route("/api/logs/<path:name>")
def api_log_tail(name):
    path = safe_log_path(name)
    try:
        lines = int(request.args.get("lines", DEFAULT_TAIL_LINES))
    except ValueError:
        lines = DEFAULT_TAIL_LINES
    return jsonify({
        "name": name,
        "path": path,
        "lines_requested": lines,
        "content": tail_file(path, lines),
    })


if __name__ == "__main__":
    # threaded=True so the log-tail / refresh requests don't block each other
    app.run(host="0.0.0.0", port=8000, threaded=True)
