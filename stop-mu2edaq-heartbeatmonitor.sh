#!/usr/bin/env bash
#
# stop-mu2edaq-heartbeatmonitor.sh - standardized Mu2e control-room stop
# script. Launched as `crs-app stop heartbeatmonitor`. Stops the daemon via its
# pid file (SIGTERM then SIGKILL after a timeout).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_FILE="${1:-$SCRIPT_DIR/heartbeat_monitor.pid}"
TIMEOUT="${CRS_STOP_TIMEOUT:-10}"

if [[ ! -f "$PID_FILE" ]]; then
  echo "Heartbeat Monitor not running (no pid file: $PID_FILE)"
  exit 0
fi
pid="$(cat "$PID_FILE")"
if ! kill -0 "$pid" 2>/dev/null; then
  echo "Heartbeat Monitor not running (stale pid $pid); cleaning up"
  rm -f "$PID_FILE"
  exit 0
fi

echo "Stopping Heartbeat Monitor (pid $pid)..."
kill -TERM "$pid" 2>/dev/null || true
for ((i = 0; i < TIMEOUT; i++)); do
  kill -0 "$pid" 2>/dev/null || break
  sleep 1
done
if kill -0 "$pid" 2>/dev/null; then
  echo "did not exit within ${TIMEOUT}s; sending SIGKILL"
  kill -KILL "$pid" 2>/dev/null || true
  sleep 1
fi
rm -f "$PID_FILE"
echo "Heartbeat Monitor stopped"
