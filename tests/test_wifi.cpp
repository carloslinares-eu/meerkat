/**
 * @file test_wifi.cpp
 * @brief Tests for the `iw dev <if> link` parser and reader.
 */
#include <doctest/doctest.h>

#include <cerrno>
#include <system_error>

#include "net/wifi.hpp"
#include "support.hpp"

using meerkat::core::Status;
using meerkat::net::parse_iw_link;
using meerkat::net::read_wifi;
using meerkat::test::FakeRunner;
using meerkat::test::fixture;

TEST_CASE("parses a Wi-Fi 6 link (real iw output, anonymised)") {
    const auto info = parse_iw_link(fixture("iw/link_wifi6_160mhz.txt"));
    REQUIRE(info.has_value());
    CHECK(info->bssid == "02:00:00:00:00:02");
    CHECK(info->ssid == "ExampleNet");
    CHECK(info->frequency_mhz == doctest::Approx(5640.0));
    CHECK(info->signal_dbm == doctest::Approx(-58.0));
    CHECK(info->rx_bitrate_mbps == doctest::Approx(1200.9));
    CHECK(info->tx_bitrate_mbps == doctest::Approx(1200.9));
}

TEST_CASE("older iw on 2.4 GHz: integer freq, SSID with spaces, no rx bitrate") {
    const auto info = parse_iw_link(fixture("iw/link_old_iw_2ghz.txt"));
    REQUIRE(info.has_value());
    CHECK(info->ssid == "Example Net 2");
    CHECK(info->frequency_mhz == doctest::Approx(2412.0));
    CHECK(info->signal_dbm == doctest::Approx(-71.0));
    CHECK(info->tx_bitrate_mbps == doctest::Approx(72.2));
    CHECK_FALSE(info->rx_bitrate_mbps.has_value());  // missing, not 0
}

TEST_CASE("the SSID is kept byte for byte: escapes decoded, spaces not trimmed") {
    const auto info = parse_iw_link(
        "Connected to 02:00:00:00:00:04 (on wlan0)\n"
        "\tSSID: \\x20Caf\\xc3\\xa9 \\x5c Net\\x20\n"
        "\tfreq: 2437\n");
    REQUIRE(info.has_value());
    CHECK(info->ssid == " Caf\xc3\xa9 \\ Net ");  // " Café \ Net "; iw escapes \ as \x5c
    // Anything else after a backslash is taken as it is.
    const auto odd = parse_iw_link("Connected to 02:00:00:00:00:04 (on wlan0)\n"
                                   "\tSSID: a\\\\b\\xZZ\n");
    REQUIRE(odd.has_value());
    CHECK(odd->ssid == "a\\\\b\\xZZ");
}

TEST_CASE("a reading of nan or inf stays empty, not NaN (#44)") {
    const auto info = parse_iw_link(fixture("iw/link_nan.txt"));
    REQUIRE(info.has_value());
    CHECK(info->frequency_mhz == doctest::Approx(5180.0));
    CHECK_FALSE(info->signal_dbm.has_value());
    CHECK_FALSE(info->tx_bitrate_mbps.has_value());
}

TEST_CASE("'Not connected.' and garbage give no details") {
    CHECK_FALSE(parse_iw_link(fixture("iw/link_not_connected.txt")).has_value());
    CHECK_FALSE(parse_iw_link("").has_value());
    CHECK_FALSE(parse_iw_link("command failed: No such device (-19)\n").has_value());
}

TEST_CASE("read_wifi runs iw without a shell, in the C locale") {
    FakeRunner fake{FakeRunner::exited(fixture("iw/link_wifi6_160mhz.txt"))};
    const auto r = read_wifi("wlp4s0", fake.runner());
    CHECK(r.status == Status::ok);
    REQUIRE(r.info.has_value());
    CHECK(r.info->ssid == "ExampleNet");
    REQUIRE(fake.calls.size() == 1);
    CHECK(fake.calls[0].program == "iw");
    CHECK(fake.calls[0].args == std::vector<std::string>{"dev", "wlp4s0", "link"});
    REQUIRE(fake.calls[0].env.size() == 1);
    CHECK(fake.calls[0].env[0].first == "LC_ALL");
    CHECK(fake.calls[0].env[0].second == "C");
}

TEST_CASE("read_wifi maps each failure to a status") {
    SUBCASE("not associated") {
        FakeRunner fake{FakeRunner::exited(fixture("iw/link_not_connected.txt"))};
        CHECK(read_wifi("wlp4s0", fake.runner()).status == Status::no_network);
    }
    SUBCASE("iw not installed") {
        FakeRunner fake{FakeRunner::not_found()};
        const auto r = read_wifi("wlp4s0", fake.runner());
        CHECK(r.status == Status::not_available);
        CHECK(r.error.find("iw") != std::string::npos);
    }
    SUBCASE("iw hangs") {
        FakeRunner fake{FakeRunner::timed_out()};
        CHECK(read_wifi("wlp4s0", fake.runner()).status == Status::timeout);
    }
    SUBCASE("iw fails") {
        FakeRunner fake{FakeRunner::exited("", 237, "command failed: No such device (-19)\n")};
        const auto r = read_wifi("wlan9", fake.runner());
        CHECK(r.status == Status::error);
        CHECK(r.error.find("No such device") != std::string::npos);
    }
    SUBCASE("radio off / interface down is no_network, not a tool error") {
        FakeRunner fake{FakeRunner::exited("", 156, "command failed: Network is down (-100)\n")};
        const auto r = read_wifi("wlp4s0", fake.runner());
        CHECK(r.status == Status::no_network);
        CHECK(r.error.find("Network is down") != std::string::npos);
    }
    SUBCASE("killed by a signal: the signal is named") {
        meerkat::sys::ProcessResult killed;
        killed.outcome = meerkat::sys::ProcessResult::Outcome::signaled;
        killed.signal = 9;
        FakeRunner fake{killed};
        const auto r = read_wifi("wlp4s0", fake.runner());
        CHECK(r.status == Status::error);
        CHECK(r.error == "iw was killed by signal 9");
    }
    SUBCASE("unexpected output") {
        FakeRunner fake{FakeRunner::exited("something else\n")};
        CHECK(read_wifi("wlp4s0", fake.runner()).status == Status::error);
    }
}

TEST_CASE("read_wifi never throws: an OS failure becomes a status") {
    const meerkat::sys::Runner failing = [](const meerkat::sys::Command&) -> meerkat::sys::ProcessResult {
        throw std::system_error(EMFILE, std::generic_category(), "pipe2");
    };
    meerkat::net::WifiReading r;
    CHECK_NOTHROW(r = read_wifi("wlp4s0", failing));
    CHECK(r.status == Status::error);
    CHECK(r.error.find("cannot run iw") != std::string::npos);
}

TEST_CASE("read_wifi refuses unsafe interface names without running anything") {
    FakeRunner fake{FakeRunner::exited("")};
    const auto r = read_wifi("../x", fake.runner());
    CHECK(r.status == Status::error);
    CHECK(fake.calls.empty());
}
