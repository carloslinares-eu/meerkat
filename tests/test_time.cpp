/**
 * @file test_time.cpp
 * @brief Tests for the ISO-8601 timestamp helpers.
 */
#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "core/time.hpp"

using meerkat::core::parse_iso8601;
using meerkat::core::Timestamp;
using meerkat::core::to_iso8601;
using namespace std::chrono;

namespace {
/// Builds a UTC timestamp from its parts.
Timestamp utc(int y, unsigned mo, unsigned d, int h, int mi, int s) {
    return Timestamp{sys_days{year{y} / month{mo} / day{d}} + hours{h} + minutes{mi} + seconds{s}};
}
}  // namespace

TEST_CASE("to_iso8601 formats UTC with a Z") {
    CHECK(to_iso8601(utc(2026, 11, 11, 20, 7, 36)) == "2026-11-11T20:07:36Z");
    CHECK(to_iso8601(utc(1999, 1, 2, 3, 4, 5)) == "1999-01-02T03:04:05Z");
    CHECK(to_iso8601(Timestamp{}) == "1970-01-01T00:00:00Z");
}

TEST_CASE("parse_iso8601 round-trips to_iso8601") {
    const auto t = utc(2026, 3, 29, 1, 59, 59);
    CHECK(parse_iso8601(to_iso8601(t)) == t);
}

TEST_CASE("parse_iso8601 converts offsets to UTC") {
    const auto expected = utc(2026, 11, 11, 20, 7, 36);
    CHECK(parse_iso8601("2026-11-11T20:07:36Z") == expected);
    CHECK(parse_iso8601("2026-11-11T21:07:36+01:00") == expected);  // CET
    CHECK(parse_iso8601("2026-11-11T21:07:36+0100") == expected);
    CHECK(parse_iso8601("2026-11-11T21:07:36+01") == expected);
    CHECK(parse_iso8601("2026-11-11T15:07:36-05:00") == expected);
    CHECK(parse_iso8601("2026-11-11 20:07:36Z") == expected);       // space separator
    CHECK(parse_iso8601("2026-11-11T20:07:36.987Z") == expected);   // fraction dropped
    CHECK(parse_iso8601("2026-11-11t20:07:36z") == expected);       // RFC 3339 lower case
    // Offset that crosses midnight and a month boundary.
    CHECK(parse_iso8601("2026-12-01T00:30:00+02:00") == utc(2026, 11, 30, 22, 30, 0));
}

TEST_CASE("parse_iso8601 rejects malformed or zone-less text") {
    for (const char* bad : {"", "2026-11-11", "2026-11-11T20:07:36",  // no zone
                            "2026-11-11T20:07Z", "2026-13-01T00:00:00Z", "2026-02-30T00:00:00Z",
                            "2026-11-11T24:00:00Z", "2026-11-11T20:07:36.Z",
                            "2026-11-11T20:07:36+1:00", "2026-11-11T20:07:36+25:00",
                            "2026-11-11T20:07:36Zjunk", " 2026-11-11T20:07:36Z",
                            "2026/11/11T20:07:36Z", "2026-11-11T20:07:60Z",
                            "2026-11-11T23:58:60Z"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_iso8601(bad).has_value());
    }
}

TEST_CASE("parse_iso8601 accepts a real leap second and folds it forward") {
    CHECK(parse_iso8601("2016-12-31T23:59:60Z") == utc(2017, 1, 1, 0, 0, 0));
    CHECK(parse_iso8601("2017-01-01T00:59:60+01:00") == utc(2017, 1, 1, 0, 0, 0));
    CHECK(parse_iso8601("2017-01-01T05:29:60+05:30") == utc(2017, 1, 1, 0, 0, 0));
    CHECK_FALSE(parse_iso8601("2016-12-31T12:59:60Z").has_value());  // not 23:59 UTC
}

TEST_CASE("parse_iso8601 accepts a leap day") {
    CHECK(parse_iso8601("2028-02-29T12:00:00Z") == utc(2028, 2, 29, 12, 0, 0));
    CHECK_FALSE(parse_iso8601("2026-02-29T12:00:00Z").has_value());
}
