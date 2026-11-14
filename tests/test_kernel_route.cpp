/**
 * @file test_kernel_route.cpp
 * @brief Tests for the netlink route query, on hand-built kernel replies.
 */
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "net/kernel_route.hpp"
#include "net/route.hpp"

using namespace meerkat::net;

namespace {

/// Builds one netlink message the way the kernel sends it.
class Reply {
public:
    /// An RTM_NEWROUTE of @p type for @p family, with @p flags.
    static Reply route(int family, unsigned char type = RTN_UNICAST, unsigned flags = 0) {
        Reply r;
        rtmsg rt{};
        rt.rtm_family = static_cast<unsigned char>(family);
        rt.rtm_type = type;
        rt.rtm_flags = flags;
        r.start(RTM_NEWROUTE);
        r.put(&rt, sizeof rt);
        return r;
    }
    /// An NLMSG_ERROR with errno @p e.
    static Reply error(int e) {
        Reply r;
        nlmsgerr err{};
        err.error = -e;
        r.start(NLMSG_ERROR);
        r.put(&err, sizeof err);
        return r;
    }
    /// Adds an attribute.
    Reply& attr(unsigned short type, const void* data, std::size_t size) {
        rtattr ra{};
        ra.rta_type = type;
        ra.rta_len = static_cast<unsigned short>(RTA_ALIGN(sizeof(rtattr)) + size);
        put(&ra, sizeof ra);
        put(data, size);
        return *this;
    }
    /// Adds a 32-bit attribute.
    Reply& u32(unsigned short type, std::uint32_t v) { return attr(type, &v, sizeof v); }
    /// Adds an address attribute.
    Reply& addr(unsigned short type, int family, const char* text) {
        unsigned char buf[16]{};
        REQUIRE(::inet_pton(family, text, buf) == 1);
        return attr(type, buf, family == AF_INET ? 4 : 16);
    }
    /// The bytes, with the length filled in.
    std::vector<std::byte> bytes() const {
        auto b = bytes_;
        nlmsghdr nh{};
        std::memcpy(&nh, b.data(), sizeof nh);
        nh.nlmsg_len = static_cast<std::uint32_t>(b.size());
        std::memcpy(b.data(), &nh, sizeof nh);
        return b;
    }

private:
    void start(unsigned short type) {
        nlmsghdr nh{};
        nh.nlmsg_type = type;
        put(&nh, sizeof nh);
    }
    void put(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::byte*>(data);
        bytes_.insert(bytes_.end(), p, p + size);
        bytes_.resize((bytes_.size() + 3) / 4 * 4);  // 4-byte alignment, as the kernel pads
    }
    std::vector<std::byte> bytes_;
};

/// Interface 1 is lo, 2 is eth0, 3 is wlan0.
std::string names(unsigned i) {
    return i == 1 ? "lo" : i == 2 ? "eth0" : i == 3 ? "wlan0" : "";
}

RouteAnswer parse(const Reply& r, int family) {
    const auto b = r.bytes();
    return parse_route_reply(b, family, names);
}

}  // namespace

TEST_CASE("kernel route: a unicast route with gateway and metric") {
    const auto a = parse(Reply::route(AF_INET)
                             .u32(RTA_OIF, 2)
                             .addr(RTA_GATEWAY, AF_INET, "192.0.2.1")
                             .u32(RTA_PRIORITY, 100)
                             .u32(RTA_TABLE, 254),
                         AF_INET);
    CHECK(a.answered);
    REQUIRE(a.route);
    CHECK(a.route->interface == "eth0");
    CHECK(a.route->gateway == "192.0.2.1");
    CHECK(a.route->metric == 100);
    CHECK(a.route->family == 4);
    CHECK_FALSE(a.link_down);
}

TEST_CASE("kernel route: IPv6 link-local gateway gets its zone; no gateway on a tunnel") {
    const auto a = parse(Reply::route(AF_INET6).u32(RTA_OIF, 3).addr(RTA_GATEWAY, AF_INET6, "fe80::1"),
                         AF_INET6);
    REQUIRE(a.route);
    CHECK(a.route->gateway == "fe80::1%wlan0");
    CHECK(a.route->family == 6);

    const auto tun = parse(Reply::route(AF_INET).u32(RTA_OIF, 2), AF_INET);
    REQUIRE(tun.route);
    CHECK(tun.route->gateway.empty());
}

TEST_CASE("kernel route: route types other than unicast, ENETUNREACH and lo are no route") {
    for (const unsigned char type : {RTN_BLACKHOLE, RTN_UNREACHABLE, RTN_PROHIBIT}) {
        CAPTURE(int{type});
        const auto a = parse(Reply::route(AF_INET, type), AF_INET);
        CHECK(a.answered);
        CHECK_FALSE(a.route);
    }
    const auto n = parse(Reply::error(ENETUNREACH), AF_INET6);
    CHECK(n.answered);
    CHECK_FALSE(n.route);
    const auto lo = parse(Reply::route(AF_INET).u32(RTA_OIF, 1), AF_INET);
    CHECK(lo.answered);
    CHECK_FALSE(lo.route);
}

TEST_CASE("kernel route: blackhole and prohibit answer as errors (seen on Linux 6.12)") {
    for (const int e : {EINVAL, EACCES, EPERM}) {
        CAPTURE(e);
        const auto a = parse(Reply::error(e), AF_INET);
        CHECK(a.answered);
        CHECK_FALSE(a.route);
    }
}

TEST_CASE("kernel route: an IPv4 route via an IPv6 next hop (RTA_VIA)") {
    unsigned char via[2 + 16]{};
    const __kernel_sa_family_t fam = AF_INET6;
    std::memcpy(via, &fam, sizeof fam);
    REQUIRE(::inet_pton(AF_INET6, "fe80::1", via + sizeof fam) == 1);
    const auto a = parse(Reply::route(AF_INET).u32(RTA_OIF, 2).attr(RTA_VIA, via, sizeof via), AF_INET);
    REQUIRE(a.route);
    CHECK(a.route->gateway == "fe80::1%eth0");
}

TEST_CASE("kernel route: a link-down next hop is flagged") {
    const auto a = parse(Reply::route(AF_INET, RTN_UNICAST, RTNH_F_LINKDOWN).u32(RTA_OIF, 2), AF_INET);
    REQUIRE(a.route);
    CHECK(a.link_down);
}

TEST_CASE("kernel route: errors and garbage are 'not answered', never a crash") {
    CHECK_FALSE(parse(Reply::error(EOPNOTSUPP), AF_INET).answered);
    CHECK_FALSE(parse(Reply::route(AF_INET6).u32(RTA_OIF, 2), AF_INET).answered);  // wrong family
    const std::vector<std::byte> empty;
    CHECK_FALSE(parse_route_reply(empty, AF_INET, names).answered);
    auto b = Reply::route(AF_INET).u32(RTA_OIF, 2).bytes();
    for (std::size_t cut = 0; cut < b.size(); ++cut) {  // every truncation
        const std::vector<std::byte> part(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_NOTHROW(parse_route_reply(part, AF_INET, names));
    }
}

TEST_CASE("choose_route: IPv4 unless missing or link down, then IPv6") {
    RouteAnswer v4{.answered = true, .route = DefaultRoute{"eth0", "192.0.2.1", 0, 4}, .link_down = false, .error = {}};
    RouteAnswer v6{.answered = true, .route = DefaultRoute{"wlan0", "fe80::1%wlan0", 0, 6}, .link_down = false, .error = {}};
    CHECK(choose_route(v4, v6)->interface == "eth0");
    v4.link_down = true;  // unplugged cable with a stale default: IPv6 Wi-Fi works
    CHECK(choose_route(v4, v6)->interface == "wlan0");
    v6.route.reset();
    CHECK(choose_route(v4, v6)->interface == "eth0");  // stale, so the link check says "no carrier"
    v4.route.reset();
    CHECK_FALSE(choose_route(v4, v6));
}

TEST_CASE("live: the kernel answers a route query (needs netlink)") {
    const auto v4 = query_kernel_route(AF_INET);
    const auto v6 = query_kernel_route(AF_INET6);
    if (!v4.answered && !v6.answered) {
        MESSAGE("netlink unusable here (" << v4.error << "); skipped");
        return;
    }
    // No claim about the result: a kill switch or a prohibit rule is a
    // correct "no route" even though /proc shows a default.
    for (const auto* a : {&v4, &v6}) {
        if (a->route) {
            CHECK_FALSE(a->route->interface.empty());
        }
    }
    CHECK_NOTHROW(system_route());
}
