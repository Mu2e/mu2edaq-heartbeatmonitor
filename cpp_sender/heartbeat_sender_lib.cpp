/**
 * heartbeat_sender_lib.cpp
 *
 * Implementation of the HeartBeat sender library.
 */

#include "heartbeat_sender_lib.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hbsender {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::string now_iso8601()
{
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;

    std::tm utc{};
    gmtime_r(&t, &utc);

    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "+00:00";
    return oss.str();
}

static std::string escape_json(const std::string& s)
{
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b";  break;
            case '\f': o << "\\f";  break;
            case '\n': o << "\\n";  break;
            case '\r': o << "\\r";  break;
            case '\t': o << "\\t";  break;
            default:
                if (c < 0x20)
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                else
                    o << c;
        }
    }
    return o.str();
}

std::string build_packet(const std::string& name, const std::string& status)
{
    return "{\"name\":\"" + escape_json(name) +
           "\",\"timestamp\":\"" + now_iso8601() +
           "\",\"status\":\"" + escape_json(status) + "\"}";
}

// ---------------------------------------------------------------------------
// Address classification helpers
// ---------------------------------------------------------------------------

static bool is_ipv4_multicast(const struct in_addr& a)
{
    return (ntohl(a.s_addr) >> 28) == 0xEu;
}

static bool is_ipv6_multicast(const struct in6_addr& a)
{
    return a.s6_addr[0] == 0xff;
}

// fe80::/10 unicast link-local OR ff02::/16 link-local multicast
static bool is_ipv6_link_local_any(const struct in6_addr& a)
{
    bool ll_unicast = (a.s6_addr[0] == 0xfe) && ((a.s6_addr[1] & 0xc0) == 0x80);
    bool ll_mcast   = (a.s6_addr[0] == 0xff) && ((a.s6_addr[1] & 0x0f) == 0x02);
    return ll_unicast || ll_mcast;
}

// ---------------------------------------------------------------------------
// Socket send helpers
// ---------------------------------------------------------------------------

static void send_ipv4(int port, const std::string& data, const struct in_addr& addr)
{
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        throw std::runtime_error(std::string("socket(): ") + strerror(errno));

    if (is_ipv4_multicast(addr)) {
        unsigned char ttl = 32;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    } else {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(static_cast<uint16_t>(port));
    dst.sin_addr   = addr;

    if (sendto(sock, data.c_str(), data.size(), 0,
               reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst)) < 0) {
        int err = errno;
        ::close(sock);
        throw std::runtime_error(std::string("sendto(): ") + strerror(err));
    }
    ::close(sock);
}

static void send_ipv6(int port, const std::string& data,
                      const struct in6_addr& addr, const std::string& iface)
{
    unsigned int if_index = iface.empty() ? 0u : if_nametoindex(iface.c_str());
    if (!iface.empty() && if_index == 0)
        throw std::runtime_error("Unknown interface: " + iface);

    if (is_ipv6_link_local_any(addr) && if_index == 0)
        throw std::runtime_error(
            "IPv6 link-local address requires --interface <name> "
            "or address%interface notation");

    int sock = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0)
        throw std::runtime_error(std::string("socket(): ") + strerror(errno));

    if (is_ipv6_multicast(addr)) {
        int hops = 32;
        setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
        if (if_index)
            setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &if_index, sizeof(if_index));
    }

    struct sockaddr_in6 dst{};
    dst.sin6_family   = AF_INET6;
    dst.sin6_port     = htons(static_cast<uint16_t>(port));
    dst.sin6_addr     = addr;
    dst.sin6_scope_id = is_ipv6_link_local_any(addr) ? if_index : 0u;

    if (sendto(sock, data.c_str(), data.size(), 0,
               reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst)) < 0) {
        int err = errno;
        ::close(sock);
        throw std::runtime_error(std::string("sendto(): ") + strerror(err));
    }
    ::close(sock);
}

static void send_hostname(const std::string& host, int port, const std::string& data)
{
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    std::string port_str = std::to_string(port);

    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
        throw std::runtime_error("Cannot resolve hostname: " + host);

    int sock = ::socket(res->ai_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        freeaddrinfo(res);
        throw std::runtime_error(std::string("socket(): ") + strerror(errno));
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    sendto(sock, data.c_str(), data.size(), 0, res->ai_addr, res->ai_addrlen);
    ::close(sock);
    freeaddrinfo(res);
}

void send_packet(const std::string& host, int port,
                 const std::string& data, const std::string& iface)
{
    // Strip scope suffix (e.g. "ff02::1%eth0")
    std::string actual_host = host;
    std::string scope_iface = iface;
    auto pct = host.find('%');
    if (pct != std::string::npos) {
        actual_host = host.substr(0, pct);
        if (scope_iface.empty())
            scope_iface = host.substr(pct + 1);
    }

    struct in_addr  addr4{};
    struct in6_addr addr6{};
    if (inet_pton(AF_INET, actual_host.c_str(), &addr4) == 1)
        send_ipv4(port, data, addr4);
    else if (inet_pton(AF_INET6, actual_host.c_str(), &addr6) == 1)
        send_ipv6(port, data, addr6, scope_iface);
    else
        send_hostname(actual_host, port, data);
}

// ---------------------------------------------------------------------------
// Config file loading
// ---------------------------------------------------------------------------

void load_json_file(const std::string& path, Config& cfg)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("File not found: " + path);

    json j;
    try { f >> j; }
    catch (const json::parse_error& e) {
        throw std::runtime_error("Invalid JSON in " + path + ": " + e.what());
    }

    if (j.contains("name"))      cfg.name     = j["name"].get<std::string>();
    if (j.contains("status"))    cfg.status   = j["status"].get<std::string>();
    if (j.contains("host"))      cfg.host     = j["host"].get<std::string>();
    if (j.contains("port"))      cfg.port     = j["port"].get<int>();
    if (j.contains("interval"))  cfg.interval = j["interval"].get<double>();
    if (j.contains("count"))     cfg.count    = j["count"].get<int>();
    if (j.contains("interface")) cfg.iface    = j["interface"].get<std::string>();
}

} // namespace hbsender
