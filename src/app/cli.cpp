/**
 * @file cli.cpp
 * @brief Implementation of the command-line entry point.
 */
#include "app/cli.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "app/check.hpp"
#include "app/log.hpp"
#include "app/once.hpp"
#include "app/run.hpp"
#include "app/settings.hpp"
#include "core/text.hpp"
#include "store/pause_file.hpp"
#include "meerkat/version.hpp"

namespace meerkat::app {

namespace {

/// Exit code for a config that can't be used.
constexpr int kConfigError = 2;

/// Flag values, bound by CLI11; turned into Overrides after parsing.
struct Flags {
    std::string config_file;                 ///< `--config`.
    std::string data_dir;                    ///< `--data-dir`.
    std::string speed_interval;              ///< `--speed-interval`.
    std::string connectivity_interval;       ///< `--connectivity-interval`.
    std::string speedtest;                   ///< `--speedtest`.
    std::int64_t server_id = 0;              ///< `--server-id`.
    std::vector<std::string> ping_hosts;     ///< `--ping`.
    std::string dns_host;                    ///< `--dns-host`.
    bool require_wired = false;              ///< `--require-wired` / `--no-require-wired`.
    std::string contract_down;               ///< `--contract-down`.
    std::string contract_up;                 ///< `--contract-up`.
};

/**
 * @brief Adds the flags every command that measures understands.
 * @param cmd The subcommand.
 * @param f   Where the values go.
 * @return The `--server-id` option (so `once` can exclude it with `--no-speed`).
 */
CLI::Option* add_probe_flags(CLI::App* cmd, Flags& f) {
    const core::Config defaults;
    std::string hosts;
    for (const auto& h : defaults.ping_hosts) {
        hosts += (hosts.empty() ? "" : " ") + h;
    }
    cmd->add_option("-c,--config", f.config_file,
                    "Config file (default: ~/.config/meerkat/config.toml, then "
                    "/etc/meerkat/config.toml)");
    cmd->add_option("--speedtest", f.speedtest, "Ookla CLI program name or path")
        ->default_str(defaults.speedtest);
    auto* server = cmd->add_option("--server-id", f.server_id,
                                   "Pin the Ookla test server (see `speedtest -L`)")
                       ->check(CLI::PositiveNumber);
    cmd->add_option("--ping", f.ping_hosts, "Public host to ping (repeatable; replaces the list)")
        ->default_str(hosts);
    cmd->add_option("--dns-host", f.dns_host, "Host name whose lookup is timed")
        ->default_str(defaults.dns_host);
    cmd->add_flag("--require-wired,!--no-require-wired", f.require_wired,
                  "Skip speed tests over Wi-Fi (status skipped_wlan), or don't");
    return server;
}

/**
 * @brief Adds the flags only needed for recording (data directory,
 *        intervals, contract).
 * @param cmd The subcommand.
 * @param f   Where the values go.
 */
void add_recording_flags(CLI::App* cmd, Flags& f) {
    const core::Config defaults{};
    cmd->add_option("--data-dir", f.data_dir, "Where the data files go");
    cmd->add_option("--speed-interval", f.speed_interval, "Time between speed tests")
        ->default_str(core::format_duration(defaults.speed_interval));
    cmd->add_option("--connectivity-interval", f.connectivity_interval,
                    "Time between connectivity probes")
        ->default_str(core::format_duration(defaults.connectivity_interval));
    cmd->add_option("--contract-down", f.contract_down,
                    "Contract download speeds MAX/NORMAL/MIN in Mbit/s");
    cmd->add_option("--contract-up", f.contract_up,
                    "Contract upload speeds MAX/NORMAL/MIN in Mbit/s");
}

/**
 * @brief The flags that were actually given on @p cmd.
 * @param cmd The parsed subcommand.
 * @param f   The bound values.
 * @return The overrides.
 */
Overrides overrides_from(const CLI::App* cmd, const Flags& f) {
    Overrides o{};
    // Only ask for flags this subcommand has; CLI11 throws for unknown names.
    const auto given = [cmd](const char* name) {
        const auto* opt = cmd->get_option_no_throw(name);
        return opt != nullptr && opt->count() > 0;
    };
    if (given("--config")) o.config_file = f.config_file;
    if (given("--data-dir")) o.data_dir = f.data_dir;
    if (given("--speed-interval")) o.speed_interval = f.speed_interval;
    if (given("--connectivity-interval")) o.connectivity_interval = f.connectivity_interval;
    if (given("--speedtest")) o.speedtest = f.speedtest;
    if (given("--server-id")) o.server_id = f.server_id;
    if (given("--ping")) o.ping_hosts = f.ping_hosts;
    if (given("--dns-host")) o.dns_host = f.dns_host;
    if (given("--require-wired")) o.require_wired = f.require_wired;
    if (given("--contract-down")) o.contract_down = f.contract_down;
    if (given("--contract-up")) o.contract_up = f.contract_up;
    return o;
}

/// Longest `meerkat pause`: the official campaign spans 3 days, but the
/// app is normally seen running anyway; this is only the fallback.
constexpr std::chrono::hours kMaxPause{72};

/**
 * @brief Adds the flags of `pause` and `resume`: where the data directory is.
 * @param cmd The subcommand.
 * @param f   Where the values go.
 */
void add_data_dir_flags(CLI::App* cmd, Flags& f) {
    cmd->add_option("-c,--config", f.config_file,
                    "Config file (default: ~/.config/meerkat/config.toml, then "
                    "/etc/meerkat/config.toml)");
    cmd->add_option("--data-dir", f.data_dir, "The data directory of the running meerkat");
}

/**
 * @brief `meerkat pause --for <duration>`: writes the pause file.
 * @param data_dir The data directory.
 * @param length   The `--for` text.
 * @param out      Where to report.
 * @param err      Where errors go.
 * @return 0, 1 if the file can't be written, kConfigError for a bad length.
 */
int pause_command(const std::filesystem::path& data_dir, const std::string& length,
                  std::ostream& out, std::ostream& err) {
    using namespace std::chrono;
    const auto d = core::parse_duration(length);
    if (!d || *d < minutes{1} || *d > kMaxPause) {
        err << "meerkat: --for must be a duration from 1m to " << kMaxPause.count()
            << "h, e.g. 30m or 2h (got '" << core::text::visible(length) << "')\n";
        return kConfigError;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        err << "meerkat: no data directory " << core::text::visible(data_dir.string())
            << " (is this the one `meerkat run` records into? see --data-dir)\n";
        return 1;
    }
    const auto until = floor<seconds>(core::now()) + *d;
    try {
        store::write_pause(data_dir, until);
    } catch (const std::exception& e) {
        err << "meerkat: " << core::text::visible(e.what()) << '\n';
        return 1;
    }
    out << "Speed tests pause until " << core::to_iso8601(until) << " (" << core::format_duration(*d)
        << " from now). The running meerkat sees it at its next round.\n"
        << "`meerkat resume` ends the pause early. While the Breitbandmessung app runs,\n"
        << "meerkat pauses by itself; this is for when it can't see the app.\n";
    return 0;
}

/**
 * @brief `meerkat resume`: removes the pause file.
 * @param data_dir The data directory.
 * @param out      Where to report.
 * @param err      Where errors go.
 * @return 0, or 1 if there's no data directory or the pause file can't be
 *         removed.
 */
int resume_command(const std::filesystem::path& data_dir, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        err << "meerkat: no data directory " << core::text::visible(data_dir.string())
            << " (is this the one `meerkat run` records into? see --data-dir)\n";
        return 1;
    }
    try {
        out << (store::remove_pause(data_dir)
                    ? "Pause ended; speed tests resume at the next slot.\n"
                    : "No pause was set.\n");
    } catch (const std::exception& e) {
        err << "meerkat: " << core::text::visible(e.what()) << '\n';
        return 1;
    }
    return 0;
}

/**
 * @brief Prints why the config can't be used.
 * @param r   The report.
 * @param err Where to print.
 */
void print_config_problems(const ConfigReport& r, std::ostream& err) {
    if (!r.load_error.empty()) {
        err << "meerkat: " << core::text::visible(r.load_error) << '\n';
    }
    for (const auto& p : r.problems) {
        err << "meerkat: " << core::text::visible(p) << '\n';
    }
    err << "Run `meerkat check` for details.\n";
}

}  // namespace

int run(std::span<const char* const> args, std::ostream& out, std::ostream& err) {
    CLI::App app{"meerkat: records evidence when your internet is slower than your contract",
                 "meerkat"};
    app.set_version_flag("-V,--version", std::string{"meerkat "} + std::string{kVersion});
    app.require_subcommand(0, 1);

    Flags flags;
    bool no_speed = false;
    auto* once = app.add_subcommand("once", "Measure the connection once and print the result");
    auto* once_server = add_probe_flags(once, flags);
    once->add_flag("--no-speed", no_speed, "Skip the speed test (only route, ping and DNS)")
        ->excludes(once_server);

    auto* run_cmd = app.add_subcommand(
        "run", "Record until stopped (Ctrl+C, SIGTERM): speed tests, connectivity, outages");
    add_probe_flags(run_cmd, flags);
    add_recording_flags(run_cmd, flags);

    auto* check = app.add_subcommand(
        "check", "Check the config, the tools, the data directory and the connection");
    add_probe_flags(check, flags);
    add_recording_flags(check, flags);

    std::string pause_for;
    auto* pause = app.add_subcommand(
        "pause", "Pause speed tests for an official measurement (fallback when the "
                 "Breitbandmessung app isn't detected)");
    add_data_dir_flags(pause, flags);
    pause->add_option("--for", pause_for, "How long, e.g. 30m or 2h (1m to 72h)")->required();

    auto* resume = app.add_subcommand("resume", "End a pause set with `meerkat pause`");
    add_data_dir_flags(resume, flags);

    try {
        app.parse(static_cast<int>(args.size()), args.data());
    } catch (const CLI::ParseError& e) {
        // Help and version requests also arrive here, with exit code 0.
        // CLI11 repeats the bad argument in its message: print it visibly.
        std::ostringstream message;
        const int code = app.exit(e, out, message);
        core::text::for_each_line(message.str(), [&err](std::string_view line) {
            err << core::text::visible(line) << '\n';
        });
        return code;
    }

    if (once->parsed()) {
        const auto s = resolve_settings(overrides_from(once, flags));
        if (!s.report.usable()) {
            print_config_problems(s.report, err);
            return kConfigError;
        }
        OnceOptions o = options_from(s.config);
        o.speed = !no_speed;
        return run_once(o, out, err);
    }
    if (run_cmd->parsed()) {
        const auto s = resolve_settings(overrides_from(run_cmd, flags));
        if (!s.report.usable()) {
            print_config_problems(s.report, err);
            return kConfigError;
        }
        const char* journal = std::getenv("JOURNAL_STREAM");
        Log log{err, detect_log_style(2, journal == nullptr ? "" : journal), core::now,
                [] { return terminal_columns(2); }};
        core::SystemClock clock;
        RunEnv env;
        env.probes = system_probes(options_from(s.config));
        env.clock = &clock;
        env.inhibit = sys::logind_inhibit;
        return run_service(s.config, std::move(env), log);
    }
    if (pause->parsed() || resume->parsed()) {
        const auto s = resolve_settings(overrides_from(pause->parsed() ? pause : resume, flags));
        if (!s.report.usable()) {
            print_config_problems(s.report, err);
            return kConfigError;
        }
        return pause->parsed() ? pause_command(s.config.data_dir, pause_for, out, err)
                               : resume_command(s.config.data_dir, out, err);
    }
    if (check->parsed()) {
        const auto s = resolve_settings(overrides_from(check, flags));
        const auto items = run_checks(s.config, s.report, system_check_env());
        print_checks(items, out);
        if (!s.report.usable()) {
            return kConfigError;
        }
        return has_failure(items) ? 1 : 0;
    }
    out << app.help();
    return 0;
}

}  // namespace meerkat::app
