/**
 * @file cli.cpp
 * @brief Implementation of the command-line entry point.
 */
#include "app/cli.hpp"

#include <CLI/CLI.hpp>

#include <ostream>
#include <string>
#include <vector>

#include "meerkat/version.hpp"

namespace meerkat::app {

int run(std::span<const char* const> args, std::ostream& out, std::ostream& err) {
    CLI::App app{"meerkat: records evidence when your internet is slower than your contract",
                 "meerkat"};
    app.set_version_flag("-V,--version", std::string{"meerkat "} + std::string{kVersion});
    app.require_subcommand(0, 1);

    // CLI11 wants the arguments without the program name, in reverse order.
    std::vector<std::string> rest;
    for (auto it = args.rbegin(); it != args.rend(); ++it) {
        if (it + 1 != args.rend()) {
            rest.emplace_back(*it);
        }
    }

    try {
        app.parse(rest);
    } catch (const CLI::ParseError& e) {
        // Help and version requests also arrive here, with exit code 0.
        return app.exit(e, out, err);
    }

    if (app.get_subcommands().empty()) {
        out << app.help();
    }
    return 0;
}

}  // namespace meerkat::app
