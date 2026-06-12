#!/usr/bin/env bash
# Start the Heartbeat Monitor as a background daemon.
# CRS_PORT_HTTP / CRS_PORT_UDP (exported by the control room crs-app
# launcher) override the configured web and UDP ingest ports.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_FILE="/tmp/heartbeat_monitor.pid"

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "Heartbeat Monitor is already running (PID $(cat "$PID_FILE"))."
  exit 0
fi

PYTHON="$SCRIPT_DIR/venv/bin/python"
[[ -x "$PYTHON" ]] || PYTHON="python3"

"$PYTHON" "$SCRIPT_DIR/heartbeat_monitor.py" --daemon --pid-file "$PID_FILE" "$@"
echo "Heartbeat Monitor started (PID $(cat "$PID_FILE"))."
