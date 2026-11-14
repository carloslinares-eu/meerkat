/**
 * @file official_guard.hpp
 * @brief Keeps meerkat from spoiling official measurements: pauses speed
 *        tests while the Breitbandmessung app runs or a manual pause is set.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/time.hpp"

namespace meerkat::core {

/// The guard's answer for one round.
struct OfficialCheck {
    /// Set while speed tests must not run: why, as the `error` text of the
    /// `paused_official` row (e.g. "the Breitbandmessung app is running
    /// (breitbandmessung)").
    std::optional<std::string> pause;
    /// Set when the decision changed since the last check: a sentence for
    /// the log (paused, why; or resumed).
    std::optional<std::string> notice;
};

/**
 * @brief Decides whether meerkat has to stand back for an official
 *        measurement (task 2.10).
 *
 * The Breitbandmessung app measures the line itself, and its results are
 * the legal finding. A meerkat speed test at the same time would take
 * bandwidth from it and lower its result. So speed tests pause:
 * - while one of the app's processes runs (the caller finds them, see
 *   sys::running_programs()), and
 * - until the time set with `meerkat pause --for <duration>`, the manual
 *   fallback for when the app can't be seen (another user's processes with
 *   `hidepid`, or the app on another computer on the same line).
 *
 * The guard itself is a pure state machine: it only remembers the last
 * decision, to report changes once.
 */
class OfficialGuard {
public:
    /**
     * @brief Decides about the current round.
     * @param running     Official-app programs running now (may be empty).
     * @param pause_until End of a manual pause, if one is set; a time at or
     *                    before @p now means no pause.
     * @param now         The current time.
     * @return The decision, and a notice if it changed.
     */
    OfficialCheck check(const std::vector<std::string>& running,
                        std::optional<Timestamp> pause_until, Timestamp now);

private:
    /// What a pause comes from.
    enum class Cause { none, app, manual };

    std::optional<std::string> last_;  ///< The pause reason at the last check.
    Cause last_cause_ = Cause::none;   ///< What caused it.
};

}  // namespace meerkat::core
