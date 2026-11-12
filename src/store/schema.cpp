/**
 * @file schema.cpp
 * @brief The CSV layouts and the row builders.
 */
#include "store/schema.hpp"

#include <array>
#include <charconv>
#include <cmath>

namespace meerkat::store {

namespace {

/**
 * @brief Formats an optional integer.
 * @tparam T Integer type.
 * @param v  The value, or empty.
 * @return The decimal text, or an empty string.
 */
template <typename T>
std::string format_int(const std::optional<T>& v) {
    return v ? std::to_string(*v) : std::string{};
}

/// Decimals for rates in Mbit/s.
constexpr int kMbps = 3;
/// Decimals for times in milliseconds.
constexpr int kMs = 3;
/// Decimals for percentages.
constexpr int kPercent = 3;

/**
 * @brief The context columns shared by both files: link_type, interface, gateway.
 * @param row Row to extend.
 * @param c   The network context.
 */
void append_context(std::vector<std::string>& row, const core::NetworkContext& c) {
    row.emplace_back(core::to_string(c.link_type));
    row.push_back(c.interface);
    row.push_back(c.gateway);
}

}  // namespace

const Schema& speed_schema() {
    static const Schema schema{
        .name = "speed",
        .version = 1,
        .columns = {"time", "status", "error", "backend", "link_type", "interface", "gateway",
                    "link_speed_mbps", "wifi_ssid", "wifi_bssid", "wifi_frequency_mhz",
                    "wifi_signal_dbm", "wifi_rx_bitrate_mbps", "wifi_tx_bitrate_mbps",
                    "download_mbps", "upload_mbps", "latency_ms", "jitter_ms",
                    "packet_loss_percent", "server_id", "server_name", "server_location", "isp",
                    "external_ip", "result_url"}};
    return schema;
}

const Schema& connectivity_schema() {
    static const Schema schema{
        .name = "connectivity",
        .version = 1,
        .columns = {"time", "probe", "target", "status", "error", "link_type", "interface",
                    "gateway", "sent", "received", "loss_percent", "rtt_min_ms", "rtt_avg_ms",
                    "rtt_max_ms", "rtt_mdev_ms", "duration_ms"}};
    return schema;
}

std::string format_number(std::optional<double> v, int decimals) {
    if (!v || !std::isfinite(*v)) {
        return {};
    }
    std::array<char, 64> buf{};
    const auto [end, ec] =
        std::to_chars(buf.data(), buf.data() + buf.size(), *v, std::chars_format::fixed, decimals);
    if (ec != std::errc{}) {
        return {};  // more than 64 characters: not a plausible measurement
    }
    std::string s{buf.data(), end};
    if (s.front() == '-' && s.find_first_not_of("-0.") == std::string::npos) {
        s.erase(0, 1);  // "-0.000" (a tiny negative value) reads as an error; store "0.000"
    }
    return s;
}

std::vector<std::string> speed_row(const core::Measurement<core::SpeedResult>& m) {
    const auto& r = m.result;
    const auto& c = m.context;
    std::vector<std::string> row;
    row.reserve(speed_schema().columns.size());
    row.push_back(core::to_iso8601(m.time));
    row.emplace_back(core::to_string(r.status));
    row.push_back(r.error);
    row.push_back(r.backend);
    append_context(row, c);
    row.push_back(format_number(c.link_speed_mbps, 0));
    if (c.wifi) {
        const auto& w = *c.wifi;
        row.push_back(w.ssid);
        row.push_back(w.bssid);
        row.push_back(format_number(w.frequency_mhz, 0));
        row.push_back(format_number(w.signal_dbm, 0));
        row.push_back(format_number(w.rx_bitrate_mbps, 1));
        row.push_back(format_number(w.tx_bitrate_mbps, 1));
    } else {
        row.insert(row.end(), 6, std::string{});
    }
    row.push_back(format_number(r.download_mbps, kMbps));
    row.push_back(format_number(r.upload_mbps, kMbps));
    row.push_back(format_number(r.latency_ms, kMs));
    row.push_back(format_number(r.jitter_ms, kMs));
    row.push_back(format_number(r.packet_loss_percent, kPercent));
    row.push_back(format_int(r.server_id));
    row.push_back(r.server_name);
    row.push_back(r.server_location);
    row.push_back(r.isp);
    row.push_back(r.external_ip);
    row.push_back(r.result_url);
    return row;
}

std::vector<std::string> ping_row(const core::Measurement<core::PingResult>& m) {
    const auto& r = m.result;
    std::vector<std::string> row;
    row.reserve(connectivity_schema().columns.size());
    row.push_back(core::to_iso8601(m.time));
    row.emplace_back("ping");
    row.push_back(r.target);
    row.emplace_back(core::to_string(r.status));
    row.push_back(r.error);
    append_context(row, m.context);
    row.push_back(format_int(r.sent));
    row.push_back(format_int(r.received));
    row.push_back(format_number(r.loss_percent(), kPercent));
    row.push_back(format_number(r.rtt_min_ms, kMs));
    row.push_back(format_number(r.rtt_avg_ms, kMs));
    row.push_back(format_number(r.rtt_max_ms, kMs));
    row.push_back(format_number(r.rtt_mdev_ms, kMs));
    row.emplace_back();  // duration_ms: DNS only
    return row;
}

std::vector<std::string> dns_row(const core::Measurement<core::DnsResult>& m) {
    const auto& r = m.result;
    std::vector<std::string> row;
    row.reserve(connectivity_schema().columns.size());
    row.push_back(core::to_iso8601(m.time));
    row.emplace_back("dns");
    row.push_back(r.host);
    row.emplace_back(core::to_string(r.status));
    row.push_back(r.error);
    append_context(row, m.context);
    row.insert(row.end(), 7, std::string{});  // sent … rtt_mdev_ms: ping only
    row.push_back(format_number(r.duration_ms, kMs));
    return row;
}

}  // namespace meerkat::store
