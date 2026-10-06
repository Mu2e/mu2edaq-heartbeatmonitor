---
title: mu2edaq-heartbeatmonitor
group: monitoring
tier: A
audience: expert
summary: UDP heartbeat liveness tracker that ingests JSON heartbeats on UDP 9999 (IPv4 and IPv6), marks each named sender healthy or timed out against a per-system timeout, and serves a dashboard, JSON API and SSE stream on HTTP 8081
---

# mu2edaq-heartbeatmonitor

> UDP heartbeat liveness tracker that ingests JSON heartbeats on UDP 9999 (IPv4
> and IPv6), marks each named sender healthy or timed out against a per-system
> timeout, and serves a dashboard, JSON API and SSE stream on HTTP 8081.

| | |
|---|---|
| Repository | `Mu2e/mu2edaq-heartbeatmonitor` |
| Language | Python 3.9 (monitor, Python sender), C++17 (sender binary and static library) |
| Ports | 9999/udp (heartbeat ingest, IPv4 and IPv6), 8081/tcp (web UI and API under the control room) |
| Depends on | `mu2edaq-discovery` (optional import), Flask, PyYAML; C++ sender: nlohmann/json |
| Tests | 20 GoogleTest cases for the C++ sender library; no Python tests; no CI |

Companion documents: [INTERFACES.md](INTERFACES.md) (wire format, HTTP API,
sender APIs), [OPERATIONS.md](OPERATIONS.md) (start/stop, ports, failure modes),
[ISSUES-PENDING.md](ISSUES-PENDING.md) (staged findings), and the pre-existing
user guide [README.md](README.md).

## Purpose

Any process that can emit a UDP datagram declares itself alive by sending a
small JSON object `{"name", "timestamp", "status"}` to the monitor at a fixed
interval. The monitor keeps the most recent heartbeat per `name`, compares its
receive time against a per-system timeout, and shows the result as a grid of
green/red cards. There is no registration step: the first heartbeat from a new
name creates its card.

The design is deliberately thin. There is no persistence, no authentication,
no history and no alerting. The process exists to answer the question "when did
I last hear from X?" for an operator looking at a screen, typically through the
control room's embedded browser (`mu2edaq-controlcenter/config/controlcenter.yaml:34-35`
points its "Heart Beats" pane at `http://localhost:8081`).

## Architecture

```mermaid
flowchart LR
  subgraph senders[Heartbeat senders]
    PY[heartbeat_sender.py]
    CPP[heartbeat_sender C++ binary]
    LIB[libheartbeat_sender_lib.a in any C++ app]
    ANY[any JSON-over-UDP emitter]
  end
  PY -->|UDP JSON| V4
  CPP -->|UDP JSON| V4
  LIB -->|UDP JSON| V6
  ANY -->|UDP JSON| V4
  subgraph mon[heartbeat_monitor.py process]
    V4[UDPListener v4 thread<br/>0.0.0.0:9999] --> REG[(SystemRegistry<br/>dict + threading.Lock)]
    V6[UDPListener v6 thread<br/>[::]:9999 V6ONLY] --> REG
    REG --> FLASK[Flask dev server<br/>threaded, :8081]
    RESP[discovery Responder<br/>optional]
  end
  FLASK -->|GET /, /settings| BROWSER[browser / controlcenter pane]
  FLASK -->|SSE /stream| BROWSER
  FLASK -->|GET /api/systems, /api/health| SCRIPTS[scripts, curl]
  RESP -.->|DISCOVER reply<br/>app=heartbeatmonitor| DISC[mu2edaq-discover]
```

Process model: one Python process. `main()` (`heartbeat_monitor.py:462`) parses
arguments, loads YAML, applies `CRS_PORT_HTTP`/`CRS_PORT_UDP`, optionally
double-forks into a daemon (`daemonize`, `heartbeat_monitor.py:385`), then
starts two daemon threads (`UDPListener`, `heartbeat_monitor.py:220`) and runs
Flask's built-in Werkzeug server with `threaded=True` on the main thread
(`heartbeat_monitor.py:550-556`). Every HTTP request, including each long-lived
SSE connection, gets its own Werkzeug thread.

## Components

### `SystemRegistry` (`heartbeat_monitor.py:133-202`)

In-memory `dict[name] -> record` guarded by one `threading.Lock`.

- `record()` (`:153`) overwrites the entry for `name` with the receive time
  `datetime.now(timezone.utc)`, the sender's opaque `timestamp` string, the
  `status` string, the source `host:port`, and the configured description and
  timeout. Nothing from the previous record is kept.
- `snapshot()` (`:166`) computes `age = now - last_seen` for every entry,
  **deletes** entries older than `cleanup_after` (`:175-177`), and marks the
  rest `state: "error"` if `age > timeout`, else `"ok"`. The sender's `status`
  string never influences `state`.
- `count()` (`:198`) calls `snapshot()` a second time and counts.

Two consequences matter operationally and are filed as findings:

- Purging happens only inside `snapshot()`, i.e. only when an HTTP client asks.
  With no client connected the dict grows without bound (HBM-007).
- A node that dies is shown red until `cleanup_after` and then disappears; at
  that moment `/api/health` returns to `"ok"`. A name listed under `systems:`
  in the config that has never sent a heartbeat is never shown at all
  (HBM-002).

### `UDPListener` (`heartbeat_monitor.py:220-292`)

One thread per address family. Each thread creates its socket, sets
`SO_REUSEADDR`, sets `IPV6_V6ONLY=1` on the v6 socket (`:237`), binds
(`:238`), joins any configured multicast groups, and loops on
`recvfrom(65535)` with a 1 s timeout.

The dual-stack arrangement is correct: because the v6 socket is `V6ONLY`, the
v4 socket on `0.0.0.0:9999` and the v6 socket on `[::]:9999` do not conflict,
and IPv4 traffic is never delivered as v4-mapped addresses to the v6 socket.
Verified by sending to `127.0.0.1` and `::1` against one process.

Two defects live here:

- The bind happens inside `run()`, so a bind failure kills only that thread;
  the HTTP server still starts and the process advertises itself while deaf
  (HBM-003).
- `_handle()` (`:270-289`) catches only `JSONDecodeError`/`UnicodeDecodeError`.
  Any well-formed JSON that is not an object (`[]`, `"x"`, `1`) or an object
  whose `name` is not a string raises `AttributeError` out of `run()`, which
  ends the thread permanently. Reproduced: one `[]` datagram stopped all
  subsequent IPv4 heartbeats while the v6 listener and HTTP stayed up
  (HBM-001).

### Flask application (`heartbeat_monitor.py:299-378`)

Module-global `app`, `_registry`, `_config`. Routes: `/`, `/settings`,
`/api/systems`, `/api/health`, `/api/theme` (GET/POST), `/stream` (SSE). See
[INTERFACES.md](INTERFACES.md) §2. Templates are self-contained (inline CSS
and JS, no CDN), so the UI works on an air-gapped DAQ network. The dashboard
escapes every UDP-supplied string through `escHtml()` before it reaches
`innerHTML` (`templates/index.html:560-568,600-606`); the sender's timestamp
is only passed through `Date()` formatting. No DOM-injection path was found.

### Daemon support (`heartbeat_monitor.py:385-455`)

Classic double fork, `setsid`, `chdir(working_dir)`, **`umask(0)`** (`:406`),
stdout/stderr redirected to the log file, PID file written (`:446`) and
removed by `atexit` and by a `SIGTERM` handler. The zero umask makes the PID
and log files mode 0666 (verified on AlmaLinux 9), filed as HBM-005.

### Senders

- `heartbeat_sender.py`: Python CLI and importable functions `build_packet()`
  / `send_packet()`; IPv4 unicast/broadcast/multicast, IPv6 unicast/multicast
  with scope handling, hostnames via `getaddrinfo`.
- `cpp_sender/`: the same CLI in C++17 (`heartbeat_sender`) over a static
  library `heartbeat_sender_lib` (namespace `hbsender`). JSON config files are
  parsed with nlohmann/json, found with `find_package` or fetched with
  `FetchContent` (`cpp_sender/CMakeLists.txt:11-25`). GoogleTest is handled the
  same way (`cpp_sender/tests/CMakeLists.txt:4-17`).

The Python and C++ senders produce semantically identical heartbeats but not
identical bytes (separators, non-ASCII escaping, timestamp precision). The
monitor parses JSON and treats the timestamp as opaque, so this has no effect.
They do differ in behaviour: in `--interval` mode the Python sender stops at the
first send error (for example a transient DNS failure) while the C++ sender
warns and keeps sending, and the Python sender cannot override a `--file` value
with an option equal to its default (HBM-013). Details in
[INTERFACES.md](INTERFACES.md) §1.4.

## Interfaces

Summary only; [INTERFACES.md](INTERFACES.md) is authoritative.

| Interface | Direction | Format |
|---|---|---|
| UDP 9999 v4 + v6 | in | one JSON object per datagram: `name` (required), `timestamp`, `status` |
| HTTP `/api/systems`, `/api/health` | out | JSON |
| HTTP `/stream` | out | `text/event-stream`, one `data:` line per `refresh_interval` |
| HTTP `/api/theme` POST | in | `{"theme": "dark"|"light"}`, unauthenticated |
| Discovery | out | `app=heartbeatmonitor`, `scheme=http`, `port=<web_port>`, `meta={"udp_port": "<str>"}` |

## Configuration

YAML (`config/heartbeat_config.yaml`, schema in
`man 5 heartbeat_config.yaml`) merged one level deep over `DEFAULT_CONFIG`
(`heartbeat_monitor.py:48-74`), then `CRS_PORT_HTTP` and `CRS_PORT_UDP`
override the ports (`:492-498`). The start script always passes
`--config ./config/heartbeat_config.yaml` and exports `CRS_PORT_HTTP=8081`,
`CRS_PORT_UDP=9999`.

The HTTP port has three disagreeing values (HBM-006):

| Source | HTTP port | When it applies |
|---|---|---|
| `heartbeat_monitor.py:53` (`DEFAULT_CONFIG`) | 8080 | `python heartbeat_monitor.py` with no `-c`; the default name `config.yaml` exists nowhere in the repository, so built-in defaults are used. Collides with `mu2edaq-resource-manager`. |
| `config/heartbeat_config.yaml:5` | 5003 | `-c config/heartbeat_config.yaml` without `CRS_PORT_HTTP`. Collides with `mu2edaq-fts`, which binds 5003. |
| `start-mu2edaq-heartbeatmonitor.sh:16`, `apps.yaml` | **8081** | every control-room start. This is the effective production value. |

Timeout defaults also differ: the code defaults are `default_timeout: 30`,
`cleanup_after: 300` (`heartbeat_monitor.py:61-62`); the shipped YAML uses 60
and 86400 (`config/heartbeat_config.yaml:17-18`). `CLAUDE.md` states the
fallbacks are 60 and 86400, which is the YAML, not the code.

## Operations

See [OPERATIONS.md](OPERATIONS.md). Started by `crs-app start heartbeatmonitor`
through `start-mu2edaq-heartbeatmonitor.sh`, which `exec`s the monitor in
`--daemon` mode with its PID file in the checkout directory. There is no
already-running guard; a second start orphans the first daemon (HBM-004,
reproduced on AlmaLinux 9).

## Interactions with other packages

| Package | Mechanism | Evidence |
|---|---|---|
| `mu2edaq-discovery` | optional `import mu2edaq_discovery.Responder`; advertises `app=heartbeatmonitor`, HTTP port, `meta.udp_port` as a string (§9.5 satisfied) | `heartbeat_monitor.py:541-545` |
| `mu2edaq-discovery` | bootstrap installs the sibling checkout editable | `bootstrap_heartbeat_monitor.sh:13-15` |
| `mu2edaq-controlroom-setup`, `mu2edaq-desktop` | launch via `apps.yaml` (`ports: {http: 8081, udp: 9999}`); the 8081 choice is explained at `mu2edaq-desktop/config/apps.yaml:85` | (their manifests) |
| `mu2edaq-controlcenter` | embeds `http://localhost:8081` as the "Heart Beats" pane | (its manifest) |

No package in the superproject sends heartbeats to UDP 9999 in code. The only
producers are this repository's two senders. `mu2edaq-downtime-logger` has its
own unrelated UDP heartbeat detector on port 9000 with a different payload
contract.

## Review findings

Staged in [ISSUES-PENDING.md](ISSUES-PENDING.md) (GitHub writes are disabled for
this review, contract §9.1).

| ID | Sev | Title |
|---|---|---|
| HBM-001 | S2 | [udp] one well-formed non-object datagram permanently kills a listener thread while HTTP stays up |
| HBM-002 | S2 | [registry] dead nodes are purged after cleanup_after and /api/health reports ok; configured systems that never report are never shown |
| HBM-003 | S3 | [udp] the UDP bind runs inside the listener thread, so a bind failure leaves the monitor up and deaf |
| HBM-004 | S3 | [lifecycle] a second start overwrites and then deletes the PID file, orphaning the running daemon |
| HBM-005 | S3 | [daemon] daemonize() sets umask(0), so the PID file and log are world-writable |
| HBM-006 | S3 | [config] three disagreeing HTTP ports (8080 code, 5003 YAML, 8081 start script) and a default config name that does not exist |
| HBM-007 | S3 | [registry] registry growth is unbounded when no HTTP client is polling |
| HBM-008 | S3 | [web] Flask development server on 0.0.0.0 with no authentication; --debug exposes the Werkzeug debugger |
| HBM-009 | S3 | [ci] no Python tests and no CI; the 20 gtest cases cover only the C++ sender |
| HBM-010 | S4 | [registry] liveness ages are computed from the wall clock, not a monotonic clock |
| HBM-011 | S4 | [sender] C++ heartbeat_sender aborts on a non-numeric option and ignores sendto errors for hostnames |
| HBM-012 | S4 | [docs] README, docs/README.md and CLAUDE.md describe a config file, port and defaults that do not exist |
| HBM-013 | S4 | [sender] heartbeat_sender.py stops at the first send error in --interval mode, unlike the C++ sender |
| (comment) | — | open issue #8 (bootstrap not location-independent) is still present unchanged |
