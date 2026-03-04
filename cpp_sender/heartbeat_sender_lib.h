/**
 * heartbeat_sender_lib.h
 *
 * Public API for the HeartBeat sender library.
 *
 * Core functionality for building and transmitting UDP heartbeat packets.
 * Supports IPv4, IPv6, multicast, broadcast, and hostname destinations.
 */

#pragma once

#include <string>

namespace hbsender {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    std::string name;
    std::string status   = "OK";
    std::string host     = "127.0.0.1";
    int         port     = 9999;
    double      interval = 0.0;
    int         count    = 0;
    std::string iface;
    bool        verbose  = false;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Returns the current UTC time formatted as an ISO-8601 string with milliseconds.
std::string now_iso8601();

/// Builds a JSON heartbeat packet string from the given name and status.
std::string build_packet(const std::string& name, const std::string& status);

/**
 * Sends *data* as a UDP datagram to *host*:*port*.
 *
 * *host* may be:
 *   - an IPv4 address (unicast, broadcast, or multicast)
 *   - an IPv6 address (unicast or multicast), optionally with a %scope suffix
 *   - a hostname (resolved via getaddrinfo)
 *
 * *iface* is used for IPv6 link-local and multicast destinations that require
 * a specific outgoing interface.  Pass an empty string to use the system default.
 *
 * Throws std::runtime_error on failure.
 */
void send_packet(const std::string& host, int port,
                 const std::string& data, const std::string& iface);

/**
 * Populates *cfg* from a JSON file at *path*.
 * Only keys present in the file overwrite the corresponding Config fields.
 * Throws std::runtime_error if the file cannot be opened or parsed.
 */
void load_json_file(const std::string& path, Config& cfg);

} // namespace hbsender
