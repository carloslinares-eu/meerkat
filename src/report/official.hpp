/**
 * @file official.hpp
 * @brief The official measurements in the report (task 3.7): loading
 *        `official.csv`, judging complete campaigns by the three BNetzA
 *        rules (the legal finding) and comparing each official result with
 *        meerkat's tests around it.
 *
 * The rules are the ones of stats.hpp, but read as the Bundesnetzagentur
 * wrote them, on the official data: a campaign is three measurement days of
 * ten measurements each, so a single measurement below the minimum counts
 * for its day. The thresholds are the contract figures entered in the app,
 * as the app's protocol uses them.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "probe/bbm.hpp"
#include "report/data.hpp"
#include "report/stats.hpp"

namespace meerkat::report {

/// How far around an official measurement meerkat's tests are compared with it.
inline constexpr std::chrono::minutes kCompareSpan{30};

/// An official result "agrees" with meerkat's if it is within this share
/// of the median of meerkat's tests around it. The app measures against
/// the Bundesnetzagentur's servers, meerkat against Ookla's, so a small
/// difference is normal.
inline constexpr double kAgreement = 0.15;

/// One row of `official.csv`.
struct OfficialRow {
    probe::OfficialMeasurement m;  ///< The measurement as the app exported it (time in UTC).
    std::string file;              ///< The export it came from, under `official/`.
};

/// The official measurements, all of them.
struct OfficialData {
    std::vector<OfficialRow> rows;  ///< In time order.
    std::uint64_t skipped = 0;      ///< Torn rows.
    std::uint64_t unreadable = 0;   ///< Rows whose time can't be read.
    bool file_exists = false;       ///< official.csv was there.
};

/**
 * @brief Reads `official.csv` (implemented with load_speed()).
 * @param data_dir The data directory.
 * @return The rows; empty (`file_exists` false) without the file.
 * @throws as load_speed().
 */
OfficialData load_official(const std::filesystem::path& data_dir);

/// One direction of one campaign: the legal finding.
struct OfficialDirection {
    /// The contract figures entered in the app; empty if the export has none.
    std::optional<core::ContractSpeeds> contract;
    /// The three days. `tests_below_minimum` > 0 counts for rule 3 here.
    std::array<DayVerdict, 3> days{};
    /// The rules; set only with a contract.
    WindowVerdict verdict;
    /// Days on which a measurement was below the minimum (rule 3 needs 2).
    int days_below_minimum = 0;
    /// Days on which 90 % of the maximum was never reached (rule 1 needs 2).
    int days_missing_max90 = 0;
};

/// One campaign: three measurement days of the official app.
struct OfficialCampaign {
    std::array<std::chrono::sys_days, 3> days{};  ///< Local dates, in order.
    std::size_t measurements = 0;                 ///< On those days.
    OfficialDirection download;                   ///< Download.
    OfficialDirection upload;                     ///< Upload.
    /// What doesn't look like a valid campaign (day spacing, Wi-Fi, battery,
    /// network card, contract figures that change); empty if nothing.
    std::vector<std::string> problems;
};

/// One official result next to meerkat's tests around it.
struct Comparison {
    core::Timestamp time{};                 ///< Of the official measurement.
    std::optional<double> official_down;    ///< Its download.
    std::optional<double> official_up;      ///< Its upload.
    std::size_t tests = 0;                  ///< meerkat's valid cable tests within kCompareSpan.
    std::optional<double> meerkat_down;     ///< Their median download.
    std::optional<double> meerkat_up;       ///< Their median upload.

    /// @return (official − meerkat) / meerkat for download, if both are there.
    [[nodiscard]] std::optional<double> difference_down() const;
    /// @return (official − meerkat) / meerkat for upload, if both are there.
    [[nodiscard]] std::optional<double> difference_up() const;
    /// @return Whether both directions there are agree within kAgreement;
    ///         empty if there is nothing to compare.
    [[nodiscard]] std::optional<bool> agrees() const;
};

/// What the report says about the official measurements.
struct OfficialAnalysis {
    std::vector<OfficialRow> rows;          ///< Those in the period, in time order.
    std::size_t outside = 0;                ///< Official measurements outside the period.
    /// Complete campaigns with a day in the period. Built from all official
    /// measurements, not just the period's: the local days with at least
    /// kMinTestsPerDay measurements, in order, up to three within
    /// kWindowSpan of the first; a day beyond that starts a new campaign.
    std::vector<OfficialCampaign> campaigns;
    /// Such days in the period that are in no complete campaign: one still
    /// running, or one given up.
    std::vector<std::chrono::sys_days> incomplete_days;
    /// Days with official measurements but fewer than kMinTestsPerDay.
    std::vector<std::chrono::sys_days> short_days;
    std::vector<Comparison> comparisons;    ///< One per row, in order.
    /// The contract entered in the app (from the latest measurement), for
    /// comparing with meerkat's.
    core::Contract app_contract;
    /// Rows of official.csv that couldn't be used: torn or with an
    /// unreadable time (OfficialData::skipped + unreadable).
    std::uint64_t unusable = 0;

    /// @return Whether there is anything to show.
    [[nodiscard]] bool any() const { return !rows.empty(); }
};

/**
 * @brief Analyses the official measurements in @p period.
 * @param official The rows (load_official()).
 * @param speed    meerkat's tests, for the comparison.
 * @param local    Local day of a time.
 * @param period   The report's period.
 * @return The analysis.
 */
OfficialAnalysis analyse_official(const OfficialData& official, const SpeedData& speed, const LocalOf& local,
                                  const Period& period);

}  // namespace meerkat::report
