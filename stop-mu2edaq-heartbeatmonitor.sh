#!/usr/bin/env bash
# Stop the Heartbeat Monitor daemon.
set -uo pipefail

PID_FILE="/tmp/heartbeat_monitor.pid"

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  kill "$(cat "$PID_FILE")"
  rm -f "$PID_FILE"
  echo "Heartbeat Monitor stopped."
else
  rm -f "$PID_FILE" 2>/dev/null || true
  echo "Heartbeat Monitor is not running."
fi
exit 0
