#!/usr/bin/env python3
"""
HeartBeat Monitor
Listens for UDP heartbeat packets from remote systems and displays
their status via an embedded web server.

Packet format (JSON over UDP):
{
    "name": "my-application",
    "timestamp": "2024-01-01T12:00:00.123456",
    "status": "OK"
}
"""

import atexit
import json
import logging
import os
import signal
import socket
import struct
import sys
import threading
import time
from datetime import datetime, timezone
from typing import Dict, List, Tuple
from pathlib import Path

import yaml
from flask import Flask, Response, jsonify, render_template

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("heartbeat_monitor")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DEFAULT_CONFIG = {
    "server": {
        "udp_host": "0.0.0.0",
        "udp_port": 9999,
        "web_host": "0.0.0.0",
        "web_port": 8080,
        "multicast_groups": [],
        "ipv6_enabled": True,
        "udp6_host": "::",
        "udp6_port": 9999,
        "multicast6_groups": [],
    },
    "monitor": {
        "default_timeout": 30,
        "cleanup_after": 300,
        "refresh_interval": 2,
        "grid_columns": 3,
    },
    "daemon": {
        "enabled": False,
        "pid_file": "/tmp/heartbeat_monitor.pid",
        "log_file": "/tmp/heartbeat_monitor.log",
        "working_dir": "/",
    },
    "systems": [],
}


# Directories searched (in order) when the config filename is not an absolute path
# and the file is not found in the current working directory.
CONFIG_SEARCH_DIRS = [
    Path.cwd(),
    Path.home() / ".config" / "heartbeat_monitor",
    Path("/etc/heartbeat_monitor"),
    Path(__file__).resolve().parent / "config",
    Path(__file__).resolve().parent,
]


def find_config(name: str) -> Path | None:
    """
    Resolve *name* to an existing file.

    If *name* is an absolute path or exists relative to cwd, return it directly.
    Otherwise walk CONFIG_SEARCH_DIRS and return the first match.
    Returns None if the file cannot be found anywhere.
    """
    p = Path(name)
    if p.is_absolute() or p.exists():
        return p if p.exists() else None
    for d in CONFIG_SEARCH_DIRS:
        candidate = d / p
        if candidate.exists():
            return candidate
    return None


def load_config(path: str = "config.yaml") -> dict:
    config = DEFAULT_CONFIG.copy()
    resolved = find_config(path)
    if resolved is None:
        searched = "\n  ".join(str(d / path) for d in CONFIG_SEARCH_DIRS)
        log.warning("Config file %r not found in any search location:\n  %s\nUsing defaults.",
                    path, searched)
        return config
    try:
        with open(resolved) as f:
            user_cfg = yaml.safe_load(f) or {}
        # Deep merge top-level sections
        for section, values in user_cfg.items():
            if section in config and isinstance(config[section], dict):
                config[section].update(values)
            else:
                config[section] = values
        log.info("Loaded configuration from %s", resolved)
    except OSError as exc:
        log.warning("Could not read config file %s: %s — using defaults", resolved, exc)
    return config


# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------

class SystemRegistry:
    """Thread-safe registry of monitored systems and their last heartbeat."""

    def __init__(self, config: dict):
        self._lock = threading.Lock()
        self._systems: Dict[str, dict] = {}
        self._default_timeout = config["monitor"]["default_timeout"]
        self._cleanup_after = config["monitor"]["cleanup_after"]

        # Build per-system timeout overrides from config
        self._timeouts: Dict[str, int] = {}
        self._descriptions: Dict[str, str] = {}
        for sys_cfg in config.get("systems", []):
            name = sys_cfg.get("name")
            if name:
                if "timeout" in sys_cfg:
                    self._timeouts[name] = sys_cfg["timeout"]
                if "description" in sys_cfg:
                    self._descriptions[name] = sys_cfg["description"]

    def record(self, name: str, sent_ts: str, status: str, source_addr: str):
        now = datetime.now(timezone.utc)
        with self._lock:
            self._systems[name] = {
                "name": name,
                "last_seen": now,
                "sent_timestamp": sent_ts,
                "status": status,
                "source": source_addr,
                "description": self._descriptions.get(name, ""),
                "timeout": self._timeouts.get(name, self._default_timeout),
            }

    def snapshot(self) -> List[dict]:
        """Return a sorted list of system status dicts."""
        now = datetime.now(timezone.utc)
        result = []
        with self._lock:
            for name, info in list(self._systems.items()):
                age = (now - info["last_seen"]).total_seconds()

                # Drop entries that have been silent far too long
                if age > self._cleanup_after:
                    del self._systems[name]
                    continue

                timeout = info["timeout"]
                timed_out = age > timeout
                entry = {
                    "name": name,
                    "age_seconds": round(age, 1),
                    "age_display": _fmt_age(age),
                    "sent_timestamp": info["sent_timestamp"],
                    "status": info["status"],
                    "source": info["source"],
                    "description": info["description"],
                    "timeout": timeout,
                    "timed_out": timed_out,
                    "state": "error" if timed_out else "ok",
                }
                result.append(entry)

        result.sort(key=lambda x: x["name"])
        return result

    def count(self) -> Tuple[int, int]:
        """Return (total, healthy) counts."""
        data = self.snapshot()
        healthy = sum(1 for s in data if not s["timed_out"])
        return len(data), healthy


def _fmt_age(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:.1f}s"
    if seconds < 3600:
        m, s = divmod(int(seconds), 60)
        return f"{m}m {s}s"
    h, rem = divmod(int(seconds), 3600)
    m = rem // 60
    return f"{h}h {m}m"


# ---------------------------------------------------------------------------
# UDP Listener
# ---------------------------------------------------------------------------

class UDPListener(threading.Thread):
    def __init__(self, host: str, port: int, registry: SystemRegistry,
                 family: int = socket.AF_INET, multicast_groups: list = None):
        v = "v6" if family == socket.AF_INET6 else "v4"
        super().__init__(daemon=True, name=f"udp-listener-{v}")
        self._host = host
        self._port = port
        self._registry = registry
        self._family = family
        self._multicast_groups = multicast_groups or []
        self._stop_event = threading.Event()

    def run(self):
        with socket.socket(self._family, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.settimeout(1.0)
            if self._family == socket.AF_INET6:
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            sock.bind((self._host, self._port))
            label = "v6" if self._family == socket.AF_INET6 else "v4"
            log.info("UDP %s listener bound to %s:%d", label, self._host, self._port)

            for entry in self._multicast_groups:
                if self._family == socket.AF_INET:
                    # entry is a plain IPv4 address string
                    mreq = struct.pack("4sL", socket.inet_aton(entry), socket.INADDR_ANY)
                    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
                    log.info("Joined IPv4 multicast group %s", entry)
                else:
                    # entry is {"group": "ff02::1", "interface": "eth0"}
                    group = entry["group"]
                    iface = entry.get("interface", "")
                    if_index = socket.if_nametoindex(iface) if iface else 0
                    mreq = struct.pack("16sI",
                                       socket.inet_pton(socket.AF_INET6, group), if_index)
                    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)
                    log.info("Joined IPv6 multicast group %s (interface=%s)",
                             group, iface or "default")

            while not self._stop_event.is_set():
                try:
                    data, addr = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                except Exception as exc:
                    log.error("UDP receive error: %s", exc)
                    continue

                self._handle(data, addr)

    def _handle(self, data: bytes, addr: tuple):
        # addr is (host, port) for IPv4; (host, port, flowinfo, scope_id) for IPv6
        host, port = addr[0], addr[1]
        source = f"[{host}]:{port}" if self._family == socket.AF_INET6 else f"{host}:{port}"
        try:
            packet = json.loads(data.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            log.warning("Malformed packet from %s: %s", source, exc)
            return

        name = packet.get("name", "").strip()
        timestamp = packet.get("timestamp", "")
        status = packet.get("status", "")

        if not name:
            log.warning("Packet from %s missing 'name' field", source)
            return

        log.debug("Heartbeat from %s (%s): %s", name, source, status)
        self._registry.record(name, timestamp, status, source)

    def stop(self):
        self._stop_event.set()


# ---------------------------------------------------------------------------
# Flask Web Application
# ---------------------------------------------------------------------------

app = Flask(__name__)
_registry: SystemRegistry = None
_config: dict = {}


@app.route("/")
def index():
    return render_template(
        "index.html",
        refresh_interval=_config["monitor"]["refresh_interval"],
        udp_port=_config["server"]["udp_port"],
        grid_columns=_config["monitor"].get("grid_columns", 3),
    )


@app.route("/api/systems")
def api_systems():
    systems = _registry.snapshot()
    total, healthy = _registry.count()
    return jsonify({
        "systems": systems,
        "total": total,
        "healthy": healthy,
        "unhealthy": total - healthy,
        "server_time": datetime.now(timezone.utc).isoformat(),
    })


@app.route("/api/health")
def api_health():
    total, healthy = _registry.count()
    status = "ok" if total == healthy else "degraded"
    return jsonify({"status": status, "total": total, "healthy": healthy})


@app.route("/stream")
def stream():
    """Server-Sent Events endpoint for real-time updates."""
    def generate():
        while True:
            systems = _registry.snapshot()
            total, healthy = _registry.count()
            payload = json.dumps({
                "systems": systems,
                "total": total,
                "healthy": healthy,
                "unhealthy": total - healthy,
                "server_time": datetime.now(timezone.utc).isoformat(),
            })
            yield f"data: {payload}\n\n"
            time.sleep(_config["monitor"]["refresh_interval"])

    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache",
                             "X-Accel-Buffering": "no"})


# ---------------------------------------------------------------------------
# Daemon support
# ---------------------------------------------------------------------------

def daemonize(pid_file: str, log_file: str, working_dir: str = "/"):
    """
    Detach the process from the terminal using the classic double-fork technique.
    Writes the daemon PID to *pid_file* and redirects stdout/stderr to *log_file*.
    Registers an atexit handler to remove the PID file on clean exit.
    """
    if not hasattr(os, "fork"):
        raise RuntimeError("Daemon mode is only supported on POSIX systems.")

    # --- First fork -----------------------------------------------------------
    try:
        pid = os.fork()
        if pid > 0:
            # Parent exits; child becomes a session leader.
            sys.exit(0)
    except OSError as exc:
        raise RuntimeError(f"First fork failed: {exc}") from exc

    # Decouple from the parent environment
    os.chdir(working_dir)
    os.setsid()
    os.umask(0)

    # --- Second fork ----------------------------------------------------------
    try:
        pid = os.fork()
        if pid > 0:
            # First child exits; grandchild can never re-acquire a tty.
            sys.exit(0)
    except OSError as exc:
        raise RuntimeError(f"Second fork failed: {exc}") from exc

    # Flush before redirecting
    sys.stdout.flush()
    sys.stderr.flush()

    # Redirect stdin to /dev/null, stdout/stderr to the log file
    with open(os.devnull, "rb") as devnull:
        os.dup2(devnull.fileno(), sys.stdin.fileno())

    log_path = Path(log_file).expanduser()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with open(log_path, "ab") as lf:
        os.dup2(lf.fileno(), sys.stdout.fileno())
        os.dup2(lf.fileno(), sys.stderr.fileno())

    # Reconfigure the root logging handler to write to the log file now that
    # stderr has been redirected.
    root_logger = logging.getLogger()
    for handler in list(root_logger.handlers):
        root_logger.removeHandler(handler)
    file_handler = logging.FileHandler(str(log_path))
    file_handler.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    ))
    root_logger.addHandler(file_handler)

    # Write PID file
    pid_path = Path(pid_file).expanduser()
    pid_path.parent.mkdir(parents=True, exist_ok=True)
    pid_path.write_text(str(os.getpid()))
    atexit.register(lambda: pid_path.unlink(missing_ok=True))

    # Remove PID file on SIGTERM as well
    def _sigterm(signum, frame):
        pid_path.unlink(missing_ok=True)
        sys.exit(0)
    signal.signal(signal.SIGTERM, _sigterm)

    log.info("Daemon started (PID %d). Logging to %s", os.getpid(), log_path)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    global _registry, _config

    import argparse
    parser = argparse.ArgumentParser(description="HeartBeat Monitor")
    parser.add_argument("-c", "--config", default="config.yaml",
                        help="Config file name or path (default: config.yaml). "
                             "If not an absolute path, searched in: cwd, "
                             "~/.config/heartbeat_monitor/, /etc/heartbeat_monitor/, "
                             "<install>/config/, <install>/")
    parser.add_argument("--debug", action="store_true",
                        help="Enable Flask debug mode")
    parser.add_argument("-d", "--daemon", action="store_true", default=False,
                        help="Run as a background daemon")
    parser.add_argument("--pid-file", default=None,
                        help="PID file path when running as a daemon "
                             "(default: from config or /tmp/heartbeat_monitor.pid)")
    parser.add_argument("--log-file", default=None,
                        help="Log file path when running as a daemon "
                             "(default: from config or /tmp/heartbeat_monitor.log)")
    args = parser.parse_args()

    _config = load_config(args.config)
    _registry = SystemRegistry(_config)

    # Daemon mode: CLI flags override config file
    daemon_cfg = _config.get("daemon", {})
    run_as_daemon = args.daemon or daemon_cfg.get("enabled", False)

    if run_as_daemon:
        pid_file = args.pid_file or daemon_cfg.get("pid_file", "/tmp/heartbeat_monitor.pid")
        log_file = args.log_file or daemon_cfg.get("log_file", "/tmp/heartbeat_monitor.log")
        working_dir = daemon_cfg.get("working_dir", "/")
        print(f"Starting daemon (PID file: {pid_file}, log: {log_file})")
        daemonize(pid_file, log_file, working_dir)

    udp_cfg = _config["server"]

    v4_listener = UDPListener(
        udp_cfg["udp_host"], udp_cfg["udp_port"], _registry,
        family=socket.AF_INET,
        multicast_groups=udp_cfg.get("multicast_groups", []),
    )
    v4_listener.start()

    if udp_cfg.get("ipv6_enabled", True):
        v6_listener = UDPListener(
            udp_cfg["udp6_host"], udp_cfg["udp6_port"], _registry,
            family=socket.AF_INET6,
            multicast_groups=udp_cfg.get("multicast6_groups", []),
        )
        v6_listener.start()

    web_host = udp_cfg["web_host"]
    web_port = _config["server"]["web_port"]
    log.info("Web server starting on http://%s:%d", web_host, web_port)

    app.run(
        host=web_host,
        port=web_port,
        debug=args.debug,
        use_reloader=False,   # reloader conflicts with our background thread
        threaded=True,
    )


if __name__ == "__main__":
    main()
