/**
 * @file test_report.cpp
 * @brief Tests for the report's data loading and analysis (task 3.1), with
 *        the made-up tariff 250/200/150 down, 50/40/30 up.
 */
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "core/time.hpp"
#include "report/data.hpp"
#include "report/stats.hpp"
#include "store/csv.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace std::chrono_literals;
using report::Period;
using report::SpeedRow;

namespace {

/// 2026-03-10T00:00:00Z plus @p offset.
core::Timestamp at(std::chrono::seconds offset = {}) {
    return core::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 10}} + offset;
}

/// The made-up contract.
core::Contract contract() {
    core::Contract c;
    c.download = core::ContractSpeeds{250, 200, 150};
    c.upload = core::ContractSpeeds{50, 40, 30};
    return c;
}

/// A valid cable test.
SpeedRow test_at(core::Timestamp t, double down, double up = 45,
                 core::LinkType link = core::LinkType::wired) {
    SpeedRow r;
    r.time = t;
    r.status = core::Status::ok;
    r.link_type = link;
    r.download_mbps = down;
    r.upload_mbps = up;
    return r;
}

/// @p n tests on day @p day (0 = 2026-03-10), one per hour from 08:00.
std::vector<SpeedRow> day_of(int day, int n, double down, double up = 45) {
    std::vector<SpeedRow> v;
    for (int i = 0; i < n; ++i) {
        v.push_back(test_at(at(std::chrono::days{day} + std::chrono::hours{8 + i}), down, up));
    }
    return v;
}

/// Analyses @p rows in UTC.
report::Analysis analyse(std::vector<SpeedRow> rows) {
    report::SpeedData d;
    d.rows = std::move(rows);
    return report::analyse(d, {}, contract(), report::utc_local(), {at(), at(24h * 60)});
}

/// Appends @p a to @p b.
void add(std::vector<SpeedRow>& b, const std::vector<SpeedRow>& a) {
    b.insert(b.end(), a.begin(), a.end());
}

}  // namespace

TEST_CASE("summarize: percentiles between the nearest ranks") {
    const auto s = report::summarize({5, 1, 4, 2, 3});
    CHECK(s.count == 5);
    CHECK(s.min == 1);
    CHECK(s.max == 5);
    CHECK(s.median == 3);
    CHECK(s.p10 == doctest::Approx(1.4));
    CHECK(s.p90 == doctest::Approx(4.6));
    CHECK(s.mean == 3);
    CHECK_FALSE(report::summarize({}).median.has_value());
}

TEST_CASE("BNetzA rules: a good line has no deviation") {
    std::vector<SpeedRow> rows;
    for (int d = 0; d < 3; ++d) {
        add(rows, day_of(d, 10, 240, 48));
    }
    const auto a = analyse(rows);
    REQUIRE(a.download.windows.size() == 1);
    const auto& w = a.download.windows[0];
    CHECK(w.tests == 30);
    CHECK(w.normal_share == 1.0);
    CHECK_FALSE(w.deviation());
    CHECK(a.download.deviating_windows() == 0);
    CHECK(a.download.below_normal == 0);
    CHECK(a.upload.windows.size() == 1);
    CHECK_FALSE(a.upload.windows[0].deviation());
}

TEST_CASE("BNetzA rule 1: 90 % of the maximum missed on 2 of 3 days") {
    std::vector<SpeedRow> rows;
    add(rows, day_of(0, 10, 220));  // 220 < 225 all day
    add(rows, day_of(1, 10, 220));
    add(rows, day_of(2, 10, 240));
    auto a = analyse(rows);
    REQUIRE(a.download.windows.size() == 1);
    CHECK(a.download.windows[0].rule_max90);
    CHECK_FALSE(a.download.windows[0].rule_normal);
    CHECK_FALSE(a.download.windows[0].rule_minimum);
    CHECK(a.download.below_max90 == 20);

    SUBCASE("one test reaching it on day 2 is enough for that day") {
        rows[15].download_mbps = 226;
        a = analyse(rows);
        CHECK_FALSE(a.download.windows[0].rule_max90);
    }
}

TEST_CASE("BNetzA rule 2: the normal speed reached in under 90 % of the tests") {
    std::vector<SpeedRow> rows;
    for (int d = 0; d < 3; ++d) {
        add(rows, day_of(d, 10, 240));
    }
    for (int i = 0; i < 4; ++i) {  // 26 of 30 reach it: 86.7 %
        rows[static_cast<std::size_t>(i * 7)].download_mbps = 190;
    }
    auto a = analyse(rows);
    REQUIRE(a.download.windows.size() == 1);
    CHECK(a.download.windows[0].rule_normal);
    CHECK(a.download.windows[0].normal_share == doctest::Approx(26.0 / 30));
    CHECK(a.download.below_normal == 4);

    SUBCASE("exactly 90 % is no deviation") {
        rows[21].download_mbps = 240;  // 27 of 30
        a = analyse(rows);
        CHECK_FALSE(a.download.windows[0].rule_normal);
    }
}

TEST_CASE("BNetzA rule 3: below the minimum on 2 of 3 days") {
    std::vector<SpeedRow> rows;
    for (int d = 0; d < 3; ++d) {
        add(rows, day_of(d, 10, 240, 48));
    }
    rows[3].upload_mbps = 25;   // day 1, below 30
    rows[13].upload_mbps = 29;  // day 2
    auto a = analyse(rows);
    REQUIRE(a.upload.windows.size() == 1);
    CHECK(a.upload.windows[0].rule_minimum);
    CHECK(a.upload.below_minimum == 2);
    CHECK_FALSE(a.download.windows[0].deviation());  // checked per direction

    SUBCASE("on one day only: no deviation") {
        rows[13].upload_mbps = 48;
        a = analyse(rows);
        CHECK_FALSE(a.upload.windows[0].rule_minimum);
    }
}

TEST_CASE("measurement days and windows") {
    SUBCASE("a day with fewer than 10 tests isn't a measurement day") {
        std::vector<SpeedRow> rows;
        add(rows, day_of(0, 10, 220));
        add(rows, day_of(1, 9, 220));
        add(rows, day_of(2, 10, 220));
        const auto a = analyse(rows);
        REQUIRE(a.download.days.size() == 3);
        CHECK_FALSE(a.download.days[1].measurement_day);
        CHECK(a.download.windows.empty());  // only two measurement days
    }
    SUBCASE("windows slide over measurement days, skipping days without tests") {
        std::vector<SpeedRow> rows;
        for (const int d : {0, 1, 5, 6}) {
            add(rows, day_of(d, 10, 240));
        }
        const auto a = analyse(rows);
        REQUIRE(a.download.windows.size() == 2);
        CHECK(a.download.windows[0].days[2] == std::chrono::sys_days{std::chrono::year{2026} / 3 / 15});
        CHECK(a.download.windows[1].days[0] == std::chrono::sys_days{std::chrono::year{2026} / 3 / 11});
    }
    SUBCASE("three days spread over more than 14 days make no window") {
        std::vector<SpeedRow> rows;
        for (const int d : {0, 7, 15}) {
            add(rows, day_of(d, 10, 240));
        }
        CHECK(analyse(rows).download.windows.empty());
    }
    SUBCASE("without contract figures: days, but no thresholds and no windows") {
        std::vector<SpeedRow> rows;
        for (int d = 0; d < 3; ++d) {
            add(rows, day_of(d, 10, 240));
        }
        report::SpeedData data;
        data.rows = rows;
        const auto a = report::analyse(data, {}, {}, report::utc_local(), {at(), at(72h)});
        CHECK(a.download.days.size() == 3);
        CHECK_FALSE(a.download.thresholds.has_value());
        CHECK(a.download.windows.empty());
    }
}

TEST_CASE("only ok cable tests count; other links are summarised apart") {
    std::vector<SpeedRow> rows;
    for (int d = 0; d < 3; ++d) {
        add(rows, day_of(d, 10, 240));
    }
    rows.push_back(test_at(at(1h), 50, 5, core::LinkType::wifi));
    rows.push_back(test_at(at(2h), 60, 6, core::LinkType::wifi));
    rows.push_back(test_at(at(3h), 70, 7, core::LinkType::tunnel));
    auto failed = test_at(at(4h), 0);
    failed.status = core::Status::timeout;
    failed.download_mbps.reset();
    rows.push_back(failed);
    const auto a = analyse(rows);
    CHECK(a.download.speeds.count == 30);
    CHECK(a.download.speeds.min == 240);
    CHECK(a.wired_status.at(core::Status::ok) == 30);
    CHECK(a.wired_status.at(core::Status::timeout) == 1);
    CHECK(a.other_status.at(core::Status::ok) == 3);
    REQUIRE(a.other_links.size() == 2);
    CHECK(a.other_links[0].type == core::LinkType::wifi);
    CHECK(a.other_links[0].tests == 2);
    CHECK(a.other_links[0].download.median == 55);
    CHECK(a.other_links[1].type == core::LinkType::tunnel);
}

TEST_CASE("hour-of-day buckets use the local hour") {
    std::vector<SpeedRow> rows{test_at(at(8h), 240), test_at(at(8h + 30min), 190),
                               test_at(at(9h), 230)};
    auto a = analyse(rows);
    CHECK(a.hours[8].tests == 2);
    CHECK(a.hours[8].median_down == 215);
    CHECK(a.hours[8].below_normal_down == 1);
    CHECK(a.hours[9].tests == 1);

    // One hour ahead of UTC: the same tests fall an hour later.
    report::SpeedData d;
    d.rows = rows;
    const report::LocalOf plus_one = [](core::Timestamp t) {
        return report::utc_local()(t + 1h);
    };
    a = report::analyse(d, {}, contract(), plus_one, {at(), at(24h)});
    CHECK(a.hours[9].tests == 2);
    CHECK(a.hours[10].tests == 1);
}

namespace {

/// Writes connectivity rounds: each with a gateway ping, one public ping
/// and a DNS lookup; @p up says whether the public host answered.
void write_rounds(const std::filesystem::path& dir, const std::vector<std::pair<core::Timestamp, bool>>& rounds) {
    store::CsvWriter w{dir / store::kConnectivityFile, store::connectivity_schema()};
    core::NetworkContext ctx;
    ctx.link_type = core::LinkType::wired;
    ctx.interface = "eth0";
    ctx.gateway = "192.0.2.1";
    for (const auto& [t, up] : rounds) {
        core::PingResult gw;
        gw.status = core::Status::ok;
        gw.target = "192.0.2.1";
        gw.sent = 3;
        gw.received = 3;
        w.write_row(store::ping_row({t, ctx, gw}));
        core::PingResult pub;
        pub.status = core::Status::ok;
        pub.target = "203.0.113.5";
        pub.sent = 3;
        pub.received = up ? 3 : 0;
        w.write_row(store::ping_row({t, ctx, pub}));
        core::DnsResult dns;
        dns.status = up ? core::Status::ok : core::Status::timeout;
        dns.host = "example.com";
        w.write_row(store::dns_row({t, ctx, dns}));
    }
}

}  // namespace

TEST_CASE("load_outages rebuilds outages from connectivity.csv") {
    test::TempDir dir;
    std::vector<std::pair<core::Timestamp, bool>> rounds;
    for (int i = 0; i < 10; ++i) {
        rounds.emplace_back(at(std::chrono::seconds{30 * i}), i < 3 || i > 6);  // 3..6 down
    }
    write_rounds(dir.path, rounds);
    const auto settings = core::OutageSettings::for_interval(30s, 2);
    const Period all{at(), at(24h)};

    auto d = report::load_outages(dir.path, all, settings, {});
    CHECK(d.file_exists);
    CHECK(d.rounds == 10);
    CHECK(d.down_rounds == 4);
    REQUIRE(d.outages.size() == 1);
    CHECK(d.outages[0].start == at(90s));
    CHECK(d.outages[0].end == at(210s));
    CHECK(d.outages[0].scope == core::OutageScope::internet);  // the gateway answered

    SUBCASE("down rounds just after a speed test started don't count") {
        d = report::load_outages(dir.path, all, settings, {at(60s)});  // shadows until 150 s
        REQUIRE(d.outages.size() == 1);
        CHECK(d.outages[0].start == at(150s));
    }
    SUBCASE("rounds outside the period are left out") {
        d = report::load_outages(dir.path, {at(), at(150s)}, settings, {});
        CHECK(d.rounds == 5);  // two of them down
        REQUIRE(d.outages.size() == 1);  // still down at the end: ended there
        CHECK(d.outages[0].end_reason == core::OutageEnd::stopped);
    }
    SUBCASE("no connectivity.csv: nothing, and said so") {
        test::TempDir empty;
        d = report::load_outages(empty.path, all, settings, {});
        CHECK_FALSE(d.file_exists);
        CHECK(d.outages.empty());
    }
}

TEST_CASE("load_speed keeps the period's rows and counts unreadable ones") {
    test::TempDir dir;
    {
        store::CsvWriter w{dir.path / store::kSpeedFile, store::speed_schema()};
        for (int i = 0; i < 3; ++i) {
            core::Measurement<core::SpeedResult> m;
            m.time = at(std::chrono::hours{i});
            m.context.link_type = core::LinkType::wired;
            m.context.interface = "eth0";
            m.result.status = core::Status::ok;
            m.result.download_mbps = 240;
            m.result.upload_mbps = 45;
            m.result.server_id = 12345;
            m.result.server_name = "Example Server";
            w.write_row(store::speed_row(m));
        }
        auto row = store::speed_row({at(5h), {}, {}});
        row[0] = "yesterday";  // never written by meerkat
        w.write_row(row);
    }
    const auto d = report::load_speed(dir.path, {at(1h), at(24h)});
    CHECK(d.file_exists);
    REQUIRE(d.rows.size() == 2);
    CHECK(d.rows[0].time == at(1h));
    CHECK(d.rows[0].download_mbps == 240);
    CHECK(d.rows[0].server_id == 12345);
    CHECK(d.rows[0].server_name == "Example Server");
    CHECK(d.rows[0].link_type == core::LinkType::wired);
    CHECK(d.unreadable == 1);
}

TEST_CASE("system_local: the system zone, or UTC without one") {
    const auto local = report::system_local();
    const auto t = at(12h);
    const auto l = local(t);
    // Whatever the zone, it is within a day of UTC and a valid hour.
    CHECK(l.hour >= 0);
    CHECK(l.hour < 24);
    CHECK(std::chrono::abs(l.day - std::chrono::sys_days{std::chrono::year{2026} / 3 / 10}) <=
          std::chrono::days{1});
}
