/**
 * @file test_ping.cpp
 * @brief Tests for the ping parser and probe.
 */
#include <doctest/doctest.h>

#include <algorithm>
#include <cerrno>
#include <system_error>

#include "probe/ping.hpp"
#include "support.hpp"

using meerkat::core::Status;
using meerkat::probe::is_valid_ping_target;
using meerkat::probe::parse_ping;
using meerkat::probe::ping;
using meerkat::probe::PingOptions;
using meerkat::test::FakeRunner;
using meerkat::test::fixture;
using namespace std::chrono_literals;

TEST_CASE("parses a clean run (real output, anonymised)") {
    const auto r = parse_ping(fixture("ping/gateway_ok.txt"));
    CHECK(r.status == Status::ok);
    CHECK(r.sent == 5);
    CHECK(r.received == 5);
    CHECK(r.loss_percent() == doctest::Approx(0.0));
    CHECK(r.rtt_min_ms == doctest::Approx(1.872));
    CHECK(r.rtt_avg_ms == doctest::Approx(3.391));
    CHECK(r.rtt_max_ms == doctest::Approx(4.605));
    CHECK(r.rtt_mdev_ms == doctest::Approx(1.167));
}

TEST_CASE("total loss: counts present, round-trip times empty (not 0)") {
    const auto r = parse_ping(fixture("ping/loss_100.txt"));
    CHECK(r.status == Status::ok);
    CHECK(r.sent == 5);
    CHECK(r.received == 0);
    CHECK(r.loss_percent() == doctest::Approx(100.0));
    CHECK_FALSE(r.rtt_avg_ms.has_value());
}

TEST_CASE("errors and duplicates in the statistics line") {
    const auto u = parse_ping(fixture("ping/host_unreachable.txt"));
    CHECK(u.sent == 5);
    CHECK(u.received == 0);
    const auto d = parse_ping(fixture("ping/partial_loss_dup.txt"));
    CHECK(d.sent == 4);
    CHECK(d.received == 2);
    CHECK(d.loss_percent() == doctest::Approx(50.0));
    CHECK(d.rtt_max_ms == doctest::Approx(38.9));
}

TEST_CASE("output without statistics is an error") {
    CHECK(parse_ping("").status == Status::error);
    CHECK(parse_ping("PING x (192.0.2.1) 56(84) bytes of data.\n").status == Status::error);
}

TEST_CASE("ping() builds a safe, bounded command") {
    FakeRunner fake{FakeRunner::exited(fixture("ping/gateway_ok.txt"))};
    const auto r = ping("192.0.2.1", PingOptions{.count = 5, .interval = 200ms, .reply_timeout = 1s},
                        fake.runner());
    CHECK(r.status == Status::ok);
    CHECK(r.target == "192.0.2.1");
    REQUIRE(fake.calls.size() == 1);
    const auto& c = fake.calls[0];
    CHECK(c.program == "ping");
    CHECK(c.args == std::vector<std::string>{"-n", "-c", "5", "-i", "0.200", "-W", "1", "-w", "3",
                                             "192.0.2.1"});
    CHECK(c.timeout == 8s);  // ping's own deadline (3 s) + 5 s
    CHECK(c.env.at(0) == std::pair<std::string, std::string>{"LC_ALL", "C"});
}

TEST_CASE("ping() clamps silly options") {
    FakeRunner fake{FakeRunner::exited(fixture("ping/gateway_ok.txt"))};
    ping("192.0.2.1", PingOptions{.count = 0, .interval = 1ms, .reply_timeout = 0s}, fake.runner());
    const auto& a = fake.calls.at(0).args;
    CHECK(a[2] == "1");      // count >= 1
    CHECK(a[4] == "0.200");  // interval >= 200 ms
    CHECK(a[6] == "1");      // reply timeout >= 1 s
}

TEST_CASE("ping() maps outcomes to statuses") {
    SUBCASE("total loss (exit 1) is a valid measurement") {
        FakeRunner fake{FakeRunner::exited(fixture("ping/loss_100.txt"), 1)};
        const auto r = ping("198.51.100.1", {}, fake.runner());
        CHECK(r.status == Status::ok);
        CHECK(r.loss_percent() == doctest::Approx(100.0));
    }
    SUBCASE("cannot send at all") {
        FakeRunner fake{FakeRunner::exited("", 2, fixture("ping/network_unreachable.err"))};
        const auto r = ping("192.0.2.1", {}, fake.runner());
        CHECK(r.status == Status::no_network);
        CHECK(r.error == "ping: connect: Network is unreachable");
    }
    SUBCASE("name doesn't resolve") {
        FakeRunner fake{FakeRunner::exited("", 2, "ping: nonexistent.invalid: Name or service not known\n")};
        const auto r = ping("nonexistent.invalid", {}, fake.runner());
        CHECK(r.status == Status::error);
        CHECK(r.error.find("Name or service not known") != std::string::npos);
    }
    SUBCASE("not installed") {
        FakeRunner fake{FakeRunner::not_found()};
        CHECK(ping("192.0.2.1", {}, fake.runner()).status == Status::not_available);
    }
    SUBCASE("overran the deadline") {
        FakeRunner fake{FakeRunner::timed_out()};
        CHECK(ping("192.0.2.1", {}, fake.runner()).status == Status::timeout);
    }
}

TEST_CASE("ping() never throws: an OS failure becomes a status") {
    const meerkat::sys::Runner failing = [](const meerkat::sys::Command&) -> meerkat::sys::ProcessResult {
        throw std::system_error(EAGAIN, std::generic_category(), "fork");
    };
    meerkat::core::PingResult r;
    CHECK_NOTHROW(r = ping("192.0.2.1", {}, failing));
    CHECK(r.status == Status::error);
    CHECK(r.error.find("cannot run ping") != std::string::npos);
}

TEST_CASE("targets that could be read as options or split are refused") {
    for (const char* ok : {"192.0.2.1", "2001:db8::1", "fe80::1%wlp4s0", "example.com"}) {
        CAPTURE(ok);
        CHECK(is_valid_ping_target(ok));
    }
    for (const char* bad : {"", "-f", "--help", "a b", "a\tb", "a\nb"}) {
        CAPTURE(bad);
        CHECK_FALSE(is_valid_ping_target(bad));
    }
    FakeRunner fake{FakeRunner::exited("")};
    const auto r = ping("-f", {}, fake.runner());
    CHECK(r.status == Status::error);
    CHECK(fake.calls.empty());
}

TEST_CASE("live: pinging localhost works (needs iputils ping)") {
    if (!meerkat::sys::find_executable("ping", "/usr/bin:/bin")) {
        MESSAGE("ping not installed; skipped");
        return;
    }
    const auto r = ping("127.0.0.1", PingOptions{.count = 2});
    // Containers may forbid ICMP sockets; then ping fails cleanly instead.
    if (r.status == Status::ok) {
        CHECK(r.sent == 2);
        CHECK(r.received == 2);
        CHECK(r.rtt_avg_ms.has_value());
    } else {
        MESSAGE("ping unavailable here: " << r.error);
        CHECK(r.status != Status::timeout);
    }
}
