/**
 * @file process.hpp
 * @brief Runs external programs safely: no shell, with a timeout, output captured.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @namespace meerkat::sys
 * @brief Operating-system services: processes, and later sleep inhibition.
 */
namespace meerkat::sys {

/// What to run and how. Arguments are passed to the program as-is; nothing is
/// ever interpreted by a shell.
struct Command {
    std::string program{};                   ///< Program name (searched, see find_executable()) or path.
    std::vector<std::string> args{};         ///< Arguments, without the program name.
    std::chrono::milliseconds timeout{30'000};   ///< Wall-clock limit; then SIGTERM.
    std::chrono::milliseconds kill_grace{2'000}; ///< Time between SIGTERM and SIGKILL.
    /// Environment variables to set or override, e.g. `{"LC_ALL", "C"}`. The
    /// rest of meerkat's environment is inherited.
    std::vector<std::pair<std::string, std::string>> env{};
    std::size_t max_output = 4u << 20;       ///< Bytes kept per stream; the rest is read and dropped.
};

/// How a run ended and what it printed.
struct ProcessResult {
    /// How the program ended.
    enum class Outcome {
        exited,        ///< Exited normally; see exit_code.
        signaled,      ///< Killed by a signal it didn't get from us; see signal.
        timed_out,     ///< Ran past the timeout and was stopped by us; see signal.
        spawn_failed,  ///< Couldn't be started; see error.
    };

    Outcome outcome = Outcome::spawn_failed; ///< How it ended.
    int exit_code = -1;                      ///< Exit status when `exited`, else -1.
    int signal = 0;                          ///< Terminating signal when `signaled`/`timed_out`.
    bool not_found = false;                  ///< `spawn_failed` because the program doesn't exist.
    std::string error;                       ///< Why it couldn't be started (`spawn_failed`).
    std::string out;                         ///< Captured stdout (up to Command::max_output).
    std::string err;                         ///< Captured stderr (up to Command::max_output).
    bool out_truncated = false;              ///< stdout was longer than max_output.
    bool err_truncated = false;              ///< stderr was longer than max_output.
    std::chrono::milliseconds duration{0};   ///< Wall-clock run time.

    /**
     * @brief True if the program exited normally with status 0.
     * @return Whether the run succeeded.
     */
    [[nodiscard]] bool ok() const { return outcome == Outcome::exited && exit_code == 0; }
};

/**
 * @brief Something that runs a Command; normally sys::run().
 *
 * Probes take a Runner so tests can hand them canned output (from the
 * fixtures) instead of running real tools.
 */
using Runner = std::function<ProcessResult(const Command&)>;

/**
 * @brief Finds an executable the way a shell would, plus the sbin directories.
 *
 * A name containing `/` is checked as a path. Otherwise each directory of
 * @p path_env is tried, then each of @p extra_dirs. The sbin directories are
 * the default extras because on Debian tools like `iw` live in `/usr/sbin`,
 * which isn't on a normal user's PATH.
 *
 * @param name       Program name or path.
 * @param path_env   Colon-separated directory list (normally `$PATH`).
 * @param extra_dirs Directories tried after @p path_env.
 * @return The path of the first regular, executable file, or `std::nullopt`.
 */
std::optional<std::filesystem::path> find_executable(
    std::string_view name, std::string_view path_env,
    const std::vector<std::filesystem::path>& extra_dirs = {"/usr/local/sbin", "/usr/sbin",
                                                            "/sbin"});

/**
 * @brief Runs a program to completion (or timeout) and captures its output.
 *
 * - The program is looked up with find_executable() using the current `$PATH`.
 * - stdin is `/dev/null`; stdout and stderr are captured separately.
 * - It runs in its own process group. On timeout the whole group gets
 *   SIGTERM, and SIGKILL after Command::kill_grace.
 * - It is killed (SIGKILL) if meerkat dies (`PR_SET_PDEATHSIG`), so no
 *   orphan is left behind. The kernel drops this for set-user-ID programs and
 *   programs with file capabilities; those must limit themselves (e.g.
 *   `ping -c N -w S`). On Debian 13, `ping` is neither.
 * - If run() itself fails (an exception), the program's group is killed and
 *   the program is reaped before the exception propagates.
 * - Command::timeout and Command::kill_grace are capped at one year.
 * - If the program exits but something it started keeps the output pipes
 *   open, the rest of its process group is killed after a short grace.
 *
 * Safe to call from several threads at once.
 *
 * @param cmd What to run.
 * @return The outcome; never throws for problems with the program itself.
 * @throws std::system_error if the operating system refuses to create pipes
 *         or processes (out of resources).
 */
ProcessResult run(const Command& cmd);

}  // namespace meerkat::sys
