/**
 * @file kernel_route.hpp
 * @brief Asks the kernel which route it would use to reach the internet
 *        (netlink `RTM_GETROUTE`, like `ip route get`).
 */
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "net/route.hpp"

namespace meerkat::net {

/// The kernel's answer to a route query for one address family.
struct RouteAnswer {
    /// The kernel answered (with a route or with "no route"). False when
    /// netlink itself failed; then `error` says why and /proc is the fallback.
    bool answered = false;
    std::optional<DefaultRoute> route;  ///< The route ordinary traffic takes; empty: none.
    /// The route's next hop has no carrier (`RTNH_F_LINKDOWN`): a stale
    /// default on an unplugged cable. `route` is still filled in.
    bool link_down = false;
    std::string error;                  ///< Why netlink failed, or why there is no route.
};

/// Looks up an interface name by index (if_indextoname(); tests pass a fake).
using InterfaceNamer = std::function<std::string(unsigned index)>;

/**
 * @brief Parses the kernel's reply to an `RTM_GETROUTE` query.
 *
 * - `RTM_NEWROUTE` of type `RTN_UNICAST` with an outgoing interface (other
 *   than `lo`): that route, with `RTA_GATEWAY` as the gateway (a link-local
 *   IPv6 gateway gets the interface as its zone) and `RTA_PRIORITY` as the
 *   metric.
 * - Another type (`blackhole`, `unreachable`, `prohibit`, e.g. a VPN kill
 *   switch): answered, no route.
 * - `NLMSG_ERROR` with `ENETUNREACH`/`EHOSTUNREACH`, `EINVAL` (how the
 *   kernel answers for a blackhole route or rule, e.g. a VPN kill switch) or
 *   `EACCES`/`EPERM` (a prohibit route or rule): answered, no route. Any
 *   other error, or a reply that can't be parsed: not answered.
 * - An IPv4 route via an IPv6 next hop (`RTA_VIA`, RFC 5549) keeps that
 *   next hop as its gateway.
 *
 * @param reply  The bytes received.
 * @param family `AF_INET` or `AF_INET6`.
 * @param name   Interface index → name.
 * @return The answer.
 */
RouteAnswer parse_route_reply(std::span<const std::byte> reply, int family,
                              const InterfaceNamer& name);

/**
 * @brief Asks the kernel how it would route a packet to a public address
 *        (`1.1.1.1` or `2606:4700:4700::1111`; nothing is sent there).
 *
 * The answer takes everything into account that /proc doesn't show:
 * policy rules and other routing tables (wg-quick), source-specific and
 * blackhole routes, split VPN defaults (`0.0.0.0/1`). It asks for the
 * matching FIB entry (`RTM_F_FIB_MATCH`, kernel 4.13+), which carries the
 * next hop's `RTNH_F_LINKDOWN`. The reply wait is 250 ms.
 * @param family `AF_INET` or `AF_INET6`.
 * @return The answer; `answered` is false if netlink can't be used. Never
 *         throws.
 */
RouteAnswer query_kernel_route(int family);

/**
 * @brief Chooses between the kernel's IPv4 and IPv6 answers: IPv4 unless it
 *        has no route or its link is down, then IPv6, then (link down or
 *        not) whichever has a route.
 * @param v4 The IPv4 answer.
 * @param v6 The IPv6 answer.
 * @return The route, or nothing. Nothing as well if neither was answered.
 */
std::optional<DefaultRoute> choose_route(const RouteAnswer& v4, const RouteAnswer& v6);

/**
 * @brief The route the computer uses to reach the internet: the kernel's
 *        answers, chosen with choose_route(). A family netlink can't answer
 *        for (a sandbox, IPv6 disabled) is read from /proc instead.
 * @return The route, or nothing if the computer is offline.
 */
std::optional<DefaultRoute> system_route();

}  // namespace meerkat::net
