/**
 * @file test_rate_limit.cpp
 * @brief Tests for waiting out the speed-test service's limit (#125, #126)
 *        and its state file.
 */
#include <doctest/doctest.h>

#include <fstream>
#include <stdexcept>

#include "core/rate_limit.hpp"
#include "store/rate_limit_file.hpp"
#include "support.hpp"

using namespace std::chrono_literals;
using meerkat::core::RateLimitState;
using meerkat::core::Status;
using meerkat::core::Timestamp;
using meerkat::core::rate_limit_after;
using meerkat::core::rate_limit_waiting;

namespace {
/// @return A fixed time plus @p d.
Timestamp at(std::chrono::seconds d = 0s) { return Timestamp{1'773'100'000s} + d; }
}  // namespace

TEST_CASE("rate limit: waits 1, 2, 4, then 8 h, timed from the round's start") {
    std::optional<RateLimitState> s;
    CHECK_FALSE(rate_limit_waiting(s, at()));
    s = rate_limit_after(s, Status::rate_limited, at());
    REQUIRE(s);
    CHECK(s->until == at(1h));
    CHECK(rate_limit_waiting(s, at(59min)) == at(1h));
    CHECK_FALSE(rate_limit_waiting(s, at(1h)));  // the round one wait later tests
    s = rate_limit_after(s, Status::rate_limited, at(1h));
    CHECK(s->until == at(3h));
    s = rate_limit_after(s, Status::rate_limited, at(3h));
    CHECK(s->until == at(7h));
    s = rate_limit_after(s, Status::rate_limited, at(7h));
    CHECK(s->until == at(15h));
    s = rate_limit_after(s, Status::rate_limited, at(15h));
    CHECK(s->until == at(23h));  // stays at 8 h
    CHECK(s->refusals == 4);     // and the count stops there
}

TEST_CASE("rate limit: only a result ends it; a failure of the line doesn't (#126)") {
    const std::optional<RateLimitState> s = RateLimitState{.until = at(4h), .refusals = 3};
    CHECK(rate_limit_after(s, Status::no_network, at(4h)) == s);
    CHECK(rate_limit_after(s, Status::timeout, at(4h)) == s);
    CHECK(rate_limit_after(s, Status::error, at(4h)) == s);
    CHECK_FALSE(rate_limit_after(s, Status::ok, at(4h)));
    CHECK_FALSE(rate_limit_after(std::nullopt, Status::error, at()));
}

TEST_CASE("rate-limit file: written, read back, removed; a broken one is an error") {
    meerkat::test::TempDir dir;
    using meerkat::store::read_rate_limit;
    using meerkat::store::write_rate_limit;
    CHECK_FALSE(read_rate_limit(dir.path));
    write_rate_limit(dir.path, RateLimitState{.until = at(2h), .refusals = 2});
    CHECK(read_rate_limit(dir.path) == RateLimitState{.until = at(2h), .refusals = 2});
    write_rate_limit(dir.path, std::nullopt);
    CHECK_FALSE(std::filesystem::exists(dir.path / "rate-limit"));
    write_rate_limit(dir.path, std::nullopt);  // nothing to remove: fine

    for (const auto* bad : {"", "2026-03-10T13:00:00Z", "2026-03-10T13:00:00Z 0", "soon 2", "2026-03-10T13:00:00Z 2x"}) {
        CAPTURE(bad);
        std::ofstream{dir.path / "rate-limit"} << bad << "\n";
        CHECK_THROWS_AS(read_rate_limit(dir.path), std::runtime_error);
    }
}
