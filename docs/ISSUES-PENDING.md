# Staged issues — mu2edaq-heartbeatmonitor

Findings from the September 2026 DAQ mega-review (`mu2e-sept-mega-review`),
2026-10-06. Target repository: `Mu2e/mu2edaq-heartbeatmonitor` (public, issues
enabled).

**Why staged.** Contract §9.1: no GitHub write is attempted in this review. Each
entry below is the issue body verbatim, with its fingerprint, for the
orchestrator's filing script.

**Dedup.** `gh issue list -R Mu2e/mu2edaq-heartbeatmonitor --state all` returned
4 issues, none carrying a mega-review fingerprint:

- #8 (open) "bootstrap_heartbeat_monitor.sh: not location-independent, silently
  reports success on failure" — still accurate on this branch. **Not refiled**;
  a comment with fresh evidence is at the end of this file (`HBM-C08`).
- #1 (closed) "Add start/stop scripts and make the HTTP port configurable (8080
  clashes with resource-manager)". The scripts were added and `CRS_PORT_HTTP`
  works, but the 8080 code default and a new 5003 YAML value remain; HBM-006
  covers what is left and links #1.
- #6 (closed) Python 3.9 import crash — fixed; the monitor ran on Python 3.9.25
  in this review.
- #2 (closed) discovery support — done.

13 issues + 1 comment: 2 × S2, 7 × S3, 4 × S4. HBM-013 was added in the
review's completion pass; the rest were re-checked against the source then.

Reproduction environment for anything marked *verified*: AlmaLinux 9
`almalinux:9` container, Python 3.9.25, Flask from `requirements.txt`; and
macOS 15 / Python 3.12 with the checkout's venv.

---

## HBM-001

**Title:** `[udp] one well-formed non-object datagram permanently kills a listener thread while HTTP stays up`

**Severity:** S2 — production breakage or silent wrong behaviour; workaround exists
**Category:** bug
**Location:** `heartbeat_monitor.py:280`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
`UDPListener._handle()` catches only `JSONDecodeError` and `UnicodeDecodeError`.
A datagram that is valid JSON but not an object (`[]`, `"x"`, `1`, `null`), or an
object whose `name` is not a string, raises `AttributeError` at
`packet.get("name", "").strip()`. Nothing in `run()` catches it, so that
address family's listener thread ends for good. The process, the HTTP server
and the discovery responder keep running.

## Evidence
```
$ grep -n 'packet.get\|except (json' heartbeat_monitor.py
276:        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
280:        name = packet.get("name", "").strip()
```
Reproduced: monitor running, send `[]` to 127.0.0.1:9999, then a valid heartbeat:
```
4. send b'[]' on IPv4, then a valid heartbeat
  health=ok total=2 ['node-FAIL:ok', 'node-v6:ok']      <- 'after-[]' never recorded
Exception in thread udp-listener-v4:
AttributeError: 'list' object has no attribute 'get'
```
The IPv6 listener kept working, which confirms it is per-thread.

## Why it matters
Any host on the DAQ network, or a single buggy sender, blinds the monitor to
every IPv4 heartbeat until someone restarts it. The UI then shows every node
going red and, after `cleanup_after`, disappearing, which reads as a mass
outage of the senders rather than a dead monitor.

## Suggested fix
In `_handle()`, reject anything that is not a `dict` and coerce/validate `name`
with `isinstance(name, str)`; wrap the body of the `run()` loop in a broad
`except Exception: log.exception(...)` so no single datagram can end the thread.
Add a regression test that sends `[]`, `1` and `{"name": 5}` and then a valid
heartbeat.

## Related
- `mu2edaq-bigredbox` fixed the same class of defect (its #8, #9) with exactly
  this pattern (`src/mu2edaq_bigredbox/daq_alert.py:171-191`).

<!-- mega-review-fingerprint: 7e9b0592 -->

---

## HBM-002

**Title:** `[registry] dead nodes are purged after cleanup_after and /api/health reports ok; configured systems that never report are never shown`

**Severity:** S2 — production breakage or silent wrong behaviour; workaround exists
**Category:** bug
**Location:** `heartbeat_monitor.py:175`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
`SystemRegistry.snapshot()` deletes any entry silent for longer than
`cleanup_after`, including names listed under `systems:` in the config.
`/api/health` counts only names currently in the registry, so once a dead node
is purged the health endpoint returns to `"ok"`. Names in `systems:` are never
pre-registered, so an expected node that is already down when the monitor
starts (or restarts) is never displayed at all.

## Evidence
```
$ sed -n 174,177p heartbeat_monitor.py
                # Drop entries that have been silent far too long
                if age > self._cleanup_after:
                    del self._systems[name]
                    continue
```
Reproduced with `default_timeout: 2`, `cleanup_after: 5`, and
`systems: [{name: configured-but-silent}]`:
```
t=  0.3s health=ok       total=1 ['node-A:ok']
t=  2.4s health=degraded total=1 ['node-A:error']
t=  4.4s health=degraded total=1 ['node-A:error']
t=  6.4s health=ok       total=0 []
```
`configured-but-silent` never appears.

## Why it matters
The monitor's one job is to show which senders have gone quiet. With the
shipped config a dead node is reported for 24 h and then silently becomes
"healthy" by absence; with the code defaults that happens after 5 minutes. After
any monitor restart, nodes that died earlier are invisible. A script or
dashboard keyed on `/api/health` will report all-clear while nodes are down.

## Suggested fix
Pre-populate the registry from `systems:` with a "never seen" state that counts
as unhealthy, and exempt configured names from `cleanup_after` purging (purge
only unconfigured names). Optionally report `expected` / `missing` counts in
`/api/health`.

## Related
- HBM-007 (purging only happens on HTTP reads)

<!-- mega-review-fingerprint: 4b073934 -->

---

## HBM-003

**Title:** `[udp] the UDP bind runs inside the listener thread, so a bind failure leaves the monitor up and deaf`

**Severity:** S3 — correctness or maintainability risk, no current outage
**Category:** bug
**Location:** `heartbeat_monitor.py:238`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
`UDPListener.run()` creates and binds its socket on the background thread. If
`bind()` raises, the exception ends the thread and is only printed by the
threading excepthook; `main()` has already moved on and starts the HTTP server,
so the process runs, advertises itself via discovery, and receives nothing.
Separately, both sockets set `SO_REUSEADDR`; on Linux this lets a second UDP
socket bind the same address and port, after which unicast heartbeats are
delivered only to the most recently bound socket.

## Evidence
```
$ sed -n 232,238p heartbeat_monitor.py
    def run(self):
        with socket.socket(self._family, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.settimeout(1.0)
            if self._family == socket.AF_INET6:
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            sock.bind((self._host, self._port))
```
Linux behaviour, verified on AlmaLinux 9 (Python 3.9.25) with two sockets both
setting `SO_REUSEADDR` on the same port: second bind succeeds; a unicast
datagram is received by the second socket only.

## Why it matters
A port clash (another monitor, a test sender bound to 9999, a leftover process)
produces a monitor that looks healthy and shows every node timing out. The same
defect was fixed in `mu2edaq-bigredbox` by binding in the constructor.

## Suggested fix
Create and bind both sockets in `UDPListener.__init__` (calling thread) and exit
non-zero from `main()` if either fails. Drop `SO_REUSEADDR` (there is no
TIME_WAIT for UDP) or set `SO_EXCLUSIVEADDRUSE` on Windows, so that a second
instance fails instead of silently stealing unicast traffic.

## Related
- HBM-004
- Mu2e/mu2edaq-bigredbox: the same `SO_REUSEADDR` single-instance problem is
  staged there as BRB-002.

<!-- mega-review-fingerprint: fc4fb096 -->

---

## HBM-004

**Title:** `[lifecycle] a second start overwrites and then deletes the PID file, orphaning the running daemon`

**Severity:** S3 — correctness or maintainability risk, no current outage
**Category:** bug
**Location:** `start-mu2edaq-heartbeatmonitor.sh:29`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
The start script has no already-running check. A second invocation daemonizes,
writes its own PID into the shared PID file (`heartbeat_monitor.py:446`), fails
to bind the HTTP port, exits, and its `atexit` handler (`:447`) removes the PID
file. The original daemon keeps running with no PID file, so the stop script
reports "not running".

## Evidence
Verified on AlmaLinux 9 with the real scripts:
```
=== first start ===        start rc=0   pid file: 43  alive: 43
=== second start ===       start rc=0   pid file present? NO  alive: 43
Address already in use
=== stop ===               Heartbeat Monitor not running (no pid file: ...)
after stop alive: 43
```

## Why it matters
`crs-app start heartbeatmonitor` pressed twice leaves a daemon that the control
room can no longer stop, and later starts fail on the port until someone finds
the process by hand. Both starts return exit status 0.

## Suggested fix
In the start script, check `kill -0 "$(cat "$PID_FILE")"` and refuse to start
if alive (as `start-mu2edaq-bigredbox.sh:21-24` does). In `daemonize()`, write
the PID file only after the HTTP and UDP sockets are bound, and only unlink it
at exit if it still contains this process's PID.

## Related
- HBM-003, HBM-005

<!-- mega-review-fingerprint: 9e5b5e6e -->

---

## HBM-005

**Title:** `[daemon] daemonize() sets umask(0), so the PID file and log are world-writable`

**Severity:** S3 — correctness or maintainability risk, no current outage
**Category:** bug
**Location:** `heartbeat_monitor.py:406`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
`daemonize()` calls `os.umask(0)` before creating the PID file and opening the
log, so both are created mode 0666.

## Evidence
```
$ sed -n 404,406p heartbeat_monitor.py
    os.chdir(working_dir)
    os.setsid()
    os.umask(0)
```
Verified on AlmaLinux 9 after `./start-mu2edaq-heartbeatmonitor.sh`:
```
  mode -rw-rw-rw- /tmp/heartbeat_monitor.log
  mode -rw-rw-rw- heartbeat_monitor.pid
```

## Why it matters
Any local account on a shared DAQ host can write an arbitrary PID into the PID
file; `stop-mu2edaq-heartbeatmonitor.sh` then sends SIGTERM and SIGKILL to that
PID as the operator. Any local account can also append forged lines to the log.
Requires a local account, hence S3 rather than S1.

## Suggested fix
Use `os.umask(0o022)` (or `0o027`). Optionally have the stop script check that
the PID's command line contains `heartbeat_monitor.py` before killing, as
`stop-mu2edaq-bigredbox.sh:21-22` does for its fallback path.

## Related
- HBM-004

<!-- mega-review-fingerprint: 1d351035 -->

---

## HBM-006

**Title:** `[config] three disagreeing HTTP ports (8080 code, 5003 YAML, 8081 start script) and a default config name that does not exist`

**Severity:** S3 — correctness or maintainability risk, no current outage
**Category:** bug
**Location:** `heartbeat_monitor.py:53`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
The web port is 8080 in `DEFAULT_CONFIG`, 5003 in the only shipped YAML, and
8081 in the start script and `apps.yaml`. The `-c` default is `config.yaml`,
which exists in none of the five search locations, so a bare
`python heartbeat_monitor.py` silently runs on built-in defaults. The code
defaults for `default_timeout`/`cleanup_after` (30/300) also differ from the
YAML (60/86400) and from what `CLAUDE.md` says the fallbacks are (60/86400).
A non-numeric `CRS_PORT_HTTP`/`CRS_PORT_UDP` crashes startup with a raw
`ValueError`.

## Evidence
```
$ grep -n 'web_port\|default_timeout\|cleanup_after' heartbeat_monitor.py config/heartbeat_config.yaml | head -6
heartbeat_monitor.py:53:        "web_port": 8080,
heartbeat_monitor.py:61:        "default_timeout": 30,
heartbeat_monitor.py:62:        "cleanup_after": 300,
config/heartbeat_config.yaml:5:  web_port: 5003             # Web server port
config/heartbeat_config.yaml:17:  default_timeout: 60 ...
config/heartbeat_config.yaml:18:  cleanup_after: 86400 ...
$ grep -n CRS_PORT_HTTP start-mu2edaq-heartbeatmonitor.sh
16:export CRS_PORT_HTTP="${CRS_PORT_HTTP:-8081}"   # web server port
```
Verified on AlmaLinux 9, no `-c`, no `CRS_PORT_*`:
```
[WARNING] Config file 'config.yaml' not found in any search location:
[INFO] Web server starting on http://0.0.0.0:8080
```
and `CRS_PORT_HTTP=abc` → `ValueError: invalid literal for int() with base 10: 'abc'`.

## Why it matters
Run by hand without the start script, the monitor takes either
`mu2edaq-resource-manager`'s port (8080) or `mu2edaq-fts`'s port (5003, which
fts binds under the control room). Either collision makes one of the two
services fail to start. Operators reading the YAML or `CLAUDE.md` get the wrong
timeout and cleanup values for a bare run.

## Suggested fix
Set `web_port: 8081` in both `DEFAULT_CONFIG` and `config/heartbeat_config.yaml`;
change the `-c` default to `config/heartbeat_config.yaml`; make the code
timeout defaults match the YAML; parse `CRS_PORT_*` with a fallback and a
warning, as `mu2edaq-bigredbox`'s `config._port()` does.

## Related
- #1 (closed) — introduced 8081 via the start script but left the defaults.
- `mu2edaq-desktop/config/apps.yaml:85` — 8081 rationale.

<!-- mega-review-fingerprint: 3edfefe3 -->

---

## HBM-007

**Title:** `[registry] registry growth is unbounded when no HTTP client is polling`

**Severity:** S3 — correctness or maintainability risk, no current outage
**Category:** bug
**Location:** `heartbeat_monitor.py:156`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
Every distinct `name` creates a registry entry. Entries are removed only inside
`snapshot()`, which runs only for `/`, `/api/*` and `/stream` requests. With no
browser or poller connected, nothing is ever removed, and the number of entries
is limited only by how many distinct names arrive.

## Evidence
```
$ grep -n 'def snapshot\|del self._systems\|self._systems\[name\] =' heartbeat_monitor.py
156:            self._systems[name] = {
166:    def snapshot(self) -> List[dict]:
176:                    del self._systems[name]
```

## Why it matters
A sender that embeds a PID, timestamp or counter in `name`, or any host on the
network sending arbitrary names, grows the process without bound while nobody is
watching the UI (overnight). Even with the UI open, the bound is
`rate × cleanup_after`, and the shipped `cleanup_after` is 86400 s.

## Suggested fix
Purge from a periodic timer (or opportunistically in `record()`), and cap the
number of unconfigured names (drop or log new names beyond, say, 1000).

## Related
- HBM-002

<!-- mega-review-fingerprint: 39b50332 -->

---

## HBM-008

**Title:** `[web] Flask development server on 0.0.0.0 with no authentication; --debug exposes the Werkzeug debugger`

**Severity:** S3 — correctness or maintainability risk, no current outage
**Category:** bug
**Location:** `heartbeat_monitor.py:550`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
The production path runs `app.run(..., threaded=True)` — Werkzeug's development
server — on `web_host: 0.0.0.0`. No route is authenticated; `POST /api/theme`
changes server state for every viewer. Each `/stream` client holds a thread for
as long as it is connected, with no cap. `--debug` turns on Flask debug mode on
the same 0.0.0.0 bind, exposing the PIN-protected Werkzeug interactive debugger
to the network.

## Evidence
```
$ sed -n 550,556p heartbeat_monitor.py
        app.run(
            host=web_host,
            port=web_port,
            debug=args.debug,
            use_reloader=False,   # reloader conflicts with our background thread
            threaded=True,
        )
```

## Why it matters
The data is read-only and low-sensitivity, so the exposure today is small.
`--debug` on a DAQ host is the real risk: an interactive Python console sits
behind a PIN that is printed to the log, which is world-readable (HBM-005).
The unbounded per-SSE-client thread is a resource exhaustion path.

## Suggested fix
Refuse `--debug` unless `web_host` is a loopback address. Serve with
`waitress` or `gunicorn` (both small), or at least document that the dev
server is intentional. Consider binding `127.0.0.1` when the only consumer is
the local controlcenter pane.

## Related
- HBM-005

<!-- mega-review-fingerprint: 43ab272a -->

---

## HBM-009

**Title:** `[ci] no Python tests and no CI; the 20 gtest cases cover only the C++ sender`

**Severity:** S3 — correctness or maintainability risk, no current outage
**Category:** enhancement
**Location:** `cpp_sender/tests/test_heartbeat_sender_lib.cpp:1`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
The only test file is `cpp_sender/tests/test_heartbeat_sender_lib.cpp` (20
GoogleTest cases, all passing). `heartbeat_monitor.py` (registry, expiry,
packet handling, HTTP routes) and `heartbeat_sender.py` have no tests. There is
no `.github/workflows/`.

## Evidence
```
$ git ls-files | grep -i test
cpp_sender/tests/CMakeLists.txt
cpp_sender/tests/test_heartbeat_sender_lib.cpp
$ ctest --test-dir build
100% tests passed, 0 tests failed out of 20
$ ls .github 2>&1
ls: .github: No such file or directory
```

## Why it matters
HBM-001 and HBM-002 are both one-line regressions a minimal pytest suite would
catch (send a bad datagram; advance time past `cleanup_after`). The Python
sender's wire format is pinned only by convention.

## Suggested fix
Add `tests/` with pytest: `SystemRegistry` expiry with an injectable clock,
`_handle()` against malformed datagrams, Flask test-client checks of
`/api/systems` and `/api/health`, and a Python-vs-C++ payload equivalence test.
Add a GitHub Actions workflow running pytest on Python 3.9 and the C++ ctest.

## Related
- HBM-001, HBM-002

<!-- mega-review-fingerprint: 581bf4c5 -->

---

## HBM-010

**Title:** `[registry] liveness ages are computed from the wall clock, not a monotonic clock`

**Severity:** S4 — cosmetic, stale, hygiene
**Category:** bug
**Location:** `heartbeat_monitor.py:154`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
`record()` stores `datetime.now(timezone.utc)` and `snapshot()` subtracts it
from the current wall-clock time. A clock step on the monitor host shifts every
age at once.

## Evidence
```
$ grep -n 'datetime.now(timezone.utc)' heartbeat_monitor.py
154:        now = datetime.now(timezone.utc)
168:        now = datetime.now(timezone.utc)
```

## Why it matters
A forward step marks every node red simultaneously; a backward step shows
negative ages. chrony normally slews rather than steps, so in practice this is
transient and cosmetic, hence S4. The sender's timestamp is correctly not used,
so sender clock skew is not an issue.

## Suggested fix
Store `time.monotonic()` for age computation and keep the wall-clock time only
for display.

## Related
- none

<!-- mega-review-fingerprint: 88b830df -->

---

## HBM-011

**Title:** `[sender] C++ heartbeat_sender aborts on a non-numeric option and ignores sendto errors for hostnames`

**Severity:** S4 — cosmetic, stale, hygiene
**Category:** bug
**Location:** `cpp_sender/heartbeat_sender.cpp:92`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
`-p/--port`, `-i/--interval` and `--count` are parsed with `std::stoi`/`std::stod`
outside any `try`, so a non-numeric value terminates the process with an
uncaught exception (SIGABRT, possible core dump). `send_hostname()` discards
the `sendto()` return value, so a send failure to a named host reports success,
unlike the IP-literal paths which throw.

## Evidence
```
$ ./heartbeat_sender --name x -p abc; echo "exit=$?"
libc++abi: terminating due to uncaught exception of type std::invalid_argument: stoi: no conversion
exit=134
$ sed -n 188p cpp_sender/heartbeat_sender_lib.cpp
    sendto(sock, data.c_str(), data.size(), 0, res->ai_addr, res->ai_addrlen);
```

## Why it matters
A typo in a sender's launch line produces a core dump instead of a usage
message; a heartbeat to a hostname can fail silently in one-shot mode.

## Suggested fix
Wrap option parsing in `try { ... } catch (const std::exception&)` and print
usage; check `sendto()` in `send_hostname()` and throw like the other paths.

## Related
- none

<!-- mega-review-fingerprint: 67c939e6 -->

---

## HBM-012

**Title:** `[docs] README, docs/README.md and CLAUDE.md describe a config file, port and defaults that do not exist`

**Severity:** S4 — cosmetic, stale, hygiene
**Category:** documentation
**Location:** `README.md:111`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
The user-facing docs tell operators to copy and edit `config/config.yaml`
(absent; the file is `config/heartbeat_config.yaml`), say the dashboard is on
8080 (8081 under the control room), and `CLAUDE.md` gives fallback timeouts that
are the YAML's values rather than the code's. The package had no man pages
before this review.

## Evidence
```
$ grep -n 'config/config.yaml\|8080' README.md docs/README.md | head -5
docs/README.md:62:# Start with the default config file (config/config.yaml)
docs/README.md:72:... starts the web dashboard on port `8080`.
README.md:49:The web based dashboard will be available by default at `http://localhost:8080` ...
README.md:111:Copy `config/config.yaml` and edit as needed:
README.md:181:config/config.yaml       Server configuration
```
In the superproject, `DAQ-ControlRoom-Testing.md:88` says the monitor takes
"`-c` (not `--config`)"; both spellings are accepted
(`heartbeat_monitor.py:467`).

## Why it matters
An operator following the README edits a file the program never reads, or
opens the wrong port.

## Suggested fix
Point the READMEs at `config/heartbeat_config.yaml`, state 8081 as the
control-room port, and link `docs/OPERATIONS.md` and the new man pages. Correct
the superproject note (outside this repository).

## Related
- HBM-006

<!-- mega-review-fingerprint: 7f45bdcb -->

---

## HBM-013

**Title:** `[sender] heartbeat_sender.py stops at the first send error in --interval mode, unlike the C++ sender`

**Severity:** S4 — cosmetic, stale, hygiene
**Category:** bug
**Location:** `heartbeat_sender.py:263`
**Found by:** mega-review (`mu2e-sept-mega-review`), 2026-10-06

## What
In `--interval` mode the Python sender's loop catches only
`KeyboardInterrupt`. Any send error (a host name that briefly fails to
resolve, `ENETUNREACH` while an interface flaps, an unknown `--interface`)
raises out of the loop, prints a traceback and exits 1, so heartbeats stop
for good. The C++ sender prints a warning and keeps sending. Separately, the
Python sender decides whether an option "was given" by comparing it with its
default, so `--status OK`, `--host 127.0.0.1` or `--port 9999` cannot override
a different value in the `--file` JSON; the C++ sender tracks which options
were given.

## Evidence
```
$ sed -n 263,271p heartbeat_sender.py
        try:
            while count == 0 or iteration < count:
                send_one(iteration)
                ...
        except KeyboardInterrupt:
$ python3 heartbeat_sender.py -n x --host no-such-host.invalid --interval 0.2 --count 3
socket.gaierror: [Errno 8] nodename nor servname provided, or not known      (exit 1, one attempt)
$ cpp_sender/build/heartbeat_sender -n x --host no-such-host.invalid --interval 0.2 --count 3
Warning: Cannot resolve hostname: no-such-host.invalid                       (three attempts, exit 0)
$ sed -n 218,223p heartbeat_sender.py
    if args.status != "OK" or not args.file:
        cfg["status"] = args.status
    if args.host != "127.0.0.1" or not args.file:
        cfg["host"] = args.host
    if args.port != 9999 or not args.file:
        cfg["port"] = args.port
```

## Why it matters
A long-running Python heartbeat sender is killed by one transient network
error, after which its node turns red on the monitor although the node is
healthy. The failure is visible, which is why this is S4, but it costs an
operator a false alarm and a manual restart. The two senders are documented
as equivalent and are not.

## Suggested fix
Catch `OSError` (and `ValueError` from the link-local check) around
`send_one()` inside the loop, print a warning and continue, matching the C++
sender. Use `default=None` for `--status/--host/--port` and test `is not None`
to decide whether the option was given.

## Related
- HBM-011 (C++ sender error handling)
- `docs/INTERFACES.md` §1.4

<!-- mega-review-fingerprint: 4d8bfd73 -->

---

## Comment to add to existing issue #8

Target: `Mu2e/mu2edaq-heartbeatmonitor#8` (open). Comment only; do not open a
new issue. `package.yaml` lists this as `HBM-C08`.

> Re-checked on `mu2e-sept-mega-review` (HEAD `c68b5b4`), 2026-10-06: items 1–3 of
> this issue are unchanged. `bootstrap_heartbeat_monitor.sh` still has no
> `set -e`, still runs `python3 -m venv venv`, `pip install -r requirements.txt`
> and `mkdir -p data logs config` relative to the caller's working directory
> (lines 9, 17, 19), and still prints success unconditionally (line 20). Item 4
> is resolved: `requirements.txt` no longer names `mu2edaq-discovery`, and the
> script installs `../mu2edaq-discovery` when present (lines 13–15), but that
> path is also relative to the working directory. The `data/` and `logs/`
> directories it creates are not used by any code in the repository. Also note
> the off-convention name; sibling packages use `bootstrap.sh`.
>
> <!-- mega-review-fingerprint: 22ae614c -->
