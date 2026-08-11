"""
supervisor.py
-------------
A tiny process supervisor for apps we launch ourselves (so we're the real
parent process and can catch the real exit status - including which signal
killed it, e.g. SIGSEGV on a segfault).

Config lives in apps.json (see apps.example.json). Each configured app is
started as a subprocess; stdout/stderr are redirected into /tmp/<name>.out.log
and /tmp/<name>.err.log (so they show up automatically in the dashboard's
existing "Logs" card). Crashes/restarts are appended to /tmp/<name>.crashes.log.
"""

import os
import json
import time
import signal
import threading
import subprocess
from datetime import datetime

import psutil

CONFIG_PATH = os.environ.get("APPS_CONFIG", os.path.join(os.path.dirname(__file__), "apps.json"))
LOG_DIR = "/tmp"

# Crash-loop protection: if an app restarts more than MAX_RESTARTS times
# within RESTART_WINDOW_SEC, stop auto-restarting it and mark it as
# needing manual attention.
MAX_RESTARTS = 5
RESTART_WINDOW_SEC = 60
STOP_KILL_TIMEOUT = 5  # seconds to wait after SIGTERM before SIGKILL

# Signals worth calling out specifically as "crash-like" vs. a clean exit
CRASH_SIGNALS = {
    signal.SIGSEGV: "SIGSEGV (segmentation fault)",
    signal.SIGABRT: "SIGABRT (abort - e.g. assert() or malloc corruption)",
    signal.SIGBUS: "SIGBUS (bus error - bad memory access)",
    signal.SIGFPE: "SIGFPE (floating point exception - e.g. divide by zero)",
    signal.SIGILL: "SIGILL (illegal instruction)",
    signal.SIGKILL: "SIGKILL (killed - possibly OOM killer, or `kill -9`)",
}


def _now():
    return datetime.now().isoformat(timespec="seconds")


class ManagedApp:
    """Supervises a single executable: start/stop/restart, track state."""

    def __init__(self, config):
        self.name = config["name"]
        self.command = config["command"]  # list, e.g. ["/home/pi/myapp", "--flag"]
        self.cwd = config.get("cwd") or os.path.dirname(self.command[0]) or "."
        self.env = config.get("env") or None
        self.autostart = bool(config.get("autostart", False))
        self.restart_policy = config.get("restart_policy", "on-failure")  # never | on-failure | always
        self.description = config.get("description", "")

        self.lock = threading.RLock()
        self.proc = None  # subprocess.Popen
        self.pid = None
        self.status = "stopped"  # stopped | running | crashed | exited | crash_loop | failed_to_start
        self.started_at = None
        self.stopped_at = None
        self.last_exit_code = None
        self.last_signal = None
        self.restart_count = 0
        self._restart_times = []  # timestamps of recent restarts, for loop detection
        self._stop_requested = False
        self._disabled = False  # user asked to stop - blocks a pending auto-restart too
        self._watcher_thread = None

        self.out_log = os.path.join(LOG_DIR, f"{self.name}.out.log")
        self.err_log = os.path.join(LOG_DIR, f"{self.name}.err.log")
        self.crash_log = os.path.join(LOG_DIR, f"{self.name}.crashes.log")

    # -- lifecycle -----------------------------------------------------

    def start(self, manual=False):
        with self.lock:
            if self.proc is not None and self.proc.poll() is None:
                return False, "already running"

            if manual:
                # Manual restart resets crash-loop tracking
                self._restart_times = []
                self.restart_count = 0

            try:
                out_f = open(self.out_log, "ab")
                err_f = open(self.err_log, "ab")
                self.proc = subprocess.Popen(
                    self.command,
                    cwd=self.cwd,
                    env=self.env,
                    stdout=out_f,
                    stderr=err_f,
                    start_new_session=True,  # own process group -> can stop children too
                )
            except (FileNotFoundError, PermissionError, OSError) as e:
                self.status = "failed_to_start"
                self._append_crash_log(f"FAILED TO START: {e}")
                return False, str(e)

            self.pid = self.proc.pid
            self.status = "running"
            self.started_at = _now()
            self.stopped_at = None
            self._stop_requested = False

            self._watcher_thread = threading.Thread(target=self._watch, daemon=True)
            self._watcher_thread.start()
            return True, "started"

    def stop(self, manual=True):
        with self.lock:
            if self.proc is None or self.proc.poll() is not None:
                self.status = "stopped"
                return False, "not running"
            self._stop_requested = True
            pid = self.proc.pid

        try:
            os.killpg(os.getpgid(pid), signal.SIGTERM)
        except ProcessLookupError:
            return True, "already gone"
        except PermissionError as e:
            return False, str(e)

        try:
            self.proc.wait(timeout=STOP_KILL_TIMEOUT)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
        return True, "stopped"

    def restart(self):
        self.stop(manual=True)
        time.sleep(0.3)  # let resources/ports release before relaunch
        return self.start(manual=True)

    # -- internal: waits on the child, records exit info, maybe restarts --

    def _watch(self):
        proc = self.proc
        returncode = proc.wait()
        with self.lock:
            self.stopped_at = _now()
            if returncode < 0:
                sig_num = -returncode
                try:
                    sig_name = signal.Signals(sig_num).name
                except ValueError:
                    sig_name = f"signal {sig_num}"
                self.last_signal = CRASH_SIGNALS.get(sig_num, sig_name)
                self.last_exit_code = None
                crashed = True
            else:
                self.last_signal = None
                self.last_exit_code = returncode
                crashed = returncode != 0

            requested = self._stop_requested
            self.status = "stopped" if requested else ("crashed" if crashed else "exited")

            if not requested:
                self._append_crash_log(self._format_exit_reason())

            should_restart = (
                not requested
                and self.restart_policy != "never"
                and (self.restart_policy == "always" or crashed)
            )

        if should_restart:
            self._maybe_restart()

    def _format_exit_reason(self):
        if self.last_signal:
            return f"CRASHED - killed by {self.last_signal}"
        return f"EXITED - exit code {self.last_exit_code}"

    def _append_crash_log(self, reason):
        try:
            with open(self.crash_log, "a") as f:
                f.write(f"[{_now()}] pid={self.pid} {reason}\n")
        except OSError:
            pass

    def _maybe_restart(self):
        now = time.time()
        with self.lock:
            self._restart_times = [t for t in self._restart_times if now - t < RESTART_WINDOW_SEC]
            self._restart_times.append(now)
            self.restart_count += 1

            if len(self._restart_times) > MAX_RESTARTS:
                self.status = "crash_loop"
                self._append_crash_log(
                    f"giving up: {len(self._restart_times)} restarts in "
                    f"{RESTART_WINDOW_SEC}s - use manual restart once fixed"
                )
                return

        time.sleep(1)  # brief backoff before relaunch
        self.start(manual=False)

    # -- status ----------------------------------------------------------

    def get_status(self):
        with self.lock:
            info = {
                "name": self.name,
                "description": self.description,
                "command": " ".join(self.command),
                "status": self.status,
                "pid": self.pid if self.status == "running" else None,
                "started_at": self.started_at,
                "stopped_at": self.stopped_at,
                "last_exit_code": self.last_exit_code,
                "last_signal": self.last_signal,
                "restart_count": self.restart_count,
                "restart_policy": self.restart_policy,
                "autostart": self.autostart,
                "cpu_percent": None,
                "memory_bytes": None,
                "uptime_sec": None,
            }

        if info["status"] == "running" and self.pid:
            try:
                p = psutil.Process(self.pid)
                info["cpu_percent"] = p.cpu_percent(interval=None)
                info["memory_bytes"] = p.memory_info().rss
                info["uptime_sec"] = round(time.time() - p.create_time(), 1)
            except psutil.NoSuchProcess:
                pass
        return info


class Supervisor:
    def __init__(self, config_path=CONFIG_PATH):
        self.config_path = config_path
        self.apps = {}  # name -> ManagedApp
        self._load()

    def _load(self):
        if not os.path.exists(self.config_path):
            return
        with open(self.config_path) as f:
            data = json.load(f)
        for entry in data.get("apps", []):
            app = ManagedApp(entry)
            self.apps[app.name] = app

    def autostart_all(self):
        for app in self.apps.values():
            if app.autostart:
                app.start(manual=False)

    def get(self, name):
        return self.apps.get(name)

    def list_status(self):
        return [app.get_status() for app in self.apps.values()]

    def shutdown_all(self):
        for app in self.apps.values():
            if app.status == "running":
                app.stop(manual=True)
