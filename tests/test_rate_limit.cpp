/**
 * @file test_rate_limit.cpp
 * @brief Tests for RateLimitBackoff (#125).
 */
#include <doctest/doctest.h>

#include "core/rate_limit.hpp"

using namespace std::chrono_literals;
using meerkat::core::RateLimitBackoff;
using meerkat::core::Status;
using meerkat::core::Timestamp;

namespace {
/// @return A fixed time plus @p d.
Timestamp at(std::chrono::seconds d = 0s) { return Timestamp{1'773'100'000s} + d; }
}  // namespace

TEST_CASE("RateLimitBackoff: waits 1 h, doubles to 8 h, ends with a test that gets through") {
    RateLimitBackoff b;
    CHECK_FALSE(b.waiting(at()));
    CHECK_FALSE(b.record(Status::ok, at()));
    CHECK_FALSE(b.record(Status::error, at()));  // a failure of the line isn't the limit

    CHECK(b.record(Status::rate_limited, at()) == at(1h));
    CHECK(b.waiting(at(59min)) == at(1h));
    CHECK_FALSE(b.waiting(at(1h)));
    CHECK(b.record(Status::rate_limited, at(1h)) == at(3h));    // 2 h
    CHECK(b.record(Status::rate_limited, at(3h)) == at(7h));    // 4 h
    CHECK(b.record(Status::rate_limited, at(7h)) == at(15h));   // 8 h
    CHECK(b.record(Status::rate_limited, at(15h)) == at(23h));  // still 8 h

    CHECK_FALSE(b.record(Status::ok, at(23h)));
    CHECK_FALSE(b.waiting(at(23h)));
    CHECK(b.record(Status::rate_limited, at(24h)) == at(25h));  // back to 1 h
}
