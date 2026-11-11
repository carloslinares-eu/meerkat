/**
 * @file measurement.hpp
 * @brief The records meerkat measures and stores.
 *
 * Rule for every field: a value that wasn't measured is empty
 * (`std::nullopt` or an empty string), never 0. Only `Status::ok` results are
 * guaranteed to have their main values filled in.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/status.hpp"
#include "core/time.hpp"

namespace meerkat::core {

/**
 * @brief Converts bytes per second to megabits per second (10^6 bit/s).
 *
 * The Ookla CLI reports `bandwidth` in bytes/s; contracts are in Mbit/s.
 *
 * @param bytes_per_second Throughput in bytes per second.
 * @return Throughput in Mbit/s.
 */
constexpr double bytes_per_second_to_mbps(double bytes_per_second) {
    return bytes_per_second * 8.0 / 1'000'000.0;
}

/// Wi-Fi details of the active interface, from `iw dev <if> link`.
struct WifiInfo {
    std::string ssid;                          ///< Network name.
    std::string bssid;                         ///< Access point MAC address.
    std::optional<double> frequency_mhz;       ///< Channel frequency.
    std::optional<double> signal_dbm;          ///< Signal strength (negative; closer to 0 is better).
    std::optional<double> rx_bitrate_mbps;     ///< Current receive PHY rate.
    std::optional<double> tx_bitrate_mbps;     ///< Current transmit PHY rate.
};

/// Where and how the computer was connected when a measurement ran.
struct NetworkContext {
    LinkType link_type = LinkType::unknown;    ///< Cable, Wi-Fi or unknown.
    std::string interface;                     ///< Interface of the default route, e.g. `enp0s25`.
    std::string gateway;                       ///< Default gateway address; empty if none.
    std::optional<double> link_speed_mbps;     ///< Ethernet link speed from sysfs (wired only).
    std::optional<WifiInfo> wifi;              ///< Wi-Fi details (Wi-Fi only).
};

/// Result of one `ping` run against one target.
struct PingResult {
    Status status = Status::error;             ///< How the run ended.
    std::string error;                         ///< Human-readable reason when not `ok`.
    std::string target;                        ///< Address or host name pinged.
    std::optional<int> sent;                   ///< Echo requests sent.
    std::optional<int> received;               ///< Echo replies received.
    std::optional<double> rtt_min_ms;          ///< Fastest round trip.
    std::optional<double> rtt_avg_ms;          ///< Average round trip.
    std::optional<double> rtt_max_ms;          ///< Slowest round trip.
    std::optional<double> rtt_mdev_ms;         ///< Round-trip deviation (jitter).

    /**
     * @brief Packet loss in percent, computed from sent and received.
     * @return Loss in [0, 100], or `std::nullopt` if nothing was sent or the
     *         counts are invalid.
     */
    [[nodiscard]] std::optional<double> loss_percent() const;
};

/// Result of one DNS lookup.
struct DnsResult {
    Status status = Status::error;             ///< How the lookup ended.
    std::string error;                         ///< Human-readable reason when not `ok`.
    std::string host;                          ///< Name that was resolved.
    std::optional<double> duration_ms;         ///< Time the lookup took.
};

/// Result of one full speed test.
struct SpeedResult {
    Status status = Status::error;             ///< How the test ended.
    std::string error;                         ///< Human-readable reason when not `ok`.
    std::string backend;                       ///< Which tool measured, e.g. `ookla`.
    std::optional<double> download_mbps;       ///< Download throughput.
    std::optional<double> upload_mbps;         ///< Upload throughput.
    std::optional<double> latency_ms;          ///< Idle latency to the test server.
    std::optional<double> jitter_ms;           ///< Idle jitter.
    std::optional<double> packet_loss_percent; ///< Loss reported by the backend.
    std::optional<std::int64_t> server_id;     ///< Backend's server id.
    std::string server_name;                   ///< Server operator / name.
    std::string server_location;               ///< Server city.
    std::string isp;                           ///< ISP as seen by the backend.
    std::string external_ip;                   ///< Public address used.
    std::string result_url;                    ///< Link to the result at the backend, if any.
    std::string raw;                           ///< Unmodified backend output, for the audit log.
};

/**
 * @brief One measurement record: when, over what, and what came out.
 * @tparam Result The result type: PingResult, DnsResult or SpeedResult.
 */
template <typename Result>
struct Measurement {
    Timestamp time{};                          ///< Start of the measurement (UTC).
    NetworkContext context;                    ///< Connection at that time.
    Result result;                             ///< What was measured.
};

}  // namespace meerkat::core
