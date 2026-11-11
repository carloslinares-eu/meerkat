/**
 * @file test_route.cpp
 * @brief Tests for the default-route parsers.
 */
#include <doctest/doctest.h>

#include <filesystem>

#include "net/route.hpp"

using namespace meerkat::net;

namespace {
/// Root of a hand-made /proc tree; see tests/fixtures/README.md.
std::filesystem::path proc(const char* name) {
    return std::filesystem::path{MEERKAT_FIXTURES_DIR} / "proc" / name;
}
}  // namespace

TEST_CASE("Wi-Fi only: default route is the Wi-Fi interface") {
    const auto r = default_route(proc("wifi_only"));
    REQUIRE(r.has_value());
    CHECK(r->interface == "wlp4s0");
    CHECK(r->gateway == "192.0.2.1");
    CHECK(r->metric == 600);
    CHECK(r->family == 4);
}

TEST_CASE("cable and Wi-Fi: the lower metric (cable) wins") {
    const auto r = default_route(proc("cable_and_wifi"));
    REQUIRE(r.has_value());
    CHECK(r->interface == "enp0s25");
    CHECK(r->metric == 100);
}

TEST_CASE("no IPv4 default: falls back to IPv6 and skips the rejecting route on lo") {
    const auto r = default_route(proc("ipv6_only"));
    REQUIRE(r.has_value());
    CHECK(r->interface == "wlp4s0");
    CHECK(r->gateway == "fe80::1%wlp4s0");  // link-local needs its zone to be pingable
    CHECK(r->metric == 0x258);
    CHECK(r->family == 6);
}

TEST_CASE("offline: no default route") {
    CHECK_FALSE(default_route(proc("offline")).has_value());
    CHECK_FALSE(default_route("/nonexistent").has_value());
}

TEST_CASE("parse_ipv4_routes ignores routes that are down, rejecting, non-default or malformed") {
    const char* text =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "eth0\t00000000\t010200C0\t0002\t0\t0\t1\t00000000\t0\t0\t0\n"   // not RTF_UP
        "eth1\t00000000\t010200C0\t0203\t0\t0\t1\t00000000\t0\t0\t0\n"   // RTF_REJECT
        "eth2\t000200C0\t00000000\t0001\t0\t0\t1\t00FFFFFF\t0\t0\t0\n"   // not default
        "eth3\t0000000G\t010200C0\t0003\t0\t0\t1\t00000000\t0\t0\t0\n"   // bad hex
        "eth4\t00000000\t010200C0\t0003\t0\t0\t1x\t00000000\t0\t0\t0\n"  // junk after metric
        "lo\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n"     // loopback
        "../x\t00000000\t010200C0\t0003\t0\t0\t1\t00000000\t0\t0\t0\n"   // bad name
        "short line\n"
        "\n";
    CHECK_FALSE(parse_ipv4_routes(text).has_value());
}

TEST_CASE("parse_ipv4_routes: point-to-point default (PPP) has no gateway") {
    const auto r = parse_ipv4_routes("ppp0\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n");
    REQUIRE(r.has_value());
    CHECK(r->interface == "ppp0");
    CHECK(r->gateway.empty());
}

TEST_CASE("parse_ipv4_routes: a default on an unusually named interface still counts") {
    // Legal for the kernel (only '/', ':' and whitespace are forbidden).
    const auto r = parse_ipv4_routes(
        "wlp4s0\t00000000\t010200C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
        "wg+home\t00000000\t00000000\t0001\t0\t0\t50\t00000000\t0\t0\t0\n");
    REQUIRE(r.has_value());
    CHECK(r->interface == "wg+home");
}

TEST_CASE("parse_ipv6_routes: same gateway rules as IPv4") {
    // ::/0 with a non-zero nexthop but without RTF_GATEWAY: no gateway.
    const auto r = parse_ipv6_routes(
        "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
        "20010db8000000000000000000000001 00000400 00000001 00000000 00000001 tun0\n");
    REQUIRE(r.has_value());
    CHECK(r->interface == "tun0");
    CHECK(r->gateway.empty());

    // A global (non-link-local) gateway gets no zone.
    const auto g = parse_ipv6_routes(
        "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
        "20010db8000000000000000000000001 00000400 00000001 00000000 00000003 eth0\n");
    REQUIRE(g.has_value());
    CHECK(g->gateway == "2001:db8::1");
}

TEST_CASE("parse_ipv6_routes rejects malformed fields") {
    for (const char* bad : {
             // prefix length "000" (must be exactly two hex digits)
             "00000000000000000000000000000000 000 00000000000000000000000000000000 00 "
             "fe800000000000000000000000000001 00000258 00000005 00000000 00000003 eth0\n",
             // destination too short
             "0000000000000000000000000000000 00 00000000000000000000000000000000 00 "
             "fe800000000000000000000000000001 00000258 00000005 00000000 00000003 eth0\n",
             // non-hex nexthop
             "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
             "fe80000000000000000000000000000x 00000258 00000005 00000000 00000003 eth0\n",
             // not a default route (/64)
             "20010db8000100000000000000000000 40 00000000000000000000000000000000 00 "
             "fe800000000000000000000000000001 00000258 00000005 00000000 00000003 eth0\n",
         }) {
        CHECK_FALSE(parse_ipv6_routes(bad).has_value());
    }
}
