/**
 * @file link.cpp
 * @brief Implementation of net::read_link().
 */
#include "net/link.hpp"

#include <charconv>
#include <fstream>
#include <system_error>

#include "net/route.hpp"

namespace meerkat::net {

namespace {

/**
 * @brief Reads the first line of a sysfs attribute, without trailing whitespace.
 * @param path The attribute file.
 * @return The value, or `std::nullopt` if it can't be read (some attributes,
 *         like `speed` on Wi-Fi, fail to read with EINVAL).
 */
std::optional<std::string> read_attr(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::string line;
    if (!in || !std::getline(in, line)) {
        return std::nullopt;
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

/**
 * @brief Parses a whole string as a decimal integer.
 * @param s The text.
 * @return The number, or `std::nullopt`.
 */
std::optional<long> to_long(const std::optional<std::string>& s) {
    if (!s || s->empty()) {
        return std::nullopt;
    }
    long v = 0;
    const auto* end = s->data() + s->size();
    const auto [ptr, ec] = std::from_chars(s->data(), end, v);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return v;
}

}  // namespace

LinkInfo read_link(std::string_view interface, const std::filesystem::path& sys_root) {
    LinkInfo info;
    info.interface = std::string{interface};
    if (!is_valid_interface_name(interface)) {
        return info;
    }
    const auto dir = sys_root / "class/net" / info.interface;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return info;
    }
    info.exists = true;

    const auto has = [&](const char* entry) { return std::filesystem::exists(dir / entry, ec); };
    const auto arp_type = to_long(read_attr(dir / "type"));
    if (has("wireless") || has("phy80211")) {
        info.type = core::LinkType::wifi;
    } else if (arp_type == 1 && has("device")) {
        info.type = core::LinkType::wired;
    }

    if (const auto carrier = to_long(read_attr(dir / "carrier"))) {
        info.carrier = *carrier != 0;
    }
    info.operstate = read_attr(dir / "operstate").value_or("");
    if (info.type == core::LinkType::wired) {
        if (const auto speed = to_long(read_attr(dir / "speed")); speed && *speed > 0) {
            info.speed_mbps = static_cast<double>(*speed);
        }
    }
    return info;
}

}  // namespace meerkat::net
