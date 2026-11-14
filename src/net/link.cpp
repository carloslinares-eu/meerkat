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
    const bool present = std::filesystem::exists(dir, ec);
    if (!present && !ec) {
        return info;  // really not there
    }
    // Present, or it can't be checked (EACCES, a sysfs glitch): don't call
    // it gone, which would skip measurements as no_network. With an error
    // the type stays unknown below.
    info.exists = true;
    if (ec) {
        return info;
    }

    // A missing entry is a plain `false`; any other error (EACCES in a
    // sandbox, a transient sysfs failure) means we can't tell, and the type
    // stays `unknown`: a Wi-Fi interface must never pass for a cable.
    bool sysfs_error = false;
    const auto has = [&](const char* entry) {
        std::error_code e;
        const bool found = std::filesystem::exists(dir / entry, e);
        sysfs_error = sysfs_error || static_cast<bool>(e);
        return found;
    };
    const bool wireless = has("wireless") || has("phy80211");
    const bool device = has("device");
    if (wireless) {
        info.type = core::LinkType::wifi;
    } else if (!sysfs_error && device && to_long(core::text::read_attr(dir / "type")) == 1) {
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
