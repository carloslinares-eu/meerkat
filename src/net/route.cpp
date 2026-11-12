/**
 * @file route.cpp
 * @brief Parsers for /proc/net/route and /proc/net/ipv6_route.
 */
#include "net/route.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <vector>

#include "net/detail/text.hpp"

namespace meerkat::net {

namespace {

using detail::for_each_line;
using detail::parse_int;

constexpr unsigned kRtfUp = 0x0001;       ///< RTF_UP: route is usable.
constexpr unsigned kRtfGateway = 0x0002;  ///< RTF_GATEWAY: route has a next hop.
constexpr unsigned kRtfReject = 0x0200;   ///< RTF_REJECT: route rejects traffic.

/**
 * @brief Splits a line on runs of spaces and tabs.
 * @param line The line.
 * @return The non-empty fields.
 */
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

/**
 * @brief Keeps @p candidate if it beats @p best (lower metric; first wins ties).
 * @param best      The best route so far.
 * @param candidate Another default route.
 */
void keep_best(std::optional<DefaultRoute>& best, DefaultRoute candidate) {
    if (!best || candidate.metric < best->metric) {
        best = std::move(candidate);
    }
}

/**
 * @brief True if the route flags describe a usable route.
 * @param flags RTF_* flags.
 * @return Up and not rejecting.
 */
bool usable(unsigned flags) {
    return (flags & kRtfUp) != 0 && (flags & kRtfReject) == 0;
}

/**
 * @brief Parses 32 hex digits (as in /proc/net/ipv6_route) into an address.
 * @param hex  The digits.
 * @param addr Receives the address.
 * @return True if @p hex is exactly 32 valid hex digits.
 */
bool parse_in6(std::string_view hex, in6_addr& addr) {
    if (hex.size() != 32) {
        return false;
    }
    for (std::size_t i = 0; i < 16; ++i) {
        const auto byte = parse_int<unsigned>(hex.substr(i * 2, 2), 16);
        if (!byte) {
            return false;
        }
        addr.s6_addr[i] = static_cast<std::uint8_t>(*byte);
    }
    return true;
}

}  // namespace

std::optional<DefaultRoute> parse_ipv4_routes(std::string_view text) {
    // Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT
    std::optional<DefaultRoute> best;
    for_each_line(text, [&](std::string_view line) {
        const auto f = fields(line);
        if (f.size() < 8 || f[0] == "Iface" || f[0] == "lo" || !is_valid_interface_name(f[0])) {
            return;
        }
        const auto dest = parse_int<std::uint32_t>(f[1], 16);
        const auto gw = parse_int<std::uint32_t>(f[2], 16);
        const auto flags = parse_int<unsigned>(f[3], 16);
        const auto metric = parse_int<std::uint32_t>(f[6], 10);
        const auto mask = parse_int<std::uint32_t>(f[7], 16);
        if (!dest || !gw || !flags || !metric || !mask) {
            return;  // malformed line
        }
        if (*dest != 0 || *mask != 0 || !usable(*flags)) {
            return;
        }
        DefaultRoute r{.interface = std::string{f[0]}, .gateway = {}, .metric = *metric, .family = 4};
        if ((*flags & kRtfGateway) != 0 && *gw != 0) {
            // The kernel prints the address as a host-order number, so its
            // in-memory bytes are already in network order.
            in_addr addr{};
            addr.s_addr = *gw;
            std::array<char, INET_ADDRSTRLEN> buf{};
            if (::inet_ntop(AF_INET, &addr, buf.data(), buf.size()) != nullptr) {
                r.gateway = buf.data();
            }
        }
        keep_best(best, std::move(r));
    });
    return best;
}

std::optional<DefaultRoute> parse_ipv6_routes(std::string_view text) {
    // dest dest_plen src src_plen nexthop metric refcnt use flags iface
    std::optional<DefaultRoute> best;
    for_each_line(text, [&](std::string_view line) {
        const auto f = fields(line);
        if (f.size() < 10 || f[9] == "lo" || !is_valid_interface_name(f[9])) {
            return;
        }
        in6_addr dest{}, nexthop{};
        const auto plen = f[1].size() == 2 ? parse_int<unsigned>(f[1], 16) : std::nullopt;
        const auto metric = parse_int<std::uint32_t>(f[5], 16);
        const auto flags = parse_int<unsigned>(f[8], 16);
        if (!parse_in6(f[0], dest) || !plen || !parse_in6(f[4], nexthop) || !metric || !flags) {
            return;  // malformed line
        }
        if (*plen != 0 || !IN6_IS_ADDR_UNSPECIFIED(&dest) || !usable(*flags)) {
            return;
        }
        DefaultRoute r{.interface = std::string{f[9]}, .gateway = {}, .metric = *metric, .family = 6};
        if ((*flags & kRtfGateway) != 0 && !IN6_IS_ADDR_UNSPECIFIED(&nexthop)) {
            std::array<char, INET6_ADDRSTRLEN> buf{};
            if (::inet_ntop(AF_INET6, &nexthop, buf.data(), buf.size()) != nullptr) {
                r.gateway = buf.data();
                if (IN6_IS_ADDR_LINKLOCAL(&nexthop)) {
                    r.gateway += "%" + r.interface;  // link-local needs its zone
                }
            }
        }
        keep_best(best, std::move(r));
    });
    return best;
}

std::optional<DefaultRoute> default_route(const std::filesystem::path& proc_root) {
    if (const auto v4 = detail::read_file(proc_root / "net/route")) {
        if (auto r = parse_ipv4_routes(*v4)) {
            return r;
        }
    }
    if (const auto v6 = detail::read_file(proc_root / "net/ipv6_route")) {
        return parse_ipv6_routes(*v6);
    }
    return std::nullopt;
}

}  // namespace meerkat::net
