/**
 * @file test_link.cpp
 * @brief Tests for net::read_link() against a hand-made sysfs tree.
 */
#include <doctest/doctest.h>

#include <filesystem>

#include "net/link.hpp"

using meerkat::core::LinkType;
using meerkat::net::read_link;

namespace {
/// Root of the hand-made sysfs tree; see tests/fixtures/README.md.
const std::filesystem::path kSys = std::filesystem::path{MEERKAT_FIXTURES_DIR} / "sys";
}  // namespace

TEST_CASE("Wi-Fi interface is classified as wifi, without a speed") {
    const auto l = read_link("wlp4s0", kSys);
    CHECK(l.exists);
    CHECK(l.type == LinkType::wifi);
    CHECK(l.carrier == true);
    CHECK(l.operstate == "up");
    CHECK_FALSE(l.speed_mbps.has_value());
}

TEST_CASE("built-in Ethernet with cable is wired, with its link speed") {
    const auto l = read_link("enp0s25", kSys);
    CHECK(l.type == LinkType::wired);
    CHECK(l.carrier == true);
    CHECK(l.speed_mbps == 1000.0);
}

TEST_CASE("unplugged USB Ethernet is still wired, but has no carrier and no speed") {
    const auto l = read_link("enx0c3796000001", kSys);
    CHECK(l.type == LinkType::wired);
    CHECK(l.carrier == false);
    CHECK(l.operstate == "down");
    CHECK_FALSE(l.speed_mbps.has_value());  // sysfs says -1
}

TEST_CASE("administratively down Ethernet has carrier false, not unknown") {
    const auto l = read_link("enp0s31f6", kSys);  // no readable carrier file
    CHECK(l.type == LinkType::wired);
    CHECK(l.operstate == "down");
    CHECK(l.carrier == false);
}

TEST_CASE("virtual bridges and loopback are unknown") {
    CHECK(read_link("docker0", kSys).type == LinkType::unknown);
    CHECK(read_link("lo", kSys).type == LinkType::unknown);
}

TEST_CASE("missing or unsafe interface names don't touch the filesystem") {
    const auto missing = read_link("eth9", kSys);
    CHECK_FALSE(missing.exists);
    CHECK(missing.type == LinkType::unknown);
    CHECK_FALSE(read_link("../../..", kSys).exists);
    CHECK_FALSE(read_link("", kSys).exists);
}
