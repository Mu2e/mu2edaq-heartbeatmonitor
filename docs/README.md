# HeartBeat Monitor

A lightweight, real-time health monitoring system for distributed services. Remote systems send periodic UDP heartbeat packets; the monitor tracks their status and displays it on a live web dashboard.

## Table of Contents

- [Architecture](#architecture)
- [Installation](#installation)
- [Monitor Server](#monitor-server)
  - [Running the Server](#running-the-server)
  - [Configuration Reference](#configuration-reference)
  - [REST API](#rest-api)
- [Heartbeat Sender](#heartbeat-sender)
  - [CLI Usage](#cli-usage)
  - [JSON File Config](#json-file-config)
- [Packet Format](#packet-format)
- [Networking](#networking)
  - [IPv4 Unicast](#ipv4-unicast)
  - [IPv4 Broadcast](#ipv4-broadcast)
  - [IPv4 Multicast](#ipv4-multicast)
  - [IPv6 Unicast](#ipv6-unicast)
  - [IPv6 Link-Local](#ipv6-link-local)
  - [IPv6 Multicast](#ipv6-multicast)

---

## Architecture

```
  Remote systems                Monitor server
  ─────────────                 ──────────────────────────────────────
  heartbeat_sender.py  ──UDP──► UDPListener (v4, AF_INET)  ─┐
                                UDPListener (v6, AF_INET6)  ─┤─► SystemRegistry
                                                              │         │
  Web browser          ◄─SSE──  Flask /stream  ◄─────────────┘         │
                       ◄─HTTP── Flask /api/*   ◄───────────────────────┘
```

The server runs two independent daemon threads — one IPv4 and one IPv6 UDP listener — that both write into a single thread-safe `SystemRegistry`. Flask serves the dashboard and REST API. The dashboard receives live updates via Server-Sent Events (SSE) without polling.

A system is considered **healthy** if a heartbeat has been received within its configured timeout window. Systems that remain silent beyond `cleanup_after` seconds are removed from the registry entirely.

---

## Installation

Requires Python 3.8+.

```bash
pip install -r requirements.txt
```

Dependencies: `flask>=3.0.0`, `pyyaml>=6.0`

---

## Monitor Server

### Running the Server

```bash
# Start with the default config file (config/config.yaml)
python heartbeat_monitor.py

# Start with a custom config file
python heartbeat_monitor.py --config /path/to/my-config.yaml

# Enable Flask debug mode
python heartbeat_monitor.py --debug
```

On startup the server binds two UDP sockets (IPv4 on `0.0.0.0` and IPv6 on `::`, both port `9999` by default) and starts the web dashboard on port `8080`. All addresses and ports are configurable.

---

### Configuration Reference

Configuration is loaded from a YAML file. Unknown keys are ignored; all keys are optional and fall back to the defaults shown below.

```yaml
server:
  # ── IPv4 UDP listener ──────────────────────────────────────────────────────
  udp_host: "0.0.0.0"       # Bind address. Use a specific IP to restrict the interface.
  udp_port: 9999

  # Optional: IPv4 multicast groups to join on the UDP socket.
  # multicast_groups:
  #   - "224.0.0.1"

  # ── IPv6 UDP listener ──────────────────────────────────────────────────────
  ipv6_enabled: true         # Set to false to disable the IPv6 listener entirely.
  udp6_host: "::"            # Bind address. "::" accepts from all interfaces.
  udp6_port: 9999            # Can share the same port number as udp_port.

  # Optional: IPv6 multicast groups to join. Link-local groups require an interface.
  # multicast6_groups:
  #   - group: "ff02::1"
  #     interface: "eth0"
  #   - group: "ff0e::1"    # Global-scope multicast needs no interface.

  # ── Web server ─────────────────────────────────────────────────────────────
  web_host: "0.0.0.0"
  web_port: 8080

monitor:
  default_timeout: 60        # Seconds without a heartbeat before a system is marked timed out.
  cleanup_after: 86400       # Seconds of silence before a system entry is removed (24 h).
  refresh_interval: 2        # Seconds between SSE pushes to the dashboard.
  grid_columns: 3            # Dashboard grid width: 1–6 columns.

# Optional per-system overrides. Any system not listed uses default_timeout.
systems:
  - name: "critical-service"
    timeout: 10              # Override timeout for this system only.
    description: "Critical production service"
  - name: "batch-processor"
    timeout: 120
    description: "Nightly batch processing job"
```

**Timeout logic:**

| Condition | Dashboard state |
|---|---|
| Age ≤ timeout | Green — healthy |
| Age > timeout | Red — timed out |
| Age > `cleanup_after` | Entry removed |

---

### REST API

All responses are JSON.

#### `GET /api/systems`

Returns the current state of every known system.

```json
{
  "server_time": "2024-01-01T12:00:00.000000+00:00",
  "total": 3,
  "healthy": 2,
  "unhealthy": 1,
  "systems": [
    {
      "name": "my-app",
      "state": "ok",
      "timed_out": false,
      "status": "running normally",
      "age_seconds": 4.2,
      "age_display": "4.2s",
      "timeout": 60,
      "sent_timestamp": "2024-01-01T11:59:55.123456+00:00",
      "source": "192.168.1.10:54321",
      "description": ""
    }
  ]
}
```

`source` is formatted as `host:port` for IPv4 and `[host]:port` for IPv6.

#### `GET /api/health`

Aggregate health summary. Returns HTTP 200 in both cases; inspect `status` to determine health.

```json
{
  "status": "ok",
  "total": 3,
  "healthy": 3
}
```

`status` is `"ok"` when all known systems are healthy, `"degraded"` otherwise.

#### `GET /stream`

Server-Sent Events stream. The browser dashboard connects here automatically. Each event is a `data:` line containing the same JSON payload as `GET /api/systems`, pushed every `refresh_interval` seconds.

---

## Heartbeat Sender

### CLI Usage

```
python heartbeat_sender.py [options]

Options:
  -n, --name NAME           Name of the sending system (required)
  -s, --status STATUS       Status string (default: OK)
      --host HOST           Monitor host or IP address (default: 127.0.0.1)
  -p, --port PORT           Monitor UDP port (default: 9999)
  -i, --interval SECONDS    Send continuously every N seconds (0 = one-shot)
      --count N             Stop after N packets in continuous mode (0 = unlimited)
  -I, --interface IFACE     Network interface for IPv6 link-local addresses
  -f, --file JSON_FILE      Load config from a JSON file (see below)
  -v, --verbose             Print each packet as it is sent
```

**One-shot:**
```bash
python heartbeat_sender.py --name my-app --status "OK"
```

**Continuous, every 10 seconds:**
```bash
python heartbeat_sender.py --name my-app --status "running" --interval 10
```

**Send 5 packets then stop:**
```bash
python heartbeat_sender.py --name my-app --interval 5 --count 5
```

**Send to a remote monitor:**
```bash
python heartbeat_sender.py --name my-app --host 192.168.1.100 --port 9999
```

---

### JSON File Config

All CLI options can be stored in a JSON file and loaded with `--file`. CLI flags take precedence over file values.

```json
{
  "name": "my-application",
  "status": "running normally",
  "host": "192.168.1.100",
  "port": 9999,
  "interval": 10,
  "count": 0,
  "interface": ""
}
```

```bash
python heartbeat_sender.py --file heartbeat.json
python heartbeat_sender.py --file heartbeat.json --interval 30   # override interval
```

---

## Packet Format

Heartbeat packets are UTF-8 encoded JSON sent over UDP. The maximum supported packet size is 65535 bytes.

```json
{
  "name": "my-application",
  "timestamp": "2024-01-01T12:00:00.123456+00:00",
  "status": "OK"
}
```

| Field | Required | Description |
|---|---|---|
| `name` | Yes | Unique identifier for the sending system. Packets with a missing or empty `name` are silently dropped. |
| `timestamp` | No | ISO-8601 UTC timestamp set by the sender. Stored for display; not used for timeout calculations. |
| `status` | No | Arbitrary status string displayed on the dashboard. |

---

## Networking

### IPv4 Unicast

The default case. No special configuration needed.

```bash
python heartbeat_sender.py --name my-app --host 192.168.1.100
```

### IPv4 Broadcast

Send to a subnet broadcast address or the limited broadcast address `255.255.255.255`. The sender enables `SO_BROADCAST` automatically when the destination is not a multicast address.

```bash
# Subnet broadcast
python heartbeat_sender.py --name my-app --host 192.168.1.255

# Limited broadcast (received only on the local subnet)
python heartbeat_sender.py --name my-app --host 255.255.255.255
```

The monitor receives IPv4 broadcast packets without any extra configuration because it binds to `0.0.0.0`.

### IPv4 Multicast

The sender sets `IP_MULTICAST_TTL` automatically when the destination is in the `224.0.0.0/4` multicast range.

```bash
python heartbeat_sender.py --name my-app --host 224.0.0.1 --interval 5
```

To receive multicast packets, the monitor must join the group:

```yaml
# config/config.yaml
server:
  multicast_groups:
    - "224.0.0.1"
```

### IPv6 Unicast

The sender creates an `AF_INET6` socket automatically when the destination is an IPv6 address.

```bash
python heartbeat_sender.py --name my-app --host 2001:db8::1
```

The monitor accepts IPv6 unicast packets on its `AF_INET6` listener (`::`) without additional configuration.

### IPv6 Link-Local

Link-local addresses (`fe80::/10`) are scoped to a single network interface. **An interface must be specified** or the OS cannot determine which link to use. This applies to both the sender and the multicast group join on the monitor.

Pass the interface with `--interface` or embed it in the address using the `%` scope notation:

```bash
# Using --interface flag
python heartbeat_sender.py --name my-app --host fe80::1 --interface eth0

# Using % scope suffix (equivalent)
python heartbeat_sender.py --name my-app --host fe80::1%eth0
```

If a link-local address is given without an interface, the sender raises an error immediately rather than sending a packet that would be silently dropped by the OS.

### IPv6 Multicast

IPv6 multicast addresses are in the `ff00::/8` range. The sender sets `IPV6_MULTICAST_HOPS` automatically.

Common well-known groups:

| Address | Scope | Meaning |
|---|---|---|
| `ff02::1` | Link-local | All nodes on the local link |
| `ff02::2` | Link-local | All routers on the local link |
| `ff05::1` | Site-local | All nodes in the site |
| `ff0e::1` | Global | All nodes globally |

**Sending to a link-local multicast group (requires interface):**

```bash
python heartbeat_sender.py --name my-app --host ff02::1 --interface eth0 --interval 5

# Equivalent using % notation
python heartbeat_sender.py --name my-app --host ff02::1%eth0 --interval 5
```

**Sending to a global-scope multicast group (no interface needed):**

```bash
python heartbeat_sender.py --name my-app --host ff0e::1 --interval 5
```

**Receiving IPv6 multicast on the monitor** requires joining the group. Link-local groups require the interface name:

```yaml
# config/config.yaml
server:
  multicast6_groups:
    - group: "ff02::1"
      interface: "eth0"   # required for link-local (ff02::/16)
    - group: "ff0e::1"    # global scope — interface optional
```

Multiple groups and interfaces can be listed:

```yaml
server:
  multicast6_groups:
    - group: "ff02::1"
      interface: "eth0"
    - group: "ff02::1"
      interface: "eth1"   # join the same group on a second interface
    - group: "ff0e::1"
```
