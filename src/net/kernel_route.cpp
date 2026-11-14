/**
 * @file kernel_route.cpp
 * @brief Implementation of the netlink route query.
 */
#include "net/kernel_route.hpp"

#include "core/text.hpp"
#include "sys/fd.hpp"

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <vector>

namespace meerkat::net {

namespace {

/// Public addresses the route is looked up for (no packet is sent).
constexpr const char* kProbeV4 = "1.1.1.1";
constexpr const char* kProbeV6 = "2606:4700:4700::1111";

/// Longest wait for the kernel's reply (it comes at once; this only
/// bounds a sandbox that swallows it).
constexpr suseconds_t kReplyTimeoutUsec = 250'000;

}  // namespace

RouteAnswer parse_route_reply(std::span<const std::byte> reply, int family,
                              const InterfaceNamer& name) {
    RouteAnswer a;
    // Walk the messages with offsets and memcpy instead of the NLMSG_*/RTA_*
    // macros (C casts, and they assume an aligned buffer).
    const auto read = [&](auto& out, std::size_t at) {
        if (at + sizeof out > reply.size()) {
            return false;
        }
        std::memcpy(&out, reply.data() + at, sizeof out);
        return true;
    };
    constexpr std::size_t kHeader = NLMSG_ALIGN(sizeof(nlmsghdr));
    std::size_t at = 0;
    nlmsghdr nh{};
    while (read(nh, at) && nh.nlmsg_len >= sizeof(nlmsghdr) && at + nh.nlmsg_len <= reply.size()) {
        const std::size_t body = at + kHeader;
        const std::size_t end = at + nh.nlmsg_len;
        at += NLMSG_ALIGN(nh.nlmsg_len);
        if (nh.nlmsg_type == NLMSG_ERROR) {
            nlmsgerr err{};
            if (body + sizeof err > end || !read(err, body)) {
                break;
            }
            const int e = -err.error;
            // EINVAL is how the kernel answers for a blackhole route or rule
            // (e.g. a VPN kill switch); our request itself is well formed.
            if (e == ENETUNREACH || e == EHOSTUNREACH || e == EACCES || e == EPERM || e == EINVAL) {
                a.answered = true;  // the kernel's answer: no way there
            }
            a.error = std::string{"no route: "} + core::text::errno_text(e);
            return a;
        }
        rtmsg rt{};
        if (nh.nlmsg_type != RTM_NEWROUTE || body + sizeof rt > end || !read(rt, body)) {
            continue;
        }
        if (rt.rtm_family != family) {
            a.error = "reply for another address family";
            return a;
        }
        a.answered = true;
        if (rt.rtm_type != RTN_UNICAST) {
            // blackhole / unreachable / prohibit: e.g. a VPN kill switch.
            a.error = "no route (route type " + std::to_string(rt.rtm_type) + ")";
            return a;
        }
        std::optional<std::uint32_t> oif;
        std::vector<std::byte> gw;
        int gw_family = family;
        std::uint32_t metric = 0;
        std::size_t attr = body + NLMSG_ALIGN(sizeof(rtmsg));
        rtattr ra{};
        while (read(ra, attr) && ra.rta_len >= sizeof(rtattr) && attr + ra.rta_len <= end) {
            const std::size_t data = attr + RTA_ALIGN(sizeof(rtattr));
            const std::size_t size = ra.rta_len - RTA_ALIGN(sizeof(rtattr));
            std::uint32_t v{};
            const bool u32 = size == sizeof v && read(v, data);
            if (ra.rta_type == RTA_OIF && u32) {
                oif = v;
            } else if (ra.rta_type == RTA_PRIORITY && u32) {
                metric = v;
            } else if (ra.rta_type == RTA_GATEWAY) {
                gw_family = family;
                gw.assign(reply.data() + data, reply.data() + data + size);
            } else if (ra.rta_type == RTA_VIA && size > sizeof(__kernel_sa_family_t)) {
                // `via inet6 fe80::1` for an IPv4 route (RFC 5549): the
                // next hop's family comes first, then its address.
                __kernel_sa_family_t via_family{};
                if (read(via_family, data)) {
                    gw_family = via_family;
                    gw.assign(reply.data() + data + sizeof via_family, reply.data() + data + size);
                }
            }
            attr += RTA_ALIGN(ra.rta_len);
        }
        const std::string interface = oif ? name(*oif) : std::string{};
        if (interface.empty() || interface == "lo") {
            a.error = "no route (no outgoing interface)";
            return a;
        }
        DefaultRoute r{.interface = interface, .gateway = {}, .metric = metric,
                       .family = family == AF_INET ? 4 : 6};
        if (gw_family == AF_INET && gw.size() == 4) {
            r.gateway = format_gateway(4, gw.data(), interface);
        } else if (gw_family == AF_INET6 && gw.size() == 16) {
            r.gateway = format_gateway(6, gw.data(), interface);
        }
        a.link_down = (rt.rtm_flags & RTNH_F_LINKDOWN) != 0;
        a.route = std::move(r);
        return a;
    }
    a.error = "no route in the netlink reply";
    return a;
}

RouteAnswer query_kernel_route(int family) {
    RouteAnswer failed;
    try {
        const sys::Fd sock{::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)};
        if (sock.get() < 0) {
            failed.error = std::string{"netlink socket: "} + core::text::errno_text(errno);
            return failed;
        }
        const timeval timeout{.tv_sec = 0, .tv_usec = kReplyTimeoutUsec};
        ::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

        const bool v4 = family == AF_INET;
        std::array<unsigned char, 16> addr{};
        ::inet_pton(family, v4 ? kProbeV4 : kProbeV6, addr.data());
        const std::size_t addr_size = v4 ? 4 : 16;

        // nlmsghdr | rtmsg | rtattr(RTA_DST) + address, each 4-byte aligned.
        constexpr std::size_t kHeader = NLMSG_ALIGN(sizeof(nlmsghdr));
        constexpr std::size_t kRt = NLMSG_ALIGN(sizeof(rtmsg));
        const std::size_t attr_len = RTA_ALIGN(sizeof(rtattr)) + addr_size;
        std::array<std::byte, 128> req{};
        nlmsghdr nh{};
        nh.nlmsg_len = static_cast<std::uint32_t>(kHeader + kRt + RTA_ALIGN(attr_len));
        nh.nlmsg_type = RTM_GETROUTE;
        nh.nlmsg_flags = NLM_F_REQUEST;
        nh.nlmsg_seq = 1;
        rtmsg rt{};
        rt.rtm_family = static_cast<unsigned char>(family);
        rt.rtm_dst_len = static_cast<unsigned char>(addr_size * 8);
        // The matching FIB entry rather than the resolved destination: only
        // that carries the next hop's flags (RTNH_F_LINKDOWN for a stale
        // default on an unplugged cable). Kernel 4.13+.
        rt.rtm_flags = RTM_F_FIB_MATCH;
        rtattr ra{};
        ra.rta_type = RTA_DST;
        ra.rta_len = static_cast<unsigned short>(attr_len);
        std::memcpy(req.data(), &nh, sizeof nh);
        std::memcpy(req.data() + kHeader, &rt, sizeof rt);
        std::memcpy(req.data() + kHeader + kRt, &ra, sizeof ra);
        std::memcpy(req.data() + kHeader + kRt + RTA_ALIGN(sizeof(rtattr)), addr.data(), addr_size);

        sockaddr_nl kernel{};
        kernel.nl_family = AF_NETLINK;
        if (::sendto(sock.get(), req.data(), nh.nlmsg_len, 0, reinterpret_cast<sockaddr*>(&kernel),
                     sizeof kernel) < 0) {
            failed.error = std::string{"netlink send: "} + core::text::errno_text(errno);
            return failed;
        }
        std::array<std::byte, 8192> buf{};
        const auto n = ::recv(sock.get(), buf.data(), buf.size(), 0);
        if (n < 0) {
            failed.error = std::string{"netlink receive: "} + core::text::errno_text(errno);
            return failed;
        }
        return parse_route_reply(std::span{buf.data(), static_cast<std::size_t>(n)}, family,
                                 [](unsigned index) {
                                     std::array<char, IF_NAMESIZE> name{};
                                     return ::if_indextoname(index, name.data())
                                                ? std::string{name.data()}
                                                : std::string{};
                                 });
    } catch (const std::exception& e) {
        failed.error = std::string{"netlink: "} + e.what();
    } catch (...) {
        failed.error = "netlink: unknown exception";
    }
    return failed;
}

std::optional<DefaultRoute> choose_route(const RouteAnswer& v4, const RouteAnswer& v6) {
    if (v4.route && !v4.link_down) {
        return v4.route;
    }
    if (v6.route && !v6.link_down) {
        return v6.route;
    }
    // Both down (or one missing): report the stale one, so the link check
    // can say "no carrier on <interface>" instead of "no route".
    return v4.route ? v4.route : v6.route;
}

std::optional<DefaultRoute> system_route() {
    // A family netlink can't answer for (a sandbox, IPv6 disabled) falls
    // back to /proc on its own; the other keeps the kernel's answer.
    const auto from_proc = [](const char* file, auto parse) {
        RouteAnswer a;
        a.answered = true;
        if (const auto text = core::text::read_file(std::filesystem::path{"/proc/net"} / file)) {
            a.route = parse(*text);
        }
        return a;
    };
    auto v4 = query_kernel_route(AF_INET);
    if (!v4.answered) {
        v4 = from_proc("route", parse_ipv4_routes);
    }
    auto v6 = query_kernel_route(AF_INET6);
    if (!v6.answered) {
        v6 = from_proc("ipv6_route", parse_ipv6_routes);
    }
    return choose_route(v4, v6);
}

}  // namespace meerkat::net
