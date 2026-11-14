/**
 * @file official.hpp
 * @brief What `meerkat run` and `meerkat once` both ask before and during a
 *        speed test: is an official measurement under way?
 */
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/time.hpp"
#include "sys/proc_list.hpp"
#include "sys/process.hpp"

namespace meerkat::app {

/// How often a running speed test asks the official guard whether the
/// Breitbandmessung app has started (each ask scans /proc).
inline constexpr std::chrono::seconds kOfficialPoll{2};

/// What core::OfficialGuard decides on, read from the system.
struct OfficialInputs {
    std::vector<std::string> running;          ///< Official-app programs running now.
    std::optional<core::Timestamp> pause_until; ///< End of a manual pause, if one is set.
    std::string pause_error;                   ///< Why the pause file is unusable; empty if it is fine.
};

/**
 * @brief Looks for the official app and the pause file.
 * @param programs  Program names of the official app (`[official] programs`);
 *                  empty: don't look.
 * @param data_dir  The data directory holding the pause file; empty, or a
 *                  directory that doesn't exist: no pause.
 * @param processes Lists the running processes; empty: none are seen.
 * @return What was found. A pause file that can't be read is reported in
 *         `pause_error` and counts as no pause.
 */
OfficialInputs read_official(const std::vector<std::string>& programs,
                             const std::filesystem::path& data_dir,
                             const std::function<std::vector<sys::ProcessEntry>()>& processes);

/**
 * @brief A stop check that asks @p ask at most once per @p poll.
 *
 * The first call within @p poll of @p start answers false without asking.
 * If the clock goes back, the next call asks.
 * @param now   The clock.
 * @param start When the test started.
 * @param poll  Least time between two asks.
 * @param ask   True if the test must stop.
 * @return The stop check.
 */
sys::StopCheck polled_stop(std::function<core::Timestamp()> now, core::Timestamp start,
                           std::chrono::seconds poll, std::function<bool()> ask);

}  // namespace meerkat::app
