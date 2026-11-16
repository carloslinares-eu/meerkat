/**
 * @file format.hpp
 * @brief Small text formats shared by the report page and the commands
 *        that print report results: percentages, dates, hours.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "core/outage.hpp"
#include "core/time.hpp"

namespace meerkat::report {

/**
 * @brief A share as a percentage with one decimal.
 * @param part  The part.
 * @param total The whole.
 * @return E.g. `12.5 %`, or `–` without a total.
 */
inline std::string percent(std::size_t part, std::size_t total) {
    if (total == 0) {
        return "–";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f %%", 100.0 * static_cast<double>(part) / static_cast<double>(total));
    return buf;
}

/**
 * @brief A number with at least two digits.
 * @param v The number (0 or more).
 * @return E.g. `07`.
 */
inline std::string two(long v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02ld", v);
    return buf;
}

/**
 * @brief A day as `YYYY-MM-DD`.
 * @param d The day.
 * @return The text.
 */
inline std::string date(std::chrono::sys_days d) { return core::to_iso_day(d); }

/**
 * @brief The short name of a weekday.
 * @param c_encoding 0 = Sunday … 6 = Saturday.
 * @return `Sun` … `Sat`.
 */
inline std::string weekday_name(unsigned c_encoding) {
    static constexpr std::array<std::string_view, 7> kDays{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return std::string{kDays.at(c_encoding)};
}

/**
 * @brief A day with its weekday.
 * @param d The day.
 * @return E.g. `Tue 2026-03-10`.
 */
inline std::string weekday_date(std::chrono::sys_days d) {
    return weekday_name(std::chrono::weekday{d}.c_encoding()) + " " + date(d);
}

/**
 * @brief A local hour slot.
 * @param hour 0–23.
 * @return E.g. `19:00–20:00`.
 */
inline std::string hour_slot(int hour) {
    return two(hour) + ":00–" + two((hour + 1) % 24) + ":00";
}

/**
 * @brief A length of time, rounded down to whole units.
 * @param d The length.
 * @return E.g. `45 s`, `27 min`, `2 h 05 min`.
 */
inline std::string duration_text(std::chrono::seconds d) {
    const auto s = d.count();
    if (s < 60) {
        return std::to_string(s) + " s";
    }
    const auto m = s / 60;
    if (m < 60) {
        return std::to_string(m) + " min";
    }
    return std::to_string(m / 60) + " h " + two(m % 60) + " min";
}

/**
 * @brief What an outage's scope means, in the page and the chart.
 * @param s The scope.
 * @return E.g. `line (beyond the router)`.
 */
inline std::string scope_text(core::OutageScope s) {
    switch (s) {
    case core::OutageScope::internet: return "line (beyond the router)";
    case core::OutageScope::local:    return "local network";
    case core::OutageScope::unknown:  return "unclear";
    }
    return "unclear";
}

}  // namespace meerkat::report
