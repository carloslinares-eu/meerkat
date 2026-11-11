/**
 * @file test_interface.cpp
 * @brief Tests for net::is_valid_interface_name().
 */
#include <doctest/doctest.h>

#include "net/interface.hpp"

using meerkat::net::is_valid_interface_name;

TEST_CASE("names the kernel accepts are accepted") {
    for (const char* ok : {"eth0", "wlp4s0", "enx0c3796000001", "br-923f3fb21f1b", "veth1@if2",
                           "eth0.100", "wg+home", "tun#1", "x;rm", "a"}) {
        CAPTURE(ok);
        CHECK(is_valid_interface_name(ok));
    }
}

TEST_CASE("names the kernel forbids are refused") {
    for (const char* bad : {"", ".", "..", "a/b", "../x", "eth0:1", "eth 0", "eth\t0",
                            "sixteen-chars-xx"}) {
        CAPTURE(bad);
        CHECK_FALSE(is_valid_interface_name(bad));
    }
    CHECK(is_valid_interface_name("fifteen-chars-x"));
}
