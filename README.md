# HeartBeat Monitor

A lightweight, real-time health monitoring system for distributed services. Remote systems send periodic UDP heartbeat packets; the monitor tracks their status and displays it on a live web dashboard.

```
  Remote systems                    Monitor server
  ──────────────                    ────────────────────────────────────
  heartbeat_sender  ──── UDP ────►  UDPListener (IPv4 / IPv6)
  (Python or C++)                         │
                                    SystemRegistry (thread-safe)
                                          │
  Web browser  ◄──── SSE / HTTP ──  Flask  /stream  /api/*  /
```

---

## Features

- **UDP heartbeat reception** — dual-stack IPv4 and IPv6 listeners, with support for unicast, broadcast, and multicast
- **Live web dashboard** — real-time status cards with progress bars and age display, pushed via Server-Sent Events (no polling)
- **REST API** — `/api/systems` and `/api/health` for programmatic access
- **Per-system timeouts** — global default with optional per-system overrides in config
- **Automatic cleanup** — stale entries are removed after a configurable silence period
- **Sender clients** — Python and C++ (CMake) implementations with one-shot, continuous, and file-based config modes
- **Minimal dependencies** — Flask and PyYAML only; no database, no message broker

---

## Quick Start

### Monitor server

**Requirements:** Python 3.8+

```bash
pip install -r requirements.txt
python heartbeat_monitor.py
```

The dashboard is available at `http://localhost:8080`. The UDP listener is on port `9999`.

### Python sender

```bash
# One-shot
python heartbeat_sender.py --name my-app --status "OK"

# Continuous (every 10 s)
python heartbeat_sender.py --name my-app --status "running" --interval 10

# From a JSON config file
python heartbeat_sender.py --file example_sender.json
```

### C++ sender

**Requirements:** CMake 3.14+, C++17 compiler

```bash
cd cpp_sender
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```bash
# One-shot
./cpp_sender/build/heartbeat_sender --name my-app --status OK

# Continuous (every 10 s)
./cpp_sender/build/heartbeat_sender --name my-app --status running --interval 10

# From a JSON config file
./cpp_sender/build/heartbeat_sender --file example_sender.json
```

The C++ sender has the same CLI interface as the Python version. It fetches [nlohmann/json](https://github.com/nlohmann/json) automatically at configure time if not found on the system.

---

## Sender options

| Flag | Short | Default | Description |
|------|-------|---------|-------------|
| `--name` | `-n` | *(required)* | Unique name for this system |
| `--status` | `-s` | `OK` | Status string shown on the dashboard |
| `--host` | | `127.0.0.1` | Monitor host or IP address |
| `--port` | `-p` | `9999` | Monitor UDP port |
| `--interval` | `-i` | `0` | Send every N seconds; `0` = one-shot |
| `--count` | | `0` | Stop after N packets; `0` = unlimited |
| `--interface` | `-I` | | Network interface for IPv6 link-local |
| `--file` | `-f` | | JSON config file (CLI args override) |
| `--verbose` | `-v` | | Print each packet as it is sent |

---

## Configuration

Copy `config/config.yaml` and edit as needed:

```yaml
server:
  udp_host: "0.0.0.0"
  udp_port: 9999
  ipv6_enabled: true
  udp6_host: "::"
  udp6_port: 9999
  web_host: "0.0.0.0"
  web_port: 8080

monitor:
  default_timeout: 60       # seconds before marking a system timed out
  cleanup_after: 86400      # seconds before removing a silent system (24 h)
  refresh_interval: 2       # SSE push interval in seconds
  grid_columns: 3           # dashboard grid width (1–6)

# Optional per-system overrides
systems:
  - name: "critical-service"
    timeout: 10
    description: "Critical production service"
```

```bash
python heartbeat_monitor.py --config /path/to/my-config.yaml
```

---

## REST API

| Endpoint | Description |
|----------|-------------|
| `GET /` | Web dashboard |
| `GET /api/systems` | JSON snapshot of all system states |
| `GET /api/health` | Aggregate `{"status": "ok"\|"degraded", "total": N, "healthy": N}` |
| `GET /stream` | Server-Sent Events stream (used by the dashboard) |

---

## Networking

The sender and monitor both support IPv4 unicast, IPv4 broadcast, IPv4 multicast, IPv6 unicast, IPv6 link-local (with interface scope), and IPv6 multicast. See [docs/README.md](docs/README.md) for full details and configuration examples.

---

## Packet format

Heartbeat packets are UTF-8 JSON sent over UDP:

```json
{
  "name": "my-application",
  "timestamp": "2024-01-01T12:00:00.123456+00:00",
  "status": "OK"
}
```

`name` is required; `timestamp` and `status` are optional.

---

## Project layout

```
heartbeat_monitor.py     Monitor server (Flask + UDP listeners)
heartbeat_sender.py      Python sender client
example_sender.json      Example JSON sender config
config/config.yaml       Server configuration
templates/index.html     Web dashboard (single-file, no build step)
cpp_sender/
  heartbeat_sender.cpp   C++ sender client
  CMakeLists.txt         CMake build definition
docs/README.md           Full documentation
```
