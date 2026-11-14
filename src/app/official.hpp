/**
 * @file official.hpp
 * @brief What `meerkat run` and `meerkat once` both ask before and during a
 *        speed test: is an official measurement under way?
 */
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/measurement.hpp"
#include "core/official_guard.hpp"
#include "core/time.hpp"
#include "sys/proc_list.hpp"
#include "sys/process.hpp"

namespace meerkat::app {

/// How often a running speed test asks the official guard whether the
/// Breitbandmessung app has started (each ask scans /proc).
inline constexpr std::chrono::seconds kOfficialPoll{2};

/**
 * @brief Watches for an official measurement: the official app's processes
 *        and the pause file, judged by core::OfficialGuard.
 *
 * Changes are reported once each: the guard's notices (paused, why;
 * resumed) and a pause file that becomes unusable (it then counts as no
 * pause). check() may be called from several threads.
 */
class OfficialWatch {
public:
    /// Lists the running processes.
    using ProcessLister = std::function<std::vector<sys::ProcessEntry>()>;
    /// Gets one message.
    using Sink = std::function<void(const std::string&)>;

    /**
     * @brief Sets up the watch.
     * @param programs  Program names of the official app (`[official]
     *                  programs`); empty: don't look.
     * @param data_dir  The data directory holding the pause file; empty, or
     *                  a directory that doesn't exist: no pause.
     * @param processes Lists the running processes; empty: none are seen.
     * @param notice    Gets the guard's notices; may be empty.
     * @param warning   Gets "the pause file is ignored: …"; may be empty.
     */
    OfficialWatch(std::vector<std::string> programs, std::filesystem::path data_dir,
                  ProcessLister processes, Sink notice, Sink warning);

    /**
     * @brief Looks now.
     * @param now The current time.
     * @return Why speed tests pause; nothing if they may run.
     */
    std::optional<std::string> check(core::Timestamp now);

private:
    std::vector<std::string> programs_;  ///< Official-app program names.
    std::filesystem::path data_dir_;     ///< Where the pause file is.
    ProcessLister processes_;            ///< Lists the processes.
    Sink notice_;                        ///< Gets notices.
    Sink warning_;                       ///< Gets warnings.
    std::mutex mutex_;                   ///< Guards guard_ and last_pause_error_.
    core::OfficialGuard guard_;          ///< The decision.
    std::string last_pause_error_;       ///< Why the pause file was last unusable.
};

/**
 * @brief The result of a speed test that didn't run, or was stopped, for an
 *        official measurement.
 * @param why     The `error` text ("not started: …", "stopped: …").
 * @param backend The backend, if it had started.
 * @return A `paused_official` result without figures.
 */
core::SpeedResult official_paused(std::string why, std::string backend = {});

/**
 * @brief Runs a speed test that stops when an official measurement starts.
 *
 * @p watch is asked at most every kOfficialPoll while the test runs. If it
 * says to pause and the backend stops the test for it
 * (core::SpeedResult::stopped), the result is official_paused() with
 * "stopped: …"; a test that finished or failed by itself is kept as it is.
 * @param speed The speed test.
 * @param watch The watch.
 * @param now   The clock.
 * @return The result.
 */
core::SpeedResult watched_speed_test(
    const std::function<core::SpeedResult(const sys::StopCheck&)>& speed, OfficialWatch& watch,
    const std::function<core::Timestamp()>& now);

}  // namespace meerkat::app
