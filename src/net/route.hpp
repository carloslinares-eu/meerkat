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
    /// Next hop as text, ready to ping: `192.0.2.1`, or for an IPv6
    /// link-local router `fe80::1%wlp4s0` (with the zone, which link-local
    /// addresses need). Empty for point-to-point links without a gateway.
    std::string gateway;
    std::uint32_t metric = 0; ///< Route metric; lower wins.
    int family = 4;           ///< 4 or 6.
};

/**
 * @brief Formats a next hop as DefaultRoute::gateway: the address, and for
 *        an IPv6 link-local one the interface as its zone (`fe80::1%wlp4s0`).
 * @param family    4 or 6.
 * @param address   4 or 16 address bytes, in network order.
 * @param interface The outgoing interface.
 * @return The text, or empty if it can't be formatted.
 */
std::string format_gateway(int family, const void* address, const std::string& interface);

/**
 * @brief Picks the default route from the text of `/proc/net/route` (IPv4).
 *
 * Only routes that are up, not rejecting, have destination and mask 0.0.0.0,
 * and aren't on `lo` count. If there are several (e.g. cable and Wi-Fi both
 * connected), the lowest metric wins, as in the kernel. The gateway is only
 * set for routes with `RTF_GATEWAY`. Malformed lines are ignored.
 *
 * Blackhole routes (interface `*`) don't count.
 *
 * @note This is the fallback when netlink can't be used; system_route()
 *       asks the kernel instead. /proc/net/route only shows the main table,
 *       so policy routing (e.g. wg-quick's fwmark table) or split defaults
 *       (OpenVPN's 0.0.0.0/1 + 128.0.0.0/1) aren't detected here.
 *
 * @param text The file's contents, header line included.
 * @return The default route, or `std::nullopt` if there is none.
 */
std::optional<DefaultRoute> parse_ipv4_routes(std::string_view text);

/**
 * @brief Picks the default route from the text of `/proc/net/ipv6_route`.
 *
 * Same rules as parse_ipv4_routes(): `::/0` routes that are down or
 * rejecting (`RTF_REJECT`, like the one the kernel keeps on `lo`) are skipped,
 * and the gateway is only set with `RTF_GATEWAY`. A link-local gateway gets
 * the interface as its zone (`fe80::1%wlp4s0`). Source-specific defaults
 * (`default from <prefix>`) are skipped too.
 *
 * @note Unlike /proc/net/route, this file lists every routing table, without
 *       saying which; a VPN's default in its own table (wg-quick) can win
 *       here. That's why system_route() asks the kernel first.
 *
 * @param text The file's contents.
 * @return The default route, or `std::nullopt` if there is none.
 */
std::optional<DefaultRoute> parse_ipv6_routes(std::string_view text);

/**
 * @brief Reads the current default route from /proc: IPv4 if there is one,
 *        else IPv6. The fallback of system_route().
 * @param proc_root Where procfs is mounted; tests pass a fixture directory.
 * @return The default route, or `std::nullopt` if the computer has none
 *         (cable unplugged, Wi-Fi disconnected).
 */
std::optional<DefaultRoute> default_route(const std::filesystem::path& proc_root = "/proc");

}  // namespace meerkat::net
