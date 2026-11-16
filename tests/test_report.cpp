/**
 * @file test_report.cpp
 * @brief Tests for the report's data loading and analysis (task 3.1), with
 *        the made-up tariff 250/200/150 down, 50/40/30 up.
 */
#include <doctest/doctest.h>

#include <array>
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

/// Local day @p d (0 = 2026-03-10).
std::chrono::sys_days day(int d) {
    return std::chrono::sys_days{std::chrono::year{2026} / 3 / 10} + std::chrono::days{d};
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
        add(rows, day_of(2 * d, 10, 240, 48));  // a day off in between
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
    add(rows, day_of(2, 10, 220));
    add(rows, day_of(4, 10, 240));
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
        add(rows, day_of(2 * d, 10, 240));
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
        add(rows, day_of(2 * d, 10, 240, 48));  // a day off in between
    }
    rows[3].upload_mbps = 25;   // day 1, below 30
    rows[4].upload_mbps = 28;
    rows[13].upload_mbps = 29;  // day 2
    rows[18].upload_mbps = 20;
    auto a = analyse(rows);
    REQUIRE(a.upload.windows.size() == 1);
    CHECK(a.upload.windows[0].rule_minimum);
    CHECK(a.upload.below_minimum == 4);
    CHECK(a.upload.days[0].tests_below_minimum == 2);
    CHECK_FALSE(a.download.windows[0].deviation());  // checked per direction

    SUBCASE("on one day only: no deviation") {
        rows[13].upload_mbps = 48;
        rows[18].upload_mbps = 48;
        a = analyse(rows);
        CHECK_FALSE(a.upload.windows[0].rule_minimum);
    }
    SUBCASE("on a measurement day with only 10 tests a single dip counts") {
        rows[18].upload_mbps = 48;  // day 2: one test below the minimum, of 10
        a = analyse(rows);
        CHECK(a.upload.days[1].dips_needed() == 1);
        CHECK(a.upload.days[1].below_minimum());
        CHECK(a.upload.windows[0].rule_minimum);
    }
    SUBCASE("a single dip among more than 10 tests doesn't make a day count (kMinBelowMinimumPerDay)") {
        rows[18].upload_mbps = 48;         // day 2: one test below the minimum
        rows.push_back(test_at(at(std::chrono::days{2} + 20h), 240, 48));  // and an 11th test
        a = analyse(rows);
        CHECK(a.upload.days[1].dips_needed() == 2);
        CHECK(a.upload.days[1].tests_below_minimum == 1);
        CHECK_FALSE(a.upload.days[1].below_minimum());
        CHECK_FALSE(a.upload.windows[0].rule_minimum);
        CHECK(a.upload.below_minimum == 3);  // still counted as tests
    }
}

TEST_CASE("measurement days and windows") {
    SUBCASE("a day with fewer than 10 tests isn't a measurement day") {
        std::vector<SpeedRow> rows;
        add(rows, day_of(0, 10, 220));
        add(rows, day_of(2, 9, 220));
        add(rows, day_of(4, 10, 220));
        const auto a = analyse(rows);
        REQUIRE(a.download.days.size() == 3);
        CHECK_FALSE(a.download.days[1].measurement_day);
        CHECK(a.download.windows.empty());  // only two measurement days
    }
    SUBCASE("a test every day: windows 1/3/5, 2/4/6, … (a day off between days)") {
        std::vector<SpeedRow> rows;
        for (int d = 0; d < 6; ++d) {
            add(rows, day_of(d, 10, 240));
        }
        const auto a = analyse(rows);
        REQUIRE(a.download.windows.size() == 2);
        CHECK(a.download.windows[0].days == std::array{day(0), day(2), day(4)});
        CHECK(a.download.windows[1].days == std::array{day(1), day(3), day(5)});
    }
    SUBCASE("the worst days a campaign could pick, not the earliest (#97)") {
        // Measurement days 0, 2, 4, 5; below the minimum only on 2 and 5.
        // The earliest days 0/2/4 have one bad day; 0/2/5 has two.
        std::vector<SpeedRow> rows;
        add(rows, day_of(0, 10, 240));
        add(rows, day_of(2, 10, 140));
        add(rows, day_of(4, 10, 240));
        add(rows, day_of(5, 10, 140));
        const auto a = analyse(rows);
        REQUIRE(a.download.windows.size() == 1);  // 2 can't start one: 2/4 or 2/5, then nothing
        CHECK(a.download.windows[0].days == std::array{day(0), day(2), day(5)});
        CHECK(a.download.windows[0].rule_minimum);
        CHECK(a.download.deviating_windows() == 1);
    }
    SUBCASE("between equal rule counts, the lower normal share") {
        std::vector<SpeedRow> rows;
        add(rows, day_of(0, 10, 240));
        add(rows, day_of(2, 10, 240));
        add(rows, day_of(4, 10, 240));
        auto mixed = day_of(6, 10, 240);  // two tests below normal
        mixed[0].download_mbps = 190;
        mixed[1].download_mbps = 190;
        add(rows, mixed);
        const auto a = analyse(rows);
        REQUIRE(a.download.windows.size() == 2);  // starts 0 and 2
        CHECK(a.download.windows[0].days == std::array{day(0), day(2), day(6)});
        CHECK(a.download.windows[0].normal_share == doctest::Approx(28.0 / 30));
        CHECK_FALSE(a.download.windows[0].deviation());
    }
    SUBCASE("near misses before the normal share; ties by day (#104)") {
        // No set breaks a rule. 0/2/4 doesn't have the lowest normal share
        // (0/2/6 has) but two near misses: day 2 below the minimum twice,
        // day 4 under 90 % of the maximum all day.
        std::vector<SpeedRow> rows;
        add(rows, day_of(0, 10, 240));
        auto low = day_of(2, 10, 240);
        low[0].download_mbps = 140;  // below the minimum twice, below normal too
        low[1].download_mbps = 140;
        add(rows, low);
        add(rows, day_of(4, 10, 220));  // under 225 (90 % of 250) all day
        auto mixed = day_of(6, 10, 240);
        mixed[0].download_mbps = 190;  // 0/2/6: 27 of 30 normally, just no rule 2
        add(rows, mixed);
        const auto a = analyse(rows);
        REQUIRE_FALSE(a.download.windows.empty());
        CHECK(a.download.windows[0].days == std::array{day(0), day(2), day(4)});
        CHECK(a.download.windows[0].bad_days == 2);
        CHECK_FALSE(a.download.windows[0].deviation());

        report::WindowVerdict x;
        x.days = {day(0), day(2), day(4)};
        auto y = x;
        y.days = {day(1), day(3), day(5)};
        CHECK(report::worse_window(x, y));
        CHECK_FALSE(report::worse_window(y, x));
        CHECK_FALSE(report::worse_window(x, x));
    }
    SUBCASE("a single dip still decides between otherwise equal days (#106)") {
        // 12 tests a day, so a single dip doesn't count for rule 3.
        // From day 0: 0/2/4 or 0/2/5, with the same normal share. Day 5 has
        // one test below the minimum (not enough for rule 3), day 4 one just
        // below normal; without the dip the earlier 0/2/4 would be shown.
        std::vector<SpeedRow> rows;
        add(rows, day_of(0, 12, 240));
        add(rows, day_of(2, 12, 240));
        auto normal_dip = day_of(4, 12, 240);
        normal_dip[3].download_mbps = 190;
        add(rows, normal_dip);
        auto dip = day_of(5, 12, 240);
        dip[3].download_mbps = 140;
        add(rows, dip);
        const auto a = analyse(rows);
        REQUIRE_FALSE(a.download.windows.empty());
        CHECK(a.download.days[3].tests_below_minimum == 1);
        CHECK_FALSE(a.download.days[3].below_minimum());
        CHECK(a.download.windows[0].days == std::array{day(0), day(2), day(5)});
        CHECK(a.download.windows[0].bad_days == 0);
        CHECK(a.download.windows[0].tests_below_minimum == 1);
    }
    SUBCASE("one bad pair of days shows in many windows, but is one separate deviation (#104)") {
        std::vector<SpeedRow> rows;
        for (int d = 0; d < 16; ++d) {
            add(rows, day_of(d, 10, d == 10 || d == 12 ? 140 : 240));
        }
        const auto a = analyse(rows);
        CHECK(a.download.deviating_windows() > 5);
        CHECK(a.download.separate_deviations() == 1);
    }
    SUBCASE("consecutive days alone make no window") {
        std::vector<SpeedRow> rows;
        for (int d = 0; d < 3; ++d) {
            add(rows, day_of(d, 10, 240));
        }
        CHECK(analyse(rows).download.windows.empty());
    }
    SUBCASE("gaps without tests are skipped") {
        std::vector<SpeedRow> rows;
        for (const int d : {0, 1, 5, 8}) {
            add(rows, day_of(d, 10, 240));
        }
        const auto a = analyse(rows);
        REQUIRE(a.download.windows.size() == 2);  // 0/5/8 and 1/5/8
        CHECK(a.download.windows[0].days[1] == std::chrono::sys_days{std::chrono::year{2026} / 3 / 15});
        CHECK(a.download.windows[1].days[0] == std::chrono::sys_days{std::chrono::year{2026} / 3 / 11});
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
        add(rows, day_of(2 * d, 10, 240));
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
        auto lt = report::utc_local()(t + 1h);
        lt.offset = 1h;
        return lt;
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
    report::ReplayContext ctx;
    ctx.settings = core::OutageSettings::for_interval(30s, 2);
    ctx.interval = 30s;
    const Period all{at(), at(24h)};

    auto d = report::load_outages(dir.path, all, ctx);
    CHECK(d.file_exists);
    CHECK(d.rounds == 10);
    CHECK(d.down_rounds == 4);
    REQUIRE(d.outages.size() == 1);
    CHECK(d.outages[0].start == at(90s));
    CHECK(d.outages[0].end == at(210s));
    CHECK(d.outages[0].scope == core::OutageScope::internet);  // the gateway answered

    SUBCASE("down rounds while a speed test kept the line busy don't count") {
        ctx.busy = {{at(60s), at(150s)}};
        d = report::load_outages(dir.path, all, ctx);
        REQUIRE(d.outages.size() == 1);
        CHECK(d.outages[0].start == at(150s));
    }
    SUBCASE("an outage across the period's start keeps its real start") {
        d = report::load_outages(dir.path, {at(120s), at(24h)}, ctx);
        CHECK(d.rounds == 6);  // from 120 s on
        REQUIRE(d.outages.size() == 1);
        CHECK(d.outages[0].start == at(90s));
    }
    SUBCASE("rounds after the period are left out") {
        d = report::load_outages(dir.path, {at() - 48h, at(150s) - 48h}, ctx);
        CHECK(d.rounds == 0);
        CHECK(d.outages.empty());
    }
    SUBCASE("an outage still open where the data ends ends there") {
        std::vector<std::pair<core::Timestamp, bool>> down;
        for (int i = 0; i < 4; ++i) {
            down.emplace_back(at(24h + std::chrono::seconds{30 * i}), false);
        }
        test::TempDir other;
        write_rounds(other.path, down);
        d = report::load_outages(other.path, {at(), at(48h)}, ctx);
        REQUIRE(d.outages.size() == 1);
        CHECK(d.outages[0].end_reason == core::OutageEnd::stopped);
    }
    SUBCASE("an official pause bridges the slots the recorder left out") {
        // Rounds every 2 min during a pause, an outage across it: with the
        // pause known it stays one outage; without, the gaps end it.
        std::vector<std::pair<core::Timestamp, bool>> paused;
        for (int i = 0; i < 6; ++i) {
            paused.emplace_back(at(std::chrono::seconds{120 * i}), i == 0 || i == 5);
        }
        test::TempDir other;
        write_rounds(other.path, paused);
        auto without = report::load_outages(other.path, all, ctx);
        CHECK(without.outages.empty());  // every down round stands alone after a gap
        ctx.paused = {{at() - 300s, at(900s)}};
        const auto with = report::load_outages(other.path, all, ctx);
        REQUIRE(with.outages.size() == 1);
        CHECK(with.outages[0].start == at(120s));
        CHECK(with.outages[0].end == at(600s));
        CHECK(with.outages[0].end_reason == core::OutageEnd::recovered);
    }
    SUBCASE("a long busy span is found behind many short ones (#96)") {
        ctx.busy = {{at(60s), at(300s)}};
        for (int i = 0; i < 6; ++i) {
            ctx.busy.emplace_back(at(std::chrono::seconds{61 + i}), at(std::chrono::seconds{62 + i}));
        }
        d = report::load_outages(dir.path, all, ctx);
        CHECK(d.outages.empty());  // every down round (90–180 s) lies in the long span
    }
    SUBCASE("a round stamped ahead hides none after it (#96)") {
        auto shuffled = rounds;
        shuffled.insert(shuffled.begin() + 2, {at(23h), true});  // clock ahead, then corrected
        test::TempDir other;
        write_rounds(other.path, shuffled);
        d = report::load_outages(other.path, all, ctx);
        CHECK(d.rounds == 11);
        CHECK(d.out_of_order == 1);
        REQUIRE(d.outages.size() == 1);
        CHECK(d.outages[0].start == at(90s));
        CHECK(d.outages[0].end == at(210s));
    }
    SUBCASE("a jump back in time ends an outage there, no phantom one (#96)") {
        // Rounds 3..6 down; after round 4 the clock goes back an hour.
        auto jumped = rounds;
        for (std::size_t i = 5; i < jumped.size(); ++i) {
            jumped[i].first -= 1h;
        }
        test::TempDir other;
        write_rounds(other.path, jumped);
        d = report::load_outages(other.path, {at() - 2h, at(24h)}, ctx);
        CHECK(d.out_of_order == 1);
        REQUIRE(d.outages.size() == 2);
        CHECK(d.outages[0].start == at(90s));
        CHECK(d.outages[0].end_reason == core::OutageEnd::stopped);  // cut at the jump
        CHECK(d.outages[1].start == at(150s) - 1h);                  // rounds 5, 6 again down
    }
    SUBCASE("no connectivity.csv: nothing, and said so") {
        test::TempDir empty;
        d = report::load_outages(empty.path, all, ctx);
        CHECK_FALSE(d.file_exists);
        CHECK(d.outages.empty());
    }
}

TEST_CASE("load_speed keeps the period's rows and counts unreadable ones") {
    test::TempDir dir;
    {
        store::CsvWriter w{dir.path / store::kSpeedFile, store::speed_schema()};
        for (const int i : {0, 1, 30, 2}) {  // 30 h: a clock ahead, past the period (#96)
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
    CHECK(d.rows[1].time == at(2h));  // after the row stamped ahead
    CHECK(d.unreadable == 1);
    CHECK(d.out_of_order == 1);
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

TEST_CASE("replay_context: busy spans for tests that ran, paused spans for official pauses") {
    std::vector<SpeedRow> rows;
    rows.push_back(test_at(at(), 240));
    auto timed_out = test_at(at(300s), 0);
    timed_out.status = core::Status::timeout;
    rows.push_back(timed_out);
    auto skipped = test_at(at(600s), 0);
    skipped.status = core::Status::skipped_wlan;
    rows.push_back(skipped);
    for (const auto t : {900s, 1200s}) {
        auto p = test_at(at(t), 0);
        p.status = core::Status::paused_official;
        rows.push_back(p);
    }
    rows.push_back(test_at(at(1500s), 240));
    const auto c = report::replay_context(rows, 20s, 300s, 30s, 3);
    CHECK(c.interval == 30s);
    CHECK(c.settings.failures_to_start == 3);
    REQUIRE(c.busy.size() == 3);  // not the skipped or paused rows
    CHECK(c.busy[0] == report::Span{at() - 20s, at(90s)});
    CHECK(c.busy[1] == report::Span{at(280s), at(422s)});  // ran into its time limit
    REQUIRE(c.paused.size() == 1);
    CHECK(c.paused[0] == report::Span{at(600s), at(1500s)});
}

TEST_CASE("load_speed: rows out of time order are sorted in (#96)") {
    test::TempDir dir;
    {
        store::CsvWriter w{dir.path / store::kSpeedFile, store::speed_schema()};
        for (const int i : {1, 5, 2, 3}) {  // 5 h: ahead, inside the period
            core::Measurement<core::SpeedResult> m;
            m.time = at(std::chrono::hours{i});
            m.context.link_type = core::LinkType::wired;
            m.result.status = core::Status::ok;
            w.write_row(store::speed_row(m));
        }
    }
    const auto d = report::load_speed(dir.path, {at(), at(24h)});
    REQUIRE(d.rows.size() == 4);
    CHECK(d.rows[0].time == at(1h));
    CHECK(d.rows[1].time == at(2h));
    CHECK(d.rows[2].time == at(3h));
    CHECK(d.rows[3].time == at(5h));
    CHECK(d.out_of_order == 1);
}

TEST_CASE("load_speed stops a day of rows past the period, not at the first (#96)") {
    const auto rows_for = [](std::initializer_list<int> hours) {
        test::TempDir dir;
        {
            store::CsvWriter w{dir.path / store::kSpeedFile, store::speed_schema()};
            for (const int i : hours) {
                core::Measurement<core::SpeedResult> m;
                m.time = at(std::chrono::hours{i});
                m.context.link_type = core::LinkType::wired;
                m.result.status = core::Status::ok;
                w.write_row(store::speed_row(m));
            }
        }
        return report::load_speed(dir.path, {at(), at(24h)});
    };
    CHECK(rows_for({1, 30, 40, 2}).rows.size() == 2);  // 10 h past the end, then back
    CHECK(rows_for({1, 30, 55, 2}).rows.size() == 1);  // over a day past it: stop
    CHECK(rows_for({1, 30, 2, 50, 3}).rows.size() == 3);  // back inside: counting starts again
}
