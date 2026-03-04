#!/usr/bin/env python3
"""
HeartBeat Sender
Sends a heartbeat packet to a HeartBeat Monitor instance over UDP.

Usage examples:
  # One-shot via CLI options:
  python heartbeat_sender.py --name my-app --status "OK" --host 127.0.0.1 --port 9999

  # Continuous mode (send every N seconds):
  python heartbeat_sender.py --name my-app --status "running" --interval 10

  # Load config from a JSON file:
  python heartbeat_sender.py --file heartbeat.json

  # JSON file with continuous mode:
  python heartbeat_sender.py --file heartbeat.json --interval 5
"""

import argparse
import ipaddress
import json
import socket
import struct
import sys
import time
from datetime import datetime, timezone


# ---------------------------------------------------------------------------
# Packet builder
# ---------------------------------------------------------------------------

def build_packet(name: str, status: str, timestamp: str = None) -> bytes:
    if timestamp is None:
        timestamp = datetime.now(timezone.utc).isoformat()
    payload = {
        "name": name,
        "timestamp": timestamp,
        "status": status,
    }
    return json.dumps(payload).encode("utf-8")


def send_packet(host: str, port: int, data: bytes, interface: str = "") -> None:
    """Send a UDP heartbeat packet.

    Supports IPv4 unicast, IPv4 broadcast, IPv4 multicast, IPv6 unicast,
    IPv6 link-local unicast (fe80::/10), and IPv6 multicast (including
    link-local multicast ff02::/16).

    For IPv6 link-local addresses an *interface* must be provided either via
    the ``interface`` parameter or as a scope suffix on the address
    (e.g. ``ff02::1%eth0``).
    """
    # Pull scope suffix out of the host string (e.g. "ff02::1%eth0")
    scope_iface = interface
    if "%" in host:
        host, pct_iface = host.split("%", 1)
        scope_iface = scope_iface or pct_iface

    try:
        addr_obj = ipaddress.ip_address(host)
    except ValueError:
        # Hostname — resolve and send; enable broadcast in case it resolves to one
        results = socket.getaddrinfo(host, port, socket.AF_UNSPEC, socket.SOCK_DGRAM)
        if not results:
            raise RuntimeError(f"Cannot resolve hostname: {host}")
        family, _, _, _, sockaddr = results[0]
        with socket.socket(family, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.sendto(data, sockaddr)
        return

    if isinstance(addr_obj, ipaddress.IPv6Address):
        _send_ipv6(host, port, data, addr_obj, scope_iface)
    else:
        _send_ipv4(host, port, data, addr_obj)


def _send_ipv4(host: str, port: int, data: bytes,
               addr_obj: ipaddress.IPv4Address) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if addr_obj.is_multicast:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)
        else:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.sendto(data, (host, port))


def _send_ipv6(host: str, port: int, data: bytes,
               addr_obj: ipaddress.IPv6Address, interface: str = "") -> None:
    if_index = socket.if_nametoindex(interface) if interface else 0

    # Link-local unicast (fe80::/10) and link-local multicast (ff02::/16)
    # require a scope ID; without one the OS cannot pick the right interface.
    is_link_local = addr_obj.is_link_local or (
        addr_obj.is_multicast and (addr_obj.packed[1] & 0x0F) == 2
    )
    if is_link_local and not if_index:
        raise ValueError(
            f"IPv6 link-local address {host} requires an interface. "
            "Pass --interface <name> or use the address%interface notation."
        )

    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as sock:
        if addr_obj.is_multicast:
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS,
                            struct.pack("I", 32))
            if if_index:
                sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF,
                                struct.pack("I", if_index))
        scope_id = if_index if is_link_local else 0
        sock.sendto(data, (host, port, 0, scope_id))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send heartbeat packets to HeartBeat Monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    src = parser.add_mutually_exclusive_group()
    src.add_argument(
        "-f", "--file",
        metavar="JSON_FILE",
        help="Load sender config from a JSON file",
    )

    parser.add_argument(
        "-n", "--name",
        metavar="NAME",
        help="Name of this sending system/application",
    )
    parser.add_argument(
        "-s", "--status",
        metavar="STATUS",
        default="OK",
        help="Status string to include in the heartbeat (default: OK)",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="HeartBeat Monitor host (default: 127.0.0.1)",
    )
    parser.add_argument(
        "-p", "--port",
        type=int,
        default=9999,
        help="HeartBeat Monitor UDP port (default: 9999)",
    )
    parser.add_argument(
        "-i", "--interval",
        type=float,
        metavar="SECONDS",
        default=0,
        help="Send heartbeats continuously every N seconds (0 = send once)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=0,
        help="Number of packets to send in continuous mode (0 = unlimited)",
    )
    parser.add_argument(
        "-I", "--interface",
        metavar="IFACE",
        default="",
        help="Network interface for IPv6 link-local addresses (e.g. eth0)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print each packet as it is sent",
    )

    return parser.parse_args()


def load_json_file(path: str) -> dict:
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        print(f"Error: file not found: {path}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as exc:
        print(f"Error: invalid JSON in {path}: {exc}", file=sys.stderr)
        sys.exit(1)


def main():
    args = parse_args()

    # Build effective config, merging file + CLI args (CLI takes precedence)
    cfg = {
        "name": None,
        "status": "OK",
        "host": "127.0.0.1",
        "port": 9999,
        "interval": 0,
        "count": 0,
        "interface": "",
    }

    if args.file:
        file_cfg = load_json_file(args.file)
        cfg.update(file_cfg)

    # CLI args override file values when explicitly provided
    if args.name:
        cfg["name"] = args.name
    if args.status != "OK" or not args.file:
        cfg["status"] = args.status
    if args.host != "127.0.0.1" or not args.file:
        cfg["host"] = args.host
    if args.port != 9999 or not args.file:
        cfg["port"] = args.port
    if args.interval:
        cfg["interval"] = args.interval
    if args.count:
        cfg["count"] = args.count
    if args.interface:
        cfg["interface"] = args.interface

    if not cfg["name"]:
        print("Error: --name is required (or set 'name' in the JSON file)", file=sys.stderr)
        sys.exit(1)

    host = cfg["host"]
    port = int(cfg["port"])
    name = cfg["name"]
    status = cfg["status"]
    interval = float(cfg["interval"])
    count = int(cfg["count"])
    interface = cfg.get("interface", "")

    def send_one(iteration: int = 0):
        # Allow dynamic status via callable format strings from file
        current_status = status
        packet = build_packet(name, current_status)
        send_packet(host, port, packet, interface=interface)
        if args.verbose:
            ts = datetime.now(timezone.utc).isoformat()
            print(f"[{ts}] Sent heartbeat #{iteration + 1} → {host}:{port}  "
                  f"name={name!r}  status={current_status!r}")

    if interval <= 0:
        # One-shot
        send_one()
        if args.verbose or True:
            print(f"Heartbeat sent to {host}:{port} (name={name!r}, status={status!r})")
    else:
        # Continuous
        iteration = 0
        print(f"Sending heartbeats every {interval}s to {host}:{port} "
              f"(name={name!r}, status={status!r}). Press Ctrl+C to stop.")
        try:
            while count == 0 or iteration < count:
                send_one(iteration)
                iteration += 1
                if count and iteration >= count:
                    break
                time.sleep(interval)
        except KeyboardInterrupt:
            print(f"\nStopped after {iteration} heartbeat(s).")


if __name__ == "__main__":
    main()
