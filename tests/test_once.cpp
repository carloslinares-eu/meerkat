/**
 * @file test_once.cpp
 * @brief Tests for `meerkat once` with fake probes (no network needed).
 */
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/cli.hpp"
#include "app/official.hpp"
#include "app/once.hpp"
#include "core/config.hpp"
#include "core/time.hpp"
#include "store/pause_file.hpp"
#include "support.hpp"

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
    p.sensors.route = [] {
        return std::optional<meerkat::net::DefaultRoute>{{.interface = "eth0", .gateway = "192.0.2.1"}};
    };
    p.sensors.link = [](std::string_view i) {
        meerkat::net::LinkInfo l;
        l.interface = std::string{i};
        l.exists = true;
        l.type = LinkType::wired;
        l.carrier = true;
        l.speed_mbps = 1000;
        return l;
    };
    p.sensors.wifi = [](std::string_view) {
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
    p.speed = [&speed_runs](const meerkat::sys::StopCheck&) {
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
    CHECK(r.dns.host == "example.com");
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
    probes.sensors.link = [](std::string_view) {
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
    CHECK(speed_runs == 1);  // only a warning unless require_wired is set

    std::ostringstream out;
    print_report(r, out);
    CHECK(out.str().find("ExampleNet, signal -55 dBm, 5180 MHz") != std::string::npos);
    CHECK(out.str().find("Warning: the connection is Wi-Fi") != std::string::npos);

    SUBCASE("an SSID with control characters is printed visibly (#60)") {
        auto odd = r;
        odd.context.wifi->ssid = "Example\x1b[2JNet\n";
        std::ostringstream o2;
        print_report(odd, o2);
        CHECK(o2.str().find("Example\\x1b[2JNet\\x0a, signal") != std::string::npos);
        CHECK(o2.str().find('\x1b') == std::string::npos);
    }
}

TEST_CASE("once: Wi-Fi details unavailable are reported, not hidden") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.sensors.link = [](std::string_view) {
        meerkat::net::LinkInfo l;
        l.exists = true;
        l.type = LinkType::wifi;
        return l;
    };
    probes.sensors.wifi = [](std::string_view) {
        meerkat::net::WifiReading w;
        w.status = Status::not_available;
        w.error = "iw is not installed";
        return w;
    };
    const auto r = measure_once({}, probes);
    CHECK(r.wifi_error == "not_available: iw is not installed");
}

TEST_CASE("once: no carrier is offline: warning, no speed test") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.sensors.link = [](std::string_view) {
        meerkat::net::LinkInfo l;
        l.exists = true;
        l.type = LinkType::wired;
        l.carrier = false;
        return l;
    };
    const auto r = measure_once({}, probes);
    CHECK_FALSE(r.link_up);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].starts_with("no carrier on eth0"));
    CHECK(speed_runs == 0);
    REQUIRE(r.speed.has_value());
    CHECK(r.speed->status == Status::no_network);
    CHECK(r.speed->error == "not started: no carrier");
}

TEST_CASE("once: interface missing from sysfs gets its own warning") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.sensors.link = [](std::string_view) { return meerkat::net::LinkInfo{}; };
    const auto r = measure_once({}, probes);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("isn't in /sys/class/net") != std::string::npos);
}

TEST_CASE("once: report keeps the stream's flags and handles a missing SSID") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.sensors.link = [](std::string_view) {
        meerkat::net::LinkInfo l;
        l.exists = true;
        l.type = LinkType::wifi;
        return l;
    };
    probes.sensors.wifi = [](std::string_view) {
        meerkat::net::WifiReading w;
        w.status = Status::ok;
        w.info = meerkat::core::WifiInfo{};
        return w;
    };
    std::ostringstream out;
    const auto flags = out.flags();
    print_report(measure_once({}, probes), out);
    CHECK(out.flags() == flags);
    CHECK(out.str().find("Wi-Fi         –, signal –, –, rx –, tx –") != std::string::npos);
}

TEST_CASE("once: unknown link type warns") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.sensors.link = [](std::string_view) {
        meerkat::net::LinkInfo l;
        l.exists = true;  // e.g. a bridge
        return l;
    };
    const auto r = measure_once({}, probes);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("can't tell whether eth0 is a cable") != std::string::npos);
}

TEST_CASE("once: offline still pings and resolves, but doesn't start a speed test") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.sensors.route = [] { return std::optional<meerkat::net::DefaultRoute>{}; };
    const auto r = measure_once({}, probes);
    CHECK_FALSE(r.has_route);
    CHECK(pinged == std::vector<std::string>{"1.1.1.1", "9.9.9.9"});  // no gateway
    CHECK(r.dns.host == "example.com");
    CHECK(speed_runs == 0);
    REQUIRE(r.speed.has_value());
    CHECK(r.speed->status == Status::no_network);

    std::ostringstream out;
    print_report(r, out);
    CHECK(out.str().find("none (offline)") != std::string::npos);
    CHECK(out.str().find("no_network: not started: no default route") != std::string::npos);
}

TEST_CASE("once: require_wired skips the speed test over Wi-Fi, not over a cable") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    OnceOptions options;
    options.require_wired = true;
    auto probes = fake_probes(pinged, speed_runs);
    auto r = measure_once(options, probes);  // wired
    CHECK(speed_runs == 1);
    REQUIRE(r.speed.has_value());
    CHECK(r.speed->status == Status::ok);

    probes.sensors.link = [](std::string_view) {
        meerkat::net::LinkInfo l;
        l.exists = true;
        l.type = LinkType::wifi;
        l.carrier = true;
        return l;
    };
    r = measure_once(options, probes);
    CHECK(speed_runs == 1);  // not run again
    REQUIRE(r.speed.has_value());
    CHECK(r.speed->status == Status::skipped_wlan);
    CHECK(r.speed->error.find("require_wired") != std::string::npos);
    CHECK_FALSE(r.speed->download_mbps.has_value());
    CHECK(pinged.size() == 6);  // the connectivity probes still ran
}

TEST_CASE("once: ping_gateway off pings only the public hosts") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    OnceOptions options;
    options.speed = false;
    options.ping_gateway = false;
    measure_once(options, fake_probes(pinged, speed_runs));
    CHECK(pinged == std::vector<std::string>{"1.1.1.1", "9.9.9.9"});
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
    CHECK(r.dns.host == "example.net");
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
    probes.speed = [](const meerkat::sys::StopCheck&) {
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

TEST_CASE("cli: --server-id and --no-speed exclude each other") {
    const std::array<const char*, 5> args{"meerkat", "once", "--no-speed", "--server-id", "12345"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) != 0);
    CHECK(err.str().find("excludes") != std::string::npos);
}

namespace {

/// The official app's main process as /proc shows it.
const meerkat::sys::ProcessEntry kApp{4242, "breitbandmessung", "breitbandmessun"};

/// 2026-03-10T12:00:00Z.
meerkat::core::Timestamp noon() {
    return meerkat::core::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 10} +
                                    std::chrono::hours{12}};
}

/// A collected progress output.
struct Progress {
    std::vector<std::string> lines;  ///< Every line, in order.

    /// @return The callback for measure_once().
    std::function<void(std::string_view)> sink() {
        return [this](std::string_view l) { lines.emplace_back(l); };
    }

    /// @return True if a line contains @p text.
    [[nodiscard]] bool has(std::string_view text) const {
        return std::ranges::any_of(lines, [&](const std::string& l) {
            return l.find(text) != std::string::npos;
        });
    }
};

}  // namespace

TEST_CASE("once: no speed test while the Breitbandmessung app runs (#66)") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.processes = [] { return std::vector{kApp}; };
    Progress progress;
    const auto r = measure_once(OnceOptions{}, probes, progress.sink());
    CHECK(speed_runs == 0);
    REQUIRE(r.speed.has_value());
    CHECK(r.speed->status == Status::paused_official);
    CHECK(r.speed->error ==
          "not started: the Breitbandmessung app is running (breitbandmessung)");
    CHECK(progress.has("speed tests paused: the Breitbandmessung app is running"));
    CHECK(pinged.size() == 3);  // connectivity is still measured

    std::ostringstream out;
    print_report(r, out);
    CHECK(out.str().find("paused_official") != std::string::npos);

    SUBCASE("detection off: an empty program list") {
        OnceOptions o;
        o.official_programs.clear();
        measure_once(o, probes);
        CHECK(speed_runs == 1);
    }
}

TEST_CASE("once: a pause set with meerkat pause is honoured (#66)") {
    meerkat::test::TempDir dir;
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    probes.now = noon;
    OnceOptions o;
    o.data_dir = dir.path;
    meerkat::store::write_pause(dir.path, noon() + std::chrono::minutes{30});
    auto r = measure_once(o, probes);
    CHECK(speed_runs == 0);
    REQUIRE(r.speed.has_value());
    CHECK(r.speed->status == Status::paused_official);
    CHECK(r.speed->error.starts_with("not started: "));

    SUBCASE("a pause that has ended doesn't count") {
        meerkat::store::write_pause(dir.path, noon() - std::chrono::minutes{1});
        r = measure_once(o, probes);
        CHECK(speed_runs == 1);
    }
    SUBCASE("an unreadable pause file is a warning, and ignored") {
        std::ofstream{dir.path / std::string{meerkat::store::kPauseFile}} << "not a time\n";
        Progress progress;
        r = measure_once(o, probes, progress.sink());
        CHECK(speed_runs == 1);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].starts_with("the pause file is ignored: "));
        CHECK(progress.has("Warning: the pause file is ignored: "));
    }
    SUBCASE("a data directory that doesn't exist: no pause") {
        o.data_dir = dir.path / "missing";
        r = measure_once(o, probes);
        CHECK(speed_runs == 1);
        CHECK(r.warnings.empty());
    }
}

TEST_CASE("once: the speed test is stopped when the app starts (#66)") {
    std::vector<std::string> pinged;
    int speed_runs = 0;
    auto probes = fake_probes(pinged, speed_runs);
    auto clock = noon();
    probes.now = [&clock] { return clock; };
    std::vector<meerkat::sys::ProcessEntry> running;
    probes.processes = [&running] { return running; };
    auto finished = Status::error;  // what a stopped Ookla run reports
    bool stopped_by_check = true;
    std::vector<bool> answers;
    probes.speed = [&](const meerkat::sys::StopCheck& stop) {
        ++speed_runs;
        answers.push_back(stop());  // at once: not asked
        running = {kApp};
        clock += std::chrono::seconds{1};
        answers.push_back(stop());  // within kOfficialPoll: not asked
        clock += meerkat::app::kOfficialPoll;
        answers.push_back(stop());  // asked: the app is there
        meerkat::core::SpeedResult r;
        r.status = finished;
        r.backend = "ookla";
        if (finished == Status::ok) {
            r.download_mbps = 200.0;
            r.upload_mbps = 40.0;
        } else {
            r.download_mbps = 12.0;  // a partial figure: no evidence
            r.error = "speedtest was stopped before it finished";
            r.stopped = stopped_by_check;
        }
        return r;
    };
    Progress progress;

    SUBCASE("stopped before a result: paused_official, no figures") {
        const auto r = measure_once(OnceOptions{}, probes, progress.sink());
        CHECK(answers == std::vector<bool>{false, false, true});
        REQUIRE(r.speed.has_value());
        CHECK(r.speed->status == Status::paused_official);
        CHECK(r.speed->error ==
              "stopped: the Breitbandmessung app is running (breitbandmessung)");
        CHECK(r.speed->backend == "ookla");
        CHECK_FALSE(r.speed->download_mbps.has_value());
        CHECK(progress.has("speed tests paused: the Breitbandmessung app is running"));
        CHECK(progress.has("Speed test stopped."));
    }
    SUBCASE("failed by itself while the app started: the real failure is kept") {
        stopped_by_check = false;
        const auto r = measure_once(OnceOptions{}, probes, progress.sink());
        REQUIRE(r.speed.has_value());
        CHECK(r.speed->status == Status::error);
        CHECK(r.speed->error == "speedtest was stopped before it finished");
    }
    SUBCASE("finished anyway: the result is kept") {
        finished = Status::ok;
        const auto r = measure_once(OnceOptions{}, probes, progress.sink());
        REQUIRE(r.speed.has_value());
        CHECK(r.speed->status == Status::ok);
        CHECK(r.speed->download_mbps == 200.0);
        CHECK_FALSE(progress.has("stopped"));
    }
}

TEST_CASE("once: options_from carries the official settings") {
    meerkat::core::Config c;
    c.data_dir = "/var/lib/meerkat";
    c.official_programs = {"breitbandmessung", "other-app"};
    const auto o = meerkat::app::options_from(c);
    CHECK(o.data_dir == c.data_dir);
    CHECK(o.official_programs == c.official_programs);
}
