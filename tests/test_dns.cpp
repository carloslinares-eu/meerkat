/**
 * @file test_dns.cpp
 * @brief Tests for the DNS probe.
 */
#include <doctest/doctest.h>

#include <netdb.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <memory>
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

    // An exception with an empty message must still count as a failure.
    const Resolver silent = [](const std::string&) -> int { throw std::runtime_error(""); };
    const auto u = resolve("example.com", 1s, silent);
    CHECK(u.status == Status::error);
    CHECK(u.error == "resolver failed: ");
}

TEST_CASE("offline lookups are no_network, like ping's") {
    const Resolver again = [](const std::string&) { return EAI_AGAIN; };
    const auto a = resolve("example.com", 1s, again);
    CHECK(a.status == Status::no_network);
    CHECK(a.error == ::gai_strerror(EAI_AGAIN));

    const Resolver no_route = [](const std::string&) {
        errno = ENETUNREACH;
        return EAI_SYSTEM;
    };
    const auto n = resolve("example.com", 1s, no_route);
    CHECK(n.status == Status::no_network);
    CHECK(n.error == std::string{::gai_strerror(EAI_SYSTEM)} + ": " + std::strerror(ENETUNREACH));
}

TEST_CASE("EAI_SYSTEM keeps the errno of the lookup thread") {
    const Resolver denied = [](const std::string&) {
        errno = EACCES;
        return EAI_SYSTEM;
    };
    const auto r = resolve("example.com", 1s, denied);
    CHECK(r.status == Status::error);
    CHECK(r.error == std::string{::gai_strerror(EAI_SYSTEM)} + ": " + std::strerror(EACCES));

    const Resolver bare = [](const std::string&) { return EAI_SYSTEM; };
    CHECK(resolve("example.com", 1s, bare).error == ::gai_strerror(EAI_SYSTEM));
}

TEST_CASE("a negative timeout counts as 0 ms") {
    const Resolver slow = [](const std::string&) {
        std::this_thread::sleep_for(200ms);
        return 0;
    };
    const auto r = resolve("example.com", -5ms, slow);
    CHECK(r.status == Status::timeout);
    CHECK(r.error == "no answer within 0 ms");
}

TEST_CASE("hanging lookups are capped, and the cap frees up again") {
    // The flag is shared by ownership, never by reference (see Resolver).
    auto release = std::make_shared<std::atomic<bool>>(false);
    const Resolver stuck = [release](const std::string&) {
        while (!release->load()) {
            std::this_thread::sleep_for(5ms);
        }
        return 0;
    };
    int started = 0;
    bool refused = false;
    for (int i = 0; i < 20 && !refused; ++i) {
        const auto r = resolve("example.com", 0ms, stuck);
        if (r.status == Status::timeout) {
            ++started;
        } else {
            CHECK(r.error == "too many earlier DNS lookups are still hanging");
            refused = true;
        }
    }
    CHECK(refused);
    CHECK(started <= 8);
    release->store(true);

    const Resolver fast = [](const std::string&) { return 0; };
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto r = resolve("example.com", 1s, fast);
    while (r.status != Status::ok && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        r = resolve("example.com", 1s, fast);
    }
    CHECK(r.status == Status::ok);
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
    // Sandboxes without an /etc/hosts entry for localhost can't do this.
    if (r.status != Status::ok) {
        MESSAGE("localhost doesn't resolve here (" << r.error << "); skipped");
        return;
    }
    CHECK(r.duration_ms.has_value());
}

TEST_CASE("live: a name under .invalid never resolves (RFC 6761)") {
    const auto r = resolve("meerkat-test.invalid", 10s);
    CHECK(r.status != Status::ok);
}
