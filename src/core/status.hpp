/**
 * @file status.hpp
 * @brief Outcome of a measurement and the kind of link it ran over.
 */
#pragma once

#include <optional>
#include <string>
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
    skipped_tunnel,   ///< Not run because traffic goes through a VPN/tunnel and `require_wired` is set.
    /// The speed-test service refused: too many tests (Ookla: "Limit reached"),
    /// or not started while meerkat waits for that limit to pass. Never a
    /// measurement of the line.
    rate_limited,
    // New values go here, at the end, and kLastStatus moves with them.
};

/// The highest Status value; parse_status() tries every value up to it.
inline constexpr Status kLastStatus = Status::rate_limited;

/**
 * @brief The physical kind of the interface a measurement ran over.
 *
 * The text form is stored in the CSVs; see to_string(LinkType).
 */
enum class LinkType {
    unknown,  ///< Couldn't be determined.
    wired,    ///< Ethernet (cable to the router): what evidence should use.
    wifi,     ///< Wi-Fi (WLAN): providers will blame it, so it is flagged.
    /// A VPN or tunnel (WireGuard, OpenVPN tun/tap, GRE, IPIP): the results
    /// measure the VPN as much as the line, so it is flagged like Wi-Fi.
    tunnel,
};

/// The highest LinkType value; parse_link_type() tries every value up to it.
inline constexpr LinkType kLastLinkType = LinkType::tunnel;

/**
 * @brief Returns the stable text form of a status, e.g. `"no_network"`.
 * @param s The status.
 * @return The name, identical to the enumerator.
 */
std::string_view to_string(Status s);

/**
 * @brief Status word, plus the error text when there is one.
 * @param status The status.
 * @param error  Its error text.
 * @return For example `ok` or `timeout: no answer within 5000 ms`.
 */
std::string status_text(Status status, const std::string& error);

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

/// How the `error` of a speed test that was never started begins; the
/// report tells such rows from runs that did happen by it (#126).
inline constexpr std::string_view kNotStarted = "not started: ";

/**
 * @brief The `error` of a speed test that was never started.
 * @param why Why, e.g. `no carrier`.
 * @return `not started: ` + @p why.
 */
inline std::string not_started(std::string_view why) { return std::string{kNotStarted} + std::string{why}; }

/**
 * @brief Whether an `error` says the test was never started.
 * @param error The text.
 * @return True if it begins with kNotStarted.
 */
inline bool never_started(std::string_view error) { return error.starts_with(kNotStarted); }

}  // namespace meerkat::core
