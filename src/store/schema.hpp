/**
 * @file schema.hpp
 * @brief The layouts of `speed.csv` and `connectivity.csv`, and the functions
 *        that turn measurements into their rows.
 *
 * Text forms: timestamps are ISO 8601 UTC (`2026-11-11T20:07:36Z`), statuses
 * and link types use their to_string() names, numbers use `.` as the decimal
 * point regardless of the locale. A value that wasn't measured is an empty
 * field, never 0.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/measurement.hpp"
#include "store/csv.hpp"

namespace meerkat::store {

/**
 * @brief The layout of `speed.csv`: one row per speed test.
 *
 * Columns: time, status, error, backend, link_type, interface, gateway,
 * link_speed_mbps, wifi_ssid, wifi_bssid, wifi_frequency_mhz,
 * wifi_signal_dbm, wifi_rx_bitrate_mbps, wifi_tx_bitrate_mbps,
 * download_mbps, upload_mbps, latency_ms, jitter_ms, packet_loss_percent,
 * server_id, server_name, server_location, isp, external_ip, result_url.
 * The backend's raw output isn't stored here (see the raw log).
 *
 * @return The schema (version 1).
 */
const Schema& speed_schema();

/**
 * @brief The layout of `connectivity.csv`: one row per ping target or DNS
 *        lookup.
 *
 * Columns: time, probe (`ping` or `dns`), target, status, error, link_type,
 * interface, gateway, sent, received, loss_percent, rtt_min_ms, rtt_avg_ms,
 * rtt_max_ms, rtt_mdev_ms, duration_ms. Ping rows leave duration_ms empty;
 * DNS rows leave the ping columns empty.
 *
 * @return The schema (version 1).
 */
const Schema& connectivity_schema();

/**
 * @brief Formats a measured number for a CSV field.
 * @param v        The value, or empty.
 * @param decimals Digits after the decimal point.
 * @return The number with `.` as the decimal point, or an empty string if
 *         @p v is empty, NaN or infinite.
 */
std::string format_number(std::optional<double> v, int decimals);

/**
 * @brief The `speed.csv` row for one speed test.
 * @param m The measurement.
 * @return One field per column of speed_schema().
 */
std::vector<std::string> speed_row(const core::Measurement<core::SpeedResult>& m);

/**
 * @brief The `connectivity.csv` row for one ping run.
 * @param m The measurement.
 * @return One field per column of connectivity_schema().
 */
std::vector<std::string> ping_row(const core::Measurement<core::PingResult>& m);

/**
 * @brief The `connectivity.csv` row for one DNS lookup.
 * @param m The measurement.
 * @return One field per column of connectivity_schema().
 */
std::vector<std::string> dns_row(const core::Measurement<core::DnsResult>& m);

}  // namespace meerkat::store
