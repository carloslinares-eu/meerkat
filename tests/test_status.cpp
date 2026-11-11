/**
 * @file test_status.cpp
 * @brief Tests for the Status and LinkType text forms.
 */
#include <doctest/doctest.h>

#include "core/status.hpp"

using namespace meerkat::core;

TEST_CASE("every Status round-trips through its text form") {
    for (const auto s : {Status::ok, Status::timeout, Status::error, Status::no_network,
                         Status::not_available, Status::skipped_wlan, Status::paused_official}) {
        CAPTURE(to_string(s));
        CHECK(parse_status(to_string(s)) == s);
    }
}

TEST_CASE("stored Status names never change") {
    // These strings are in users' CSV files; renaming one breaks old data.
    CHECK(to_string(Status::ok) == "ok");
    CHECK(to_string(Status::timeout) == "timeout");
    CHECK(to_string(Status::error) == "error");
    CHECK(to_string(Status::no_network) == "no_network");
    CHECK(to_string(Status::not_available) == "not_available");
    CHECK(to_string(Status::skipped_wlan) == "skipped_wlan");
    CHECK(to_string(Status::paused_official) == "paused_official");
}

TEST_CASE("LinkType round-trips and has stable names") {
    CHECK(to_string(LinkType::wired) == "wired");
    CHECK(to_string(LinkType::wifi) == "wifi");
    CHECK(to_string(LinkType::unknown) == "unknown");
    for (const auto t : {LinkType::unknown, LinkType::wired, LinkType::wifi}) {
        CHECK(parse_link_type(to_string(t)) == t);
    }
}

TEST_CASE("unknown text is rejected") {
    CHECK_FALSE(parse_status("OK").has_value());
    CHECK_FALSE(parse_status("").has_value());
    CHECK_FALSE(parse_link_type("wlan").has_value());
}
