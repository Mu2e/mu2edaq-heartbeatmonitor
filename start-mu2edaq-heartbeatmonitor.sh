#!/usr/bin/env bash
#
# start-mu2edaq-heartbeatmonitor.sh - standardized Mu2e control-room start
# script. Launched as `crs-app start heartbeatmonitor`, which exports
# CRS_PORT_HTTP and CRS_PORT_UDP from apps.yaml. heartbeat_monitor.py honors
# both (overriding server.web_port / server.udp_port in the config). Runs in
# daemon mode with a pid file.
#
# Note: the control room assigns HTTP 8081 (8080 is taken by resource-manager).
# Port precedence: CRS_PORT_* env > config file > built-in default.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

export CRS_PORT_HTTP="${CRS_PORT_HTTP:-8081}"   # web server port
export CRS_PORT_UDP="${CRS_PORT_UDP:-9999}"     # UDP listener port
CONFIG_FILE="${1:-./config/heartbeat_config.yaml}"
PID_FILE="$SCRIPT_DIR/heartbeat_monitor.pid"

if [[ ! -x ./venv/bin/python ]]; then
  echo "error: virtual environment not found; run ./bootstrap_heartbeat_monitor.sh first" >&2
  exit 1
fi
# shellcheck disable=SC1091
source ./venv/bin/activate

echo "Starting Heartbeat Monitor (http=$CRS_PORT_HTTP udp=$CRS_PORT_UDP, config: $CONFIG_FILE)"
exec python heartbeat_monitor.py --config "$CONFIG_FILE" --daemon --pid-file "$PID_FILE"
