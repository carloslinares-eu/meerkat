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
