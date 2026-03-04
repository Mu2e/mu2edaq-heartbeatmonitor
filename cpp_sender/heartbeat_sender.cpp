/**
 * heartbeat_sender.cpp
 *
 * C++ UDP heartbeat sender for HeartBeat Monitor.
 *
 * Usage examples:
 *   ./heartbeat_sender --name my-app --status OK
 *   ./heartbeat_sender --name my-app --status running --interval 10
 *   ./heartbeat_sender --file heartbeat.json
 *   ./heartbeat_sender --file heartbeat.json --interval 5
 */

#include <arpa/inet.h>
#include <getopt.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;

static void handle_sigint(int) { g_stop = 1; }

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

static std::string now_iso8601()
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

static std::string build_packet(const std::string& name, const std::string& status)
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

static void send_ipv4(const std::string& /*host*/, int port, const std::string& data,
                      const struct in_addr& addr)
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

static void send_ipv6(const std::string& /*host*/, int port, const std::string& data,
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

static void send_packet(const std::string& host, int port, const std::string& data,
                        const std::string& iface)
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
        send_ipv4(actual_host, port, data, addr4);
    else if (inet_pton(AF_INET6, actual_host.c_str(), &addr6) == 1)
        send_ipv6(actual_host, port, data, addr6, scope_iface);
    else
        send_hostname(actual_host, port, data);
}

// ---------------------------------------------------------------------------
// Config
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

static void load_json_file(const std::string& path, Config& cfg)
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

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void print_help(const char* prog)
{
    std::cout <<
        "Usage: " << prog << " [OPTIONS]\n\n"
        "Send heartbeat packets to a HeartBeat Monitor over UDP.\n\n"
        "Options:\n"
        "  -f, --file FILE        Load config from a JSON file\n"
        "  -n, --name NAME        Name of this sending system/application\n"
        "  -s, --status STATUS    Status string (default: OK)\n"
        "      --host HOST        Monitor host (default: 127.0.0.1)\n"
        "  -p, --port PORT        Monitor UDP port (default: 9999)\n"
        "  -i, --interval SECS    Send continuously every N seconds (0 = once)\n"
        "      --count N          Stop after N packets in continuous mode (0 = unlimited)\n"
        "  -I, --interface IFACE  Network interface for IPv6 link-local addresses\n"
        "  -v, --verbose          Print each packet as it is sent\n"
        "  -h, --help             Show this help\n\n"
        "Examples:\n"
        "  " << prog << " --name my-app --status OK\n"
        "  " << prog << " --name my-app --status running --interval 10\n"
        "  " << prog << " --file heartbeat.json\n"
        "  " << prog << " --file heartbeat.json --interval 5\n";
}

// Long-only option values (> 127 to avoid clash with short opts)
enum { OPT_HOST = 256, OPT_COUNT };

int main(int argc, char* argv[])
{
    Config cfg;
    bool   cli_name = false, cli_status = false, cli_host = false;
    bool   cli_port = false, cli_interval = false, cli_count = false;
    std::string file_path;

    static const struct option long_opts[] = {
        {"file",      required_argument, nullptr, 'f'},
        {"name",      required_argument, nullptr, 'n'},
        {"status",    required_argument, nullptr, 's'},
        {"host",      required_argument, nullptr, OPT_HOST},
        {"port",      required_argument, nullptr, 'p'},
        {"interval",  required_argument, nullptr, 'i'},
        {"count",     required_argument, nullptr, OPT_COUNT},
        {"interface", required_argument, nullptr, 'I'},
        {"verbose",   no_argument,       nullptr, 'v'},
        {"help",      no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:n:s:p:i:I:vh", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'f': file_path     = optarg;                        break;
            case 'n': cfg.name      = optarg; cli_name     = true;   break;
            case 's': cfg.status    = optarg; cli_status   = true;   break;
            case OPT_HOST: cfg.host = optarg; cli_host     = true;   break;
            case 'p': cfg.port      = std::stoi(optarg); cli_port    = true; break;
            case 'i': cfg.interval  = std::stod(optarg); cli_interval= true; break;
            case OPT_COUNT: cfg.count = std::stoi(optarg); cli_count = true; break;
            case 'I': cfg.iface     = optarg;                        break;
            case 'v': cfg.verbose   = true;                          break;
            case 'h': print_help(argv[0]); return 0;
            default:  std::cerr << "Use --help for usage.\n"; return 1;
        }
    }

    // If a file was provided, load it first then let explicit CLI args win
    if (!file_path.empty()) {
        Config file_cfg;
        try { load_json_file(file_path, file_cfg); }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        if (!cli_name)     cfg.name     = file_cfg.name;
        if (!cli_status)   cfg.status   = file_cfg.status;
        if (!cli_host)     cfg.host     = file_cfg.host;
        if (!cli_port)     cfg.port     = file_cfg.port;
        if (!cli_interval) cfg.interval = file_cfg.interval;
        if (!cli_count)    cfg.count    = file_cfg.count;
        if (cfg.iface.empty()) cfg.iface = file_cfg.iface;
    }

    if (cfg.name.empty()) {
        std::cerr << "Error: --name is required (or set 'name' in the JSON file)\n";
        return 1;
    }

    auto send_heartbeat = [&](int iteration) {
        std::string packet = build_packet(cfg.name, cfg.status);
        send_packet(cfg.host, cfg.port, packet, cfg.iface);
        if (cfg.verbose) {
            std::cout << "[" << now_iso8601() << "] Sent heartbeat #" << (iteration + 1)
                      << " -> " << cfg.host << ":" << cfg.port
                      << "  name=" << cfg.name << "  status=" << cfg.status << "\n";
        }
    };

    signal(SIGINT, handle_sigint);

    if (cfg.interval <= 0.0) {
        // One-shot
        try { send_heartbeat(0); }
        catch (const std::exception& e) {
            std::cerr << "Error sending: " << e.what() << "\n";
            return 1;
        }
        std::cout << "Heartbeat sent to " << cfg.host << ":" << cfg.port
                  << " (name=" << cfg.name << ", status=" << cfg.status << ")\n";
    } else {
        // Continuous
        std::cout << "Sending heartbeats every " << cfg.interval << "s to "
                  << cfg.host << ":" << cfg.port
                  << " (name=" << cfg.name << ", status=" << cfg.status
                  << "). Press Ctrl+C to stop.\n";

        int iteration = 0;
        while (!g_stop && (cfg.count == 0 || iteration < cfg.count)) {
            try { send_heartbeat(iteration); }
            catch (const std::exception& e) {
                std::cerr << "Warning: " << e.what() << "\n";
            }
            ++iteration;
            if (cfg.count && iteration >= cfg.count) break;

            // Sleep in small chunks so SIGINT is handled promptly
            int sleep_ms = static_cast<int>(cfg.interval * 1000);
            int elapsed  = 0;
            while (!g_stop && elapsed < sleep_ms) {
                int chunk = std::min(100, sleep_ms - elapsed);
                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                elapsed += chunk;
            }
        }
        if (g_stop)
            std::cout << "\nStopped after " << iteration << " heartbeat(s).\n";
    }

    return 0;
}
