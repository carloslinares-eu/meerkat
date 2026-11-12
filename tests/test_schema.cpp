/**
 * @file test_schema.cpp
 * @brief Tests for the speed.csv and connectivity.csv layouts and row builders.
 */
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "store/schema.hpp"

using namespace meerkat;
using namespace meerkat::store;

namespace {

/// 2026-11-11T20:05:00Z.
core::Timestamp t0() {
    using namespace std::chrono;
    return sys_days{year{2026} / 11 / 11} + hours{20} + minutes{5};
}

/// Field @p column of @p row, looked up by name in @p schema.
std::string field(const Schema& schema, const std::vector<std::string>& row,
                  const std::string& column) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        if (schema.columns[i] == column) {
            return row.at(i);
        }
    }
    FAIL("no column " << column);
    return {};
}

/// A wired context with documentation addresses.
core::NetworkContext wired() {
    core::NetworkContext c;
    c.link_type = core::LinkType::wired;
    c.interface = "enp0s25";
    c.gateway = "192.0.2.1";
    c.link_speed_mbps = 1000;
    return c;
}

}  // namespace

TEST_CASE("schemas are version 1 and start with the time column") {
    for (const Schema* s : {&speed_schema(), &connectivity_schema()}) {
        CHECK(s->version == 1);
        REQUIRE_FALSE(s->columns.empty());
        CHECK(s->columns.front() == "time");
        CHECK(s->name.find(' ') == std::string::npos);
    }
    CHECK(speed_schema().name == "speed");
    CHECK(connectivity_schema().name == "connectivity");
    CHECK(speed_schema().columns.size() == 25);
    CHECK(connectivity_schema().columns.size() == 16);
}

TEST_CASE("format_number is locale-free and never invents a value") {
    CHECK(format_number(std::nullopt, 3).empty());
    CHECK(format_number(std::numeric_limits<double>::quiet_NaN(), 3).empty());
    CHECK(format_number(std::numeric_limits<double>::infinity(), 3).empty());
    CHECK(format_number(0.0, 3) == "0.000");  // a measured 0 stays 0
    CHECK(format_number(187.4567, 3) == "187.457");
    CHECK(format_number(-58.0, 0) == "-58");
    CHECK(format_number(-0.0001, 3) == "0.000");
    CHECK(format_number(1000.0, 0) == "1000");
}

TEST_CASE("speed_row: a successful wired test") {
    core::Measurement<core::SpeedResult> m;
    m.time = t0();
    m.context = wired();
    auto& r = m.result;
    r.status = core::Status::ok;
    r.backend = "ookla";
    r.download_mbps = 187.5;
    r.upload_mbps = 41.25;
    r.latency_ms = 12.3456;
    r.jitter_ms = 0.8;
    r.packet_loss_percent = 0.0;
    r.server_id = 12345;
    r.server_name = "Example Server, Inc.";
    r.server_location = "Example City";
    r.isp = "Example ISP";
    r.external_ip = "203.0.113.7";
    r.result_url = "https://www.speedtest.net/result/c/00000000-0000-0000-0000-000000000000";
    r.raw = "{\"not\":\"stored here\"}";

    const auto row = speed_row(m);
    const auto& s = speed_schema();
    REQUIRE(row.size() == s.columns.size());
    CHECK(field(s, row, "time") == "2026-11-11T20:05:00Z");
    CHECK(field(s, row, "status") == "ok");
    CHECK(field(s, row, "error").empty());
    CHECK(field(s, row, "backend") == "ookla");
    CHECK(field(s, row, "link_type") == "wired");
    CHECK(field(s, row, "interface") == "enp0s25");
    CHECK(field(s, row, "gateway") == "192.0.2.1");
    CHECK(field(s, row, "link_speed_mbps") == "1000");
    CHECK(field(s, row, "wifi_ssid").empty());
    CHECK(field(s, row, "wifi_signal_dbm").empty());
    CHECK(field(s, row, "download_mbps") == "187.500");
    CHECK(field(s, row, "upload_mbps") == "41.250");
    CHECK(field(s, row, "latency_ms") == "12.346");
    CHECK(field(s, row, "jitter_ms") == "0.800");
    CHECK(field(s, row, "packet_loss_percent") == "0.000");
    CHECK(field(s, row, "server_id") == "12345");
    CHECK(field(s, row, "server_name") == "Example Server, Inc.");
    CHECK(field(s, row, "isp") == "Example ISP");
    CHECK(field(s, row, "external_ip") == "203.0.113.7");
    for (const auto& f : row) {
        CHECK(f.find("stored here") == std::string::npos);
    }
    // Commas survive the round trip through format_record as a quoted field.
    CHECK(format_record(row).find("\"Example Server, Inc.\"") != std::string::npos);
}

TEST_CASE("speed_row: a failed test over Wi-Fi leaves the values empty") {
    core::Measurement<core::SpeedResult> m;
    m.time = t0();
    m.context.link_type = core::LinkType::wifi;
    m.context.interface = "wlp4s0";
    core::WifiInfo w;
    w.ssid = "example-net";
    w.bssid = "02:00:00:00:00:01";
    w.frequency_mhz = 5180;
    w.signal_dbm = -61;
    w.rx_bitrate_mbps = 866.7;
    m.context.wifi = w;
    m.result.status = core::Status::timeout;
    m.result.error = "no result within 120 s";
    m.result.backend = "ookla";

    const auto row = speed_row(m);
    const auto& s = speed_schema();
    REQUIRE(row.size() == s.columns.size());
    CHECK(field(s, row, "status") == "timeout");
    CHECK(field(s, row, "error") == "no result within 120 s");
    CHECK(field(s, row, "link_type") == "wifi");
    CHECK(field(s, row, "gateway").empty());
    CHECK(field(s, row, "link_speed_mbps").empty());
    CHECK(field(s, row, "wifi_ssid") == "example-net");
    CHECK(field(s, row, "wifi_bssid") == "02:00:00:00:00:01");
    CHECK(field(s, row, "wifi_frequency_mhz") == "5180");
    CHECK(field(s, row, "wifi_signal_dbm") == "-61");
    CHECK(field(s, row, "wifi_rx_bitrate_mbps") == "866.7");
    CHECK(field(s, row, "wifi_tx_bitrate_mbps").empty());
    for (const char* c : {"download_mbps", "upload_mbps", "latency_ms", "jitter_ms",
                          "packet_loss_percent", "server_id", "server_name", "isp",
                          "external_ip", "result_url"}) {
        CAPTURE(c);
        CHECK(field(s, row, c).empty());
    }
}

TEST_CASE("ping_row") {
    core::Measurement<core::PingResult> m;
    m.time = t0();
    m.context = wired();
    auto& r = m.result;
    r.status = core::Status::ok;
    r.target = "192.0.2.1";
    r.sent = 10;
    r.received = 9;
    r.rtt_min_ms = 0.5;
    r.rtt_avg_ms = 0.75;
    r.rtt_max_ms = 1.25;
    r.rtt_mdev_ms = 0.125;

    const auto row = ping_row(m);
    const auto& s = connectivity_schema();
    REQUIRE(row.size() == s.columns.size());
    CHECK(field(s, row, "time") == "2026-11-11T20:05:00Z");
    CHECK(field(s, row, "probe") == "ping");
    CHECK(field(s, row, "target") == "192.0.2.1");
    CHECK(field(s, row, "status") == "ok");
    CHECK(field(s, row, "link_type") == "wired");
    CHECK(field(s, row, "sent") == "10");
    CHECK(field(s, row, "received") == "9");
    CHECK(field(s, row, "loss_percent") == "10.000");
    CHECK(field(s, row, "rtt_min_ms") == "0.500");
    CHECK(field(s, row, "rtt_mdev_ms") == "0.125");
    CHECK(field(s, row, "duration_ms").empty());

    SUBCASE("total loss: counts and loss stay, the times are empty") {
        r.received = 0;
        r.rtt_min_ms = r.rtt_avg_ms = r.rtt_max_ms = r.rtt_mdev_ms = std::nullopt;
        const auto lost = ping_row(m);
        CHECK(field(s, lost, "loss_percent") == "100.000");
        CHECK(field(s, lost, "rtt_avg_ms").empty());
    }
    SUBCASE("no network: nothing measured") {
        core::Measurement<core::PingResult> off;
        off.time = t0();
        off.result.status = core::Status::no_network;
        off.result.target = "203.0.113.1";
        const auto empty = ping_row(off);
        CHECK(field(s, empty, "status") == "no_network");
        CHECK(field(s, empty, "link_type") == "unknown");
        CHECK(field(s, empty, "sent").empty());
        CHECK(field(s, empty, "loss_percent").empty());
    }
}

TEST_CASE("dns_row") {
    core::Measurement<core::DnsResult> m;
    m.time = t0();
    m.context = wired();
    m.result.status = core::Status::ok;
    m.result.host = "example.com";
    m.result.duration_ms = 23.4;

    const auto row = dns_row(m);
    const auto& s = connectivity_schema();
    REQUIRE(row.size() == s.columns.size());
    CHECK(field(s, row, "probe") == "dns");
    CHECK(field(s, row, "target") == "example.com");
    CHECK(field(s, row, "status") == "ok");
    CHECK(field(s, row, "duration_ms") == "23.400");
    for (const char* c : {"sent", "received", "loss_percent", "rtt_min_ms", "rtt_avg_ms",
                          "rtt_max_ms", "rtt_mdev_ms"}) {
        CAPTURE(c);
        CHECK(field(s, row, c).empty());
    }

    SUBCASE("timeout") {
        m.result.status = core::Status::timeout;
        m.result.error = "no answer within 5000 ms";
        m.result.duration_ms = std::nullopt;
        const auto t = dns_row(m);
        CHECK(field(s, t, "status") == "timeout");
        CHECK(field(s, t, "duration_ms").empty());
    }
}
