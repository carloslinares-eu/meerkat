/**
 * @file link.cpp
 * @brief Implementation of net::read_link().
 */
#include "net/link.hpp"

#include <system_error>

#include "core/text.hpp"
#include "net/interface.hpp"

namespace meerkat::net {

namespace {

/**
 * @brief Parses a sysfs attribute as a decimal integer.
 * @param s The attribute value.
 * @return The number, or `std::nullopt`.
 */
std::optional<long> to_long(const std::optional<std::string>& s) {
    return s ? core::text::parse_int<long>(*s) : std::nullopt;
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
    if (has("wireless") || has("phy80211")) {
        info.type = core::LinkType::wifi;
    } else if (to_long(core::text::read_attr(dir / "type")) == 1 && has("device")) {
        info.type = core::LinkType::wired;
    }

    info.operstate = core::text::read_attr(dir / "operstate").value_or("");
    if (const auto carrier = to_long(core::text::read_attr(dir / "carrier"))) {
        info.carrier = *carrier != 0;
    } else if (info.operstate == "down" || info.operstate == "notpresent") {
        // `carrier` can't be read (EINVAL) while the interface is
        // administratively down; the link is certainly not usable then.
        info.carrier = false;
    }
    if (info.type == core::LinkType::wired) {
        if (const auto speed = to_long(core::text::read_attr(dir / "speed")); speed && *speed > 0) {
            info.speed_mbps = static_cast<double>(*speed);
        }
    }
    return info;
}

}  // namespace meerkat::net
