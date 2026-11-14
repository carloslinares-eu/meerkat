/**
 * @file check.cpp
 * @brief Implementation of `meerkat check`.
 */
#include "app/check.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <exception>
#include <cerrno>
#include <fstream>
#include <locale>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>

#include "core/link_guard.hpp"
#include "core/text.hpp"
#include "store/csv.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"

namespace meerkat::app {

namespace {

namespace fs = std::filesystem;

/// Free space below which the data directory gets a warning.
constexpr std::uintmax_t kLowSpace = 1ull << 30;

/**
 * @brief `MAX/NORMAL/MIN` of one contract direction.
 * @param s The speeds.
 * @return The text.
 */
std::string speeds(const core::ContractSpeeds& s) {
    return core::text::to_text(s.maximum) + "/" + core::text::to_text(s.normal) + "/" + core::text::to_text(s.minimum);
}

/**
 * @brief The first line of @p text, trimmed.
 * @param text The text.
 * @return The line.
 */
std::string first_line(std::string_view text) {
    return std::string{core::text::trim(text.substr(0, text.find('\n')))};
}

/// Collects findings.
struct Findings {
    std::vector<CheckItem> items;  ///< So far.

    /**
     * @brief Adds a finding.
     * @param level  How serious.
     * @param topic  Short name.
     * @param detail What was found.
     */
    void add(CheckLevel level, std::string topic, std::string detail) {
        items.push_back({level, std::move(topic), std::move(detail)});
    }
};

/**
 * @brief Config file, its permissions, and the validation results.
 * @param report How it was obtained.
 * @param f      Findings.
 */
void check_config(const ConfigReport& report, Findings& f) {
    if (!report.load_error.empty()) {
        f.add(CheckLevel::failure, "config", report.load_error);
    } else if (report.file) {
        f.add(CheckLevel::ok, "config", report.file->string());
        // Group access is fine (the service's root:meerkat 0640 file);
        // "other" means every account on the machine.
        struct stat st {};
        if (report.file_has_contract && ::stat(report.file->c_str(), &st) == 0 &&
            (st.st_mode & 0007) != 0) {
            f.add(CheckLevel::warning, "config",
                  "holds your contract figures but every user on this computer can read it: "
                  "chmod 600 " + report.file->string());
        }
    } else {
        std::string where;
        for (const auto& p : report.searched) {
            where += (where.empty() ? "" : ", ") + p.string();
        }
        f.add(CheckLevel::ok, "config",
              "no config file, using defaults" + (where.empty() ? "" : " (looked for " + where + ")"));
    }
    for (const auto& p : report.unreadable) {
        f.add(CheckLevel::warning, "config",
              p.string() + " exists but this user can't read it; skipped");
    }
    for (const auto& p : report.problems) {
        f.add(CheckLevel::failure, "config", p);
    }
}

/**
 * @brief Whether the contract figures are there.
 * @param c The config.
 * @param f Findings.
 */
void check_contract(const core::Config& c, Findings& f) {
    const auto& k = c.contract;
    if (k.download && k.upload) {
        f.add(CheckLevel::ok, "contract",
              "download " + speeds(*k.download) + ", upload " + speeds(*k.upload) +
                  " Mbit/s (maximum/normal/minimum)");
    } else {
        const std::string which = k.download ? "upload " : k.upload ? "download " : "";
        f.add(CheckLevel::warning, "contract",
              "no " + which + "contract figures: the report can't compare against the "
              "contract. Set [contract] in the config file or use --contract-down/--contract-up");
    }
}

/**
 * @brief The Ookla CLI, ping and iw.
 * @param c   The config.
 * @param env The system.
 * @param f   Findings.
 */
void check_programs(const core::Config& c, const CheckEnv& env, Findings& f) {
    if (const auto path = env.find_program(c.speedtest); !path) {
        f.add(CheckLevel::failure, "speedtest",
              "'" + c.speedtest + "' not found. Install the Ookla CLI (speedtest.net/apps/cli) "
              "or set [speed] program");
    } else {
        sys::Command cmd;
        cmd.program = path->string();
        cmd.args = {"--version"};
        cmd.timeout = std::chrono::seconds{10};
        cmd.env = {{"LC_ALL", "C"}};
        const auto r = env.run(cmd);
        const std::string text = r.out + r.err;
        if (text.find("Speedtest by Ookla") != std::string::npos) {
            std::string detail = first_line(r.out.empty() ? r.err : r.out) + " (" + path->string() + ")";
            if (c.server_id) {
                detail += ", server " + std::to_string(*c.server_id);
            }
            f.add(CheckLevel::ok, "speedtest", detail);
        } else {
            f.add(CheckLevel::failure, "speedtest",
                  path->string() + " isn't the Ookla CLI (the Python speedtest-cli?): " +
                      (text.empty() ? std::string{"no output"} : first_line(text)));
        }
    }

    if (const auto ping = env.find_program("ping"); !ping) {
        f.add(CheckLevel::failure, "ping", "not found: install iputils-ping");
    } else {
        sys::Command cmd;
        cmd.program = ping->string();
        cmd.args = {"-n", "-c", "1", "-W", "1", "127.0.0.1"};
        cmd.timeout = std::chrono::seconds{5};
        cmd.env = {{"LC_ALL", "C"}};
        const auto r = env.run(cmd);
        if (r.outcome == sys::ProcessResult::Outcome::exited && r.exit_code == 0) {
            f.add(CheckLevel::ok, "ping", ping->string());
        } else {
            const std::string why = first_line(r.err.empty() ? r.out : r.err);
            f.add(CheckLevel::failure, "ping",
                  "can't ping 127.0.0.1" + (why.empty() ? std::string{} : ": " + why) +
                      ". Check net.ipv4.ping_group_range or the cap_net_raw capability");
        }
    }

    if (const auto iw = env.find_program("iw"); iw) {
        f.add(CheckLevel::ok, "iw", iw->string());
    } else {
        f.add(CheckLevel::warning, "iw", "not found: Wi-Fi details won't be recorded (apt install iw)");
    }
}

/**
 * @brief Raw logs or manifests that aren't regular files (symlinks, say):
 *        meerkat refuses them, so sealing would stop at that day.
 * @param dir     `raw/` or `manifest/` in the data directory.
 * @param day_of  Tells the day files apart from anything else there.
 * @param what    What they are, for the message.
 * @param f       Findings.
 */
void check_day_files(const fs::path& dir,
                     std::optional<std::string> (*day_of)(std::string_view),
                     const std::string& what, Findings& f) {
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        if (!day_of(it->path().filename().string())) {
            continue;
        }
        std::error_code sec;
        if (it->symlink_status(sec).type() != fs::file_type::regular) {
            f.add(CheckLevel::failure, "data",
                  it->path().string() + " isn't a regular file (a symlink?); meerkat refuses " +
                      what + " like that, so days stop being sealed there");
        }
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        f.add(CheckLevel::failure, "data", "can't list " + dir.string() + ": " + ec.message());
    }
}

/**
 * @brief Checks an existing CSV's header without opening it for writing (a
 *        running meerkat holds the lock).
 * @param path   The file.
 * @param schema Its expected schema.
 * @param f      Findings.
 */
void check_csv(const fs::path& path, const store::Schema& schema, Findings& f) {
    std::error_code ec;
    const auto type = fs::symlink_status(path, ec).type();
    if (type == fs::file_type::not_found) {
        return;
    }
    if (ec) {
        f.add(CheckLevel::failure, "data", "can't check " + path.string() + ": " + ec.message());
        return;
    }
    if (type != fs::file_type::regular) {  // never opened: a FIFO would block
        f.add(CheckLevel::failure, "data",
              path.string() + (type == fs::file_type::symlink ? " is a symlink" : " isn't a regular file") +
                  "; meerkat writes only to regular files, so `meerkat run` would stop");
        return;
    }
    const std::string name = path.filename().string();
    const std::string expected = store::header_text(schema);
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        f.add(CheckLevel::failure, "data",
              "can't read " + path.string() + " as this user: " + std::strerror(errno));
        return;
    }
    std::string head(expected.size(), '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    if (head.empty()) {
        f.add(CheckLevel::ok, "data", name + ": empty; meerkat will write the header");
    } else if (head == expected) {
        f.add(CheckLevel::ok, "data", name + ": schema v" + std::to_string(schema.version));
    } else if (expected.starts_with(head)) {
        f.add(CheckLevel::warning, "data",
              name + ": header incomplete (crash while creating it?); meerkat will rewrite it");
    } else {
        f.add(CheckLevel::failure, "data",
              path.string() + " has a different format (" + first_line(head) +
                  "). Move it away; meerkat never mixes schemas in one file");
        return;
    }
    if (::access(path.c_str(), W_OK) != 0) {
        f.add(CheckLevel::failure, "data",
              "this user can't append to " + path.string() + ": " + std::strerror(errno));
    }
}

/**
 * @brief The data directory: writable, free space, existing files.
 * @param c The config.
 * @param f Findings.
 */
void check_data_dir(const core::Config& c, Findings& f) {
    const fs::path& dir = c.data_dir;
    std::error_code ec;
    const auto st = fs::status(dir, ec);
    if (st.type() != fs::file_type::not_found && ec) {
        f.add(CheckLevel::failure, "data", "can't access " + dir.string() + ": " + ec.message());
        return;
    }
    if (st.type() == fs::file_type::not_found) {
        // Nearest existing parent decides whether it can be created.
        fs::path parent = dir.parent_path();
        while (!parent.empty() && !fs::exists(parent, ec) && parent != parent.root_path()) {
            parent = parent.parent_path();
        }
        if (::access(parent.c_str(), W_OK | X_OK) == 0) {
            f.add(CheckLevel::ok, "data", dir.string() + " (doesn't exist yet; will be created)");
        } else {
            f.add(CheckLevel::failure, "data",
                  dir.string() + " doesn't exist and " + parent.string() + " isn't writable");
        }
        return;
    }
    if (st.type() != fs::file_type::directory) {
        f.add(CheckLevel::failure, "data", dir.string() + " isn't a directory");
        return;
    }
    // Really create a file: access() ignores read-only mounts and ACL details.
    std::string probe = (dir / ".meerkat-check-XXXXXX").string();
    const int fd = ::mkstemp(probe.data());
    if (fd < 0) {
        f.add(CheckLevel::failure, "data",
              "can't write to " + dir.string() + ": " + std::strerror(errno));
        return;
    }
    ::close(fd);
    ::unlink(probe.c_str());

    struct statvfs vfs {};
    std::string space;
    if (::statvfs(dir.c_str(), &vfs) == 0) {
        const std::uintmax_t free = static_cast<std::uintmax_t>(vfs.f_bavail) * vfs.f_frsize;
        std::ostringstream gib;
        gib.imbue(std::locale::classic());
        gib << std::fixed << std::setprecision(1)
            << static_cast<double>(free) / static_cast<double>(1ull << 30);
        space = ", " + gib.str() + " GiB free";
        if (free < kLowSpace) {
            f.add(CheckLevel::warning, "data", dir.string() + ": less than 1 GiB free");
        }
    }
    f.add(CheckLevel::ok, "data", dir.string() + " (writable" + space + ")");
    check_csv(dir / store::kSpeedFile, store::speed_schema(), f);
    check_csv(dir / store::kConnectivityFile, store::connectivity_schema(), f);
    check_day_files(dir / store::kRawDir, store::raw_log_day, "raw logs", f);
    check_day_files(dir / store::kManifestDir, store::manifest_day, "manifests", f);
}

/**
 * @brief Can meerkat keep the computer awake? Takes the logind lock and
 *        releases it at once.
 * @param env The system.
 * @param f   Findings to add to.
 */
void check_sleep(const CheckEnv& env, Findings& f) {
    if (!env.inhibit) {
        f.add(CheckLevel::warning, "sleep", "not checked: no way to ask logind");
        return;
    }
    const sys::InhibitRequest request;
    try {
        auto lock = sys::SleepInhibitor::acquire(request, env.inhibit);
        lock.release();
        f.add(CheckLevel::ok, "sleep", "logind grants the lock that keeps the computer awake (" +
                                           request.what + ")");
    } catch (const std::exception& e) {
        f.add(CheckLevel::warning, "sleep",
              std::string{"the computer may suspend and miss measurements: "} + e.what());
    }
}

/**
 * @brief Default route, link type and speed.
 * @param c   The config.
 * @param env The system.
 * @param f   Findings.
 */
void check_connection(const core::Config& c, const CheckEnv& env, Findings& f) {
    // The same sensing and wording as the guard before every speed test.
    net::LinkSensors sensors = env.sensors;
    sensors.wifi = nullptr;  // `iw` details aren't checked here
    const auto snap = net::sense_link(sensors);
    const auto warnings = core::link_warnings(snap);
    if (!snap.has_route || !snap.interface_exists || !snap.usable()) {
        f.add(CheckLevel::failure, "route", warnings.front());
        return;
    }
    const auto& ctx = snap.context;
    const std::string where = ctx.interface + (ctx.gateway.empty() ? "" : " via " + ctx.gateway);
    if (ctx.link_type != core::LinkType::wired) {
        for (const auto& w : warnings) {
            f.add(CheckLevel::warning, "route", where + ": " + w);
        }
        return;
    }
    std::string detail = where + " (cable";
    if (ctx.link_speed_mbps) {
        detail += ", link " + core::text::to_text(*ctx.link_speed_mbps) + " Mbit/s";
    }
    f.add(CheckLevel::ok, "route", detail + ")");
    const auto& down = c.contract.download;
    if (ctx.link_speed_mbps && down && *ctx.link_speed_mbps < down->maximum) {
        f.add(CheckLevel::warning, "route",
              "the link runs at " + core::text::to_text(*ctx.link_speed_mbps) +
                  " Mbit/s, below the contract maximum of " + core::text::to_text(down->maximum) +
                  " Mbit/s: the cable or port limits the measurements (use a Gigabit "
                  "port and a Cat 5e or better cable)");
    }
}

}  // namespace

CheckEnv system_check_env() {
    CheckEnv e;
    e.find_program = [](std::string_view name) {
        const char* path = std::getenv("PATH");
        return sys::find_executable(name, path == nullptr ? "" : path);
    };
    e.run = sys::run;
    e.sensors = net::system_link_sensors();
    e.inhibit = sys::logind_inhibit;
    return e;
}

std::vector<CheckItem> run_checks(const core::Config& config, const ConfigReport& report,
                                  const CheckEnv& env) {
    Findings f;
    check_config(report, f);
    // With an unusable file, the data directory and contract would be the
    // defaults: checking those would report on settings meerkat won't use.
    const bool file_ok = report.load_error.empty();
    if (file_ok) {
        check_contract(config, f);
    }
    check_programs(config, env, f);
    check_sleep(env, f);
    if (file_ok) {
        check_data_dir(config, f);
    } else {
        f.add(CheckLevel::warning, "data",
              "not checked: the config file can't be used, so the data directory is unknown");
    }
    check_connection(config, env, f);
    return std::move(f.items);
}

void print_checks(const std::vector<CheckItem>& items, std::ostream& out) {
    int warnings = 0;
    int failures = 0;
    for (const auto& i : items) {
        std::string tag;
        switch (i.level) {
        case CheckLevel::ok: tag = "[ ok ]"; break;
        case CheckLevel::warning: tag = "[warn]"; ++warnings; break;
        case CheckLevel::failure: tag = "[FAIL]"; ++failures; break;
        }
        std::string topic = i.topic;
        topic.resize(std::max<std::size_t>(topic.size(), 10), ' ');
        out << tag << ' ' << topic << ' ' << i.detail << '\n';
    }
    out << '\n';
    if (failures > 0) {
        out << failures << " problem(s) to fix before recording";
    } else {
        out << "Ready to record";
    }
    if (warnings > 0) {
        out << " (" << warnings << " warning(s))";
    }
    out << ".\n";
}

bool has_failure(const std::vector<CheckItem>& items) {
    return std::any_of(items.begin(), items.end(),
                       [](const CheckItem& i) { return i.level == CheckLevel::failure; });
}

}  // namespace meerkat::app
