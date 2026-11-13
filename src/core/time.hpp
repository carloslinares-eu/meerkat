/**
 * @file time.hpp
 * @brief UTC timestamps and their ISO-8601 text form.
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

/**
 * @namespace meerkat::core
 * @brief Shared data model: timestamps, statuses and measurement records.
 */
namespace meerkat::core {

/// A point in time in UTC, with one-second resolution (what the CSVs store).
using Timestamp = std::chrono::sys_seconds;

/**
 * @brief Returns the current time, truncated to whole seconds.
 * @return The current UTC time.
 */
Timestamp now();

/**
 * @brief Formats a timestamp as ISO 8601 in UTC, e.g. `2026-11-11T20:07:36Z`.
 * @param t The timestamp.
 * @return The text form; always 20 characters for years 0000–9999.
 */
std::string to_iso8601(Timestamp t);

/**
 * @brief Formats the clock time of a timestamp in the local time zone, with
 *        the zone's abbreviation, e.g. `21:07:36 CET`.
 *
 * For people reading the log; stored data stays in UTC. The zone comes
 * from `$TZ` or `/etc/localtime`, as for `date`.
 * @param t The timestamp.
 * @return The text, or empty if the local time can't be worked out.
 */
std::string to_local_clock(Timestamp t);

/**
 * @brief Parses an ISO-8601 date-time and converts it to UTC.
 *
 * Accepts `YYYY-MM-DDTHH:MM:SS`, optionally followed by a fraction
 * (`.123`, dropped) and then a zone: `Z`, `+HH:MM`, `-HH:MM`, `+HHMM` or
 * `+HH`. A space or `t` is accepted instead of `T`, and `z` instead of `Z`
 * (RFC 3339). Seconds of `60` are only accepted at 23:59 UTC (a leap
 * second). Text without a zone is rejected,
 * because a local time without an offset is ambiguous around daylight-saving
 * changes.
 *
 * @param text The text to parse; surrounding whitespace is not allowed.
 * @return The UTC timestamp, or `std::nullopt` if the text isn't a valid
 *         date-time with a zone.
 */
std::optional<Timestamp> parse_iso8601(std::string_view text);

}  // namespace meerkat::core
