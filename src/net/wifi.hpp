/**
 * @file wifi.hpp
 * @brief Wi-Fi link details from `iw dev <interface> link`.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/measurement.hpp"
#include "sys/process.hpp"

namespace meerkat::net {

/**
 * @brief Parses the output of `iw dev <interface> link` (run with `LC_ALL=C`).
 *
 * Reads the BSSID (`Connected to …`), `SSID`, `freq`, `signal`,
 * `rx bitrate` and `tx bitrate`. Lines it doesn't know are ignored, and
 * missing values stay empty (older `iw` versions print no `rx bitrate`).
 *
 * @param text The command's stdout.
 * @return The details, or `std::nullopt` if the output says `Not connected.`
 *         or doesn't look like `iw` link output at all.
 */
std::optional<core::WifiInfo> parse_iw_link(std::string_view text);

/// Outcome of read_wifi().
struct WifiReading {
    core::Status status = core::Status::error; ///< How reading ended.
    std::string error;                         ///< Reason when not `ok`.
    std::optional<core::WifiInfo> info;        ///< The details when `ok`.
};

/**
 * @brief Runs `iw dev <interface> link` and parses it.
 *
 * Status mapping:
 * - `ok`: connected, details in `info`;
 * - `no_network`: the interface isn't associated with an access point;
 * - `not_available`: `iw` isn't installed (Debian: package `iw`, in /usr/sbin);
 * - `timeout` / `error`: the tool hung, failed, or printed something unexpected.
 *
 * Only meaningful for Wi-Fi interfaces (see read_link()): for an Ethernet
 * interface `iw` also answers `Not connected.`, which reads as `no_network`.
 *
 * @param interface Interface name; refused without running anything if it
 *                  fails is_valid_interface_name().
 * @param run       How to run the command (tests pass a fake).
 * @return The reading.
 */
WifiReading read_wifi(std::string_view interface, const sys::Runner& run = sys::run);

}  // namespace meerkat::net
