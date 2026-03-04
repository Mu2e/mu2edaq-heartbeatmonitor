/**
 * heartbeat_sender.cpp
 *
 * Command-line interface for the HeartBeat sender.
 *
 * Usage examples:
 *   ./heartbeat_sender --name my-app --status OK
 *   ./heartbeat_sender --name my-app --status running --interval 10
 *   ./heartbeat_sender --file heartbeat.json
 *   ./heartbeat_sender --file heartbeat.json --interval 5
 */

#include "heartbeat_sender_lib.h"

#include <getopt.h>

using namespace hbsender;
#include <signal.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;

static void handle_sigint(int) { g_stop = 1; }

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
