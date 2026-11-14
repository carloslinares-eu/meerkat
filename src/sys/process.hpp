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
 * @brief Operating-system services: processes and sleep inhibition.
 */
namespace meerkat::sys {

/**
 * @brief Asked by run() while the program runs (about every 250 ms): true
 *        stops it. Must be quick; a check that is costly throttles itself.
 *        Exceptions count as false.
 */
using StopCheck = std::function<bool()>;

/**
 * @brief What to run and how.
 *
 * Arguments are passed to the program as-is; nothing is ever interpreted by
 * a shell.
 */
struct Command {
    std::string program{};                   ///< Program name (searched, see find_executable()) or path.
    std::vector<std::string> args{};         ///< Arguments, without the program name.
    std::chrono::milliseconds timeout{30'000};   ///< Wall-clock limit; then SIGTERM.
    std::chrono::milliseconds kill_grace{2'000}; ///< Time between SIGTERM and SIGKILL.
    /// Environment variables to set or override, e.g. `{"LC_ALL", "C"}`. The
    /// rest of meerkat's environment is inherited.
    std::vector<std::pair<std::string, std::string>> env{};
    std::size_t max_output = 4u << 20;       ///< Bytes kept per stream; the rest is read and dropped.
    /// Stops the program early when it returns true: SIGTERM, then SIGKILL
    /// after kill_grace, as on timeout; the outcome is `cancelled`. Empty:
    /// never asked.
    StopCheck stop{};
};

/**
 * @brief How a run ended and what it printed.
 */
struct ProcessResult {
    /**
     * @brief How the program ended.
     */
    enum class Outcome {
        /// Exited normally; see exit_code (-1 if the status was lost because
        /// someone else reaped the child; `error` says so).
        exited,
        signaled,      ///< Killed by a signal it didn't get from us; see signal.
        /// Ran past the timeout (or didn't get to exec() in time) and was
        /// stopped by us, however it then ended; see signal.
        timed_out,
        spawn_failed,  ///< Couldn't be started; see error.
        /// Stopped by us because Command::stop asked for it, however it
        /// then ended; see signal.
        cancelled,
    };

    Outcome outcome = Outcome::spawn_failed; ///< How it ended.
    int exit_code = -1;                      ///< Exit status when `exited` (and known), else -1.
    /// Terminating signal when `signaled`, or when `timed_out`/`cancelled`
    /// and the program died of one (0 if it exited after our SIGTERM).
    int signal = 0;
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
 * - Command::timeout and Command::kill_grace are capped at one year. The
 *   timeout starts before exec(), so a child stopped before exec can't
 *   block run() either. A program that doesn't die within 5 s of SIGKILL
 *   (stuck in the kernel, e.g. exec() from a hung NFS mount) is left
 *   behind: run() returns `timed_out` with `error` saying so, and the
 *   process is still killed by kill_running_children() and at meerkat's
 *   exit.
 * - If the program exits but something it started keeps the output pipes
 *   open, the rest of its process group is killed after a short grace.
 * - Signals only ever go to the program's own process group: the program
 *   is reaped last, so its pid (the group id) can't be reused meanwhile.
 *   That holds as long as nothing else in meerkat reaps children
 *   (`waitpid(-1)`); with SIGCHLD ignored, no signal is sent after the exit.
 *
 * Safe to call from several threads at once.
 *
 * @param cmd What to run.
 * @return The outcome; never throws for problems with the program itself.
 * @throws std::system_error if the operating system refuses to create pipes
 *         or processes (out of resources).
 */
ProcessResult run(const Command& cmd);

/**
 * @brief Kills (SIGKILL) the process group of every program run() has
 *        started and not reaped yet, grandchildren included.
 *
 * For a forced exit: the programs would otherwise keep running (a speed
 * test using the line) after meerkat is gone. Their run() calls then
 * return as `signaled`. Thread-safe; not async-signal-safe.
 */
void kill_running_children() noexcept;

}  // namespace meerkat::sys
