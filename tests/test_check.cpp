/**
 * @file test_check.cpp
 * @brief Tests for config resolution (app::resolve_settings) and `meerkat check`.
 */
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "app/check.hpp"
#include "app/settings.hpp"
#include "store/csv.hpp"
#include "store/dir_lock.hpp"
#include "store/schema.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace meerkat::app;
using meerkat::test::FakeRunner;
using meerkat::test::TempDir;
namespace fs = std::filesystem;

namespace {

/// Gives a directory mode 0755 back when the test leaves its scope, even
/// after a failed check, so TempDir can remove it.
struct RestoreMode {
    fs::path dir;  ///< The directory.

    /// Restores the mode.
    ~RestoreMode() { ::chmod(dir.c_str(), 0755); }
};

/// An environment with exactly @p vars.
core::EnvLookup env_of(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string{name});
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

/// Writes @p text to @p p, creating the folders.
void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream{p, std::ios::binary} << text;
}

/// A healthy system: every program installed and working, a wired gigabit route.
struct FakeSystem {
    std::map<std::string, std::string> programs{{"speedtest", "/usr/bin/speedtest"},
                                                {"ping", "/usr/bin/ping"},
                                                {"iw", "/usr/sbin/iw"}};
    std::string speedtest_version = "Speedtest by Ookla 1.2.0.84 (ea6b6773cf) Linux/x86_64\n";
    sys::ProcessResult ping_result = FakeRunner::exited("1 packets transmitted, 1 received\n");
    std::optional<net::DefaultRoute> route = net::DefaultRoute{"enp0s25", "192.0.2.1", 100, 4};
    net::LinkInfo link{};
    std::optional<sys::InhibitReply> inhibit_failure;  ///< Set: logind refuses with this.

    FakeSystem() {
        link.interface = "enp0s25";
        link.exists = true;
        link.type = core::LinkType::wired;
        link.carrier = true;
        link.speed_mbps = 1000;
    }

    /// The CheckEnv backed by this fake.
    CheckEnv env() {
        CheckEnv e;
        e.find_program = [this](std::string_view n) -> std::optional<fs::path> {
            const auto it = programs.find(std::string{n});
            if (it == programs.end()) {
                return std::nullopt;
            }
            return fs::path{it->second};
        };
        e.run = [this](const sys::Command& c) {
            if (c.program.ends_with("ping")) {
                return ping_result;
            }
            return FakeRunner::exited(speedtest_version);
        };
        e.sensors.route = [this] { return route; };
        e.sensors.link = [this](std::string_view) { return link; };
        e.inhibit = [this](const sys::InhibitRequest&) {
            if (inhibit_failure) {
                return *inhibit_failure;
            }
            sys::InhibitReply r;
            r.fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            return r;
        };
        return e;
    }
};

/// A config with its data directory in @p dir and the made-up example tariff.
core::Config config_in(const fs::path& dir) {
    core::Config c = core::default_config(env_of({{"HOME", "/home/user"}}));
    c.data_dir = dir / "data";
    c.contract.download = core::ContractSpeeds{250, 200, 150};
    c.contract.upload = core::ContractSpeeds{50, 40, 30};
    return c;
}

/// The first finding about @p topic at @p level whose detail contains @p text.
const CheckItem* find(const std::vector<CheckItem>& items, std::string_view topic, CheckLevel level,
                      std::string_view text = {}) {
    for (const auto& i : items) {
        if (i.topic == topic && i.level == level && i.detail.find(text) != std::string::npos) {
            return &i;
        }
    }
    return nullptr;
}

/// Number of findings at @p level.
std::size_t count(const std::vector<CheckItem>& items, CheckLevel level) {
    std::size_t n = 0;
    for (const auto& i : items) {
        n += i.level == level ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("resolve_settings: defaults, the file found on the search path, then flags") {
    TempDir home;
    const auto env = env_of({{"HOME", home.path.string()}});

    SUBCASE("no file: defaults") {
        const auto s = resolve_settings({}, env);
        CHECK(s.report.usable());
        CHECK_FALSE(s.report.file.has_value());
        CHECK(s.report.searched.front() == home.path / ".config/meerkat/config.toml");
        CHECK(s.config.data_dir == home.path / ".local/share/meerkat");
    }
    SUBCASE("the user's file is found and read") {
        put(home.path / ".config/meerkat/config.toml", "[speed]\ninterval = \"10m\"\nserver_id = 7\n");
        const auto s = resolve_settings({}, env);
        CHECK(s.report.usable());
        CHECK(s.report.file == home.path / ".config/meerkat/config.toml");
        CHECK(s.config.speed_interval == std::chrono::minutes{10});

        SUBCASE("flags win over the file") {
            Overrides o;
            o.speed_interval = "15m";
            o.server_id = 9;
            o.ping_hosts = std::vector<std::string>{"192.0.2.9"};
            o.dns_host = "example.org";
            o.require_wired = true;
            o.contract_down = "250/200/150";
            o.contract_up = "50/40/30";
            o.data_dir = "~/elsewhere";
            o.speedtest = "/opt/speedtest";
            o.connectivity_interval = "1m";
            const auto f = resolve_settings(o, env);
            CHECK(f.report.usable());
            CHECK(f.config.speed_interval == std::chrono::minutes{15});
            CHECK(f.config.server_id == 9);
            CHECK(f.config.ping_hosts == std::vector<std::string>{"192.0.2.9"});
            CHECK(f.config.dns_host == "example.org");
            CHECK(f.config.require_wired);
            CHECK(f.config.contract.download == core::ContractSpeeds{250, 200, 150});
            CHECK(f.config.contract.upload == core::ContractSpeeds{50, 40, 30});
            CHECK(f.config.data_dir == home.path / "elsewhere");
            CHECK(f.config.speedtest == "/opt/speedtest");
            CHECK(f.config.connectivity_interval == std::chrono::minutes{1});
        }
    }
    SUBCASE("--config names a file that must exist") {
        Overrides o;
        o.config_file = home.path / "missing.toml";
        const auto s = resolve_settings(o, env);
        CHECK_FALSE(s.report.usable());
        CHECK(s.report.load_error.find("missing.toml") != std::string::npos);
        CHECK(s.report.searched.empty());
    }
    SUBCASE("a file this user can't read is skipped while searching") {
        if (::geteuid() == 0) {
            return;  // root can read anything
        }
        const auto file = home.path / ".config/meerkat/config.toml";
        put(file, "[speed]\ninterval = \"10m\"\n");
        ::chmod(file.c_str(), 0200);
        const auto s = resolve_settings({}, env);
        CHECK(s.report.usable());
        CHECK_FALSE(s.report.file.has_value());
        CHECK(s.report.unreadable == std::vector<fs::path>{file});
        CHECK(s.config.speed_interval == std::chrono::minutes{5});

        Overrides o;  // named explicitly, it is an error
        o.config_file = file;
        CHECK_FALSE(resolve_settings(o, env).report.usable());
    }
    SUBCASE("the file's own contract is noted (for the permission check)") {
        put(home.path / ".config/meerkat/config.toml",
            "[contract]\ndownload = { maximum = 250, normal = 200, minimum = 150 }\n");
        CHECK(resolve_settings({}, env).report.file_has_contract);
        put(home.path / ".config/meerkat/config.toml", "");
        Overrides o;
        o.contract_down = "250/200/150";
        CHECK_FALSE(resolve_settings(o, env).report.file_has_contract);
    }
    SUBCASE("--no-require-wired overrides the file") {
        put(home.path / ".config/meerkat/config.toml", "[speed]\nrequire_wired = true\n");
        CHECK(resolve_settings({}, env).config.require_wired);
        Overrides o;
        o.require_wired = false;
        CHECK_FALSE(resolve_settings(o, env).config.require_wired);
    }
    SUBCASE("a broken file is reported; the defaults stay in effect") {
        put(home.path / ".config/meerkat/config.toml", "[speed]\nintervall = 5\n");
        const auto s = resolve_settings({}, env);
        CHECK(s.report.load_error.find("intervall") != std::string::npos);
        CHECK(s.config.speed_interval == std::chrono::minutes{5});
    }
    SUBCASE("bad flag values and invalid results are all collected") {
        Overrides o;
        o.speed_interval = "soon";
        o.contract_down = "250/200";
        o.contract_up = "30/40/50";
        o.data_dir = "~bob/x";
        const auto s = resolve_settings(o, env);
        CHECK_FALSE(s.report.usable());
        const auto all = [&s] {
            std::string t;
            for (const auto& p : s.report.problems) {
                t += p + "\n";
            }
            return t;
        }();
        CHECK(all.find("--speed-interval: 'soon'") != std::string::npos);
        CHECK(all.find("--contract-down:") != std::string::npos);
        CHECK(all.find("--data-dir:") != std::string::npos);
        CHECK(all.find("contract upload speeds must be maximum") != std::string::npos);
    }
}

TEST_CASE("check: a healthy wired setup is ready") {
    TempDir dir;
    FakeSystem sys;
    ConfigReport report;
    const auto items = run_checks(config_in(dir.path), report, sys.env());
    CHECK_FALSE(has_failure(items));
    CHECK(count(items, CheckLevel::warning) == 0);
    CHECK(find(items, "contract", CheckLevel::ok, "download 250/200/150, upload 50/40/30"));
    CHECK(find(items, "speedtest", CheckLevel::ok, "Speedtest by Ookla 1.2.0.84"));
    CHECK(find(items, "ping", CheckLevel::ok));
    CHECK(find(items, "sleep", CheckLevel::ok, "sleep:idle:handle-lid-switch"));
    CHECK(find(items, "data", CheckLevel::ok, "will be created"));
    CHECK(find(items, "route", CheckLevel::ok, "enp0s25 via 192.0.2.1 (cable, link 1000 Mbit/s)"));
    CHECK_FALSE(fs::exists(dir.path / "data"));  // check changes nothing

    std::ostringstream out;
    print_checks(items, out);
    CHECK(out.str().find("[ ok ] speedtest") != std::string::npos);
    CHECK(out.str().find("Ready to record.") != std::string::npos);
}

TEST_CASE("check: config findings") {
    TempDir dir;
    FakeSystem sys;
    auto c = config_in(dir.path);

    SUBCASE("load error and problems are failures") {
        ConfigReport r;
        r.load_error = "x.toml:3:1: unknown setting 'foo'";
        r.problems = {"the speed-test interval must be between 1m and 24h (got 30s)"};
        const auto items = run_checks(c, r, sys.env());
        CHECK(find(items, "config", CheckLevel::failure, "unknown setting 'foo'"));
        CHECK(find(items, "config", CheckLevel::failure, "between 1m and 24h"));
        std::ostringstream out;
        print_checks(items, out);
        CHECK(out.str().find("2 problem(s) to fix before recording") != std::string::npos);
        // The file couldn't be used: no report on the default data dir or contract.
        CHECK(find(items, "data", CheckLevel::warning, "not checked"));
        CHECK_FALSE(find(items, "data", CheckLevel::ok));
        CHECK_FALSE(find(items, "contract", CheckLevel::ok));
    }
    SUBCASE("a config with contract figures readable by others") {
        const auto file = dir.path / "config.toml";
        put(file, "");
        ::chmod(file.c_str(), 0644);
        ConfigReport r;
        r.file = file;
        r.file_has_contract = true;
        auto items = run_checks(c, r, sys.env());
        CHECK(find(items, "config", CheckLevel::warning, "chmod 600"));
        ::chmod(file.c_str(), 0640);  // group-readable is fine (the service's group)
        items = run_checks(c, r, sys.env());
        CHECK_FALSE(find(items, "config", CheckLevel::warning));
        CHECK(find(items, "config", CheckLevel::ok, file.string()));

        // Contract from the flags, file without one: nothing private in the file.
        ::chmod(file.c_str(), 0644);
        r.file_has_contract = false;
        CHECK_FALSE(find(run_checks(c, r, sys.env()), "config", CheckLevel::warning));
    }
    SUBCASE("unreadable files skipped during the search are listed") {
        ConfigReport r;
        r.unreadable = {"/etc/meerkat/config.toml"};
        CHECK(find(run_checks(c, r, sys.env()), "config", CheckLevel::warning,
                   "/etc/meerkat/config.toml exists but this user can't read it"));
    }
    SUBCASE("no contract, or only one direction") {
        c.contract = {};
        auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "contract", CheckLevel::warning, "no contract figures"));
        c.contract.download = core::ContractSpeeds{250, 200, 150};
        items = run_checks(c, {}, sys.env());
        CHECK(find(items, "contract", CheckLevel::warning, "no upload contract figures"));
        CHECK_FALSE(has_failure(items));
    }
}

TEST_CASE("check: programs") {
    TempDir dir;
    FakeSystem sys;
    const auto c = config_in(dir.path);

    SUBCASE("speedtest missing") {
        sys.programs.erase("speedtest");
        CHECK(find(run_checks(c, {}, sys.env()), "speedtest", CheckLevel::failure, "not found"));
    }
    SUBCASE("speedtest is the Python speedtest-cli") {
        sys.speedtest_version = "speedtest-cli 2.1.3\nPython 3.13.5\n";
        CHECK(find(run_checks(c, {}, sys.env()), "speedtest", CheckLevel::failure,
                   "isn't the Ookla CLI (the Python speedtest-cli?): speedtest-cli 2.1.3"));
    }
    SUBCASE("ping not allowed") {
        sys.ping_result = FakeRunner::exited("", 2, "ping: socket: Operation not permitted\n");
        CHECK(find(run_checks(c, {}, sys.env()), "ping", CheckLevel::failure,
                   "Operation not permitted"));
    }
    SUBCASE("ping missing") {
        sys.programs.erase("ping");
        CHECK(find(run_checks(c, {}, sys.env()), "ping", CheckLevel::failure, "iputils-ping"));
    }
    SUBCASE("iw missing is only a warning") {
        sys.programs.erase("iw");
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "iw", CheckLevel::warning));
        CHECK_FALSE(has_failure(items));
    }
}

TEST_CASE("check: sleep lock") {
    TempDir dir;
    FakeSystem sys;
    const auto c = config_in(dir.path);

    SUBCASE("polkit refuses: a warning with the polkit hint") {
        sys.inhibit_failure = sys::InhibitReply{-1, sys::InhibitError::Kind::denied,
                                                "org.freedesktop.DBus.Error.AccessDenied",
                                                "Permission denied"};
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "sleep", CheckLevel::warning,
                   "the computer may suspend and miss measurements: can't block suspend"));
        CHECK(find(items, "sleep", CheckLevel::warning, "usually by polkit"));
        CHECK_FALSE(has_failure(items));
    }
    SUBCASE("no system bus") {
        sys.inhibit_failure = sys::InhibitReply{
            -1, sys::InhibitError::Kind::no_bus, "",
            "can't connect to the system D-Bus: No such file or directory"};
        CHECK(find(run_checks(c, {}, sys.env()), "sleep", CheckLevel::warning, "No system D-Bus"));
    }
    SUBCASE("no inhibit function: not checked, no crash") {
        auto env = sys.env();
        env.inhibit = nullptr;
        CHECK(find(run_checks(c, {}, env), "sleep", CheckLevel::warning, "not checked"));
    }
}

TEST_CASE("check: data directory") {
    TempDir dir;
    FakeSystem sys;
    auto c = config_in(dir.path);
    fs::create_directories(c.data_dir);

    SUBCASE("writable, with CSVs of the right schema") {
        put(c.data_dir / "speed.csv", store::header_text(store::speed_schema()));
        put(c.data_dir / "connectivity.csv", store::header_text(store::connectivity_schema()));
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "data", CheckLevel::ok, "writable"));
        CHECK(find(items, "data", CheckLevel::ok, "speed.csv: schema v1"));
        CHECK(find(items, "data", CheckLevel::ok, "connectivity.csv: schema v1"));
        CHECK_FALSE(has_failure(items));
        for (const auto& e : fs::directory_iterator{c.data_dir}) {
            CHECK(e.path().filename().string().rfind(".meerkat-check", 0) != 0);  // probe removed
        }
        CHECK(store::DirectoryLock{c.data_dir}.state() ==
              store::DirectoryLock::State::held);  // check let go of the lock
    }
    SUBCASE("another meerkat recording into it is a warning (the service, say)") {
        const store::DirectoryLock other{c.data_dir};
        REQUIRE(other.state() == store::DirectoryLock::State::held);
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "data", CheckLevel::warning,
                   "another meerkat is already recording into " + c.data_dir.string()));
        CHECK_FALSE(has_failure(items));
    }
    SUBCASE("a directory run couldn't open to lock") {
        if (::geteuid() == 0) {
            return;  // root can anyway (CI containers)
        }
        const RestoreMode restore{c.data_dir};
        ::chmod(c.data_dir.c_str(), 0300);  // writable, but can't be opened for reading
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure, "can't open"));
    }
    SUBCASE("a CSV with another schema") {
        put(c.data_dir / "speed.csv", "timestamp,download,upload\n");
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure,
                   "different format (timestamp,download,upload)"));
    }
    SUBCASE("an empty CSV is fine") {
        put(c.data_dir / "speed.csv", "");
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "data", CheckLevel::ok, "speed.csv: empty"));
        CHECK_FALSE(has_failure(items));
    }
    SUBCASE("a CSV this user can't read or append to") {
        if (::geteuid() == 0) {
            return;  // root can anyway (CI containers)
        }
        put(c.data_dir / "speed.csv", store::header_text(store::speed_schema()));
        ::chmod((c.data_dir / "speed.csv").c_str(), 0444);
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure, "can't append"));
        ::chmod((c.data_dir / "speed.csv").c_str(), 0200);
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure, "can't read"));
    }
    SUBCASE("a CSV that is a symlink: run would refuse it (#35)") {
        put(dir.path / "elsewhere.csv", store::header_text(store::speed_schema()));
        fs::create_symlink(dir.path / "elsewhere.csv", c.data_dir / "speed.csv");
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure, "is a symlink"));
    }
    SUBCASE("a CSV that is a directory or a FIFO: a failure, and check doesn't hang") {
        fs::create_directory(c.data_dir / "speed.csv");
        REQUIRE(::mkfifo((c.data_dir / "connectivity.csv").c_str(), 0600) == 0);
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "data", CheckLevel::failure, "speed.csv isn't a regular file"));
        CHECK(find(items, "data", CheckLevel::failure, "connectivity.csv isn't a regular file"));
    }
    SUBCASE("a raw log or a manifest that is a symlink (#35)") {
        put(dir.path / "elsewhere", "x\n");
        put(c.data_dir / "raw" / "notes.txt", "ignored");
        fs::create_symlink(dir.path / "elsewhere", c.data_dir / "raw" / "2026-11-11.jsonl");
        fs::create_directories(c.data_dir / "manifest");
        fs::create_symlink(dir.path / "elsewhere", c.data_dir / "manifest" / "2026-11-11.sha256");
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "data", CheckLevel::failure, "2026-11-11.jsonl isn't a regular file"));
        CHECK(find(items, "data", CheckLevel::failure, "2026-11-11.sha256 isn't a regular file"));
    }
    SUBCASE("a CSV with a header cut short") {
        put(c.data_dir / "speed.csv", store::header_text(store::speed_schema()).substr(0, 9));
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::warning, "incomplete"));
    }
    SUBCASE("not a directory") {
        c.data_dir = dir.path / "file";
        put(c.data_dir, "x");
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure, "isn't a directory"));
    }
    SUBCASE("not writable") {
        if (::geteuid() == 0) {
            return;  // root can write anyway (CI containers)
        }
        const RestoreMode restore{c.data_dir};
        ::chmod(c.data_dir.c_str(), 0555);
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure, "can't write"));
        c.data_dir /= "sub";
        CHECK(find(run_checks(c, {}, sys.env()), "data", CheckLevel::failure, "isn't writable"));
    }
}

TEST_CASE("check: connection") {
    TempDir dir;
    FakeSystem sys;
    const auto c = config_in(dir.path);

    SUBCASE("offline") {
        sys.route.reset();
        CHECK(find(run_checks(c, {}, sys.env()), "route", CheckLevel::failure, "offline"));
    }
    SUBCASE("no carrier") {
        sys.link.carrier = false;
        CHECK(find(run_checks(c, {}, sys.env()), "route", CheckLevel::failure, "no carrier"));
    }
    SUBCASE("interface gone") {
        sys.link.exists = false;
        CHECK(find(run_checks(c, {}, sys.env()), "route", CheckLevel::failure, "/sys/class/net"));
    }
    SUBCASE("Wi-Fi") {
        sys.link.type = core::LinkType::wifi;
        sys.link.speed_mbps.reset();
        const auto items = run_checks(c, {}, sys.env());
        CHECK(find(items, "route", CheckLevel::warning, "is Wi-Fi"));
        CHECK_FALSE(has_failure(items));
    }
    SUBCASE("unknown link type") {
        sys.link.type = core::LinkType::unknown;
        CHECK(find(run_checks(c, {}, sys.env()), "route", CheckLevel::warning, "can't tell"));
    }
    SUBCASE("a link slower than the contract maximum") {
        sys.link.speed_mbps = 200;
        CHECK(find(run_checks(c, {}, sys.env()), "route", CheckLevel::warning,
                   "below the contract maximum of 250"));
    }
}
