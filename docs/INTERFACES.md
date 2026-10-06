# mu2edaq-heartbeatmonitor — interfaces

Authoritative description of everything this package exposes and consumes.
All `file:line` references are to the `mu2e-sept-mega-review` branch.

1. [UDP heartbeat wire format](#1-udp-heartbeat-wire-format)
2. [HTTP API](#2-http-api)
3. [Sender APIs (Python and C++)](#3-sender-apis)
4. [Service discovery](#4-service-discovery)
5. [Command-line, environment and files](#5-command-line-environment-and-files)
6. [Review facts verified](#6-review-facts-verified)

---

## 1. UDP heartbeat wire format

### 1.1 Transport

| Property | Value | Source |
|---|---|---|
| Port | 9999/udp, both families | `config/heartbeat_config.yaml:3,11`; `start-mu2edaq-heartbeatmonitor.sh:17` |
| IPv4 bind | `0.0.0.0` (`server.udp_host`) | `heartbeat_monitor.py:50,515-520` |
| IPv6 bind | `::` (`server.udp6_host`), `IPV6_V6ONLY=1` | `heartbeat_monitor.py:56,236-237,522-528` |
| Socket options | `SO_REUSEADDR` on both | `heartbeat_monitor.py:234` |
| Receive buffer | `recvfrom(65535)`; one datagram = one heartbeat | `heartbeat_monitor.py:261` |
| Multicast | optional `multicast_groups` (IPv4 strings) and `multicast6_groups` (`{group, interface}`), joined after bind | `heartbeat_monitor.py:242-257` |
| Delivery | unicast, IPv4 broadcast, IPv4/IPv6 multicast all accepted; nothing is acknowledged | — |

`CRS_PORT_UDP` overrides both `udp_port` and `udp6_port` (`heartbeat_monitor.py:495-498`).

### 1.2 Payload

UTF-8 JSON, exactly one object per datagram:

```json
{"name": "daq-node-01", "timestamp": "2026-10-06T19:21:24.594+00:00", "status": "OK"}
```

| Key | Required | Type the monitor expects | How the monitor uses it |
|---|---|---|---|
| `name` | **yes** | string | `.strip()`ped; the registry key. Empty after strip → datagram dropped with a warning (`heartbeat_monitor.py:280-286`). |
| `timestamp` | no | any (string by convention) | Stored verbatim as `sent_timestamp` and displayed. **Never used for liveness**; age is computed from the monitor's receive time. Sender clock skew therefore has no effect. |
| `status` | no | any (string by convention) | Stored and displayed verbatim. **Never interpreted**: a heartbeat with `"status": "FATAL"` is shown with `state: "ok"` as long as it arrives on time. |

Additional keys are ignored. There is no version field and no schema
negotiation; the format is identified only by port.

### 1.3 Malformed input — what the receiver actually does

| Datagram | Outcome |
|---|---|
| not UTF-8, or not JSON | warning logged, dropped (`heartbeat_monitor.py:274-278`) |
| JSON object with missing or empty `name` | warning logged, dropped |
| JSON object, `name` is a string, other keys any type | accepted |
| well-formed JSON that is **not an object** (`[]`, `"x"`, `1`, `null`) | `AttributeError` escapes `run()`; **that family's listener thread ends permanently** (HBM-001) |
| JSON object whose `name` is not a string (`5`, `null`, `{}`) | same: thread ends permanently (HBM-001) |

Reproduced on this branch: after one `[]` datagram on IPv4, subsequent IPv4
heartbeats were not recorded while the IPv6 listener and HTTP server kept
working. The process does not exit and discovery keeps advertising it.

There is no authentication or source filtering. Any host that can reach UDP
9999 can create, refresh or impersonate any `name`.

### 1.4 Do the Python and C++ senders agree?

Semantically yes; byte-for-byte no. The same heartbeat was sent through both
senders to a capture socket and diffed (timestamps masked):

```
case plain
  Py  heartbeat_sender.py  len= 88  {"name": "daq-node-01", "timestamp": "<TS>", "status": "OK"}
  C++ heartbeat_sender     len= 80  {"name":"daq-node-01","timestamp":"<TS>","status":"OK"}
case utf8  (name "trk-µ", status "ΔT → 5 µs")
  Py   {"name": "trk-µ", ... "status": "ΔT → 5 µs"}
  C++  {"name":"trk-\xc2\xb5", ... "status":"\xce\x94T \xe2\x86\x92 5 \xc2\xb5s"}   (raw UTF-8)
-> 2 distinct byte strings, 1 distinct decoded object in every case
```

| Aspect | Python (`heartbeat_sender.py:34-42`) | C++ (`cpp_sender/heartbeat_sender_lib.cpp:34-77`) |
|---|---|---|
| Key order | name, timestamp, status | name, timestamp, status |
| Separators | `", "` and `": "` (`json.dumps` default) | none (compact) |
| Non-ASCII | `\uXXXX` (`ensure_ascii=True`) | raw UTF-8 bytes |
| `\b` `\f` `\n` `\r` `\t` `"` `\\` | short escapes | short escapes |
| Other C0 controls | `\u00XX` | `\u00XX` |
| DEL (0x7F) | `\u007f` | raw byte |
| Timestamp | `datetime.now(timezone.utc).isoformat()` → `YYYY-MM-DDTHH:MM:SS.ffffff+00:00` (microseconds; **fraction omitted entirely** when microsecond == 0) | `YYYY-MM-DDTHH:MM:SS.mmm+00:00` (milliseconds, always present) |

Both decode to the same object with any JSON parser, and the monitor treats
`timestamp` as an opaque display string, so the differences are harmless
today. A future consumer that parses `timestamp` must accept both precisions
and the fraction-less Python form.

The capture was repeated in this review's second pass (macOS, C++ sender built
from `cpp_sender/` with Apple clang) with three cases: plain ASCII, UTF-8, and
`"`, backslash, TAB and DEL. In every case the two senders produced different bytes and
`json.loads` of both, timestamp removed, compared equal.

Behaviour, as opposed to bytes, differs in two places:

| Situation | `heartbeat_sender.py` | `heartbeat_sender` (C++) |
|---|---|---|
| Send error in `--interval` mode (unresolvable host, `ENETUNREACH`, unknown interface) | uncaught exception; loop ends, traceback, exit 1 (`heartbeat_sender.py:263-271` catches only `KeyboardInterrupt`) | `Warning: ...` on stderr, keeps sending (`cpp_sender/heartbeat_sender.cpp:154-157`) |
| `--status OK`, `--host 127.0.0.1` or `--port 9999` given together with `--file` holding a different value | file value wins (`heartbeat_sender.py:218-223`) | command line wins (`heartbeat_sender.cpp:110-116`) |

Verified: `--host no-such-host.invalid --interval 0.2 --count 3` gives one
`socket.gaierror` traceback from Python and three warnings from C++. Filed as
HBM-013 (S4): a stopped Python sender makes its node go red, which is visible,
but a transient resolver failure should not be what stops it.

---

## 2. HTTP API

Served by Flask's development server on `server.web_host:server.web_port`
(`0.0.0.0:8081` under the control room). No authentication on any route.

| Method | Path | Response |
|---|---|---|
| GET | `/` | dashboard HTML (`templates/index.html`), parameterised with `refresh_interval`, `udp_port`, `grid_columns`, `theme` |
| GET | `/settings` | theme settings page and API reference (`templates/settings.html`) |
| GET | `/api/systems` | JSON, schema below |
| GET | `/api/health` | `{"status": "ok"|"degraded", "total": N, "healthy": M}`; always HTTP 200 |
| GET | `/api/theme` | `{"theme": "dark"|"light"}` |
| POST | `/api/theme` | body `{"theme": "dark"|"light"}` → 200 `{"theme": ...}` or 400 `{"error": ...}`; changes the server-wide default for every viewer |
| GET | `/stream` | `text/event-stream`; one `data: <same JSON as /api/systems>` event every `monitor.refresh_interval` seconds, forever, per connection |

`/api/systems` (`heartbeat_monitor.py:315-325`, entries built at `:181-192`):

```json
{
  "systems": [
    {"name": "daq-node-01", "age_seconds": 3.2, "age_display": "3.2s",
     "sent_timestamp": "2026-10-06T19:21:24.594+00:00", "status": "OK",
     "source": "10.226.9.21:51234", "description": "", "timeout": 60,
     "timed_out": false, "state": "ok"}
  ],
  "total": 1, "healthy": 1, "unhealthy": 0,
  "server_time": "2026-10-06T19:21:27.801234+00:00"
}
```

`source` is `host:port` for IPv4 and `[host]:port` for IPv6. `systems` is
sorted by name. `state` is `"error"` iff `age_seconds > timeout`.

**Semantics a consumer must know.** `/api/health` reports on the set of names
currently in the registry, not on an expected set. A node that has been silent
longer than `cleanup_after` is deleted and no longer counts, so health returns
to `"ok"`; a configured node that never sent anything never counted. Do not use
`/api/health` as "all expected nodes are alive" (HBM-002).

---

## 3. Sender APIs

### 3.1 Python — `heartbeat_sender.py`

Importable as a module from the checkout (it is not packaged).

```python
build_packet(name: str, status: str, timestamp: str = None) -> bytes
send_packet(host: str, port: int, data: bytes, interface: str = "") -> None
```

- `build_packet` (`heartbeat_sender.py:34`): `json.dumps({"name", "timestamp", "status"})`
  UTF-8 encoded; `timestamp` defaults to UTC now.
- `send_packet` (`:45`): a `%iface` suffix on `host` is split off as the scope.
  IPv4 multicast → `IP_MULTICAST_TTL 32`; other IPv4 → `SO_BROADCAST` set;
  IPv6 multicast → `IPV6_MULTICAST_HOPS 32`, `IPV6_MULTICAST_IF` if an interface
  is given; IPv6 link-local unicast (`fe80::/10`) or link-local multicast
  (`ff02::/16`) without an interface raises `ValueError`. A non-literal host is
  resolved with `getaddrinfo(AF_UNSPEC)` and the first result is used. Socket
  errors propagate as `OSError`.

CLI: `man 1 heartbeat_sender.py`.

### 3.2 C++ — `libheartbeat_sender_lib.a` (`cpp_sender/heartbeat_sender_lib.h`)

```cpp
namespace hbsender {
struct Config { std::string name; std::string status = "OK";
                std::string host = "127.0.0.1"; int port = 9999;
                double interval = 0.0; int count = 0;
                std::string iface; bool verbose = false; };
std::string now_iso8601();
std::string build_packet(const std::string& name, const std::string& status);
void send_packet(const std::string& host, int port,
                 const std::string& data, const std::string& iface);
void load_json_file(const std::string& path, Config& cfg);
}
```

- Same address classification as Python (`heartbeat_sender_lib.cpp:83-99,193-214`).
- Errors throw `std::runtime_error`, **except** the hostname path
  (`send_hostname`, `:171-191`), which ignores the `sendto()` return value, so a
  send failure to a named host is silent (HBM-011).
- POSIX sockets only (`<sys/socket.h>`, `gmtime_r`); does not build on Windows.
- CMake: static target `heartbeat_sender_lib`, links `nlohmann_json::nlohmann_json`
  PUBLIC; installs `lib/libheartbeat_sender_lib.a` and `include/heartbeat_sender_lib.h`
  (`cpp_sender/CMakeLists.txt:68-70`). No CMake package config is exported.

Reference: `man 3 libheartbeat_sender_lib`; CLI: `man 1 heartbeat_sender`.

### 3.3 Sender JSON config file

Both senders accept `--file` with keys `name`, `status`, `host`, `port`,
`interval`, `count`, `interface` (`example_sender.json`). Explicit CLI options
win over file values. The Python sender's precedence test compares against
defaults (`heartbeat_sender.py:218-223`), so passing `--status OK` or
`--host 127.0.0.1` explicitly does not override a different value in the file;
the C++ sender tracks which options were given and has no such quirk
(`heartbeat_sender.cpp:110-116`).

---

## 4. Service discovery

When `mu2edaq_discovery` is importable the monitor starts a `Responder`
(`heartbeat_monitor.py:541-545`):

| Field | Value |
|---|---|
| `name` | `Heartbeat Monitor` |
| `app` | `heartbeatmonitor` |
| `scheme` / `port` | `http` / effective `web_port` (8081 under the control room) |
| `meta` | `{"udp_port": str(udp_port)}`; values are strings, as `mu2edaq-discovery/doc/INTERFACES.md` §6 requires |

Failure to import or start is logged as a warning and ignored. The discovery
wire protocol itself is documented in `mu2edaq-discovery/doc/INTERFACES.md`.

---

## 5. Command-line, environment and files

| Item | Detail |
|---|---|
| `heartbeat_monitor.py` options | `-c/--config` (both spellings work; default `config.yaml`), `--debug`, `--theme dark|light`, `-d/--daemon`, `--pid-file`, `--log-file` (`heartbeat_monitor.py:466-484`) |
| Config search when `-c` is relative and not in cwd | cwd, `~/.config/heartbeat_monitor/`, `/etc/heartbeat_monitor/`, `<script dir>/config/`, `<script dir>/` (`:79-85`) |
| `CRS_PORT_HTTP` | overrides `server.web_port`; must be an integer or startup fails with `ValueError` |
| `CRS_PORT_UDP` | overrides `server.udp_port` and `server.udp6_port`; same parsing |
| `CRS_STOP_TIMEOUT` | stop script SIGTERM grace period, default 10 s |
| PID file | `<checkout>/heartbeat_monitor.pid` under the start script; `daemon.pid_file` (`/tmp/heartbeat_monitor.pid`) otherwise |
| Log file | `daemon.log_file`, default `/tmp/heartbeat_monitor.log`; only used in daemon mode |

Note on the superproject's `DAQ-ControlRoom-Testing.md:88`, which says the
monitor takes "**`-c`** (not `--config`)": both spellings are accepted by the
argument parser, and the start script itself uses `--config`. The real
convention deviation is the default name `config.yaml`, which does not exist
in this repository (HBM-006).

Man pages: `heartbeat_monitor.py(1)`, `heartbeat_sender.py(1)`,
`heartbeat_sender(1)`, `start-mu2edaq-heartbeatmonitor.sh(1)`,
`stop-mu2edaq-heartbeatmonitor.sh(1)`, `bootstrap_heartbeat_monitor.sh(1)`,
`libheartbeat_sender_lib(3)`, `heartbeat_config.yaml(5)`,
`mu2edaq-heartbeatmonitor-api(7)`.

---

## 6. Review facts verified

Facts the review brief asserted or asked to settle, checked against this
branch (HEAD `c68b5b4` before the review commits):

| Claim | Result | Evidence |
|---|---|---|
| One test file, 20 GoogleTest cases | **Confirmed.** `cpp_sender/tests/test_heartbeat_sender_lib.cpp`; 20 `TEST(...)` macros; `ctest` reports 20/20 passed (macOS, Apple clang, built offline from the untracked `_deps/` sources) | `grep -c '^TEST(' cpp_sender/tests/test_heartbeat_sender_lib.cpp` |
| No Python tests | **Confirmed.** No `test_*.py`, no `tests/` at top level, no pytest config | `git ls-files \| grep -i test` |
| No CI | **Confirmed.** No `.github/` | — |
| UDP 9999, IPv4 and IPv6 | **Confirmed.** Two sockets, v6 with `IPV6_V6ONLY=1`; a heartbeat to `::1` and one to `127.0.0.1` both reach one process | `heartbeat_monitor.py:233-238,515-528` |
| HTTP 8081 because resource-manager owns 8080 | **Confirmed for the control-room path only.** 8081 comes from the start script and `apps.yaml`; the code default is 8080 and the shipped YAML says 5003 (HBM-006) | `start-mu2edaq-heartbeatmonitor.sh:9,16`; `mu2edaq-desktop/config/apps.yaml:85` |
| "`-c` not `--config`" | **Contradicted.** The parser defines both spellings, and the start script itself passes `--config` | `heartbeat_monitor.py:467`; `start-mu2edaq-heartbeatmonitor.sh:29` |
| `cpp_sender/build/_deps/` is not tracked | **Confirmed.** `git ls-files cpp_sender/build` is empty; `.gitignore:12` covers it. Not filed | — |
| Discovery `meta` values are strings (§9.5) | **Confirmed.** `meta={"udp_port": str(udp_cfg["udp_port"])}` | `heartbeat_monitor.py:544` |
| Python and C++ senders agree | **On the decoded object, yes; on bytes and on error handling, no.** See §1.4 | — |

The `mu2edaq-config` repository carries three copies of
`heartbeat_config.yaml` (`config-main`, `config-t00.02.00-rc`,
`config-t00.03.01`), each byte-identical to `config/heartbeat_config.yaml`,
so they carry the same 5003 value.
