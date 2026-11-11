/**
 * @file wifi.cpp
 * @brief Implementation of the `iw dev <interface> link` reader.
 */
#include "net/wifi.hpp"

#include <charconv>
#include <chrono>

#include "net/interface.hpp"

namespace meerkat::net {

namespace {

/**
 * @brief Removes leading and trailing spaces and tabs.
 * @param s The text.
 * @return The trimmed view.
 */
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/**
 * @brief Parses the leading number of @p s (e.g. `-58` in `-58 dBm`).
 * @param s The text, starting with the number.
 * @return The number, or `std::nullopt` if @p s doesn't start with one.
 */
std::optional<double> leading_number(std::string_view s) {
    double v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr == s.data()) {
        return std::nullopt;
    }
    return v;
}

/**
 * @brief If @p line is `key: value`, returns the trimmed value.
 * @param line The (trimmed) line.
 * @param key  The key, without the colon.
 * @return The value, or `std::nullopt` if the key doesn't match.
 */
std::optional<std::string_view> value_of(std::string_view line, std::string_view key) {
    if (line.size() <= key.size() || line.substr(0, key.size()) != key ||
        line[key.size()] != ':') {
        return std::nullopt;
    }
    return trim(line.substr(key.size() + 1));
}

}  // namespace

std::optional<core::WifiInfo> parse_iw_link(std::string_view text) {
    core::WifiInfo info;
    bool connected = false;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        constexpr std::string_view kConnected = "Connected to ";
        if (line.substr(0, kConnected.size()) == kConnected) {
            connected = true;
            const auto rest = line.substr(kConnected.size());
            info.bssid = std::string{rest.substr(0, rest.find(' '))};
        } else if (const auto v = value_of(line, "SSID")) {
            info.ssid = std::string{*v};
        } else if (const auto f = value_of(line, "freq")) {
            info.frequency_mhz = leading_number(*f);
        } else if (const auto sig = value_of(line, "signal")) {
            info.signal_dbm = leading_number(*sig);
        } else if (const auto rx = value_of(line, "rx bitrate")) {
            info.rx_bitrate_mbps = leading_number(*rx);
        } else if (const auto tx = value_of(line, "tx bitrate")) {
            info.tx_bitrate_mbps = leading_number(*tx);
        }
    }
    if (!connected) {
        return std::nullopt;
    }
    return info;
}

WifiReading read_wifi(std::string_view interface, const sys::Runner& run) {
    using namespace std::chrono_literals;
    WifiReading reading;
    if (!is_valid_interface_name(interface)) {
        reading.error = "invalid interface name";
        return reading;
    }
    const auto r = run({.program = "iw",
                        .args = {"dev", std::string{interface}, "link"},
                        .timeout = 5s,
                        .env = {{"LC_ALL", "C"}}});
    switch (r.outcome) {
        case sys::ProcessResult::Outcome::spawn_failed:
            reading.status = r.not_found ? core::Status::not_available : core::Status::error;
            reading.error = r.not_found ? "iw is not installed (Debian package: iw)" : r.error;
            return reading;
        case sys::ProcessResult::Outcome::timed_out:
            reading.status = core::Status::timeout;
            reading.error = "iw did not answer within 5 s";
            return reading;
        case sys::ProcessResult::Outcome::signaled:
        case sys::ProcessResult::Outcome::exited:
            break;
    }
    if (!r.ok()) {
        reading.error = "iw failed: " + std::string{trim(r.err.empty() ? r.error : r.err)};
        return reading;
    }
    if (trim(r.out).starts_with("Not connected")) {
        reading.status = core::Status::no_network;
        reading.error = "Wi-Fi is not connected";
        return reading;
    }
    reading.info = parse_iw_link(r.out);
    if (!reading.info) {
        reading.error = "unexpected iw output";
        return reading;
    }
    reading.status = core::Status::ok;
    return reading;
}

}  // namespace meerkat::net
