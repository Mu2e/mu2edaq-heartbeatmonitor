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

import json
import logging
import socket
import struct
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
    "systems": [],
}


def load_config(path: str = "config.yaml") -> dict:
    config = DEFAULT_CONFIG.copy()
    try:
        with open(path) as f:
            user_cfg = yaml.safe_load(f) or {}
        # Deep merge top-level sections
        for section, values in user_cfg.items():
            if section in config and isinstance(config[section], dict):
                config[section].update(values)
            else:
                config[section] = values
        log.info("Loaded configuration from %s", path)
    except FileNotFoundError:
        log.warning("Config file %s not found, using defaults", path)
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
# Entry point
# ---------------------------------------------------------------------------

def main():
    global _registry, _config

    import argparse
    parser = argparse.ArgumentParser(description="HeartBeat Monitor")
    parser.add_argument("-c", "--config", default="config.yaml",
                        help="Path to YAML config file (default: config.yaml)")
    parser.add_argument("--debug", action="store_true",
                        help="Enable Flask debug mode")
    args = parser.parse_args()

    _config = load_config(args.config)
    _registry = SystemRegistry(_config)

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
