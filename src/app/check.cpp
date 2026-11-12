/**
 * @file check.cpp
 * @brief Implementation of `meerkat check`.
 */
#include "app/check.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <locale>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>

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
 * @brief A number without trailing zeros, `.` as decimal point.
 * @param v The number.
 * @return The text.
 */
std::string num(double v) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << v;
    return s.str();
}

/**
 * @brief `MAX/NORMAL/MIN` of one contract direction.
 * @param s The speeds.
 * @return The text.
 */
std::string speeds(const core::ContractSpeeds& s) {
    return num(s.maximum) + "/" + num(s.normal) + "/" + num(s.minimum);
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
 * @param c      The config.
 * @param report How it was obtained.
 * @param f      Findings.
 */
void check_config(const core::Config& c, const ConfigReport& report, Findings& f) {
    if (!report.load_error.empty()) {
        f.add(CheckLevel::failure, "config", report.load_error);
    } else if (report.file) {
        f.add(CheckLevel::ok, "config", report.file->string());
        struct stat st {};
        const bool has_contract = c.contract.download || c.contract.upload;
        if (has_contract && ::stat(report.file->c_str(), &st) == 0 && (st.st_mode & 0077) != 0) {
            f.add(CheckLevel::warning, "config",
                  "holds your contract figures but other users can read it: chmod 600 " +
                      report.file->string());
        }
    } else {
        std::string where;
        for (const auto& p : report.searched) {
            where += (where.empty() ? "" : ", ") + p.string();
        }
        f.add(CheckLevel::ok, "config",
              "no config file, using defaults" + (where.empty() ? "" : " (looked for " + where + ")"));
    }
    for (const auto& p : report.problems) {
        f.add(CheckLevel::failure, "config", p);
    }

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
 * @brief Checks an existing CSV's header without opening it for writing (a
 *        running meerkat holds the lock).
 * @param path   The file.
 * @param schema Its expected schema.
 * @param f      Findings.
 */
void check_csv(const fs::path& path, const store::Schema& schema, Findings& f) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return;
    }
    const std::string expected = store::header_text(schema);
    std::ifstream in{path, std::ios::binary};
    std::string head(expected.size(), '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    if (head == expected) {
        f.add(CheckLevel::ok, "data", path.filename().string() + ": schema v" +
                                         std::to_string(schema.version));
    } else if (expected.starts_with(head)) {
        f.add(CheckLevel::warning, "data",
              path.filename().string() + ": header incomplete (crash while creating it?); "
              "meerkat will rewrite it");
    } else {
        f.add(CheckLevel::failure, "data",
              path.string() + " has a different format (" + first_line(head) +
                  "). Move it away; meerkat never mixes schemas in one file");
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
    if (!fs::exists(dir, ec)) {
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
    if (!fs::is_directory(dir, ec)) {
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
}

/**
 * @brief Default route, link type and speed.
 * @param c   The config.
 * @param env The system.
 * @param f   Findings.
 */
void check_connection(const core::Config& c, const CheckEnv& env, Findings& f) {
    const auto route = env.route();
    if (!route) {
        f.add(CheckLevel::failure, "route", "no default route: this computer is offline");
        return;
    }
    const auto link = env.link(route->interface);
    std::string where = route->interface + (route->gateway.empty() ? "" : " via " + route->gateway);
    if (!link.exists) {
        f.add(CheckLevel::failure, "route", where + ": interface not in /sys/class/net");
        return;
    }
    if (link.carrier && !*link.carrier) {
        f.add(CheckLevel::failure, "route",
              where + ": no carrier (cable unplugged or Wi-Fi not connected)");
        return;
    }
    switch (link.type) {
    case core::LinkType::wired: {
        std::string detail = where + " (cable";
        if (link.speed_mbps) {
            detail += ", link " + num(*link.speed_mbps) + " Mbit/s";
        }
        f.add(CheckLevel::ok, "route", detail + ")");
        const auto& down = c.contract.download;
        if (link.speed_mbps && down && *link.speed_mbps < down->maximum) {
            f.add(CheckLevel::warning, "route",
                  "the link runs at " + num(*link.speed_mbps) +
                      " Mbit/s, below the contract maximum of " + num(down->maximum) +
                      " Mbit/s: the cable or port limits the measurements (use a Gigabit "
                      "port and a Cat 5e or better cable)");
        }
        break;
    }
    case core::LinkType::wifi:
        f.add(CheckLevel::warning, "route",
              where + " is Wi-Fi: results don't count against the provider; connect a cable to "
              "the router");
        break;
    case core::LinkType::unknown:
        f.add(CheckLevel::warning, "route",
              where + ": can't tell whether this is a cable (bridge, VPN or virtual interface?)");
        break;
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
    e.route = [] { return net::default_route(); };
    e.link = [](std::string_view i) { return net::read_link(i); };
    return e;
}

std::vector<CheckItem> run_checks(const core::Config& config, const ConfigReport& report,
                                  const CheckEnv& env) {
    Findings f;
    check_config(config, report, f);
    check_programs(config, env, f);
    check_data_dir(config, f);
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
    for (const auto& i : items) {
        if (i.level == CheckLevel::failure) {
            return true;
        }
    }
    return false;
}

}  // namespace meerkat::app
