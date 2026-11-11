/**
 * @file route.cpp
 * @brief Parsers for /proc/net/route and /proc/net/ipv6_route.
 */
#include "net/route.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <vector>

namespace meerkat::net {

namespace {

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
 * @brief Parses a whole field as hexadecimal.
 * @param s     The field.
 * @param value Receives the number.
 * @return True if the entire field is valid hex that fits.
 */
template <typename T>
bool parse_hex(std::string_view s, T& value) {
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value, 16);
    return ec == std::errc{} && ptr == end && !s.empty();
}

/**
 * @brief Calls @p fn for every line of @p text.
 * @param text The text.
 * @param fn   Called with each line (without the newline).
 */
template <typename Fn>
void for_each_line(std::string_view text, Fn fn) {
    while (!text.empty()) {
        const auto nl = text.find('\n');
        fn(text.substr(0, nl));
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
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
 * @brief Reads a whole file.
 * @param path The file.
 * @return Its contents, or `std::nullopt` if it can't be opened.
 */
std::optional<std::string> slurp(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

bool is_valid_interface_name(std::string_view name) {
    if (name.empty() || name.size() > 15 || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == ':' || c == '@' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::optional<DefaultRoute> parse_ipv4_routes(std::string_view text) {
    // Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT
    std::optional<DefaultRoute> best;
    for_each_line(text, [&](std::string_view line) {
        const auto f = fields(line);
        if (f.size() < 8 || f[0] == "Iface" || f[0] == "lo" || !is_valid_interface_name(f[0])) {
            return;
        }
        std::uint32_t dest = 0, gw = 0, mask = 0, metric = 0;
        unsigned flags = 0;
        if (!parse_hex(f[1], dest) || !parse_hex(f[2], gw) || !parse_hex(f[3], flags) ||
            !parse_hex(f[7], mask)) {
            return;
        }
        if (std::from_chars(f[6].data(), f[6].data() + f[6].size(), metric).ec != std::errc{}) {
            return;
        }
        if (dest != 0 || mask != 0 || (flags & kRtfUp) == 0 || (flags & kRtfReject) != 0) {
            return;
        }
        DefaultRoute r{.interface = std::string{f[0]}, .gateway = {}, .metric = metric, .family = 4};
        if ((flags & kRtfGateway) != 0 && gw != 0) {
            // The kernel prints the address as a host-order (little-endian) number,
            // so the bytes in memory are already in network order.
            in_addr addr{};
            addr.s_addr = gw;
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
        unsigned plen = 0, flags = 0;
        std::uint32_t metric = 0;
        if (f[0].find_first_not_of('0') != std::string_view::npos || f[0].size() != 32 ||
            !parse_hex(f[1], plen) || plen != 0 || f[4].size() != 32 ||
            !parse_hex(f[5], metric) || !parse_hex(f[8], flags)) {
            return;
        }
        if ((flags & kRtfUp) == 0 || (flags & kRtfReject) != 0) {
            return;
        }
        DefaultRoute r{.interface = std::string{f[9]}, .gateway = {}, .metric = metric, .family = 6};
        if (f[4].find_first_not_of('0') != std::string_view::npos) {
            in6_addr addr{};
            for (std::size_t i = 0; i < 16; ++i) {
                unsigned byte = 0;
                if (!parse_hex(f[4].substr(i * 2, 2), byte)) {
                    return;
                }
                addr.s6_addr[i] = static_cast<std::uint8_t>(byte);
            }
            std::array<char, INET6_ADDRSTRLEN> buf{};
            if (::inet_ntop(AF_INET6, &addr, buf.data(), buf.size()) != nullptr) {
                r.gateway = buf.data();
            }
        }
        keep_best(best, std::move(r));
    });
    return best;
}

std::optional<DefaultRoute> default_route(const std::filesystem::path& proc_root) {
    if (const auto v4 = slurp(proc_root / "net/route")) {
        if (auto r = parse_ipv4_routes(*v4)) {
            return r;
        }
    }
    if (const auto v6 = slurp(proc_root / "net/ipv6_route")) {
        return parse_ipv6_routes(*v6);
    }
    return std::nullopt;
}

}  // namespace meerkat::net
