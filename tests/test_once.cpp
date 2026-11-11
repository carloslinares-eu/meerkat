/**
 * @file test_once.cpp
 * @brief Tests for `meerkat once` with fake probes (no network needed).
 */
#include <doctest/doctest.h>

#include <array>
#include <sstream>
#include <string>
#include <vector>

#include "app/cli.hpp"
#include "app/once.hpp"

using meerkat::app::measure_once;
using meerkat::app::OnceOptions;
using meerkat::app::print_report;
using meerkat::app::Probes;
using meerkat::core::LinkType;
using meerkat::core::Status;

namespace {

/// Probes for a wired connection where everything works; tests change parts.
Probes fake_probes(std::vector<std::string>& pinged, int& speed_runs) {
    Probes p;
    p.route = [] {
        return std::optional<meerkat::net::DefaultRoute>{{.interface = "eth0", .gateway = "192.0.2.1"}};
    };
    p.link = [](std::string_view i) {
        meerkat::net::LinkInfo l;
        l.interface = std::string{i};
        l.exists = true;
        l.type = LinkType::wired;
        l.carrier = true;
        l.speed_mbps = 1000;
        return l;
    };
    p.wifi = [](std::string_view) {
        meerkat::net::WifiReading w;
        w.status = Status::ok;
        meerkat::core::WifiInfo info;
        info.ssid = "ExampleNet";
        info.signal_dbm = -55;
        info.frequency_mhz = 5180;
        w.info = info;
        return w;
    };
    p.ping = [&pinged](std::string_view t) {
        pinged.emplace_back(t);
        meerkat::core::PingResult r;
        r.status = Status::ok;
        r.target = std::string{t};
        r.sent = 5;
        r.received = 4;
        r.rtt_min_ms = 1.25;
        r.rtt_avg_ms = 2.5;
        r.rtt_max_ms = 3.75;
        r.rtt_mdev_ms = 0.5;
        return r;
    };
    p.dns = [](std::string_view h) {
        meerkat::core::DnsResult r;
        r.status = Status::ok;
        r.host = std::string{h};
        r.duration_ms = 12.5;
        return r;
    };
    p.speed = [&speed_runs] {
        ++speed_runs;
        meerkat::core::SpeedResult r;
        r.status = Status::ok;
        r.backend = "ookla";
        r.download_mbps = 200.0;
        r.upload_mbps = 40.0;
        r.server_id = 12345;
        r.server_name = "Example Server";
        r.isp = "Example ISP";
        return r;
    };
    return p;
}

}  // namespace

TEST_CASE("once: wired connection, everything measured, no warnings") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    std::vector<std::string> progress;
    const auto r = measure_once({}, fake_probes(pinged, speed_runs),
                                [&](std::string_view l) { progress.emplace_back(l); });
    CHECK(r.has_route);
    CHECK(r.context.interface == "eth0");
    CHECK(r.context.link_type == LinkType::wired);
    CHECK(r.context.link_speed_mbps == 1000);
    CHECK(pinged == std::vector<std::string>{"192.0.2.1", "1.1.1.1", "9.9.9.9"});
    REQUIRE(r.pings.size() == 3);
    CHECK(r.pings[0].label == "gateway");
    CHECK(r.dns->host == "example.com");
    CHECK(speed_runs == 1);
    CHECK(r.warnings.empty());
    CHECK_FALSE(progress.empty());

    std::ostringstream out;
    print_report(r, out);
    const auto text = out.str();
    CHECK(text.find("eth0 (wired, link 1000 Mbit/s)") != std::string::npos);
    CHECK(text.find("loss 20.0 %, rtt min/avg/max 1.2 / 2.5 / 3.8 ms, jitter 0.5 ms") != std::string::npos);
    CHECK(text.find("example.com  12.5 ms") != std::string::npos);
    CHECK(text.find("Download      200.00 Mbit/s") != std::string::npos);
    CHECK(text.find("Example Server (id 12345)") != std::string::npos);
    CHECK(text.find("Latency       – (jitter –)") != std::string::npos);  // missing shows –, never 0
    CHECK(text.find("Warning") == std::string::npos);
}

TEST_CASE("once: Wi-Fi gives a warning right away and in the report") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.link = [](std::string_view) {
        meerkat::net::LinkInfo l;
        l.exists = true;
        l.type = LinkType::wifi;
        return l;
    };
    std::vector<std::string> progress;
    const auto r = measure_once({}, probes, [&](std::string_view l) { progress.emplace_back(l); });
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("Wi-Fi") != std::string::npos);
    CHECK(progress.front().starts_with("Warning: the connection is Wi-Fi"));
    REQUIRE(r.context.wifi.has_value());
    CHECK(r.context.wifi->ssid == "ExampleNet");
    CHECK(speed_runs == 1);  // only a warning; require_wired comes with task 2.7

    std::ostringstream out;
    print_report(r, out);
    CHECK(out.str().find("ExampleNet, signal -55 dBm, 5180 MHz") != std::string::npos);
    CHECK(out.str().find("Warning: the connection is Wi-Fi") != std::string::npos);
}

TEST_CASE("once: Wi-Fi details unavailable are reported, not hidden") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.link = [](std::string_view) {
        meerkat::net::LinkInfo l;
        l.type = LinkType::wifi;
        return l;
    };
    probes.wifi = [](std::string_view) {
        meerkat::net::WifiReading w;
        w.status = Status::not_available;
        w.error = "iw is not installed";
        return w;
    };
    const auto r = measure_once({}, probes);
    CHECK(r.wifi_error == "not_available: iw is not installed");
}

TEST_CASE("once: unknown link type warns") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.link = [](std::string_view) { return meerkat::net::LinkInfo{}; };
    const auto r = measure_once({}, probes);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("can't tell whether eth0 is a cable") != std::string::npos);
}

TEST_CASE("once: offline still pings and resolves, but doesn't start a speed test") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.route = [] { return std::optional<meerkat::net::DefaultRoute>{}; };
    const auto r = measure_once({}, probes);
    CHECK_FALSE(r.has_route);
    CHECK(pinged == std::vector<std::string>{"1.1.1.1", "9.9.9.9"});  // no gateway
    CHECK(r.dns.has_value());
    CHECK(speed_runs == 0);
    REQUIRE(r.speed.has_value());
    CHECK(r.speed->status == Status::no_network);

    std::ostringstream out;
    print_report(r, out);
    CHECK(out.str().find("none (offline)") != std::string::npos);
    CHECK(out.str().find("no_network: not started: no default route") != std::string::npos);
}

TEST_CASE("once: options") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    OnceOptions options;
    options.speed = false;
    options.ping_hosts = {"198.51.100.7"};
    options.dns_host = "example.net";
    const auto r = measure_once(options, fake_probes(pinged, speed_runs));
    CHECK(pinged == std::vector<std::string>{"192.0.2.1", "198.51.100.7"});
    CHECK(r.dns->host == "example.net");
    CHECK(speed_runs == 0);
    CHECK_FALSE(r.speed.has_value());
}

TEST_CASE("once: failures print their status and reason") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.ping = [](std::string_view t) {
        meerkat::core::PingResult r;
        r.status = Status::timeout;
        r.target = std::string{t};
        r.error = "ping did not finish in time";
        return r;
    };
    probes.speed = [] {
        meerkat::core::SpeedResult r;
        r.status = Status::not_available;
        r.backend = "ookla";
        r.error = "the Ookla speedtest CLI is not installed";
        return r;
    };
    std::ostringstream out;
    print_report(measure_once({}, probes), out);
    const auto text = out.str();
    CHECK(text.find("192.0.2.1  timeout: ping did not finish in time") != std::string::npos);
    CHECK(text.find("not_available: the Ookla speedtest CLI is not installed") != std::string::npos);
    CHECK(text.find("Download") == std::string::npos);  // nothing measured, nothing shown
}

TEST_CASE("cli: once rejects a bad server id") {
    const std::array<const char*, 4> args{"meerkat", "once", "--server-id", "0"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) != 0);
    CHECK_FALSE(err.str().empty());
}
