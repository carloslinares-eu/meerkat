/**
 * @file wifi.cpp
 * @brief Implementation of the `iw dev <interface> link` reader.
 */
#include "net/wifi.hpp"

#include <chrono>
#include <exception>

#include "core/text.hpp"
#include "net/interface.hpp"

namespace meerkat::net {

namespace {

using core::text::leading_double;
using core::text::trim;

/// How long `iw` may take; it normally answers in a few milliseconds.
constexpr std::chrono::seconds kIwTimeout{5};

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

/**
 * @brief Decodes the SSID as `iw` prints it.
 *
 * `iw` writes bytes outside printable ASCII, a backslash, and spaces at the
 * start or end, as `\xNN` (a backslash is `\x5c`). Decoding gives the exact
 * bytes back (UTF-8 names like `Café` included).
 *
 * @param raw The text after `SSID: ` on the line, unchanged.
 * @return The SSID.
 */
std::string decode_ssid(std::string_view raw) {
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto byte = raw[i] == '\\' && i + 3 < raw.size() && raw[i + 1] == 'x'
                              ? core::text::parse_int<unsigned char>(raw.substr(i + 2, 2), 16)
                              : std::nullopt;
        if (byte) {
            out.push_back(static_cast<char>(*byte));
            i += 3;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

}  // namespace

std::optional<core::WifiInfo> parse_iw_link(std::string_view text) {
    core::WifiInfo info;
    bool connected = false;
    core::text::for_each_line(text, [&](std::string_view raw_line) {
        const auto line = trim(raw_line);
        constexpr std::string_view kConnected = "Connected to ";
        constexpr std::string_view kSsid = "SSID: ";
        if (line.starts_with(kConnected)) {
            connected = true;
            const auto rest = line.substr(kConnected.size());
            info.bssid = std::string{rest.substr(0, rest.find(' '))};
        } else if (line.starts_with(kSsid)) {
            // Take the value verbatim (minus the line ending), not trimmed:
            // spaces belong to the name. iw escapes leading/trailing ones.
            auto raw = raw_line.substr(raw_line.find(kSsid) + kSsid.size());
            while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) {
                raw.remove_suffix(1);
            }
            info.ssid = decode_ssid(raw);
        } else if (const auto f = value_of(line, "freq")) {
            info.frequency_mhz = leading_double(*f);
        } else if (const auto sig = value_of(line, "signal")) {
            info.signal_dbm = leading_double(*sig);
        } else if (const auto rx = value_of(line, "rx bitrate")) {
            info.rx_bitrate_mbps = leading_double(*rx);
        } else if (const auto tx = value_of(line, "tx bitrate")) {
            info.tx_bitrate_mbps = leading_double(*tx);
        }
    });
    if (!connected) {
        return std::nullopt;
    }
    return info;
}

WifiReading read_wifi(std::string_view interface, const sys::Runner& run) {
    WifiReading reading;
    if (!is_valid_interface_name(interface)) {
        reading.error = "invalid interface name";
        return reading;
    }
    sys::ProcessResult r;
    try {
        r = run({.program = "iw",
                 .args = {"dev", std::string{interface}, "link"},
                 .timeout = kIwTimeout,
                 .env = {{"LC_ALL", "C"}}});
    } catch (const std::exception& e) {
        reading.error = std::string{"cannot run iw: "} + e.what();
        return reading;
    }
    switch (r.outcome) {
        case sys::ProcessResult::Outcome::spawn_failed:
            reading.status = r.not_found ? core::Status::not_available : core::Status::error;
            reading.error = r.not_found ? "iw is not installed (Debian package: iw)" : r.error;
            return reading;
        case sys::ProcessResult::Outcome::timed_out:
            reading.status = core::Status::timeout;
            reading.error = "iw did not answer within " + std::to_string(kIwTimeout.count()) + " s";
            return reading;
        case sys::ProcessResult::Outcome::signaled:
            reading.error = "iw was killed by signal " + std::to_string(r.signal);
            return reading;
        case sys::ProcessResult::Outcome::cancelled:  // no Command::stop here
            reading.error = "iw was stopped";
            return reading;
        case sys::ProcessResult::Outcome::exited:
            break;
    }
    if (!r.ok()) {
        const auto message = std::string{trim(r.err.empty() ? r.out : r.err)};
        // Radio off (rfkill) or interface down: a normal "no Wi-Fi" state,
        // not a tool failure.
        if (message.find("Network is down") != std::string::npos) {
            reading.status = core::Status::no_network;
            reading.error = "Wi-Fi is down: " + message;
            return reading;
        }
        reading.error = "iw failed (exit code " + std::to_string(r.exit_code) + ")" +
                        (message.empty() ? "" : ": " + message);
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
