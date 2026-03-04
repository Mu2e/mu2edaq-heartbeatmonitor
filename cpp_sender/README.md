# heartbeat_sender (C++)

C++ implementation of the HeartBeat Monitor sender. Sends UDP heartbeat packets to a running `heartbeat_monitor.py` instance. Feature-parity with the Python sender.

## Requirements

| Tool | Minimum version |
|------|----------------|
| CMake | 3.14 |
| C++ compiler | C++17 (GCC 7+, Clang 5+, AppleClang 10+) |
| nlohmann/json | 3.x — fetched automatically if not found on the system |

The build requires internet access the first time if `nlohmann/json` is not already installed, because CMake will clone it from GitHub. Subsequent builds reuse the cached copy in the build tree.

## Build

```bash
# 1. Configure (creates the build directory)
cmake -B build -S .

# 2. Compile
cmake --build build
```

The binary is written to `build/heartbeat_sender`.

### Build type

Pass `-DCMAKE_BUILD_TYPE=<type>` to control optimisation level. Common values:

| Type | Effect |
|------|--------|
| `Debug` | No optimisation, full debug symbols |
| `Release` | `-O3`, no debug symbols |
| `RelWithDebInfo` | `-O2` with debug symbols |

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Parallel compilation

```bash
cmake --build build --parallel      # use all available cores
cmake --build build --parallel 4    # limit to 4 cores
```

### Using a system-installed nlohmann/json

If `nlohmann_json` is installed (e.g. via `apt install nlohmann-json3-dev` or `brew install nlohmann-json`), CMake will use it and skip the network fetch:

```bash
cmake -B build -S .
# Output will NOT contain the "fetching via FetchContent" status message
```

## Install

To install the binary to a standard location (default: `/usr/local/bin`):

```bash
cmake --install build
```

To install to a custom prefix:

```bash
cmake --install build --prefix ~/.local
# binary lands at ~/.local/bin/heartbeat_sender
```

## Clean rebuild

Delete the build directory and reconfigure:

```bash
rm -rf build
cmake -B build -S .
cmake --build build
```

## Usage

```
heartbeat_sender [OPTIONS]

Options:
  -f, --file FILE        Load config from a JSON file
  -n, --name NAME        Name of this sending system/application (required)
  -s, --status STATUS    Status string (default: OK)
      --host HOST        Monitor host (default: 127.0.0.1)
  -p, --port PORT        Monitor UDP port (default: 9999)
  -i, --interval SECS    Send continuously every N seconds (0 = once)
      --count N          Stop after N packets in continuous mode (0 = unlimited)
  -I, --interface IFACE  Network interface for IPv6 link-local addresses
  -v, --verbose          Print each packet as it is sent
  -h, --help             Show this help
```

### Examples

```bash
# One-shot
./build/heartbeat_sender --name my-app --status OK

# Continuous, every 10 seconds
./build/heartbeat_sender --name my-app --status running --interval 10

# Send 5 packets then stop
./build/heartbeat_sender --name my-app --interval 5 --count 5

# Load config from a JSON file
./build/heartbeat_sender --file ../example_sender.json

# Remote monitor
./build/heartbeat_sender --name my-app --host 192.168.1.100 --port 9999

# IPv6 link-local (interface required)
./build/heartbeat_sender --name my-app --host fe80::1 --interface eth0
./build/heartbeat_sender --name my-app --host fe80::1%eth0   # equivalent
```

See the top-level [docs/README.md](../docs/README.md) for full networking documentation including IPv4/IPv6 multicast configuration.
