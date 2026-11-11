/**
 * @file once.hpp
 * @brief `meerkat once`: one connectivity and speed measurement, printed for
 *        a person to read.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/measurement.hpp"
#include "core/time.hpp"
#include "net/link.hpp"
#include "net/route.hpp"
#include "net/wifi.hpp"

namespace meerkat::app {

/// What `meerkat once` measures.
struct OnceOptions {
    bool speed = true;                                  ///< Run a speed test (`--no-speed` turns it off).
    std::optional<std::int64_t> server_id;              ///< Pinned Ookla server.
    std::string speedtest = "speedtest";                ///< Ookla CLI program name or path.
    /// Public hosts to ping besides the gateway (Cloudflare and Quad9 by default).
    std::vector<std::string> ping_hosts{"1.1.1.1", "9.9.9.9"};
    std::string dns_host = "example.com";               ///< Name whose lookup is timed.
};

/**
 * @brief The measuring functions `meerkat once` uses.
 *
 * Each one is replaceable so the tests can run without a network. An empty
 * `speed` means no speed test.
 */
struct Probes {
    std::function<std::optional<net::DefaultRoute>()> route;       ///< Finds the default route.
    std::function<net::LinkInfo(std::string_view)> link;           ///< Reads sysfs for an interface.
    std::function<net::WifiReading(std::string_view)> wifi;        ///< Reads Wi-Fi details.
    std::function<core::PingResult(std::string_view)> ping;        ///< Pings one target.
    std::function<core::DnsResult(std::string_view)> dns;          ///< Times one lookup.
    std::function<core::SpeedResult()> speed;                      ///< Runs one speed test.
};

/**
 * @brief The real probes: /proc, /sys, `iw`, `ping`, getaddrinfo() and the
 *        Ookla CLI.
 * @param options Server id, program path and whether to run a speed test.
 * @return The probes.
 */
Probes system_probes(const OnceOptions& options);

/// One ping run with a label for the printout.
struct PingEntry {
    std::string label;          ///< `gateway` or `public`.
    core::PingResult result;    ///< The run.
};

/// Everything `meerkat once` found.
struct OnceReport {
    core::Timestamp time{};                     ///< Start (UTC).
    bool has_route = false;                     ///< A default route existed.
    core::NetworkContext context;               ///< Interface, gateway, link type and speed, Wi-Fi.
    std::optional<bool> carrier;                ///< Link state from sysfs.
    std::string wifi_error;                     ///< Why the Wi-Fi details are missing, if they are.
    std::vector<PingEntry> pings;               ///< Gateway first, then the public hosts.
    std::optional<core::DnsResult> dns;         ///< The timed lookup.
    std::optional<core::SpeedResult> speed;     ///< Empty if no speed test was asked for.
    std::vector<std::string> warnings;          ///< Things that weaken the result as evidence.
};

/**
 * @brief Takes one measurement.
 *
 * Order: route → link → Wi-Fi details (Wi-Fi only) → ping the gateway and the
 * public hosts → DNS lookup → speed test. Without a default route the speed
 * test isn't started and is recorded as `no_network`; the pings and the DNS
 * lookup still run, so their failure is on record.
 *
 * Warnings: the connection is Wi-Fi (results don't count against the
 * provider), or the link type can't be determined (bridge, VPN, …).
 *
 * @param options  What to measure.
 * @param probes   How to measure.
 * @param progress Called with a short line before each slow step and with
 *                 each warning as soon as it is known; may be empty.
 * @return The report.
 */
OnceReport measure_once(const OnceOptions& options, const Probes& probes,
                        const std::function<void(std::string_view)>& progress = {});

/**
 * @brief Prints @p report for a person to read.
 *
 * Values that weren't measured are shown as `–`, never as 0. Numbers always
 * use `.` as the decimal point.
 *
 * @param report The report.
 * @param out    Where to print.
 */
void print_report(const OnceReport& report, std::ostream& out);

/**
 * @brief Runs `meerkat once` with the real probes.
 * @param options What to measure.
 * @param out     Report output.
 * @param err     Progress and warnings.
 * @return 0, or 1 if there was no default route (offline).
 */
int run_once(const OnceOptions& options, std::ostream& out, std::ostream& err);

}  // namespace meerkat::app
