/**
 * @file ping.hpp
 * @brief Round-trip time and packet loss with the system `ping`.
 */
#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "core/measurement.hpp"
#include "sys/process.hpp"

/**
 * @namespace meerkat::probe
 * @brief Measurements: ping, DNS lookup and speed tests.
 */
namespace meerkat::probe {

/// How to ping.
struct PingOptions {
    int count = 5;                                ///< Echo requests to send (1–100).
    std::chrono::milliseconds interval{200};      ///< Gap between requests (≥ 200 ms, the unprivileged minimum).
    std::chrono::seconds reply_timeout{1};        ///< How long to wait for each reply.
};

/**
 * @brief Parses the statistics at the end of `ping` output (run with `LC_ALL=C`).
 *
 * Reads `N packets transmitted, M received[, +E errors][, +D duplicates], …`
 * and, if present, `rtt min/avg/max/mdev = a/b/c/d ms`. With 100 % loss there
 * is no rtt line, so the round-trip values stay empty.
 *
 * @param text The command's stdout.
 * @return A result with status `ok` if the statistics line was found, else
 *         status `error`.
 */
core::PingResult parse_ping(std::string_view text);

/**
 * @brief Checks that @p target can safely be passed to `ping`.
 *
 * Non-empty, at most 253 characters, no whitespace or control characters,
 * and not starting with `-` (which `ping` would read as an option). An IPv6
 * zone (`fe80::1%wlp4s0`) is allowed.
 *
 * @param target Host name or address.
 * @return True if it is acceptable.
 */
bool is_valid_ping_target(std::string_view target);

/**
 * @brief Pings @p target and returns loss and round-trip times.
 *
 * Runs `ping -n -c N -i I -W R -w D <target>` (numeric output, a hard
 * deadline D) through sys::run without a shell, in the C locale.
 *
 * Status mapping:
 * - `ok`: the run completed; that includes partial or total loss (look at
 *   PingResult::loss_percent()). A line that is down shows up as 100 % loss.
 * - `no_network`: `ping` couldn't even send (`Network is unreachable`).
 * - `not_available`: `ping` isn't installed.
 * - `timeout`: `ping` overran its own deadline.
 * - `error`: anything else (e.g. a host name that doesn't resolve, or the OS
 *   refusing to start a process).
 *
 * @param target  Address or host name; see is_valid_ping_target().
 * @param options Count, interval and reply timeout.
 * @param run     How to run the command (tests pass a fake).
 * @return The result; `target` is always filled in. Never throws.
 */
core::PingResult ping(std::string_view target, const PingOptions& options = {},
                      const sys::Runner& run = sys::run);

}  // namespace meerkat::probe
