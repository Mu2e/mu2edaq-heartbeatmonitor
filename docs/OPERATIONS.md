# mu2edaq-heartbeatmonitor — operations

For a DAQ expert running or debugging the monitor. Mechanisms are cited by
`file:line`; behaviour marked *verified* was reproduced on AlmaLinux 9 /
Python 3.9.25 (container) on 2026-10-06. The listener-thread death (HBM-001),
the double-start PID-file loss (HBM-004), the 0666 file modes (HBM-005) and the
Linux `SO_REUSEADDR` co-bind (HBM-003) were reproduced a second time in the
review's completion pass (macOS / Python 3.12, and an `almalinux:9` container
for the socket test).

Installation is covered by §1 below together with the top-level `README.md`
and `cpp_sender/README.md`; there is no separate `INSTALL.md`.

## 1. Install

```bash
cd ~/mu2edaq-main/mu2edaq-heartbeatmonitor
./bootstrap_heartbeat_monitor.sh       # creates ./venv, installs flask + pyyaml,
                                       # and ../mu2edaq-discovery editable if present
```

Caveat (open issue #8): the bootstrap script uses paths relative to the
**current directory** and has no `set -e`. Run it from the package directory;
check its output, because it prints "initialized successfully" even if pip
failed.

C++ sender (optional; only needed on hosts that emit heartbeats from C++):

```bash
cmake -S cpp_sender -B cpp_sender/build      # fetches nlohmann/json and googletest if not installed
cmake --build cpp_sender/build -j
ctest --test-dir cpp_sender/build            # 20 GoogleTest cases
cmake --install cpp_sender/build --prefix /opt/mu2edaq
```

Offline hosts: pre-populate the dependencies and point FetchContent at them,
e.g. `-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=... -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=... -DFETCHCONTENT_FULLY_DISCONNECTED=ON`.

## 2. Start, stop, status

| Action | Command | Mechanism |
|---|---|---|
| Start (control room) | `crs-app start heartbeatmonitor` | runs `start-mu2edaq-heartbeatmonitor.sh` |
| Start (by hand) | `./start-mu2edaq-heartbeatmonitor.sh [CONFIG]` | exports `CRS_PORT_HTTP=8081`, `CRS_PORT_UDP=9999` unless already set, then `exec python heartbeat_monitor.py --config CONFIG --daemon --pid-file <checkout>/heartbeat_monitor.pid` (`start-mu2edaq-heartbeatmonitor.sh:16-19,29`) |
| Stop | `./stop-mu2edaq-heartbeatmonitor.sh [PIDFILE]` | SIGTERM, wait `CRS_STOP_TIMEOUT` (10 s), SIGKILL, remove PID file |
| Foreground (debugging) | `venv/bin/python heartbeat_monitor.py -c config/heartbeat_config.yaml` | logs to stderr; set `CRS_PORT_HTTP=8081` or it binds 5003 from the YAML |
| Health | `curl -s localhost:8081/api/health` | see the caveat in §5 |
| Who is reporting | `curl -s localhost:8081/api/systems \| python3 -m json.tool` | |
| Send a test heartbeat | `venv/bin/python heartbeat_sender.py --name probe --host 127.0.0.1 --port 9999` | |
| Discovery | `mu2edaq-discover --filter app=heartbeatmonitor --json` | needs `mu2edaq-discovery` installed in the venv |

Ports in production: **8081/tcp** (UI/API) and **9999/udp** (IPv4 and IPv6).
Do not run the monitor without the start script or `CRS_PORT_HTTP` unless you
mean to: the code default is 8080 (resource-manager's port) and the shipped
YAML says 5003 (fts's port). See HBM-006.

## 3. Files

| Path | Written by | Notes |
|---|---|---|
| `<checkout>/heartbeat_monitor.pid` | daemon | mode **0666** because `daemonize()` sets `umask(0)` (`heartbeat_monitor.py:406`) — *verified*. Any local user can rewrite the PID that the stop script will `kill` (HBM-005). |
| `/tmp/heartbeat_monitor.log` | daemon | mode 0666, appended forever, no rotation. Shared by every user on the host. |
| `config/heartbeat_config.yaml` | operator | `man 5 heartbeat_config.yaml` |

## 4. Failure modes and what you will see

| Situation | Behaviour | Diagnosis / recovery |
|---|---|---|
| **Start pressed twice** | The second instance loads the config, daemonizes, **overwrites the PID file**, fails to bind 8081 (`Address already in use` in the log), exits, and its `atexit` handler **deletes the PID file**. The first daemon keeps running and serving, but `stop-…sh` now reports "not running" and later starts fail on the port. *Verified* (HBM-004). | `pgrep -af heartbeat_monitor.py`; `kill <pid>`; then start once. |
| Start script exit status | Always 0 once the parent has forked; a daemon that dies immediately afterwards (port in use, bad YAML) is not reported. | Check `/tmp/heartbeat_monitor.log` and `curl localhost:8081/api/health`. |
| UDP port already bound by another process | On Linux, `SO_REUSEADDR` lets a second UDP socket bind the same port, and unicast datagrams then go to the **most recently bound** socket only. If the bind does fail (another OS, or a non-`SO_REUSEADDR` holder), the exception ends that listener thread and the HTTP server still starts: the UI shows every node timing out (HBM-003). | `ss -ulpn 'sport = :9999'` (Linux) or `lsof -nP -iUDP:9999`. |
| One malformed datagram (`[]`, `"x"`, `{"name": 5}`) | That family's listener thread ends with an `AttributeError` traceback in the log; heartbeats on that family stop being recorded; everything goes red and is then purged (HBM-001). *Verified.* | Look for `Exception in thread udp-listener-v4` / `-v6` in the log. Restart. |
| IPv6 disabled in the kernel | `socket(AF_INET6)` fails inside the v6 thread, which ends with a traceback; IPv4 is unaffected. | Set `server.ipv6_enabled: false`. |
| Node dies | Card turns red after its `timeout` (default 60 s in the shipped YAML). After `cleanup_after` (86400 s shipped, 300 s code default) the card **disappears** and `/api/health` goes back to `ok` (HBM-002). | Keep an independent list of expected nodes; do not treat `ok` as "everything expected is alive". |
| Monitor restarted | All state is lost (in-memory only). A node that died before the restart never reappears, and names under `systems:` are not pre-populated. | Same as above. |
| No browser connected for a long time | Nothing is purged until somebody calls `/`, `/api/*` or `/stream`; the registry grows with every new `name` seen (HBM-007). | Benign for a fixed set of senders; a sender that puts a PID or timestamp in `name` creates a new entry per restart. |
| NTP step on the monitor host | Ages are computed from `datetime.now(timezone.utc)`; a step forward marks every node red at once, a step backward makes ages negative (HBM-010). | Transient; resolves after one heartbeat interval. |
| `CRS_PORT_HTTP=abc` | `ValueError` traceback at startup. *Verified.* | Fix `apps.yaml`. |
| A config section present but empty (`monitor:` with every key commented out), or a YAML syntax error | `TypeError` / `yaml` exception traceback at startup, exit 1 (`heartbeat_monitor.py:118-125` catches only `OSError`). *Verified.* | Delete the empty key; fix the YAML. |
| `--debug` | Enables Flask debug mode on `web_host` (0.0.0.0 by default), i.e. the Werkzeug interactive debugger is reachable from the network (PIN-protected) (HBM-008). | Do not use on a DAQ host; if needed, bind `web_host: 127.0.0.1`. |

## 5. What the monitor does not do

- It does not interpret `status`. A sender reporting `"FATAL"` on time is green.
- It does not alarm, page, log transitions, or push to `mu2edaq-dashboard` or
  `mu2edaq-bigredbox`. It is a display.
- It does not authenticate senders or HTTP clients. Anyone who can reach UDP
  9999 can create or refresh any name; anyone who can reach 8081 can change the
  default theme for all viewers.
- It does not persist anything across restarts.

## 6. Tests

```bash
# C++ sender library (the only automated tests in the package)
cmake -S cpp_sender -B /tmp/hb-build && cmake --build /tmp/hb-build -j && ctest --test-dir /tmp/hb-build
```

20/20 GoogleTest cases pass on macOS (Apple clang, this review). There are no
tests for `heartbeat_monitor.py` or `heartbeat_sender.py`, and no CI (HBM-009).
