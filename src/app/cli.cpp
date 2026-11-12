/**
 * @file cli.cpp
 * @brief Implementation of the command-line entry point.
 */
#include "app/cli.hpp"

#include <CLI/CLI.hpp>

#include <ostream>
#include <string>
#include <vector>

#include "app/check.hpp"
#include "app/once.hpp"
#include "app/settings.hpp"
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
    const core::Config defaults;
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
    Overrides o;
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

/**
 * @brief Prints why the config can't be used.
 * @param r   The report.
 * @param err Where to print.
 */
void print_config_problems(const ConfigReport& r, std::ostream& err) {
    if (!r.load_error.empty()) {
        err << "meerkat: " << r.load_error << '\n';
    }
    for (const auto& p : r.problems) {
        err << "meerkat: " << p << '\n';
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

    auto* check = app.add_subcommand(
        "check", "Check the config, the tools, the data directory and the connection");
    add_probe_flags(check, flags);
    add_recording_flags(check, flags);

    try {
        app.parse(static_cast<int>(args.size()), args.data());
    } catch (const CLI::ParseError& e) {
        // Help and version requests also arrive here, with exit code 0.
        return app.exit(e, out, err);
    }

    if (once->parsed()) {
        const auto s = resolve_settings(overrides_from(once, flags));
        if (!s.report.usable()) {
            print_config_problems(s.report, err);
            return kConfigError;
        }
        const auto& c = s.config;
        OnceOptions o;
        o.speed = !no_speed;
        o.server_id = c.server_id;
        o.speedtest = c.speedtest;
        o.ping_hosts = c.ping_hosts;
        o.ping_gateway = c.ping_gateway;
        o.dns_host = c.dns_host;
        o.require_wired = c.require_wired;
        return run_once(o, out, err);
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
