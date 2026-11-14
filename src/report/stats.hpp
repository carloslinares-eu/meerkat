/**
 * @file stats.hpp
 * @brief What `meerkat report` concludes from the data: summaries, the
 *        contract comparison and the three BNetzA rules.
 *
 * The rules (Allgemeinverfügung of the Bundesnetzagentur, Dec 2021; see
 * PLAN.md §4) are checked separately for download and upload. Over three
 * measurement days, a deviation exists if any one holds:
 * 1. 90 % of the maximum speed isn't reached at least once on each of 2 of
 *    the 3 days;
 * 2. the normally available speed isn't reached in 90 % of the tests;
 * 3. the speed is below the minimum at least once on each of 2 of the 3
 *    days.
 *
 * The measurement days of a window are chosen as the official campaign
 * requires (Bundesnetzagentur, "Internetgeschwindigkeit"): three calendar
 * days with at least ten measurements each, at least one calendar day
 * without measurement between two of them, all within 14 days. meerkat
 * uses every valid test of such a day, not just ten.
 *
 * Only `ok` tests over a cable count (LinkType::wired): Wi-Fi, VPN and
 * links meerkat can't classify are summarised apart and never compared
 * with the contract.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "core/config.hpp"
#include "report/data.hpp"

namespace meerkat::report {

/// Valid cable tests a calendar day needs to be a measurement day: the
/// official campaign takes ten a day.
inline constexpr std::size_t kMinTestsPerDay = 10;

/// The most calendar days between the first and the last measurement day of
/// one window: the official campaign's 30 measurements are done within 14
/// days of its start, so day 1 to day 14.
inline constexpr std::chrono::days kWindowSpan{13};

/// The least calendar days from one measurement day to the next: the
/// official campaign needs at least one calendar day without measurement
/// between two measurement days.
inline constexpr std::chrono::days kDayGap{2};

/// A moment as the report shows it: local calendar day and hour.
struct LocalTime {
    std::chrono::sys_days day{};  ///< The local date (as a sys_days value).
    int hour = 0;                 ///< 0–23, local.
};

/// Converts a UTC time to LocalTime; tests pass a fixed one.
using LocalOf = std::function<LocalTime(core::Timestamp)>;

/**
 * @brief The system's time zone (`TZ`, `/etc/localtime`).
 * @return The conversion; UTC if the zone can't be found.
 */
LocalOf system_local();

/**
 * @brief UTC as the local time.
 * @return The conversion.
 */
LocalOf utc_local();

/// Spread of a set of values.
struct Summary {
    std::size_t count = 0;            ///< Values.
    std::optional<double> min;        ///< Smallest.
    std::optional<double> p10;        ///< 10th percentile.
    std::optional<double> median;     ///< 50th percentile.
    std::optional<double> p90;        ///< 90th percentile.
    std::optional<double> max;        ///< Largest.
    std::optional<double> mean;       ///< Average.
};

/**
 * @brief Summarises @p values (percentiles interpolated linearly between
 *        the nearest ranks).
 * @param values The values, in any order.
 * @return The summary; only `count` = 0 if there are none.
 */
Summary summarize(std::vector<double> values);

/// The contract's speeds as test thresholds, in Mbit/s.
struct Thresholds {
    double max90 = 0;    ///< 90 % of the maximum speed (rule 1).
    double normal = 0;   ///< The normally available speed (rule 2).
    double minimum = 0;  ///< The minimum speed (rule 3).

    /**
     * @brief From the contract figures.
     * @param c The figures.
     * @return The thresholds.
     */
    static Thresholds from(const core::ContractSpeeds& c);
};

/// One direction on one local calendar day.
struct DayVerdict {
    std::chrono::sys_days day{};     ///< The local date.
    std::size_t tests = 0;           ///< Valid cable tests.
    bool measurement_day = false;    ///< At least kMinTestsPerDay tests.
    bool reached_max90 = false;      ///< A test reached 90 % of the maximum.
    std::size_t reached_normal = 0;  ///< Tests at or above the normal speed.
    bool below_minimum = false;      ///< A test was below the minimum.
    std::optional<double> best;      ///< Fastest test.
    std::optional<double> worst;     ///< Slowest test.
};

/// The three rules over one window of three measurement days.
struct WindowVerdict {
    std::array<std::chrono::sys_days, 3> days{};  ///< The measurement days, in order.
    std::size_t tests = 0;               ///< Valid cable tests on them.
    double normal_share = 0;             ///< Share of them at or above the normal speed.
    bool rule_max90 = false;             ///< Rule 1 holds: deviation.
    bool rule_normal = false;            ///< Rule 2 holds: deviation.
    bool rule_minimum = false;           ///< Rule 3 holds: deviation.

    /// @return True if any rule holds.
    [[nodiscard]] bool deviation() const { return rule_max90 || rule_normal || rule_minimum; }
};

/// One direction (download or upload) of the cable tests.
struct DirectionReport {
    Summary speeds;                        ///< All valid cable tests.
    std::optional<Thresholds> thresholds;  ///< Empty without contract figures.
    std::size_t below_max90 = 0;           ///< Tests below 90 % of the maximum.
    std::size_t below_normal = 0;          ///< Tests below the normal speed.
    std::size_t below_minimum = 0;         ///< Tests below the minimum.
    std::vector<DayVerdict> days;          ///< Every local day with a test, in order.
    /// One window per measurement day that can start one: that day, then
    /// the next measurement day at least kDayGap later, then the next one
    /// at least kDayGap after that, all within kWindowSpan. With a test
    /// every day: days 1/3/5, 2/4/6, … Empty without thresholds.
    std::vector<WindowVerdict> windows;

    /// @return The number of windows with a deviation.
    [[nodiscard]] std::size_t deviating_windows() const;
};

/// One local hour of the day, over all days.
struct HourBucket {
    std::size_t tests = 0;                 ///< Valid cable tests.
    std::optional<double> median_down;     ///< Median download.
    std::optional<double> median_up;       ///< Median upload.
    std::size_t below_normal_down = 0;     ///< Downloads below the normal speed.
    std::size_t below_normal_up = 0;       ///< Uploads below the normal speed.
};

/// Tests over one kind of link that doesn't count (Wi-Fi, tunnel, unknown).
struct LinkGroup {
    core::LinkType type = core::LinkType::unknown;  ///< The kind.
    std::size_t tests = 0;                          ///< Valid tests.
    Summary download;                               ///< Their downloads.
    Summary upload;                                 ///< Their uploads.
};

/// The outages in short.
struct OutageSummary {
    std::size_t count = 0;                           ///< Outages.
    std::chrono::seconds total{0};                   ///< Their summed duration.
    std::chrono::seconds longest{0};                 ///< The longest.
    std::map<core::OutageScope, std::size_t> by_scope;  ///< How many per scope.
};

/// Everything the report states.
struct Analysis {
    Period period;                                  ///< What it covers.
    /// Speed rows per status, cable rows only (`ok` included).
    std::map<core::Status, std::size_t> wired_status;
    /// Speed rows per status over other links.
    std::map<core::Status, std::size_t> other_status;
    DirectionReport download;                       ///< Download, cable tests.
    DirectionReport upload;                         ///< Upload, cable tests.
    std::array<HourBucket, 24> hours{};             ///< By local hour.
    std::vector<LinkGroup> other_links;             ///< Wi-Fi, tunnel, unknown (those present).
    OutageSummary outages;                          ///< From the connectivity probes.
};

/**
 * @brief Analyses a period.
 * @param speed    The speed tests (load_speed()).
 * @param outages  The outages (load_outages()).
 * @param contract The contract figures; a direction without them gets no
 *                 thresholds and no windows.
 * @param local    Local day and hour of a time.
 * @param period   The period: speed rows outside it are left out.
 * @return The analysis.
 */
Analysis analyse(const SpeedData& speed, const OutageData& outages,
                 const core::Contract& contract, const LocalOf& local, const Period& period);

}  // namespace meerkat::report
