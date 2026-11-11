/**
 * @file cli.cpp
 * @brief Implementation of the command-line entry point.
 */
#include "app/cli.hpp"

#include <CLI/CLI.hpp>

#include <ostream>
#include <string>

#include "app/once.hpp"
#include "meerkat/version.hpp"

namespace meerkat::app {

int run(std::span<const char* const> args, std::ostream& out, std::ostream& err) {
    CLI::App app{"meerkat: records evidence when your internet is slower than your contract",
                 "meerkat"};
    app.set_version_flag("-V,--version", std::string{"meerkat "} + std::string{kVersion});
    app.require_subcommand(0, 1);

    OnceOptions once_options;
    auto* once = app.add_subcommand("once", "Measure the connection once and print the result");
    auto* no_speed = once->add_flag_callback(
        "--no-speed", [&once_options] { once_options.speed = false; },
        "Skip the speed test (only route, ping and DNS)");
    once->add_option("--server-id", once_options.server_id,
                     "Pin the Ookla test server (see `speedtest -L`)")
        ->check(CLI::PositiveNumber)
        ->excludes(no_speed);
    once->add_option("--speedtest", once_options.speedtest, "Ookla CLI program name or path")
        ->capture_default_str();
    once->add_option("--ping", once_options.ping_hosts, "Public hosts to ping (repeatable)")
        ->capture_default_str();
    once->add_option("--dns-host", once_options.dns_host, "Host name whose lookup is timed")
        ->capture_default_str();

    try {
        app.parse(static_cast<int>(args.size()), args.data());
    } catch (const CLI::ParseError& e) {
        // Help and version requests also arrive here, with exit code 0.
        return app.exit(e, out, err);
    }

    if (once->parsed()) {
        return run_once(once_options, out, err);
    }
    out << app.help();
    return 0;
}

}  // namespace meerkat::app
