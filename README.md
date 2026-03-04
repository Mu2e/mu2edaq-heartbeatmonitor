# Mu2e DAQ HeartBeat Monitor

This is a lightweight, real-time health monitoring system intended for the Mu2e DAQ systems.  This operates much like other heartbeat systems that we have used for NOvA and other experiments.  Individual DAQ applications (or even light weight scripts) send periodic UDP heartbeat packets either to a specific host or via broadcast.  The monitoring application intercepts these packets and uses them to track which applications are alive.

The initial ping from a DAQ component registers it with the monitor and records the initial time of the heartbeat.  The monitor then expects to receive a similar heartbeat at a later time.  It measures the amount of time that has been elapsed between the latest heartbeat and the current time.  If that elapsed time exceeds a threshold (and here each application can have specific time duration, or can use a default one) then the application is considered to be "dead" and an error can be thrown.

The status of all the applications and their reported status are displays on a live webpage dashboard.  This dashboard updates/refreshes frequently so that you get a live view of the system.

Schematically the system is pretty simple.  It looks like:
```
  Remote systems                    Monitor server
  ──────────────                    ────────────────────────────────────
  heartbeat_sender  ──── UDP ────►  UDPListener (IPv4 / IPv6)
  (Python or C++)                         │
                                    SystemRegistry (thread-safe)
                                          │
  Web browser  ◄──── SSE / HTTP ──  Flask  /stream  /api/*  /
```

However this is a pretty major improvement over the equivalent heartbeat system we have used before, and it should be more compatible with our systems in terms of integration (e.g. this supports UDP ipv4 and ipv6 packet sending and supports broadcast and multicast so DAQ systems should be "auto-discovered" if they just ping the local network or link-local network.  Pretty slick....and SO MUCH BETTER than using DDS messages.

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

This package should be compatible with Alma Linux 9 (i.e. Python 3.9), but you will need to bootstrap the Python virtual environment to install dependancies before it will run.  To do this run the bootstrap script after cloning the repo.  This will install all the requirements from the requirements.txt file.

**Requirements:** Python 3.8+

```bash
pip install -r requirements.txt
python heartbeat_monitor.py
```

The web based dashboard will be available by default at `http://localhost:8080` and the server will be listening on UDP on port `9999`.  These configurations can be changed via the config file.

### Python sender

For sending packets, there are two separate sender implimentations.  There is a simple python script that can send packets either one at a time or in sequence.  Packet information can be specified via the commandline, or via a json file.

```bash
# One-shot
python heartbeat_sender.py --name my-app --status "OK"

# Continuous (every 10 s)
python heartbeat_sender.py --name my-app --status "running" --interval 10

# From a JSON config file
python heartbeat_sender.py --file example_sender.json
```

### C++ sender

There is also a C++ based sender application.  This is really just a commandline application wrapped around a simple library that calls a 'send_packet()' function.  See the 'heartbeat_sender_lib header.h' for details

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
