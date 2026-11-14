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
    std::chrono::milliseconds interval{200};      ///< Gap between requests (200 ms – 10 s; 200 ms is the unprivileged minimum).
    std::chrono::seconds reply_timeout{1};        ///< How long to wait for each reply.
};

/**
 * @brief Parses the statistics at the end of `ping` output (run with `LC_ALL=C`).
 *
 * Reads `N packets transmitted, M received[, +E errors][, +D duplicates], …`
 * and, if present, `rtt min/avg/max/mdev = a/b/c/d ms` (iputils) or
 * `round-trip min/avg/max = a/b/c ms` (BusyBox, no mdev). With 100 % loss
 * there is no rtt line, so the round-trip values stay empty. Numbers are
 * parsed without the locale (`.` is always the decimal point).
 *
 * @param text The command's stdout.
 * @return A result with status `ok` if the statistics line was found, else
 *         status `error`.
 */
core::PingResult parse_ping(std::string_view text);

/**
 * @brief Checks that @p target can safely be passed to `ping`.
 *
 * Non-empty, at most 253 characters, no whitespace or control characters
 * (core::text::is_host_text()), and not starting with `-` (which `ping` would read as an option). An IPv6
 * zone (`fe80::1%wlp4s0`) is allowed.
 *
 * @param target Host name or address.
 * @return True if it is acceptable.
 */
bool is_valid_ping_target(std::string_view target);

/**
 * @brief The process timeout of a ping() with @p options: N requests, the
 *        last reply wait and 5 s slack. The process is then stopped, which
 *        can take another sys::Command::kill_grace.
 * @param options Count, interval and reply timeout (clamped as ping() does).
 * @return The limit.
 */
std::chrono::milliseconds ping_time_limit(const PingOptions& options = {});

/**
 * @brief Pings @p target and returns loss and round-trip times.
 *
 * Runs `ping -n -c N -i I -W R <target>` (numeric output, exactly N
 * requests) through sys::run without a shell, in the C locale. The process is
 * stopped if it runs 5 s longer than N requests plus the last reply wait.
 * For a host name that includes ping's own lookup, so with a DNS server that
 * doesn't answer the result can be `timeout` (still a failed ping for the
 * outage tracker); use address literals for the connectivity probes.
 * (`-w` is deliberately not used: combined with `-c`, iputils keeps sending
 * past N until it has N replies, which would distort the loss.)
 *
 * Status mapping:
 * - `ok`: statistics were printed; that includes partial or total loss (see
 *   PingResult::loss_percent()). A line that is down shows up as 100 % loss.
 *   If ping also reported an error (exit code 2), its text is in `error`
 *   (repeated lines once; "ping exited with code 2" if stderr was empty).
 * - `no_network`: `ping` couldn't even send (`Network is unreachable`). Also
 *   when the route went away during the run: `Network is unreachable` on
 *   stderr, no reply, and `+N errors` for every request sent; the counts
 *   are kept.
 * - `not_available`: `ping` isn't installed.
 * - `timeout`: `ping` ran far longer than it should.
 * - `error`: anything else (e.g. a host name that doesn't resolve, also
 *   `Temporary failure in name resolution`, or the OS refusing to start a
 *   process). See core::LinkSnapshot::probe_status() for the offline case.
 *
 * @param target  Address or host name; see is_valid_ping_target().
 * @param options Count, interval and reply timeout.
 * @param run     How to run the command (tests pass a fake).
 * @return The result; `target` is always filled in. Never throws.
 */
core::PingResult ping(std::string_view target, const PingOptions& options = {},
                      const sys::Runner& run = sys::run);

}  // namespace meerkat::probe
