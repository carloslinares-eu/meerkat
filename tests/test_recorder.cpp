/**
 * @file test_recorder.cpp
 * @brief Tests for app::Recorder and app::run_service with fake probes.
 */
#include <doctest/doctest.h>

#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "app/recorder.hpp"
#include "app/run.hpp"
#include "core/text.hpp"
#include "store/dir_lock.hpp"
#include "store/manifest.hpp"
#include "store/pause_file.hpp"
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
    core::DnsResult dns{.status = Status::ok, .error = {}, .host = {}, .duration_ms = 5};  ///< What every lookup returns.
    core::SpeedResult speed;
    int speed_runs = 0;
    std::function<void()> during_speed;    ///< Runs inside the speed test.
    std::function<bool()> stop_asked;      ///< Set during a test: asks its StopCheck.
    core::Timestamp now = at("2026-03-10T12:00:00Z");
    std::vector<std::string> pinged;
    std::vector<sys::ProcessEntry> processes;  ///< What runs besides meerkat.

    FakeSystem() {
        speed.status = Status::ok;
        speed.backend = "ookla";
        speed.download_mbps = 200;
        speed.upload_mbps = 40;
        speed.raw = "{}";
    }

    Probes probes() {
        Probes p;
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
        p.dns = [this](std::string_view h) {
            core::DnsResult d = dns;
            d.host = std::string{h};
            return d;
        };
        p.speed = [this](const sys::StopCheck& stop) {
            ++speed_runs;
            stop_asked = [&stop] { return stop(); };
            if (during_speed) {
                during_speed();
            }
            stop_asked = nullptr;
            return speed;
        };
        p.now = [this] { return now; };
        p.processes = [this] { return processes; };
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

TEST_CASE("Recorder: one recorder per data directory, for its whole life (#37)") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, core::now, {}, {}};
    {
        Recorder first{config_in(dir.path), sys.probes(), log};
        try {
            Recorder second{config_in(dir.path), sys.probes(), log};
            FAIL("a second recorder started");
        } catch (const DataDirBusy& e) {
            CHECK(std::string{e.what()} ==
                  "another meerkat is already recording into " + dir.path.string());
        }
        // The lock doesn't depend on the CSV writers: even with both data
        // files gone (as after a write error closed them), it holds.
        fs::remove(dir.path / "speed.csv");
        fs::remove(dir.path / "connectivity.csv");
        CHECK_THROWS_AS(Recorder(config_in(dir.path), sys.probes(), log), DataDirBusy);
    }
    CHECK_NOTHROW(Recorder(config_in(dir.path), sys.probes(), log));  // released at the end
    CHECK_FALSE(has(out.str(), "can't lock the data directory"));  // flock() works here
}

TEST_CASE("Recorder: a connectivity round writes one row per ping and one for DNS") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, core::now, {}, {}};  // UTC only: no host zone
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

TEST_CASE("Recorder: failures while offline are no_network, with a link they stay error") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, core::now, {}, {}};
    sys.dns = {.status = Status::error, .error = "Temporary failure in name resolution",
               .host = {}, .duration_ms = std::nullopt};

    SUBCASE("cable pulled: no route") {
        sys.route = std::nullopt;
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.connectivity_round();
        const auto lines = rows(dir.path / "connectivity.csv");
        REQUIRE(lines.size() == 3);  // no gateway to ping
        CHECK(lines[2].starts_with("2026-03-10T12:00:00Z,dns,example.com,no_network,"
                                   "Temporary failure in name resolution,"));
    }
    SUBCASE("router up, answering SERVFAIL") {
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.connectivity_round();
        const auto lines = rows(dir.path / "connectivity.csv");
        REQUIRE(lines.size() == 4);
        CHECK(lines[3].starts_with("2026-03-10T12:00:00Z,dns,example.com,error,"));
    }
}

TEST_CASE("Recorder: outages are logged when they start and end") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }, {}, {}};
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
    Log log{out, LogStyle::plain, core::now, {}, {}};  // UTC only: no host zone
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
    CHECK(has(r.status_line(), "busy (speed test)"));
}

TEST_CASE("Recorder: speed rounds") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, core::now, {}, {}};  // UTC only: no host zone
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

TEST_CASE("Recorder: speed tests pause while the Breitbandmessung app runs (2.10)") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }, {}, {}};
    const sys::ProcessEntry app{4242, "breitbandmessung", "breitbandmessun"};
    Recorder r{config_in(dir.path), sys.probes(), log};

    sys.processes = {{1, "systemd", "systemd"}, app};
    r.speed_round();
    CHECK(sys.speed_runs == 0);
    auto lines = rows(dir.path / "speed.csv");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].starts_with("2026-03-10T12:00:00Z,paused_official,not started: the "
                               "Breitbandmessung app is running (breitbandmessung),"));
    CHECK(has(out.str(), "notice: speed tests paused: the Breitbandmessung app is running"));
    CHECK(has(r.status_line(), "speed 12:00:00 paused_official"));

    SUBCASE("only a helper process with a rewritten cmdline is left: still paused") {
        sys.processes = {{4243, "", "breitbandmessun"}};
        sys.now += std::chrono::minutes{5};
        r.speed_round();
        CHECK(sys.speed_runs == 0);
    }
    SUBCASE("the app stops: tests resume, said once") {
        sys.processes.clear();
        sys.now += std::chrono::minutes{5};
        r.speed_round();
        CHECK(sys.speed_runs == 1);
        CHECK(has(out.str(), "notice: the Breitbandmessung app has stopped; speed tests resume"));
        out.str("");
        sys.now += std::chrono::minutes{5};
        r.speed_round();
        CHECK_FALSE(has(out.str(), "resume"));
    }
    SUBCASE("detection off: an empty program list") {
        auto c = config_in(dir.path / "b");
        fs::create_directories(c.data_dir);
        c.official_programs.clear();
        Recorder r2{c, sys.probes(), log};
        r2.speed_round();
        CHECK(sys.speed_runs == 1);
    }
}

TEST_CASE("Recorder: a running test is stopped when the app starts (#62)") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }, {}, {}};
    Recorder r{config_in(dir.path), sys.probes(), log};
    std::vector<bool> answers;
    sys.during_speed = [&] {
        answers.push_back(sys.stop_asked());  // at once: nothing new
        sys.now += std::chrono::seconds{1};
        answers.push_back(sys.stop_asked());  // within kOfficialPoll: not checked again
        sys.processes = {{4242, "breitbandmessung", "breitbandmessun"}};
        answers.push_back(sys.stop_asked());  // still within kOfficialPoll
        sys.now += std::chrono::seconds{2};
        answers.push_back(sys.stop_asked());  // checked: the app is there
    };
    sys.speed.status = Status::error;  // what a stopped Ookla run reports
    sys.speed.error = "speedtest was stopped before it finished";
    sys.speed.stopped = true;  // the backend stopped it for the check
    sys.speed.download_mbps.reset();
    sys.speed.upload_mbps.reset();
    sys.speed.raw = "{\"partial\":true}";
    r.speed_round();
    CHECK(answers == std::vector<bool>{false, false, false, true});
    const auto lines = rows(dir.path / "speed.csv");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].starts_with("2026-03-10T12:00:00Z,paused_official,stopped: the "
                               "Breitbandmessung app is running (breitbandmessung),"));
    CHECK_FALSE(fs::exists(dir.path / "raw" / "2026-03-10.jsonl"));  // not evidence
    CHECK(has(out.str(), "notice: speed tests paused: the Breitbandmessung app is running"));
    CHECK(has(out.str(), "info: speed test stopped"));

    SUBCASE("no app: the test runs to its end") {
        sys.processes.clear();
        sys.during_speed = [&] {
            for (int i = 0; i < 5; ++i) {
                sys.now += std::chrono::seconds{3};
                CHECK_FALSE(sys.stop_asked());
            }
        };
        sys.speed.status = Status::ok;
        sys.speed.error.clear();
        sys.speed.download_mbps = 200;
        sys.speed.upload_mbps = 40;
        sys.speed.raw = "{}";
        r.speed_round();
        CHECK(rows(dir.path / "speed.csv")[1].find(",ok,") != std::string::npos);
    }
    SUBCASE("the test finished anyway: its result is kept") {
        sys.speed.status = Status::ok;
        sys.speed.error.clear();
        sys.speed.download_mbps = 200;
        sys.speed.upload_mbps = 40;
        sys.speed.raw = "{}";
        sys.processes.clear();
        sys.now += std::chrono::minutes{5};
        sys.during_speed = [&] {
            sys.processes = {{4242, "breitbandmessung", "breitbandmessun"}};
            sys.now += std::chrono::seconds{3};
            CHECK(sys.stop_asked());
        };
        r.speed_round();
        CHECK(rows(dir.path / "speed.csv")[1].find(",ok,") != std::string::npos);
        CHECK(fs::exists(dir.path / "raw" / "2026-03-10.jsonl"));
    }
    SUBCASE("failed by itself as the app started: the real failure is kept") {
        sys.speed.stopped = false;  // exited on its own, e.g. the line dropped
        sys.speed.error = "speedtest exited with status 2";
        sys.processes.clear();
        sys.now += std::chrono::minutes{5};
        sys.during_speed = [&] {
            sys.processes = {{4242, "breitbandmessung", "breitbandmessun"}};
            sys.now += std::chrono::seconds{3};
            CHECK(sys.stop_asked());
        };
        r.speed_round();
        CHECK(rows(dir.path / "speed.csv")[1].find(",error,speedtest exited with status 2") !=
              std::string::npos);
    }
}

TEST_CASE("Recorder: `meerkat pause` pauses speed tests until its time (2.10)") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }, {}, {}};
    Recorder r{config_in(dir.path), sys.probes(), log};

    store::write_pause(dir.path, at("2026-03-10T12:30:00Z"));
    r.speed_round();
    CHECK(sys.speed_runs == 0);
    CHECK(rows(dir.path / "speed.csv")[0].starts_with(
        "2026-03-10T12:00:00Z,paused_official,not started: paused with `meerkat pause` until "
        "2026-03-10T12:30:00Z,"));

    SUBCASE("the time runs out") {
        sys.now = at("2026-03-10T12:30:00Z");
        r.speed_round();
        CHECK(sys.speed_runs == 1);
        CHECK(has(out.str(), "notice: the pause is over; speed tests resume"));
    }
    SUBCASE("`meerkat resume` removes it") {
        CHECK(store::remove_pause(dir.path));
        sys.now += std::chrono::minutes{5};
        r.speed_round();
        CHECK(sys.speed_runs == 1);
    }
}

TEST_CASE("Recorder: during a pause, connectivity rounds thin out but still find outages") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }, {}, {}};
    auto c = config_in(dir.path);  // every 30 s; 2 min while paused (the default)
    Recorder r{c, sys.probes(), log};
    sys.processes = {{4242, "breitbandmessung", "breitbandmessun"}};

    std::vector<std::size_t> row_counts;
    for (int i = 0; i < 9; ++i) {  // 12:00:00 to 12:04:00
        r.connectivity_round();
        row_counts.push_back(rows(dir.path / "connectivity.csv").size());
        sys.now += std::chrono::seconds{30};
    }
    // Probed at 12:00, 12:02 and 12:04 only (4 rows each round).
    CHECK(row_counts == std::vector<std::size_t>{4, 4, 4, 4, 8, 8, 8, 8, 12});
    CHECK(has(r.status_line(), "net 12:04:00 up (official pause)"));

    SUBCASE("the line goes down: the slow rounds still record the outage") {
        sys.line_down(true);
        for (int i = 0; i < 5; ++i) {  // 12:04:30 to 12:06:30; probed 12:06
            r.connectivity_round();
            sys.now += std::chrono::seconds{30};
        }
        CHECK_FALSE(has(out.str(), "outage"));  // one failed round so far
        for (int i = 0; i < 4; ++i) {  // probed 12:08
            r.connectivity_round();
            sys.now += std::chrono::seconds{30};
        }
        CHECK(has(out.str(), "warning: outage since 2026-03-10T12:06:00Z"));
    }
    SUBCASE("after the pause the normal rate is back") {
        sys.processes.clear();
        r.connectivity_round();
        sys.now += std::chrono::seconds{30};
        r.connectivity_round();
        CHECK(rows(dir.path / "connectivity.csv").size() == 20);
        CHECK(has(out.str(), "notice: the Breitbandmessung app has stopped"));
    }
}

TEST_CASE("Recorder: a broken pause file is a warning, once, and no pause") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }, {}, {}};
    Recorder r{config_in(dir.path), sys.probes(), log};
    std::ofstream{dir.path / "pause"} << "2026-03-1";
    r.speed_round();
    r.speed_round();
    CHECK(sys.speed_runs == 2);
    const auto text = out.str();
    const std::string warning = "warning: the pause file is ignored: ";
    const auto first = text.find(warning);
    REQUIRE(first != std::string::npos);
    CHECK(text.find(warning, first + warning.size()) == std::string::npos);
}

TEST_CASE("Recorder: past days with data are sealed, in order, missed ones too") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, [&sys] { return sys.now; }, {}, {}};
    const auto manifest = [&dir](const char* day) {
        return dir.path / "manifest" / (std::string{day} + ".sha256");
    };

    SUBCASE("first start, data only from today: nothing to seal") {
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.connectivity_round();
        r.startup();
        CHECK_FALSE(fs::exists(dir.path / "manifest"));
    }
    SUBCASE("yesterday has only connectivity rows (e.g. skipped speed tests): sealed") {
        sys.now = at("2026-03-09T22:00:00Z");
        { Recorder r{config_in(dir.path), sys.probes(), log}; r.connectivity_round(); }
        sys.now = at("2026-03-10T08:00:00Z");  // restarted next morning
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.startup();
        CHECK(fs::exists(manifest("2026-03-09")));
        CHECK(has(out.str(), "notice: sealed 2026-03-09"));
        out.str("");
        r.seal_pending();  // nothing left: quiet
        CHECK(out.str().empty());
    }
    SUBCASE("stopped before midnight, restarted two days later: both days, chained") {
        sys.now = at("2026-03-08T20:00:00Z");
        { Recorder r{config_in(dir.path), sys.probes(), log}; r.connectivity_round(); }
        sys.now = at("2026-03-10T09:00:00Z");
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.startup();
        REQUIRE(fs::exists(manifest("2026-03-08")));
        REQUIRE(fs::exists(manifest("2026-03-09")));
        CHECK_FALSE(fs::exists(manifest("2026-03-10")));  // today isn't over
        const auto second = store::parse_manifest(*core::text::read_file(manifest("2026-03-09")));
        REQUIRE(second);
        REQUIRE_FALSE(second->entries.empty());
        CHECK(second->entries.front().path == "manifest/2026-03-08.sha256");  // the chain
    }
    SUBCASE("a directory named like a manifest isn't taken for one (#34)") {
        sys.now = at("2026-03-08T20:00:00Z");
        { Recorder r{config_in(dir.path), sys.probes(), log}; r.connectivity_round(); }
        fs::create_directories(manifest("2026-03-09"));
        sys.now = at("2026-03-10T09:00:00Z");
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.seal_pending();
        CHECK(fs::is_regular_file(manifest("2026-03-08")));  // not skipped as "sealed"
        CHECK(has(out.str(), "error: can't write the manifest for 2026-03-09: not a regular file"));
    }
    SUBCASE("manifest/ can't be listed: an error, nothing sealed") {
        sys.now = at("2026-03-08T20:00:00Z");
        { Recorder r{config_in(dir.path), sys.probes(), log}; r.connectivity_round(); }
        std::ofstream{dir.path / "manifest"} << "not a directory";
        sys.now = at("2026-03-10T09:00:00Z");
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.seal_pending();
        CHECK(has(out.str(), "error: can't list the manifests"));
        CHECK(fs::is_regular_file(dir.path / "manifest"));  // left as it was
    }
    SUBCASE("a raw log counts as data too") {
        fs::create_directories(dir.path / "raw");
        std::ofstream{dir.path / "raw" / "2026-03-09.jsonl"} << "{}\n";
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.startup();
        CHECK(fs::exists(manifest("2026-03-09")));
    }
    SUBCASE("a long backlog: only the last 31 days, with a warning") {
        sys.now = at("2026-01-01T12:00:00Z");
        { Recorder r{config_in(dir.path), sys.probes(), log}; r.connectivity_round(); }
        sys.now = at("2026-03-10T12:00:00Z");
        Recorder r{config_in(dir.path), sys.probes(), log};
        r.seal_pending();
        CHECK(has(out.str(), "warning: days 2026-01-01 to 2026-02-06 have no manifest"));
        CHECK_FALSE(fs::exists(manifest("2026-02-06")));
        CHECK(fs::exists(manifest("2026-02-07")));
        CHECK(fs::exists(manifest("2026-03-09")));
    }
}

TEST_CASE("run_service: starts, holds the sleep lock, stops cleanly") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, core::now, {}, {}};  // UTC only: no host zone
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
    SUBCASE("another meerkat recording into the data directory stops it at once") {
        const store::DirectoryLock other{c.data_dir};
        REQUIRE(other.state() == store::DirectoryLock::State::held);
        out.str("");
        CHECK(run_service(c, env, log) == kRunFailed);
        CHECK(has(out.str(),
                  "error: another meerkat is already recording into " + c.data_dir.string()));
        CHECK_FALSE(has(out.str(), "can't open the data files"));  // not an I/O problem
    }
    SUBCASE("a data directory that can't be created") {
        std::ofstream{dir.path / "file"} << "x";
        c.data_dir = dir.path / "file" / "data";
        CHECK(run_service(c, env, log) == kRunFailed);
        CHECK(has(out.str(), "can't create the data directory"));
    }
}

TEST_CASE("run_service: SIGTERM stops it cleanly, and the process survives") {
    TempDir dir;
    FakeSystem sys;
    std::ostringstream out;
    Log log{out, LogStyle::plain, core::now, {}, {}};  // UTC only: no host zone
    core::SystemClock clock;
    RunEnv env;
    env.probes = sys.probes();
    env.clock = &clock;
    env.handle_signals = true;
    env.on_started = [](core::Scheduler&) { ::kill(::getpid(), SIGTERM); };
    CHECK(run_service(config_in(dir.path / "data"), env, log) == kRunStopped);
    CHECK(has(out.str(), "notice: stopping (SIGTERM)"));
    CHECK(has(out.str(), "notice: stopped"));
    // The signal mask is back to what it was: SIGTERM isn't blocked.
    sigset_t now;
    ::pthread_sigmask(SIG_SETMASK, nullptr, &now);
    CHECK(::sigismember(&now, SIGTERM) == 0);
}
