/**
 * @file html.hpp
 * @brief The report page: one self-contained HTML file that prints well to
 *        PDF (`meerkat report`).
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/time.hpp"
#include "report/advice.hpp"
#include "report/data.hpp"
#include "report/stats.hpp"
#include "store/manifest.hpp"

namespace meerkat::report {

/// One day's manifest as the report lists it.
struct ManifestInfo {
    std::string day;                   ///< `YYYY-MM-DD` (UTC).
    std::string sha256;                ///< Of the manifest file; empty if it can't be read.
    std::optional<store::Manifest> manifest;  ///< Its content; empty if missing or unreadable.
    std::string problem;               ///< Why it's missing or unreadable; empty if fine.
};

/// What the report says about itself and the setup, besides the data.
struct ReportMeta {
    Period period;                           ///< The period, in UTC.
    std::chrono::sys_days first_day{};       ///< First local day of it.
    std::chrono::sys_days last_day{};        ///< Last local day of it (inclusive).
    std::string zone;                        ///< Name of the local time zone, e.g. `Europe/Berlin`.
    core::Timestamp generated{};             ///< When the report was made.
    std::string version;                     ///< meerkat's version.
    core::Contract contract;                 ///< The contract figures used.
    std::chrono::seconds speed_interval{300};  ///< Time between speed tests (for the chart gaps).
    std::vector<ManifestInfo> manifests;     ///< The manifests of the UTC days the period touches.
    /// When to run the official campaign (advise()); empty without contract
    /// figures, and then the section is left out.
    std::optional<Advice> advice;
};

/**
 * @brief Renders the report.
 *
 * Sections: the verdict per direction, the summary table, the charts
 * (speed over time, by hour of day, outages), the per-day and window
 * tables, the outage list, tests over other links (Wi-Fi, VPN, unknown) with
 * a warning, the measurement setup (interfaces, link speeds, test servers),
 * the advice for the official campaign (if `meta.advice`), the data
 * quality counts and the manifest hashes. Times are local
 * (@p local, named by `meta.zone`). The page has no scripts and loads
 * nothing; it is light when printed and follows the viewer's dark mode on
 * screen.
 * @param meta     The period and the setup.
 * @param speed    The speed tests of the period (load_speed()).
 * @param outages  The outages (load_outages()).
 * @param analysis The analysis of both (analyse()).
 * @param local    Local day, hour and offset.
 * @return The HTML document.
 */
std::string render_html(const ReportMeta& meta, const SpeedData& speed, const OutageData& outages,
                        const Analysis& analysis, const LocalOf& local);

}  // namespace meerkat::report
