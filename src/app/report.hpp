/**
 * @file report.hpp
 * @brief `meerkat report`: writes the evidence report for a range of days.
 */
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>

#include "core/config.hpp"
#include "core/time.hpp"
#include "report/stats.hpp"

namespace meerkat::app {

/// A time zone as the report needs it.
struct Zone {
    std::string name;      ///< E.g. `Europe/Berlin`; shown in the report.
    report::LocalOf local; ///< Local day, hour and offset of a time.
    /// The first moment of a local day, in UTC (on a day that starts with a
    /// DST gap: the end of the gap).
    std::function<core::Timestamp(std::chrono::sys_days)> midnight;
};

/**
 * @brief The system's time zone (`TZ`, `/etc/localtime`).
 * @return The zone; UTC if it can't be found.
 */
Zone system_zone();

/// @return UTC.
Zone utc_zone();

/// What `meerkat report` was asked for.
struct ReportOptions {
    std::string from;                ///< `--from YYYY-MM-DD`; empty: the first day with data.
    std::string to;                  ///< `--to YYYY-MM-DD` (included); empty: today.
    std::filesystem::path output;    ///< `-o`; empty: `meerkat-report-FROM-to-TO.html`.
};

/// What the command depends on besides the config; tests replace it.
struct ReportEnv {
    Zone zone = system_zone();                          ///< Local time.
    std::function<core::Timestamp()> now = core::now;   ///< The clock.
    std::string version;                                ///< meerkat's version, for the page.
};

/// The local days a report covers, both included.
struct ReportRange {
    std::chrono::sys_days from{};  ///< First day.
    std::chrono::sys_days to{};    ///< Last day.
};

/**
 * @brief The days `--from`/`--to` ask for, with their defaults: from the
 *        first speed test (at most 366 days back) to today.
 * @param config  The settings (the data directory, for the first test).
 * @param options `--from` and `--to`.
 * @param env     Zone and clock.
 * @param err     Where a bad date or range is explained.
 * @return The range, or nothing (explained on @p err) for a bad date, an
 *         empty range or one over 366 days.
 */
std::optional<ReportRange> report_range(const core::Config& config, const ReportOptions& options,
                                        const ReportEnv& env, std::ostream& err);

/// A rendered report.
struct BuiltReport {
    std::string html;            ///< The page.
    std::string summary;         ///< The verdict lines and the outage count, one per line.
    bool speed_file = false;     ///< Whether speed.csv exists.
};

/**
 * @brief Reads the data directory and renders the report for @p range.
 * @param config The settings.
 * @param range  The days.
 * @param env    Zone, clock and version.
 * @return The report.
 * @throws std::exception if the data can't be read.
 */
BuiltReport build_report(const core::Config& config, const ReportRange& range, const ReportEnv& env);

/**
 * @brief Whether @p output would land in the data directory.
 * @param output   The output file.
 * @param data_dir The data directory.
 * @return True if its directory is @p data_dir or below it.
 */
bool inside_data_dir(const std::filesystem::path& output, const std::filesystem::path& data_dir);

/**
 * @brief `YYYY-MM-DD` of a day.
 * @param d The day.
 * @return The text.
 */
std::string day_text(std::chrono::sys_days d);

/**
 * @brief Writes the report.
 *
 * Reads the data directory (without locking it: `meerkat run` may keep
 * recording), analyses the local days from `--from` to `--to` and writes
 * one HTML file. The file is written to a temporary name next to it first
 * and renamed, so a half-written report never replaces a good one; it is
 * readable by its owner only (it has the contract figures in it). The range
 * is at most 366 days, and the file can't go into the data directory.
 * @param config  The settings (data directory, intervals, contract).
 * @param options The range and the output file.
 * @param env     Zone, clock and version.
 * @param out     Where the summary goes.
 * @param err     Where errors go.
 * @return 0; 1 if the data can't be read or the file can't be written; 2
 *         for a bad date, an empty or too long range, or an output file in
 *         the data directory.
 */
int report_command(const core::Config& config, const ReportOptions& options, const ReportEnv& env,
                   std::ostream& out, std::ostream& err);

}  // namespace meerkat::app
