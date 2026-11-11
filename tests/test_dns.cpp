/**
 * @file test_dns.cpp
 * @brief Tests for the DNS probe.
 */
#include <doctest/doctest.h>

#include <netdb.h>

#include <chrono>
#include <stdexcept>
#include <thread>

#include "probe/dns.hpp"

using meerkat::core::Status;
using meerkat::probe::is_valid_dns_host;
using meerkat::probe::resolve;
using meerkat::probe::Resolver;
using namespace std::chrono_literals;

TEST_CASE("a successful lookup reports its duration") {
    const Resolver fast = [](const std::string&) {
        std::this_thread::sleep_for(20ms);
        return 0;
    };
    const auto r = resolve("example.com", 2s, fast);
    CHECK(r.status == Status::ok);
    CHECK(r.host == "example.com");
    REQUIRE(r.duration_ms.has_value());
    CHECK(*r.duration_ms >= 15.0);
    CHECK(*r.duration_ms < 1000.0);
}

TEST_CASE("a hanging resolver gives a timeout without blocking the caller") {
    const Resolver slow = [](const std::string&) {
        std::this_thread::sleep_for(1500ms);
        return 0;
    };
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = resolve("example.com", 100ms, slow);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK(r.status == Status::timeout);
    CHECK_FALSE(r.duration_ms.has_value());
    // The detached lookup finishes later on its own; nothing it uses is gone.
}

TEST_CASE("resolver errors become an error with the resolver's message") {
    const Resolver noname = [](const std::string&) { return EAI_NONAME; };
    const auto r = resolve("nonexistent.invalid", 1s, noname);
    CHECK(r.status == Status::error);
    CHECK(r.error == ::gai_strerror(EAI_NONAME));
    CHECK_FALSE(r.duration_ms.has_value());

    const Resolver throwing = [](const std::string&) -> int { throw std::runtime_error("boom"); };
    const auto t = resolve("example.com", 1s, throwing);
    CHECK(t.status == Status::error);
    CHECK(t.error == "resolver failed: boom");
}

TEST_CASE("invalid host names are refused without a lookup") {
    bool called = false;
    const Resolver spy = [&called](const std::string&) {
        called = true;
        return 0;
    };
    for (const char* bad : {"", "a b", "a\nb", "a\tb"}) {
        CAPTURE(bad);
        CHECK(resolve(bad, 1s, spy).status == Status::error);
    }
    CHECK_FALSE(called);
    CHECK(is_valid_dns_host("example.com"));
    CHECK(is_valid_dns_host("192.0.2.1"));
}

TEST_CASE("live: localhost resolves through the system resolver") {
    const auto r = resolve("localhost");
    CHECK(r.status == Status::ok);
    CHECK(r.duration_ms.has_value());
}

TEST_CASE("live: a name under .invalid never resolves (RFC 6761)") {
    const auto r = resolve("meerkat-test.invalid", 10s);
    CHECK(r.status != Status::ok);
}
