/**
 * @file status.hpp
 * @brief Outcome of a measurement and the kind of link it ran over.
 */
#pragma once

#include <optional>
#include <string_view>

namespace meerkat::core {

/**
 * @brief How a measurement ended.
 *
 * Every stored measurement carries one of these, so a failed test can never be
 * mistaken for a slow line. The text form (to_string()) is what goes into the
 * CSV files, so existing values must never be renamed.
 */
enum class Status {
    ok,               ///< Measured; the values are valid.
    timeout,          ///< The tool didn't finish in time.
    error,            ///< The tool failed or its output couldn't be parsed.
    no_network,       ///< No default route / no carrier: the line looked down.
    not_available,    ///< A required tool is missing (e.g. `speedtest` not installed).
    skipped_wlan,     ///< Not run because the link is Wi-Fi and `require_wired` is set.
    paused_official,  ///< Not run because the official Breitbandmessung app was measuring.
};

/**
 * @brief The physical kind of the interface a measurement ran over.
 *
 * The text form is stored in the CSVs; see to_string(LinkType).
 */
enum class LinkType {
    unknown,  ///< Couldn't be determined.
    wired,    ///< Ethernet (cable to the router): what evidence should use.
    wifi,     ///< Wi-Fi (WLAN): providers will blame it, so it is flagged.
};

/**
 * @brief Returns the stable text form of a status, e.g. `"no_network"`.
 * @param s The status.
 * @return The name, identical to the enumerator.
 */
std::string_view to_string(Status s);

/**
 * @brief Parses the text form produced by to_string(Status).
 * @param text The text.
 * @return The status, or `std::nullopt` for unknown text.
 */
std::optional<Status> parse_status(std::string_view text);

/**
 * @brief Returns the stable text form of a link type: `"unknown"`, `"wired"`
 *        or `"wifi"`.
 * @param t The link type.
 * @return The name.
 */
std::string_view to_string(LinkType t);

/**
 * @brief Parses the text form produced by to_string(LinkType).
 * @param text The text.
 * @return The link type, or `std::nullopt` for unknown text.
 */
std::optional<LinkType> parse_link_type(std::string_view text);

}  // namespace meerkat::core
