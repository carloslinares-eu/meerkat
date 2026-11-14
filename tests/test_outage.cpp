/**
 * @file test_outage.cpp
 * @brief Tests for core::assess and core::OutageTracker.
 */
#include <doctest/doctest.h>

#include <netdb.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "core/outage.hpp"

using namespace meerkat::core;
using namespace std::chrono_literals;

namespace {

/// A ping of @p target that ended with @p status and @p received replies of 3.
PingResult ping(const std::string& target, Status status, std::optional<int> received = 3,
                const std::string& error = {}) {
    PingResult p;
    p.target = target;
    p.status = status;
    p.error = error;
    if (status == Status::ok) {
        p.sent = 3;
        p.received = received;
    }
    return p;
}

/// A reply from @p target.
PingResult reply(const std::string& target = "192.0.2.10") { return ping(target, Status::ok, 3); }

/// No reply from @p target.
PingResult silent(const std::string& target = "192.0.2.10") { return ping(target, Status::ok, 0); }

/// A DNS lookup of example.com that ended with @p status.
DnsResult dns(Status status, const std::string& error = {}) {
    DnsResult d;
    d.host = "example.com";
    d.status = status;
    d.error = error;
    return d;
}

/// Time @p s seconds into the test day.
Timestamp at(long long s) { return Timestamp{std::chrono::seconds{1'780'000'000 + s}}; }

/// Up, down and unknown assessments.
const RoundAssessment kUp{Reachability::up, OutageScope::unknown, {}};
const RoundAssessment kDown{Reachability::down, OutageScope::internet, "192.0.2.10: no reply"};
const RoundAssessment kUnknown{Reachability::unknown, OutageScope::unknown, "ping missing"};

}  // namespace

TEST_CASE("assess: up as soon as one public host replies") {
    ConnectivityRound r;
    r.hosts = {silent("192.0.2.10"), reply("192.0.2.11")};
    CHECK(assess(r).reach == Reachability::up);
    CHECK(assess(r).reason.empty());
    r.hosts = {ping("192.0.2.10", Status::ok, 1)};  // 2 of 3 lost is still up
    CHECK(assess(r).reach == Reachability::up);
}

TEST_CASE("assess: down, with the scope from the gateway") {
    ConnectivityRound r;
    r.hosts = {silent("192.0.2.10"), silent("192.0.2.11")};

    SUBCASE("gateway replies: the line") {
        r.gateway = reply("192.0.2.1");
        const auto a = assess(r);
        CHECK(a.reach == Reachability::down);
        CHECK(a.scope == OutageScope::internet);
        CHECK(a.reason == "192.0.2.10: no reply; 192.0.2.11: no reply");
    }
    SUBCASE("gateway silent: local") {
        r.gateway = silent("192.0.2.1");
        CHECK(assess(r).scope == OutageScope::local);
    }
    SUBCASE("gateway not pinged or unclear: unknown") {
        CHECK(assess(r).scope == OutageScope::unknown);
        r.gateway = ping("192.0.2.1", Status::error, std::nullopt, "ping: bad");
        CHECK(assess(r).scope == OutageScope::unknown);
    }
    SUBCASE("no route: local") {
        r.hosts = {ping("192.0.2.10", Status::no_network, std::nullopt,
                        "connect: Network is unreachable")};
        const auto a = assess(r);
        CHECK(a.reach == Reachability::down);
        CHECK(a.scope == OutageScope::local);
        CHECK(a.reason == "192.0.2.10: no route");
    }
    SUBCASE("a host name that can't be resolved while the gateway answers: the line") {
        r.hosts = {ping("example.org", Status::no_network, std::nullopt,
                        "ping: example.org: Temporary failure in name resolution")};
        r.gateway = reply("192.0.2.1");
        CHECK(assess(r).reach == Reachability::down);
        CHECK(assess(r).scope == OutageScope::internet);
    }
    SUBCASE("a ping that timed out counts as failed") {
        r.hosts = {ping("192.0.2.10", Status::timeout, std::nullopt)};
        CHECK(assess(r).reach == Reachability::down);
        CHECK(assess(r).reason == "192.0.2.10: timed out");
    }
    SUBCASE("one clear failure is enough, next to unclear results") {
        r.hosts = {ping("192.0.2.10", Status::error, std::nullopt, "ping: weird"), silent("192.0.2.11")};
        const auto a = assess(r);
        CHECK(a.reach == Reachability::down);
        CHECK(a.reason == "192.0.2.10: ping: weird; 192.0.2.11: no reply");
    }
    SUBCASE("DNS doesn't rescue a round without ping replies (router cache)") {
        r.dns = dns(Status::ok);
        CHECK(assess(r).reach == Reachability::down);
    }
}

TEST_CASE("assess: unknown when nothing says anything about the line") {
    ConnectivityRound r;
    r.hosts = {ping("192.0.2.10", Status::not_available, std::nullopt, "ping is not installed"),
               ping("192.0.2.11", Status::error)};
    auto a = assess(r);
    CHECK(a.reach == Reachability::unknown);
    CHECK(a.reason == "192.0.2.10: ping is not installed; 192.0.2.11: error");

    r.hosts = {ping("192.0.2.10", Status::ok, std::nullopt)};  // ok without counts
    CHECK(assess(r).reach == Reachability::unknown);
}

TEST_CASE("assess: without public hosts the DNS lookup decides") {
    ConnectivityRound r;
    r.gateway = reply("192.0.2.1");
    CHECK(assess(r).reach == Reachability::unknown);
    CHECK(assess(r).reason == "nothing was probed");
    r.dns = dns(Status::ok);
    CHECK(assess(r).reach == Reachability::up);
    r.dns = dns(Status::timeout);
    CHECK(assess(r).reach == Reachability::down);
    CHECK(assess(r).scope == OutageScope::internet);
    CHECK(assess(r).reason == "DNS lookup of example.com timed out");
    r.dns = dns(Status::no_network, ::gai_strerror(EAI_AGAIN));  // offline computer
    CHECK(assess(r).reach == Reachability::down);
    CHECK(assess(r).scope == OutageScope::internet);  // the gateway's reply wins
    CHECK(assess(r).reason == std::string{"DNS lookup of example.com: no network ("} +
                              ::gai_strerror(EAI_AGAIN) + ")");
    {
        auto no_gateway = r;
        no_gateway.gateway.reset();
        CHECK(assess(no_gateway).scope == OutageScope::local);  // like a ping with no route
    }
    r.dns = dns(Status::error, ::gai_strerror(EAI_AGAIN));  // e.g. SERVFAIL from the router
    CHECK(assess(r).reach == Reachability::down);
    CHECK(assess(r).reason == std::string{"DNS lookup of example.com: "} + ::gai_strerror(EAI_AGAIN));
    r.dns = dns(Status::error, "Name or service not known");
    CHECK(assess(r).reach == Reachability::unknown);
    CHECK(assess(r).reason == "DNS lookup of example.com: Name or service not known");
}

TEST_CASE("OutageTracker: K down rounds in a row start an outage at the first") {
    OutageTracker t{{.failures_to_start = 3}};
    CHECK(t.observe(at(0), kUp).empty());
    CHECK(t.observe(at(30), kDown).empty());
    CHECK(t.observe(at(60), kDown).empty());
    CHECK_FALSE(t.current());
    const auto e = t.observe(at(90), kDown);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == OutageEvent::Kind::started);
    CHECK(e[0].outage.start == at(30));
    CHECK(e[0].outage.down_rounds == 3);
    CHECK(e[0].outage.scope == OutageScope::internet);
    CHECK(e[0].outage.reason == "192.0.2.10: no reply");
    CHECK_FALSE(e[0].outage.end);
    CHECK_FALSE(e[0].outage.duration());
    REQUIRE(t.current());

    CHECK(t.observe(at(120), kDown).empty());
    CHECK(t.current()->down_rounds == 4);

    const auto end = t.observe(at(150), kUp);
    REQUIRE(end.size() == 1);
    CHECK(end[0].kind == OutageEvent::Kind::ended);
    CHECK(end[0].outage.end == at(150));
    CHECK(end[0].outage.end_reason == OutageEnd::recovered);
    CHECK(end[0].outage.duration() == 120s);
    CHECK(end[0].outage.down_rounds == 4);
    CHECK_FALSE(t.current());
}

TEST_CASE("OutageTracker: short drops below the threshold are no outage") {
    OutageTracker t{{.failures_to_start = 2}};
    for (int i = 0; i < 10; ++i) {
        CHECK(t.observe(at(60 * i), kDown).empty());
        CHECK(t.observe(at(60 * i + 30), kUp).empty());
    }
    CHECK_FALSE(t.current());
}

TEST_CASE("OutageTracker: threshold 1 starts at once") {
    OutageTracker t{{.failures_to_start = 1}};
    const auto e = t.observe(at(0), kDown);
    REQUIRE(e.size() == 1);
    CHECK(e[0].outage.start == at(0));
    CHECK(e[0].outage.down_rounds == 1);
}

TEST_CASE("OutageTracker: unknown rounds neither count nor break a run") {
    OutageTracker t{{.failures_to_start = 2, .successes_to_end = 2}};
    CHECK(t.observe(at(0), kDown).empty());
    CHECK(t.observe(at(30), kUnknown).empty());
    REQUIRE(t.observe(at(60), kDown).size() == 1);
    CHECK(t.current()->start == at(0));
    CHECK(t.current()->down_rounds == 2);

    CHECK(t.observe(at(90), kUp).empty());
    CHECK(t.observe(at(120), kUnknown).empty());
    const auto e = t.observe(at(150), kUp);
    REQUIRE(e.size() == 1);
    CHECK(e[0].outage.end == at(90));
}

TEST_CASE("OutageTracker: recovery needs M up rounds in a row and ends at the first") {
    OutageTracker t{{.failures_to_start = 1, .successes_to_end = 2}};
    REQUIRE(t.observe(at(0), kDown).size() == 1);
    CHECK(t.observe(at(30), kUp).empty());
    CHECK(t.observe(at(60), kDown).empty());  // relapse: still the same outage
    CHECK(t.current()->down_rounds == 2);
    CHECK(t.observe(at(90), kUp).empty());
    const auto e = t.observe(at(120), kUp);
    REQUIRE(e.size() == 1);
    CHECK(e[0].outage.end == at(90));
    CHECK(e[0].outage.duration() == 90s);
}

TEST_CASE("OutageTracker: the scope is the first known one") {
    OutageTracker t{{.failures_to_start = 2}};
    const RoundAssessment unclear{Reachability::down, OutageScope::unknown, "a"};
    const RoundAssessment local{Reachability::down, OutageScope::local, "b"};
    t.observe(at(0), unclear);
    const auto e = t.observe(at(30), local);
    REQUIRE(e.size() == 1);
    CHECK(e[0].outage.scope == OutageScope::local);
    CHECK(e[0].outage.reason == "a");
    t.observe(at(60), kDown);  // internet now: the first known scope stays
    CHECK(t.current()->scope == OutageScope::local);
}

TEST_CASE("OutageTracker: a gap ends an open outage at its last down round") {
    OutageTracker t{{.failures_to_start = 1, .successes_to_end = 2, .max_gap = 90s}};

    SUBCASE("down before the gap") {
        t.observe(at(0), kDown);
        t.observe(at(30), kDown);
        const auto e = t.observe(at(3600), kUp);  // after a suspend
        REQUIRE(e.size() == 1);
        CHECK(e[0].outage.end == at(30));
        CHECK(e[0].outage.end_reason == OutageEnd::gap);
        CHECK_FALSE(t.current());
    }
    SUBCASE("down again after the gap: the old one ends, a new one starts") {
        t.observe(at(0), kDown);
        const auto e = t.observe(at(3600), kDown);
        REQUIRE(e.size() == 2);
        CHECK(e[0].kind == OutageEvent::Kind::ended);
        CHECK(e[0].outage.end == at(0));
        CHECK(e[0].outage.duration() == 0s);
        CHECK(e[1].kind == OutageEvent::Kind::started);
        CHECK(e[1].outage.start == at(3600));
    }
    SUBCASE("an unconfirmed recovery before the gap doesn't count") {
        t.observe(at(0), kDown);
        t.observe(at(30), kUp);  // 1 of the 2 needed
        const auto e = t.observe(at(3600), kUp);
        REQUIRE(e.size() == 1);
        CHECK(e[0].outage.end == at(0));
        CHECK(e[0].outage.end_reason == OutageEnd::gap);
    }
    SUBCASE("unknown rounds don't bridge a gap") {
        t.observe(at(0), kDown);
        for (int i = 1; i <= 100; ++i) {
            CHECK(t.observe(at(30 * i), kUnknown).empty());  // ping broken for 50 min
        }
        const auto e = t.observe(at(3030), kDown);
        REQUIRE(e.size() == 2);
        CHECK(e[0].outage.end == at(0));
        CHECK(e[0].outage.end_reason == OutageEnd::gap);
        CHECK(e[1].outage.start == at(3030));
    }
    SUBCASE("exactly max_gap is no gap") {
        t.observe(at(0), kDown);
        CHECK(t.observe(at(90), kDown).empty());
        CHECK(t.current()->down_rounds == 2);
    }
}

TEST_CASE("OutageTracker: a gap resets a run below the threshold") {
    OutageTracker t{{.failures_to_start = 2, .max_gap = 90s}};
    t.observe(at(0), kDown);
    CHECK(t.observe(at(600), kDown).empty());
    REQUIRE(t.observe(at(630), kDown).size() == 1);
    CHECK(t.current()->start == at(600));
}

TEST_CASE("OutageTracker: the clock stepping back is no gap") {
    OutageTracker t{{.failures_to_start = 1, .max_gap = 90s}};
    t.observe(at(1000), kDown);
    CHECK(t.observe(at(10), kDown).empty());  // back by 990 s
    const auto e = t.observe(at(40), kUp);
    REQUIRE(e.size() == 1);
    CHECK(e[0].outage.end_reason == OutageEnd::recovered);
    CHECK(e[0].outage.duration() == 0s);  // end before start: clamped
}

TEST_CASE("OutageTracker: finish() ends an open outage at its last down round") {
    OutageTracker t{{.failures_to_start = 1}};
    CHECK_FALSE(t.finish());
    t.observe(at(0), kDown);
    t.observe(at(30), kDown);
    const auto e = t.finish();
    REQUIRE(e);
    CHECK(e->kind == OutageEvent::Kind::ended);
    CHECK(e->outage.end == at(30));
    CHECK(e->outage.end_reason == OutageEnd::stopped);
    CHECK_FALSE(t.current());
    CHECK_FALSE(t.finish());
}

TEST_CASE("OutageTracker: observe(round) assesses the round") {
    OutageTracker t{{.failures_to_start = 1}};
    ConnectivityRound r;
    r.time = at(0);
    r.hosts = {silent()};
    r.gateway = reply("192.0.2.1");
    const auto e = t.observe(r);
    REQUIRE(e.size() == 1);
    CHECK(e[0].outage.scope == OutageScope::internet);
    CHECK(e[0].outage.start == at(0));
}

TEST_CASE("OutageSettings::for_interval: the gap follows the probe interval") {
    const auto s = OutageSettings::for_interval(std::chrono::minutes{2}, 2);
    CHECK(s.failures_to_start == 2);
    CHECK(s.max_gap == 6min);
    OutageTracker t{s};
    CHECK(t.observe(at(0), kDown).empty());
    CHECK(t.observe(at(120), kDown).size() == 1);  // 2 min apart is no gap
}

TEST_CASE("OutageTracker: invalid settings") {
    CHECK_THROWS_AS(OutageTracker({.failures_to_start = 0}), std::invalid_argument);
    CHECK_THROWS_AS(OutageTracker({.successes_to_end = 0}), std::invalid_argument);
    CHECK_THROWS_AS(OutageTracker({.max_gap = 0s}), std::invalid_argument);
}

TEST_CASE("outage names") {
    CHECK(to_string(OutageScope::internet) == "internet");
    CHECK(to_string(OutageScope::local) == "local");
    CHECK(to_string(OutageScope::unknown) == "unknown");
    CHECK(to_string(OutageEnd::recovered) == "recovered");
    CHECK(to_string(OutageEnd::gap) == "gap");
    CHECK(to_string(OutageEnd::stopped) == "stopped");
}

TEST_CASE("OutageTracker: inconclusive rounds count neither way but bridge a gap") {
    OutageTracker t{{.failures_to_start = 2, .max_gap = 90s}};
    const RoundAssessment busy{Reachability::inconclusive, OutageScope::unknown, "speed test"};
    REQUIRE(t.observe(at(0), kDown).empty());
    REQUIRE(t.observe(at(30), kDown).size() == 1);
    for (int i = 2; i <= 6; ++i) {
        CHECK(t.observe(at(30 * i), busy).empty());  // 2.5 min of a long speed test
    }
    CHECK(t.current()->down_rounds == 2);
    const auto e = t.observe(at(210), kUp);
    REQUIRE(e.size() == 1);
    CHECK(e[0].outage.end_reason == OutageEnd::recovered);  // not `gap`
    CHECK(e[0].outage.end == at(210));
}
