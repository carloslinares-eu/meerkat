/**
 * @file test_measurement.cpp
 * @brief Tests for the measurement records.
 */
#include <doctest/doctest.h>

#include "core/measurement.hpp"

using namespace meerkat::core;

TEST_CASE("bytes/s to Mbit/s matches the Ookla sample") {
    // tests/fixtures/speedtest/ookla_ok.json: download 6250000 B/s, upload 3125000 B/s.
    CHECK(bytes_per_second_to_mbps(6'250'000.0) == doctest::Approx(50.0));
    CHECK(bytes_per_second_to_mbps(3'125'000.0) == doctest::Approx(25.0));
    CHECK(bytes_per_second_to_mbps(0.0) == 0.0);
}

TEST_CASE("PingResult::loss_percent") {
    PingResult p;
    CHECK_FALSE(p.loss_percent().has_value());  // nothing sent: unknown, not 0

    p.sent = 5;
    p.received = 5;
    CHECK(p.loss_percent() == doctest::Approx(0.0));
    p.received = 0;
    CHECK(p.loss_percent() == doctest::Approx(100.0));
    p.received = 3;
    CHECK(p.loss_percent() == doctest::Approx(40.0));
    p.received = 7;  // duplicates (ping can report more replies than requests)
    CHECK(p.loss_percent() == doctest::Approx(0.0));
    p.received = -1;  // corrupt count: unknown, never above 100 %
    CHECK_FALSE(p.loss_percent().has_value());
    p.received = 5;
    p.sent = 0;
    CHECK_FALSE(p.loss_percent().has_value());
}

TEST_CASE("results default to 'not measured', never to zero values") {
    const SpeedResult s;
    CHECK(s.status == Status::error);
    CHECK_FALSE(s.download_mbps.has_value());
    CHECK_FALSE(s.upload_mbps.has_value());
    const Measurement<SpeedResult> m;
    CHECK(m.context.link_type == LinkType::unknown);
    CHECK_FALSE(m.context.wifi.has_value());
}
