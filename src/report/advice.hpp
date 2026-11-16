/**
 * @file advice.hpp
 * @brief When to run the official Breitbandmessung campaign: the hours and
 *        days where meerkat's data shows the line most often below the
 *        contract (`meerkat advise-bbm`, and a section of the report).
 *
 * The campaign (Bundesnetzagentur, "Internetgeschwindigkeit"; PLAN.md §4):
 * 30 measurements, ten on each of three calendar days, at least one
 * calendar day without measurement between two measurement days, all within
 * 14 days, at least 5 minutes between measurements and at least 3 hours
 * between the 5th and the 6th of a day. So a day is two blocks of five,
 * which the advice places in the two worst hours that far apart.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/config.hpp"
#include "report/data.hpp"
#include "report/stats.hpp"

namespace meerkat::report {

/// Days of data the advice uses by default (ending today, or at the
/// report's last day): four weeks, each weekday four times.
inline constexpr std::chrono::days kAdviceSpan{28};

/// Valid cable tests a local hour (or weekday) needs before it is ranked.
inline constexpr std::size_t kMinSlotTests = 6;

/// Days of data each weekday needs before the weekdays are ranked (and
/// kMinSlotTests tests).
inline constexpr std::size_t kMinWeekdayDays = 2;

/// The least hours between the start of a day's two blocks: five tests at
/// 5-minute steps take about half an hour, then 3 hours must pass.
inline constexpr int kBlockGapHours = 4;

/// How a group of tests (one local hour, or one weekday) fared.
struct SlotStats {
    std::size_t tests = 0;          ///< Valid cable tests.
    std::size_t days = 0;           ///< Local days they fall on.
    std::size_t below_normal = 0;   ///< Tests below the normally available speed (either direction).
    std::size_t below_minimum = 0;  ///< Tests below the minimum speed (either direction).

    /// @return Whether there are enough tests to rank it (kMinSlotTests).
    bool ranked() const { return tests >= kMinSlotTests; }
    /// @return Share of its tests below the normal speed, plus share below
    ///         the minimum (0 to 2): how likely a measurement there helps
    ///         rules 2 and 3. 0 without tests.
    double score() const;
};

/// The advice.
struct Advice {
    Period period;                      ///< The data it is based on.
    std::chrono::sys_days first_day{};  ///< First local day of @ref period.
    std::chrono::sys_days last_day{};   ///< Last local day of @ref period.
    std::chrono::sys_days start{};      ///< The first day the campaign may start.
    std::size_t tests = 0;              ///< Valid cable tests in it.
    std::size_t days = 0;               ///< Local days with such a test.
    bool download = false;              ///< Download has contract figures (and counts).
    bool upload = false;                ///< Upload has contract figures (and counts).
    std::array<SlotStats, 24> hours{};  ///< By local hour.
    /// By local weekday, 0 = Sunday … 6 = Saturday (std::chrono::weekday).
    std::array<SlotStats, 7> weekdays{};
    /// The two hours (earlier first) to start a day's two blocks of five in:
    /// the best-scoring pair at least kBlockGapHours apart. Empty with too
    /// little data.
    std::optional<std::pair<int, int>> blocks;
    bool weekdays_ranked = false;       ///< Every weekday had kMinWeekdayDays days of data.
    /// Three measurement days from the start day on: each two calendar days
    /// apart or more, within 14 days; on the worst weekdays if ranked, else
    /// the earliest such days.
    std::array<std::chrono::sys_days, 3> plan{};

    /// @return Whether any test was below the normally available speed: if
    ///         not, the data shows no bad time and the hours are as good as any.
    bool any_below() const;
    /// @return What counts: `download`, `upload` or `download or upload`.
    std::string directions() const;
};

/**
 * @brief The worst ranked hours.
 * @param a The advice.
 * @param n How many at most.
 * @return Hours (0–23), worst first; ties: the earlier hour.
 */
std::vector<int> worst_hours(const Advice& a, std::size_t n);

/// Score differences smaller than this are ties (floating-point noise).
inline constexpr double kScoreEpsilon = 1e-9;

/**
 * @brief Works out the advice.
 * @param speed    The speed tests (load_speed()); rows outside @p period
 *                 and anything but `ok` cable tests are left out.
 * @param contract The contract figures; a direction without them doesn't
 *                 count.
 * @param local    Local day and hour of a time.
 * @param period   The data to use.
 * @param start    The first day the campaign may start (local).
 * @return The advice; with no contract figures at all, `download` and
 *         `upload` are both false and nothing is ranked.
 */
Advice advise(const SpeedData& speed, const core::Contract& contract, const LocalOf& local,
              const Period& period, std::chrono::sys_days start);

}  // namespace meerkat::report
