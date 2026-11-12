/**
 * @file test_recorder.cpp
 * @brief Tests for app::Recorder and app::run_service with fake probes.
 */
#include <doctest/doctest.h>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "app/recorder.hpp"
#include "app/run.hpp"
#include "core/text.hpp"
#include "store/manifest.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace meerkat::app;
using core::LinkType;
using core::Status;
using meerkat::test::TempDir;
namespace fs = std::filesystem;

namespace {

/// Parses an ISO 8601 time; fails the test if invalid.
core::Timestamp at(const char* iso) {
    const auto t = core::parse_iso8601(iso);
    REQUIRE(t);
    return *t;
}

/// A controllable system: wired, every host answers, speed test ok.
struct FakeSystem {
    std::optional<net::DefaultRoute> route = net::DefaultRoute{"eth0", "192.0.2.1", 100, 4};
    LinkType type = LinkType::wired;
    bool carrier = true;
    std::map<std::string, int> received;  ///< Replies per target (default 3 of 3).
    core::SpeedResult speed;
    int speed_runs = 0;
    std::function<void()> during_speed;    ///< Runs inside the speed test.
    core::Timestamp now = at("2026-03-10T12:00:00Z");
    std::vector<std::string> pinged;

    FakeSystem() {
        speed.status = Status::ok;
        speed.backend = "ookla";
        speed.download_mbps = 200;
        speed.upload_mbps = 40;
        speed.raw = "{}";
    }

    RecorderProbes probes() {
        RecorderProbes p;
        p.sensors.route = [this] { return route; };
        p.sensors.link = [this](std::string_view i) {
            net::LinkInfo l;
            l.interface = std::string{i};
            l.exists = true;
            l.type = type;
            l.carrier = carrier;
            return l;
        };
        p.sensors.wifi = [](std::string_view) {
            return net::WifiReading{Status::not_available, "iw is not installed", std::nullopt};
        };
        p.ping = [this](std::string_view t) {
            pinged.emplace_back(t);
            core::PingResult r;
            r.status = Status::ok;
            r.target = std::string{t};
            r.sent = 3;
            const auto it = received.find(r.target);
            r.received = it == received.end() ? 3 : it->second;
            return r;
        };
        p.dns = [](std::string_view h) {
            core::DnsResult d;
            d.status = Status::ok;
            d.host = std::string{h};
            d.duration_ms = 5;
            return d;
        };
        p.speed = [this] {
            ++speed_runs;
            if (during_speed) {
                during_speed();
            }
            return speed;
        };
        p.now = [this] { return now; };
        return p;
    }

    /// All public hosts and the gateway stop answering (or start again).
    void line_down(bool down) {
        for (const auto* t : {"192.0.2.10", "192.0.2.11"}) {
            received[t] = down ? 0 : 3;
        }
    }
};

/// A config writing to @p dir with documentation-address hosts.
core::Config config_in(const fs::path& dir) {
    core::Config c;
    c.data_dir = dir;
    c.ping_hosts = {"192.0.2.10", "192.0.2.11"};
    c.connectivity_interval = std::chrono::seconds{30};
    c.outage_after = 2;
    return c;
}

/// Data lines of a CSV (without the two header lines).
std::vector<std::string> rows(const fs::path& file) {
    std::istringstream in{core::text::read_file(file).value_or("")};
    std::vector<std::string> out;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (++n > 2) {
            out.push_back(line);
        }
    }
    return out;
}

/// True if @p text contains @p part.
bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("Recorder: a connectivity round writes one row per ping and one for DNS") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain};
    Recorder r{config_in(dir.path), sys.probes(), log};
    r.connectivity_round();

    const auto lines = rows(dir.path / "connectivity.csv");
    REQUIRE(lines.size() == 4);  // gateway, two hosts, DNS
    CHECK(lines[0].starts_with("2026-03-10T12:00:00Z,ping,192.0.2.1,ok,,wired,eth0,192.0.2.1,3,3,"));
    CHECK(lines[1].starts_with("2026-03-10T12:00:00Z,ping,192.0.2.10,ok,"));
    CHECK(lines[3].starts_with("2026-03-10T12:00:00Z,dns,example.com,ok,"));
    CHECK(has(r.status_line(), "net 12:00:00 up"));
    CHECK(out.str().empty());  // routine rounds aren't logged

    SUBCASE("no gateway pinged when ping_gateway is off") {
        auto c = config_in(dir.path / "b");
        fs::create_directories(c.data_dir);
        c.ping_gateway = false;
        Recorder r2{c, sys.probes(), log};
        sys.pinged.clear();
        r2.connectivity_round();
        CHECK(sys.pinged == std::vector<std::string>{"192.0.2.10", "192.0.2.11"});
    }
}

TEST_CASE("Recorder: outages are logged when they start and end") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }};
    Recorder r{config_in(dir.path), sys.probes(), log};

    sys.line_down(true);
    r.connectivity_round();
    CHECK_FALSE(has(out.str(), "outage"));  // one round isn't an outage yet
    sys.now += std::chrono::seconds{30};
    r.connectivity_round();
    CHECK(has(out.str(), "warning: outage since 2026-03-10T12:00:00Z (the line: the router "
                         "answers, the internet doesn't): 192.0.2.10: no reply"));
    CHECK(has(r.status_line(), "OUTAGE since 12:00:00"));

    SUBCASE("the line comes back") {
        sys.line_down(false);
        sys.now += std::chrono::seconds{30};
        r.connectivity_round();
        CHECK(has(out.str(), "notice: outage from 2026-03-10T12:00:00Z to 2026-03-10T12:01:00Z "
                             "(1m, 2 failed rounds, internet): the connection is back"));
        CHECK(has(r.status_line(), "no outage"));
    }
    SUBCASE("meerkat stops during the outage") {
        r.finish();
        CHECK(has(out.str(), "to 2026-03-10T12:00:30Z (30s, 2 failed rounds, internet): end "
                             "unknown: meerkat is stopping"));
    }
}

TEST_CASE("Recorder: failed rounds during a speed test don't make an outage") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain};
    Recorder r{config_in(dir.path), sys.probes(), log};
    sys.line_down(true);
    sys.during_speed = [&] {
        for (int i = 0; i < 3; ++i) {
            r.connectivity_round();  // the full line drops every ping
            sys.now += std::chrono::seconds{30};
        }
    };
    r.speed_round();
    CHECK_FALSE(has(out.str(), "outage"));
    CHECK(rows(dir.path / "connectivity.csv").size() == 12);  // still recorded
    CHECK(has(r.status_line(), "unknown"));
}

TEST_CASE("Recorder: speed rounds") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain};
    auto c = config_in(dir.path);

    SUBCASE("a cable: test, row, raw log") {
        Recorder r{c, sys.probes(), log};
        r.speed_round();
        CHECK(sys.speed_runs == 1);
        const auto lines = rows(dir.path / "speed.csv");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].starts_with("2026-03-10T12:00:00Z,ok,,ookla,wired,eth0,192.0.2.1,"));
        CHECK(fs::exists(dir.path / "raw" / "2026-03-10.jsonl"));
        CHECK(has(out.str(), "info: speed test: 200.0 down / 40.0 up Mbit/s"));
        CHECK(has(r.status_line(), "speed 12:00:00 200.0/40.0 Mbit/s"));
    }
    SUBCASE("Wi-Fi with require_wired: a skipped_wlan row, no test") {
        c.require_wired = true;
        sys.type = LinkType::wifi;
        Recorder r{c, sys.probes(), log};
        r.startup();
        CHECK(has(out.str(), "warning: the connection is Wi-Fi (eth0)"));
        out.str("");
        r.speed_round();
        CHECK(sys.speed_runs == 0);
        CHECK(rows(dir.path / "speed.csv")[0].starts_with("2026-03-10T12:00:00Z,skipped_wlan,"));
        CHECK_FALSE(fs::exists(dir.path / "raw" / "2026-03-10.jsonl"));
        CHECK_FALSE(has(out.str(), "Wi-Fi (eth0)"));  // warned at start-up already
    }
    SUBCASE("offline: a no_network row") {
        sys.route.reset();
        Recorder r{c, sys.probes(), log};
        r.speed_round();
        CHECK(sys.speed_runs == 0);
        CHECK(rows(dir.path / "speed.csv")[0].starts_with("2026-03-10T12:00:00Z,no_network,"
                                                          "not started: no default route,"));
    }
    SUBCASE("a failed test is a warning") {
        sys.speed.status = Status::timeout;
        sys.speed.error = "no result in time";
        Recorder r{c, sys.probes(), log};
        r.speed_round();
        CHECK(has(out.str(), "warning: speed test failed: timeout: no result in time"));
    }
}

TEST_CASE("Recorder: the previous day is sealed only if meerkat recorded it") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }};
    const auto manifest = dir.path / "manifest" / "2026-03-09.sha256";

    SUBCASE("first start today: nothing to seal") {
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.startup();
        CHECK_FALSE(fs::exists(manifest));
    }
    SUBCASE("yesterday has a raw log: sealed at start-up") {
        fs::create_directories(dir.path / "raw");
        std::ofstream{dir.path / "raw" / "2026-03-09.jsonl"} << "{}\n";
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.startup();
        CHECK(fs::exists(manifest));
        CHECK(has(out.str(), "notice: sealed 2026-03-09"));
        out.str("");
        r.seal_previous_day();  // exists already: quietly done
        CHECK(out.str().empty());
    }
    SUBCASE("running across midnight: the scheduled seal writes it") {
        sys.now = at("2026-03-09T23:00:00Z");
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.connectivity_round();
        sys.now = at("2026-03-10T00:10:00Z");
        r.seal_previous_day();
        CHECK(fs::exists(manifest));
    }
}

TEST_CASE("run_service: starts, holds the sleep lock, stops cleanly") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain};
    core::SystemClock clock;
    auto c = config_in(dir.path / "data");

    int lock_calls = 0;
    RunEnv env;
    env.probes = sys.probes();
    env.clock = &clock;
    env.handle_signals = false;
    env.inhibit = [&lock_calls](const sys::InhibitRequest&) {
        ++lock_calls;
        sys::InhibitReply r;
        r.fd = ::dup(2);
        return r;
    };
    env.on_started = [](core::Scheduler& s) { s.stop(); };

    CHECK(run_service(c, env, log) == kRunStopped);
    CHECK(lock_calls == 1);
    CHECK(fs::is_directory(c.data_dir));
    struct stat st {};
    REQUIRE(::stat(c.data_dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0750);
    CHECK(fs::exists(c.data_dir / "speed.csv"));
    const auto text = out.str();
    CHECK(has(text, "recording to " + c.data_dir.string()));
    CHECK(has(text, "no contract figures"));
    CHECK(has(text, "holding the logind lock"));
    CHECK(has(text, "notice: stopped"));

    SUBCASE("a refused lock is a warning, not a failure") {
        out.str("");
        env.inhibit = [](const sys::InhibitRequest&) {
            return sys::InhibitReply{-1, sys::InhibitError::Kind::denied,
                                     "org.freedesktop.DBus.Error.AccessDenied", "denied"};
        };
        CHECK(run_service(c, env, log) == kRunStopped);
        CHECK(has(out.str(), "warning: the computer may suspend and miss measurements"));
    }
    SUBCASE("a CSV with another layout stops it before recording") {
        std::ofstream{c.data_dir / "speed.csv", std::ios::trunc} << "# meerkat other v1\r\na\r\n";
        out.str("");
        CHECK(run_service(c, env, log) == kRunFailed);
        CHECK(has(out.str(), "error: can't open the data files"));
    }
    SUBCASE("a data directory that can't be created") {
        std::ofstream{dir.path / "file"} << "x";
        c.data_dir = dir.path / "file" / "data";
        CHECK(run_service(c, env, log) == kRunFailed);
        CHECK(has(out.str(), "can't create the data directory"));
    }
}
