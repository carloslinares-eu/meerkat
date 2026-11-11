/**
 * @file route.hpp
 * @brief Finds the default route (interface and gateway) from /proc.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/**
 * @namespace meerkat::net
 * @brief The local network: default route, interface type, Wi-Fi details.
 */
namespace meerkat::net {

/// The route the computer uses to reach the internet.
struct DefaultRoute {
    std::string interface;    ///< Outgoing interface, e.g. `enp0s25`.
    std::string gateway;      ///< Next hop as text (`192.0.2.1`, `fe80::1`); empty for point-to-point links.
    std::uint32_t metric = 0; ///< Route metric; lower wins.
    int family = 4;           ///< 4 or 6.
};

/**
 * @brief Picks the default route from the text of `/proc/net/route` (IPv4).
 *
 * Only routes that are up, have destination and mask 0.0.0.0, and aren't on
 * `lo` count. If there are several (e.g. cable and Wi-Fi both connected), the
 * lowest metric wins, as in the kernel.
 *
 * @param text The file's contents, header line included.
 * @return The default route, or `std::nullopt` if there is none.
 */
std::optional<DefaultRoute> parse_ipv4_routes(std::string_view text);

/**
 * @brief Picks the default route from the text of `/proc/net/ipv6_route`.
 *
 * Same rules as parse_ipv4_routes(); `::/0` routes that are rejecting
 * (`RTF_REJECT`, like the one the kernel keeps on `lo`) are skipped.
 *
 * @param text The file's contents.
 * @return The default route, or `std::nullopt` if there is none.
 */
std::optional<DefaultRoute> parse_ipv6_routes(std::string_view text);

/**
 * @brief Reads the current default route: IPv4 if there is one, else IPv6.
 * @param proc_root Where procfs is mounted; tests pass a fixture directory.
 * @return The default route, or `std::nullopt` if the computer has none
 *         (cable unplugged, Wi-Fi disconnected).
 */
std::optional<DefaultRoute> default_route(const std::filesystem::path& proc_root = "/proc");

/**
 * @brief Checks that @p name looks like a Linux interface name.
 *
 * 1–15 characters from `[A-Za-z0-9_.:@-]`, not `.` or `..`. Used before an
 * interface name is put into a sysfs path or handed to a tool.
 *
 * @param name The candidate name.
 * @return True if it is safe to use.
 */
bool is_valid_interface_name(std::string_view name);

}  // namespace meerkat::net
