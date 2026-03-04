# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
# Install dependencies
pip install -r requirements.txt

# Run the monitor server (default config.yaml)
python heartbeat_monitor.py

# Run with custom config
python heartbeat_monitor.py --config my-config.yaml

# Run with Flask debug mode
python heartbeat_monitor.py --debug

# Send a one-shot heartbeat
python heartbeat_sender.py --name my-app --status "OK"

# Send continuous heartbeats (every 10 seconds)
python heartbeat_sender.py --name my-app --status "running" --interval 10

# Send heartbeats from a JSON config file
python heartbeat_sender.py --file example_sender.json
```

## Architecture

The system has two components: a **monitor server** and a **sender client**.

### Monitor Server (`heartbeat_monitor.py`)

- `SystemRegistry` — thread-safe in-memory store for all system heartbeat states. Handles timeout detection and stale entry cleanup. Per-system timeouts can override the global default.
- `UDPListener` — daemon thread that binds to the configured UDP port, parses incoming JSON packets, and writes to the registry.
- Flask web server exposes:
  - `GET /` — web dashboard
  - `GET /api/systems` — JSON snapshot of all system statuses
  - `GET /api/health` — aggregate health summary
  - `GET /stream` — SSE endpoint used by the dashboard for real-time push updates

### Sender Client (`heartbeat_sender.py`)

Sends JSON heartbeat packets over UDP. Supports one-shot, continuous interval, or file-based configuration. Packet format:
```json
{"name": "my-app", "timestamp": "...", "status": "OK"}
```

### Configuration (`config.yaml`)

Three sections: `server` (UDP/web host+port), `monitor` (timeouts, cleanup interval, dashboard grid), and optional `systems` list for per-system timeout overrides. The monitor falls back to `default_timeout` (60s) and `cleanup_after` (86400s) if not specified.

### Dashboard (`templates/index.html`)

Single-file HTML/CSS/JS dashboard. Uses SSE from `/stream` for live updates. No build step — served directly by Flask.
