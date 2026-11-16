/**
 * @file advise.hpp
 * @brief `meerkat advise-bbm`: when to run the official Breitbandmessung
 *        campaign, from meerkat's data.
 */
#pragma once

#include <chrono>
#include <ostream>
#include <string>

#include "app/report.hpp"
#include "core/config.hpp"
#include "report/advice.hpp"

namespace meerkat::app {

/// What `meerkat advise-bbm` was asked for.
struct AdviseOptions {
    std::string from;   ///< `--from YYYY-MM-DD`; empty: 28 days before `--to`.
    std::string to;     ///< `--to YYYY-MM-DD` (included); empty: today.
    std::string start;  ///< `--start YYYY-MM-DD`, the campaign's first possible day; empty: tomorrow.
};

/// Days of data the advice uses by default: four weeks, each weekday 4 times.
inline constexpr std::chrono::days kAdviceSpan{28};

/**
 * @brief The advice as text: what it is based on, the worst hours and
 *        weekdays, the suggested days and hours, and the campaign rules.
 * @param a     The advice.
 * @param range The local days of data it is based on.
 * @param zone  Name of the local time zone the hours are in.
 * @return The text.
 */
std::string advice_text(const report::Advice& a, const ReportRange& range, const std::string& zone);

/**
 * @brief Runs `meerkat advise-bbm`.
 * @param config  The settings (data directory, contract).
 * @param options The data range and the start day.
 * @param env     Zone and clock.
 * @param out     Where the advice goes.
 * @param err     Where errors go.
 * @return 0; 1 if the data can't be read; 2 for a bad date or range, or
 *         without contract figures.
 */
int advise_command(const core::Config& config, const AdviseOptions& options, const ReportEnv& env,
                   std::ostream& out, std::ostream& err);

}  // namespace meerkat::app
