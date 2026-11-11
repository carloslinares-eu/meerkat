/**
 * @file link.hpp
 * @brief Reads an interface's type, carrier and speed from sysfs.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/status.hpp"

namespace meerkat::net {

/// What sysfs says about one network interface.
struct LinkInfo {
    std::string interface;                        ///< Interface name.
    bool exists = false;                          ///< The interface is present in sysfs.
    core::LinkType type = core::LinkType::unknown;///< Cable, Wi-Fi or unknown.
    std::optional<bool> carrier;                  ///< Physical link up (cable plugged / associated).
    std::string operstate;                        ///< Kernel state: `up`, `down`, `dormant`, …
    std::optional<double> speed_mbps;             ///< Negotiated Ethernet speed; empty for Wi-Fi or when down.
};

/**
 * @brief Reads `/sys/class/net/<interface>` and classifies the interface.
 *
 * - `wifi` if it has a `wireless` or `phy80211` entry.
 * - `wired` if it is Ethernet (`type` 1, ARPHRD_ETHER) backed by a physical
 *   device (`device` entry). That includes USB Ethernet adapters.
 * - `unknown` otherwise: bridges, Docker/veth, VPN tunnels, PPP. A bridge
 *   in front of a real cable is reported as unknown on purpose: meerkat can't
 *   prove what's behind it.
 *
 * `speed` reads as -1 or fails with EINVAL when the link is down or for
 * Wi-Fi; both give an empty speed_mbps.
 *
 * @param interface Interface name; must pass is_valid_interface_name().
 * @param sys_root  Where sysfs is mounted; tests pass a fixture directory.
 * @return The details. `exists` is false for an invalid or missing interface.
 */
LinkInfo read_link(std::string_view interface, const std::filesystem::path& sys_root = "/sys");

}  // namespace meerkat::net
