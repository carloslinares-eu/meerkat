/**
 * @file once.hpp
 * @brief `meerkat once`: one connectivity and speed measurement, printed for
 *        a person to read.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.hpp"
#include "core/measurement.hpp"
#include "core/time.hpp"
#include "net/sense.hpp"
#include "probe/dns.hpp"
#include "probe/ping.hpp"
#include "sys/proc_list.hpp"
#include "sys/process.hpp"

namespace meerkat::app {

/// What `meerkat once` measures.
struct OnceOptions {
    bool speed = true;                                  ///< Run a speed test (`--no-speed` turns it off).
    std::optional<std::int64_t> server_id;              ///< Pinned Ookla server.
    std::string speedtest = "speedtest";                ///< Ookla CLI program name or path.
    /// Public hosts to ping besides the gateway (Cloudflare and Quad9 by default).
    std::vector<std::string> ping_hosts{"1.1.1.1", "9.9.9.9"};
    bool ping_gateway = true;                           ///< Also ping the default gateway.
    bool require_wired = false;                         ///< Over Wi-Fi, record `skipped_wlan` instead of testing.
    std::string dns_host = "example.com";               ///< Name whose lookup is timed.
    /// Program names of the official measurement app: while one runs, the
    /// speed test doesn't start, and it is stopped if one starts. Empty:
    /// don't look.
    std::vector<std::string> official_programs{"breitbandmessung"};
    /// The data directory, for a pause set with `meerkat pause`; empty: no
    /// pause file is read.
    std::filesystem::path data_dir;
};

/**
 * @brief The measuring functions `meerkat once` uses.
 *
 * Each one is replaceable so the tests can run without a network. An empty
 * `speed` means no speed test.
 */
struct Probes {
    net::LinkSensors sensors;                                      ///< Route, link and Wi-Fi details.
    std::function<core::PingResult(std::string_view)> ping;        ///< Pings one target.
    std::function<core::DnsResult(std::string_view)> dns;          ///< Times one lookup.
    /// Runs one speed test; the StopCheck may end it early.
    std::function<core::SpeedResult(const sys::StopCheck&)> speed;
    std::function<core::Timestamp()> now = core::now;              ///< The wall clock.
    /// The running processes, for core::OfficialGuard; empty: none are seen.
    std::function<std::vector<sys::ProcessEntry>()> processes;
    /// Longest one `ping` call can take (the system probe's process timeout
    /// plus the SIGTERM → SIGKILL grace); sets the outage tracker's gap limit.
    std::chrono::milliseconds ping_limit = probe::ping_time_limit() + sys::Command{}.kill_grace;
    std::chrono::milliseconds dns_limit = probe::kDnsTimeout;      ///< Longest one `dns` call can take.
};

/**
 * @brief The real probes: /proc, /sys, `iw`, `ping`, getaddrinfo() and the
 *        Ookla CLI.
 * @param options Server id, program path and whether to run a speed test.
 * @return The probes.
 */
Probes system_probes(const OnceOptions& options);

/**
 * @brief The measuring options in a config (`speed` stays on).
 * @param config The config.
 * @return The options.
 */
OnceOptions options_from(const core::Config& config);

/// One ping run with a label for the printout.
struct PingEntry {
    std::string label;          ///< `gateway` or `public`.
    core::PingResult result;    ///< The run.
};

/// Everything `meerkat once` found.
struct OnceReport {
    core::Timestamp time{};                     ///< Start (UTC).
    bool has_route = false;                     ///< A default route existed.
    /// The link is usable: a route exists and the interface has a carrier
    /// (or the carrier is unknown). False means offline; no speed test runs.
    bool link_up = false;
    core::NetworkContext context;               ///< Interface, gateway, link type and speed, Wi-Fi.
    std::optional<bool> carrier;                ///< Link state from sysfs.
    std::string wifi_error;                     ///< Why the Wi-Fi details are missing, if they are.
    std::vector<PingEntry> pings;               ///< Gateway first, then the public hosts.
    core::DnsResult dns;                        ///< The timed lookup.
    std::optional<core::SpeedResult> speed;     ///< Empty if no speed test was asked for.
    std::vector<std::string> warnings;          ///< Things that weaken the result as evidence.
};

/**
 * @brief Takes one measurement.
 *
 * Order: route → link → Wi-Fi details (Wi-Fi only) → ping the gateway (unless
 * OnceOptions::ping_gateway is off) and the public hosts → DNS lookup → speed
 * test. Over Wi-Fi with OnceOptions::require_wired set, the speed test isn't
 * started and is recorded as `skipped_wlan`. Without a default route, or when
 * the interface has no carrier (cable unplugged, Wi-Fi not associated), the
 * speed test isn't started and is recorded as `no_network`; the pings and the
 * DNS lookup still run, so their failure is on record.
 *
 * Like `meerkat run`, it stands back for an official measurement (task
 * 2.10): while the Breitbandmessung app runs or a `meerkat pause` is set,
 * the speed test isn't started (`paused_official`, "not started: …"); if
 * the app starts during the test, the test is stopped ("stopped: …") unless
 * it finished anyway.
 *
 * Warnings: offline (no route, no carrier), the route's interface is missing
 * from sysfs, the connection is Wi-Fi (results don't count against the
 * provider), the link type can't be determined (bridge, VPN, …), or the
 * pause file can't be read (it is then ignored).
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
 * use `.` as the decimal point. The stream's formatting flags are left as
 * they were.
 *
 * @param r   The report.
 * @param out Where to print.
 */
void print_report(const OnceReport& r, std::ostream& out);

/**
 * @brief Runs `meerkat once` with the real probes.
 * @param options What to measure.
 * @param out     Report output.
 * @param err     Progress and warnings.
 * @return 0 if the internet was reachable; 1 if offline: no default route,
 *         no carrier, or no public host answered a single ping.
 * @note Warnings go to @p err as soon as they're known (before the slow
 *       speed test) and are repeated under the report, so a report saved
 *       from stdout keeps them.
 */
int run_once(const OnceOptions& options, std::ostream& out, std::ostream& err);

}  // namespace meerkat::app
