/**
 * @file cli.hpp
 * @brief Command-line entry point: parses arguments and dispatches subcommands.
 */
#pragma once

#include <iosfwd>
#include <span>

/**
 * @namespace meerkat::app
 * @brief Command-line front end: argument parsing and the `meerkat <command>`
 *        implementations.
 */
namespace meerkat::app {

/**
 * @brief Runs meerkat with the given command line.
 *
 * Kept separate from `main()` so the tests can call it with their own
 * arguments and capture the output.
 *
 * @param args The arguments, including the program name at index 0
 *             (like `argv`).
 * @param out  Stream for normal output.
 * @param err  Stream for error messages.
 * @return The process exit code: 0 on success; 1 if `once` found the
 *         computer offline (see run_once()) or `check` found a problem;
 *         2 if the config file or a config flag is invalid; another
 *         non-zero value on a usage error.
 */
int run(std::span<const char* const> args, std::ostream& out, std::ostream& err);

}  // namespace meerkat::app
