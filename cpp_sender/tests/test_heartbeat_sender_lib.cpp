/**
 * test_heartbeat_sender_lib.cpp
 *
 * Unit tests for the hbsender library (heartbeat_sender_lib).
 *
 * Coverage:
 *   now_iso8601()    — format, UTC offset, monotonicity
 *   build_packet()   — JSON structure, field values, special-character escaping
 *   load_json_file() — full load, partial load, missing file, invalid JSON
 *   send_packet()    — loopback IPv4 round-trip, error cases
 */

#include "heartbeat_sender_lib.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace hbsender;
using json = nlohmann::json;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Write *content* to a temp file and return its path.
static fs::path write_temp(const std::string& content, const std::string& suffix = ".json")
{
    auto path = fs::temp_directory_path() / (::testing::UnitTest::GetInstance()
                    ->current_test_info()->name() + suffix);
    std::ofstream f(path);
    f << content;
    return path;
}

// ---------------------------------------------------------------------------
// now_iso8601
// ---------------------------------------------------------------------------

TEST(NowIso8601, MatchesExpectedFormat)
{
    // Expected: YYYY-MM-DDTHH:MM:SS.mmm+00:00
    static const std::regex iso_re(
        R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}\+00:00)");
    std::string ts = now_iso8601();
    EXPECT_TRUE(std::regex_match(ts, iso_re)) << "Timestamp was: " << ts;
}

TEST(NowIso8601, EndsWithUtcOffset)
{
    std::string ts = now_iso8601();
    EXPECT_EQ(ts.substr(ts.size() - 6), "+00:00");
}

TEST(NowIso8601, IsMonotonicallyNonDecreasing)
{
    std::string t1 = now_iso8601();
    std::string t2 = now_iso8601();
    // Lexicographic order is equivalent to chronological order for this format.
    EXPECT_LE(t1, t2);
}

// ---------------------------------------------------------------------------
// build_packet
// ---------------------------------------------------------------------------

TEST(BuildPacket, ProducesValidJson)
{
    std::string pkt = build_packet("my-app", "OK");
    json j;
    EXPECT_NO_THROW({ j = json::parse(pkt); });
}

TEST(BuildPacket, ContainsRequiredFields)
{
    json j = json::parse(build_packet("svc", "running"));
    EXPECT_TRUE(j.contains("name"));
    EXPECT_TRUE(j.contains("timestamp"));
    EXPECT_TRUE(j.contains("status"));
}

TEST(BuildPacket, FieldValuesAreCorrect)
{
    json j = json::parse(build_packet("alpha", "degraded"));
    EXPECT_EQ(j["name"].get<std::string>(),   "alpha");
    EXPECT_EQ(j["status"].get<std::string>(), "degraded");
}

TEST(BuildPacket, TimestampMatchesIso8601)
{
    static const std::regex iso_re(
        R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}\+00:00)");
    json j = json::parse(build_packet("x", "OK"));
    std::string ts = j["timestamp"].get<std::string>();
    EXPECT_TRUE(std::regex_match(ts, iso_re)) << "Timestamp was: " << ts;
}

TEST(BuildPacket, EscapesDoubleQuoteInName)
{
    // A name containing a double-quote must not break JSON validity.
    std::string pkt = build_packet("say \"hello\"", "OK");
    json j;
    ASSERT_NO_THROW({ j = json::parse(pkt); });
    EXPECT_EQ(j["name"].get<std::string>(), "say \"hello\"");
}

TEST(BuildPacket, EscapesBackslashInStatus)
{
    std::string pkt = build_packet("app", "C:\\path\\to\\thing");
    json j;
    ASSERT_NO_THROW({ j = json::parse(pkt); });
    EXPECT_EQ(j["status"].get<std::string>(), "C:\\path\\to\\thing");
}

TEST(BuildPacket, EscapesNewlineInName)
{
    std::string pkt = build_packet("line1\nline2", "OK");
    json j;
    ASSERT_NO_THROW({ j = json::parse(pkt); });
    EXPECT_EQ(j["name"].get<std::string>(), "line1\nline2");
}

TEST(BuildPacket, HandlesEmptyStrings)
{
    std::string pkt = build_packet("", "");
    json j;
    ASSERT_NO_THROW({ j = json::parse(pkt); });
    EXPECT_EQ(j["name"].get<std::string>(),   "");
    EXPECT_EQ(j["status"].get<std::string>(), "");
}

// ---------------------------------------------------------------------------
// load_json_file
// ---------------------------------------------------------------------------

TEST(LoadJsonFile, LoadsAllFields)
{
    auto path = write_temp(R"({
        "name":      "test-svc",
        "status":    "nominal",
        "host":      "192.168.1.1",
        "port":      5555,
        "interval":  3.5,
        "count":     10,
        "interface": "eth0"
    })");

    Config cfg;
    load_json_file(path.string(), cfg);

    EXPECT_EQ(cfg.name,     "test-svc");
    EXPECT_EQ(cfg.status,   "nominal");
    EXPECT_EQ(cfg.host,     "192.168.1.1");
    EXPECT_EQ(cfg.port,     5555);
    EXPECT_DOUBLE_EQ(cfg.interval, 3.5);
    EXPECT_EQ(cfg.count,    10);
    EXPECT_EQ(cfg.iface,    "eth0");

    fs::remove(path);
}

TEST(LoadJsonFile, PartialFilePreservesDefaults)
{
    // Only override name; everything else should stay at Config defaults.
    auto path = write_temp(R"({"name": "partial"})");

    Config cfg;
    load_json_file(path.string(), cfg);

    EXPECT_EQ(cfg.name,     "partial");
    EXPECT_EQ(cfg.status,   "OK");          // default
    EXPECT_EQ(cfg.host,     "127.0.0.1");   // default
    EXPECT_EQ(cfg.port,     9999);          // default
    EXPECT_DOUBLE_EQ(cfg.interval, 0.0);    // default
    EXPECT_EQ(cfg.count,    0);             // default
    EXPECT_EQ(cfg.iface,    "");            // default

    fs::remove(path);
}

TEST(LoadJsonFile, ThrowsOnMissingFile)
{
    Config cfg;
    EXPECT_THROW(load_json_file("/nonexistent/path/config.json", cfg),
                 std::runtime_error);
}

TEST(LoadJsonFile, ThrowsOnInvalidJson)
{
    auto path = write_temp("{ this is not json }");
    Config cfg;
    EXPECT_THROW(load_json_file(path.string(), cfg), std::runtime_error);
    fs::remove(path);
}

TEST(LoadJsonFile, HandlesEmptyObject)
{
    auto path = write_temp("{}");
    Config cfg;
    EXPECT_NO_THROW(load_json_file(path.string(), cfg));
    // Defaults must be intact.
    EXPECT_EQ(cfg.name,   "");
    EXPECT_EQ(cfg.host,   "127.0.0.1");
    EXPECT_EQ(cfg.port,   9999);
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// send_packet — loopback round-trip and error cases
// ---------------------------------------------------------------------------

/// Binds a UDP socket on localhost and returns the fd + chosen port.
static std::pair<int, int> bind_udp_loopback()
{
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT_GE(sock, 0);

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; // let the OS pick a free port

    EXPECT_EQ(::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);

    socklen_t len = sizeof(addr);
    ::getsockname(sock, reinterpret_cast<struct sockaddr*>(&addr), &len);
    return {sock, ntohs(addr.sin_port)};
}

TEST(SendPacket, LoopbackIpv4RoundTrip)
{
    auto [sock, port] = bind_udp_loopback();

    // Set a short receive timeout so the test doesn't hang on failure.
    struct timeval tv{2, 0};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string payload = "hello-loopback";
    ASSERT_NO_THROW(send_packet("127.0.0.1", port, payload, ""));

    char buf[256]{};
    ssize_t n = ::recv(sock, buf, sizeof(buf) - 1, 0);
    EXPECT_GT(n, 0);
    EXPECT_EQ(std::string(buf, n), payload);

    ::close(sock);
}

TEST(SendPacket, SendsFullHeartbeatPacket)
{
    auto [sock, port] = bind_udp_loopback();

    struct timeval tv{2, 0};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string pkt = build_packet("unit-test", "OK");
    ASSERT_NO_THROW(send_packet("127.0.0.1", port, pkt, ""));

    char buf[4096]{};
    ssize_t n = ::recv(sock, buf, sizeof(buf) - 1, 0);
    ASSERT_GT(n, 0);

    // The received bytes must be valid JSON with correct fields.
    json j;
    ASSERT_NO_THROW(j = json::parse(std::string(buf, n)));
    EXPECT_EQ(j["name"].get<std::string>(),   "unit-test");
    EXPECT_EQ(j["status"].get<std::string>(), "OK");

    ::close(sock);
}

TEST(SendPacket, ThrowsOnUnknownInterface)
{
    // An IPv6 link-local address with a bogus interface must throw.
    EXPECT_THROW(
        send_packet("fe80::1", 9999, "data", "no_such_iface_xyz"),
        std::runtime_error);
}

TEST(SendPacket, ThrowsOnLinkLocalWithoutInterface)
{
    // IPv6 link-local without any interface spec must throw.
    EXPECT_THROW(
        send_packet("fe80::1", 9999, "data", ""),
        std::runtime_error);
}
