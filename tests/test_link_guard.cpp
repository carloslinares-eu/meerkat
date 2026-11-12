/**
 * @file test_link_guard.cpp
 * @brief Tests for core::LinkGuard and net::sense_link with fake sensors.
 */
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/link_guard.hpp"
#include "net/sense.hpp"

using namespace meerkat;
using core::LinkCheck;
using core::LinkGuard;
using core::LinkSnapshot;
using core::LinkType;
using core::Status;

namespace {

/// A usable connection of @p type on @p iface.
LinkSnapshot online(LinkType type, const std::string& iface = "eth0") {
    LinkSnapshot s;
    s.has_route = true;
    s.interface_exists = true;
    s.carrier = true;
    s.context.interface = iface;
    s.context.gateway = "192.0.2.1";
    s.context.link_type = type;
    return s;
}

/// No default route.
LinkSnapshot offline() { return LinkSnapshot{}; }

/// The notice texts of @p c, warnings marked with `!`.
std::vector<std::string> texts(const LinkCheck& c) {
    std::vector<std::string> out;
    for (const auto& n : c.notices) {
        out.push_back((n.warning ? "! " : "") + n.text);
    }
    return out;
}

/// True if @p c has exactly one notice and it contains @p part.
bool one_notice(const LinkCheck& c, const std::string& part) {
    return c.notices.size() == 1 && c.notices[0].text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("LinkGuard: a cable runs the test without a word") {
    LinkGuard g{false};
    const auto c = g.check(online(LinkType::wired));
    CHECK_FALSE(c.skip);
    CHECK(c.notices.empty());
    CHECK(c.snapshot.context.interface == "eth0");
}

TEST_CASE("LinkGuard: Wi-Fi warns at the start, then only on a change back to Wi-Fi") {
    LinkGuard g{false};
    auto c = g.check(online(LinkType::wifi, "wlan0"));
    CHECK_FALSE(c.skip);  // a warning only, without require_wired
    CHECK(texts(c) == std::vector<std::string>{
                          "! the connection is Wi-Fi (wlan0). Wi-Fi results don't count against "
                          "the provider; connect a cable to the router."});
    CHECK(g.check(online(LinkType::wifi, "wlan0")).notices.empty());  // not every 5 minutes

    c = g.check(online(LinkType::wired));
    CHECK(texts(c) == std::vector<std::string>{"the connection is a cable again (eth0)."});
    CHECK_FALSE(c.notices[0].warning);
    CHECK(g.check(online(LinkType::wired)).notices.empty());

    c = g.check(online(LinkType::wifi, "wlan0"));  // cable pulled, Wi-Fi took over
    CHECK(one_notice(c, "the connection is Wi-Fi (wlan0)"));
    CHECK(c.notices[0].warning);
}

TEST_CASE("LinkGuard: require_wired skips over Wi-Fi only") {
    LinkGuard g{true};
    auto c = g.check(online(LinkType::wifi, "wlan0"));
    REQUIRE(c.skip);
    CHECK(c.skip->status == Status::skipped_wlan);
    CHECK(c.skip->error == "not started: the connection is Wi-Fi and require_wired is set");
    CHECK(one_notice(c, "Wi-Fi"));
    c = g.check(online(LinkType::wifi, "wlan0"));
    CHECK(c.skip);  // every time, warned or not
    CHECK(c.notices.empty());
    CHECK_FALSE(g.check(online(LinkType::wired)).skip);
    CHECK_FALSE(g.check(online(LinkType::unknown, "br0")).skip);
}

TEST_CASE("LinkGuard: offline skips with no_network") {
    LinkGuard g{false};

    SUBCASE("no route") {
        const auto c = g.check(offline());
        REQUIRE(c.skip);
        CHECK(c.skip->status == Status::no_network);
        CHECK(c.skip->error == "not started: no default route");
        CHECK(one_notice(c, "no default route: this computer is offline."));
    }
    SUBCASE("no carrier") {
        auto s = online(LinkType::wired);
        s.carrier = false;
        const auto c = g.check(s);
        REQUIRE(c.skip);
        CHECK(c.skip->status == Status::no_network);
        CHECK(c.skip->error == "not started: no carrier");
        CHECK(one_notice(c, "no carrier on eth0"));
    }
    SUBCASE("unknown carrier counts as online") {
        auto s = online(LinkType::wired);
        s.carrier.reset();
        CHECK_FALSE(g.check(s).skip);
    }
    SUBCASE("going offline and back") {
        CHECK(g.check(online(LinkType::wired)).notices.empty());
        CHECK(one_notice(g.check(offline()), "offline"));
        CHECK(g.check(offline()).notices.empty());
        CHECK(one_notice(g.check(online(LinkType::wired)), "a cable again"));
    }
    SUBCASE("Wi-Fi without carrier: both warnings") {
        auto s = online(LinkType::wifi, "wlan0");
        s.carrier = false;
        const auto c = g.check(s);
        REQUIRE(c.notices.size() == 2);
        CHECK(c.notices[0].text.starts_with("no carrier on wlan0"));
        CHECK(c.notices[1].text.starts_with("the connection is Wi-Fi"));
        CHECK(c.skip->status == Status::no_network);  // offline wins over skipped_wlan
    }
}

TEST_CASE("LinkGuard: unknown link type and a vanished interface") {
    LinkGuard g{false};
    CHECK(one_notice(g.check(online(LinkType::unknown, "br0")),
                     "can't tell whether br0 is a cable (bridge, VPN or virtual interface?)."));
    auto s = online(LinkType::unknown, "eth9");
    s.interface_exists = false;
    s.carrier.reset();
    const auto c = g.check(s);
    CHECK(one_notice(c, "interface eth9 of the default route isn't in /sys/class/net"));
    CHECK_FALSE(c.skip);  // can't tell: try
}

TEST_CASE("LinkGuard: switching between two cables says nothing") {
    LinkGuard g{false};
    CHECK(g.check(online(LinkType::wired, "eth0")).notices.empty());
    CHECK(g.check(online(LinkType::wired, "eth1")).notices.empty());
}

TEST_CASE("sense_link reads route, link and Wi-Fi") {
    net::LinkSensors s;
    std::optional<net::DefaultRoute> route = net::DefaultRoute{"wlan0", "192.0.2.1", 600, 4};
    net::LinkInfo link;
    link.exists = true;
    link.type = LinkType::wifi;
    link.carrier = true;
    net::WifiReading wifi;
    wifi.status = Status::ok;
    wifi.info = core::WifiInfo{};
    wifi.info->ssid = "ExampleNet";
    int wifi_reads = 0;
    s.route = [&] { return route; };
    s.link = [&](std::string_view i) {
        auto l = link;
        l.interface = std::string{i};
        return l;
    };
    s.wifi = [&](std::string_view) {
        ++wifi_reads;
        return wifi;
    };

    SUBCASE("Wi-Fi with details") {
        const auto snap = net::sense_link(s);
        CHECK(snap.has_route);
        CHECK(snap.interface_exists);
        CHECK(snap.usable());
        CHECK(snap.context.interface == "wlan0");
        CHECK(snap.context.gateway == "192.0.2.1");
        CHECK(snap.context.link_type == LinkType::wifi);
        REQUIRE(snap.context.wifi);
        CHECK(snap.context.wifi->ssid == "ExampleNet");
        CHECK(snap.wifi_error.empty());
    }
    SUBCASE("Wi-Fi details failing are kept as text") {
        wifi = net::WifiReading{Status::not_available, "iw is not installed", std::nullopt};
        const auto snap = net::sense_link(s);
        CHECK_FALSE(snap.context.wifi);
        CHECK(snap.wifi_error == "not_available: iw is not installed");
    }
    SUBCASE("a cable: no Wi-Fi read, speed kept") {
        link.type = LinkType::wired;
        link.speed_mbps = 1000;
        const auto snap = net::sense_link(s);
        CHECK(wifi_reads == 0);
        CHECK(snap.context.link_speed_mbps == 1000);
    }
    SUBCASE("no route: nothing else is read") {
        route.reset();
        s.link = [](std::string_view) -> net::LinkInfo {
            FAIL("link read without a route");
            return {};
        };
        const auto snap = net::sense_link(s);
        CHECK_FALSE(snap.has_route);
        CHECK_FALSE(snap.usable());
    }
    SUBCASE("no Wi-Fi sensor: details just stay empty") {
        s.wifi = nullptr;
        CHECK_FALSE(net::sense_link(s).context.wifi);
    }
}
