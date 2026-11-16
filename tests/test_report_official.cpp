/**
 * @file test_report_official.cpp
 * @brief Tests for the official measurements in the report (task 3.7), with
 *        the made-up tariff 250/200/150 down, 50/40/30 up.
 */
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "report/html.hpp"
#include "report/official.hpp"
#include "report/svg.hpp"
#include "store/csv.hpp"
#include "store/manifest.hpp"
#include "store/official.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace std::chrono_literals;
using report::OfficialRow;

namespace {

/// 2026-03-10 (local = UTC here) plus @p offset.
core::Timestamp at(std::chrono::seconds offset = {}) {
    return core::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 10}} + offset;
}

/// The day 2026-03-(10 + @p n).
std::chrono::sys_days day(int n) {
    return std::chrono::sys_days{std::chrono::year{2026} / 3 / 10} + std::chrono::days{n};
}

/// One official measurement with the made-up contract entered in the app.
OfficialRow official(core::Timestamp t, double down, double up) {
    OfficialRow r;
    r.m.time = t;
    r.m.download_mbps = down;
    r.m.upload_mbps = up;
    r.m.contract_down_max = 250;
    r.m.contract_down_normal = 200;
    r.m.contract_down_min = 150;
    r.m.contract_up_max = 50;
    r.m.contract_up_normal = 40;
    r.m.contract_up_min = 30;
    r.m.connection = "LAN";
    r.m.on_power = true;
    r.m.nic_sufficient = true;
    r.m.data_rate_limited = false;
    r.m.test_id = std::to_string(t.time_since_epoch().count());
    r.file = "official/export.csv";
    return r;
}

/// Ten measurements on day @p n (09:00 to 18:00), @p down for all but the
/// @p slow last ones, which get @p slow_down.
void campaign_day(report::OfficialData& d, int n, double down, int slow = 0, double slow_down = 0) {
    for (int i = 0; i < 10; ++i) {
        const auto t = core::Timestamp{day(n)} + std::chrono::hours{9 + i};
        d.rows.push_back(official(t, i >= 10 - slow ? slow_down : down, 45));
    }
}

/// A meerkat cable test.
report::SpeedRow cable_test(core::Timestamp t, double down, double up) {
    report::SpeedRow r;
    r.time = t;
    r.status = core::Status::ok;
    r.link_type = core::LinkType::wired;
    r.download_mbps = down;
    r.upload_mbps = up;
    return r;
}

const report::Period kTwoWeeks{at(), at(14 * 24h)};

}  // namespace

TEST_CASE("analyse_official: a campaign within the contract") {
    report::OfficialData d;
    campaign_day(d, 0, 230);
    campaign_day(d, 2, 230);
    campaign_day(d, 4, 230);
    const auto a = report::analyse_official(d, {}, report::utc_local(), kTwoWeeks);
    REQUIRE(a.campaigns.size() == 1);
    const auto& c = a.campaigns[0];
    CHECK(c.days == std::array{day(0), day(2), day(4)});
    CHECK(c.measurements == 30);
    CHECK(c.problems.empty());
    CHECK_FALSE(c.download.verdict.deviation());
    CHECK_FALSE(c.upload.verdict.deviation());
    CHECK(c.download.verdict.normal_share == 1.0);
    CHECK(a.open_days.empty());
    CHECK(a.short_days.empty());
    CHECK(a.app_contract.download == core::ContractSpeeds{250, 200, 150});
}

TEST_CASE("analyse_official: the three rules as written") {
    report::OfficialData d;
    SUBCASE("rule 3: one measurement below the minimum on each of two days is enough") {
        campaign_day(d, 0, 230, 1, 149);
        campaign_day(d, 2, 230, 1, 149);
        campaign_day(d, 4, 230);
        const auto c = report::analyse_official(d, {}, report::utc_local(), kTwoWeeks).campaigns.at(0);
        CHECK(c.download.verdict.rule_minimum);
        CHECK(c.download.days_below_minimum == 2);
        CHECK_FALSE(c.download.verdict.rule_normal);  // 28 of 30 = 93 % reached it
        CHECK_FALSE(c.download.verdict.rule_max90);
    }
    SUBCASE("rule 3: a dip on one day only is not") {
        campaign_day(d, 0, 230, 1, 149);
        campaign_day(d, 2, 230);
        campaign_day(d, 4, 230);
        CHECK_FALSE(report::analyse_official(d, {}, report::utc_local(), kTwoWeeks).campaigns.at(0).download.verdict.deviation());
    }
    SUBCASE("rule 2: the normal speed missed in more than 10 %") {
        campaign_day(d, 0, 230, 2, 190);
        campaign_day(d, 2, 230, 2, 190);
        campaign_day(d, 4, 230);
        const auto c = report::analyse_official(d, {}, report::utc_local(), kTwoWeeks).campaigns.at(0);
        CHECK(c.download.verdict.rule_normal);
        CHECK(c.download.verdict.normal_share == doctest::Approx(26.0 / 30));
    }
    SUBCASE("rule 1: 90 % of the maximum never reached on two days") {
        campaign_day(d, 0, 210);
        campaign_day(d, 2, 210);
        campaign_day(d, 4, 230);
        const auto c = report::analyse_official(d, {}, report::utc_local(), kTwoWeeks).campaigns.at(0);
        CHECK(c.download.verdict.rule_max90);
        CHECK(c.download.days_missing_max90 == 2);
        CHECK_FALSE(c.upload.verdict.deviation());
    }
}

TEST_CASE("analyse_official: campaigns three days at a time, and what isn't one") {
    report::OfficialData d;
    campaign_day(d, 0, 230);
    campaign_day(d, 1, 230);  // no day off before it
    campaign_day(d, 3, 230);
    campaign_day(d, 6, 230);  // a second campaign, still running
    d.rows.push_back(official(core::Timestamp{day(8)} + 10h, 230, 45));  // one measurement: not a day
    d.rows.push_back(official(at(-24h), 230, 45));                        // before the period
    auto bad = official(core::Timestamp{day(3)} + 20h, 230, 45);
    bad.m.connection = "WLAN";
    bad.m.on_power = false;
    bad.m.contract_up_normal = 41;
    d.rows.push_back(bad);
    std::stable_sort(d.rows.begin(), d.rows.end(),
                     [](const OfficialRow& a, const OfficialRow& b) { return a.m.time < b.m.time; });

    const auto a = report::analyse_official(d, {}, report::utc_local(), kTwoWeeks);
    CHECK(a.outside == 1);
    CHECK(a.rows.size() == 42);
    REQUIRE(a.campaigns.size() == 1);
    const auto& p = a.campaigns[0].problems;
    REQUIRE(p.size() == 4);
    CHECK(p[0] == "2026-03-10 and 2026-03-11 have no day without measurement between them");
    CHECK(p[1] == "1 measurement not over a cable (the app saw another connection)");
    CHECK(p[2] == "1 measurement with the computer on battery");
    CHECK(p[3].starts_with("the contract figures entered in the app change"));
    CHECK(a.campaigns[0].upload.contract == core::ContractSpeeds{50, 41, 30});  // the last ones entered
    CHECK(a.open_days == std::vector{day(6)});
    CHECK(a.short_days == std::vector{day(8)});
}

TEST_CASE("analyse_official: each result next to meerkat's tests within 30 minutes") {
    report::OfficialData d;
    d.rows.push_back(official(at(12h), 180, 45));
    d.rows.push_back(official(at(20h), 100, 45));
    d.rows.push_back(official(at(30h), 100, 45));
    report::SpeedData s;
    s.rows = {cable_test(at(11h + 20min), 999, 1),  // outside the 30 minutes
              cable_test(at(11h + 30min), 200, 40), cable_test(at(12h + 30min), 220, 50),
              cable_test(at(20h + 10min), 200, 46)};
    auto wifi = cable_test(at(12h + 5min), 10, 1);
    wifi.link_type = core::LinkType::wifi;
    s.rows.insert(s.rows.begin() + 3, wifi);
    const auto a = report::analyse_official(d, s, report::utc_local(), kTwoWeeks);
    REQUIRE(a.comparisons.size() == 3);
    const auto& c = a.comparisons[0];
    CHECK(c.tests == 2);
    CHECK(*c.meerkat_down == 210);
    CHECK(*c.meerkat_up == 45);
    CHECK(*c.difference_down() == doctest::Approx(-30.0 / 210));
    CHECK(*c.difference_up() == 0);
    CHECK(*c.agrees());                             // −14 %: within 15 %
    CHECK_FALSE(*a.comparisons[1].agrees());        // −50 %
    CHECK_FALSE(a.comparisons[2].agrees());         // no test around it
    CHECK(a.comparisons[2].tests == 0);
}

TEST_CASE("load_official reads back what import wrote") {
    test::TempDir dir;
    CHECK_FALSE(report::load_official(dir.path).file_exists);
    const auto plus_one = [](int y, int mo, int dd, int hh, int mi, int ss) -> std::optional<core::Timestamp> {
        const std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(mo)},
                                              std::chrono::day{static_cast<unsigned>(dd)}};
        return core::Timestamp{std::chrono::sys_days{ymd} + std::chrono::hours{hh - 1} + std::chrono::minutes{mi} +
                               std::chrono::seconds{ss}};
    };
    store::import_official(dir.path, test::fixture_path("bbm/three.csv"), plus_one);
    const auto d = report::load_official(dir.path);
    CHECK(d.file_exists);
    REQUIRE(d.rows.size() == 3);
    CHECK(core::to_iso8601(d.rows[0].m.time) == "2026-03-10T18:25:12Z");
    CHECK(*d.rows[0].m.number == 1);
    CHECK(*d.rows[0].m.download_mbps == doctest::Approx(187.42));
    CHECK(*d.rows[0].m.contract_down_max == 250);
    CHECK(d.rows[2].m.connection == "WLAN");
    CHECK(d.rows[2].m.on_power == false);
    CHECK(d.rows[0].file == "official/three.csv");
}

TEST_CASE("the report: the official finding first, meerkat's as support") {
    report::OfficialData d;
    campaign_day(d, 0, 230, 1, 149);
    campaign_day(d, 2, 230, 1, 149);
    campaign_day(d, 4, 230);
    report::SpeedData speed;
    speed.file_exists = true;
    for (auto t = at(); t < at(6 * 24h); t += 1h) {
        speed.rows.push_back(cable_test(t, 230, 45));
    }
    report::ReportMeta meta;
    meta.period = kTwoWeeks;
    meta.first_day = day(0);
    meta.last_day = day(13);
    meta.zone = "UTC";
    meta.contract.download = core::ContractSpeeds{250, 200, 150};
    meta.contract.upload = core::ContractSpeeds{50, 45, 30};  // not what the app has
    meta.official = report::analyse_official(d, speed, report::utc_local(), meta.period);
    const auto analysis = report::analyse(speed, {}, meta.contract, report::utc_local(), meta.period);
    const auto page = report::render_html(meta, speed, {}, analysis, report::utc_local());

    const auto official = page.find("<h2 id=\"official\">Official measurement (Breitbandmessung)</h2>");
    const auto support = page.find("<h2>meerkat's long-term data</h2>");
    REQUIRE(official != std::string::npos);
    REQUIRE(support != std::string::npos);
    CHECK(official < support);
    CHECK(page.find("<strong>the legal finding</strong>") != std::string::npos);
    CHECK(page.find("✕ Deviation: rule 3 (minimum)") != std::string::npos);
    CHECK(page.find("✓ No deviation</p>") != std::string::npos);  // upload
    CHECK(page.find("Campaign Tue 2026-03-10, Thu 2026-03-12 and Sat 2026-03-14") != std::string::npos);
    CHECK(page.find("Upload: the contract figures entered in the app (50 / 40 / 30 Mbit/s) aren't meerkat's "
                    "(50 / 45 / 30 Mbit/s)") != std::string::npos);
    CHECK(page.find("30 of 30 comparable measurements agree.") == std::string::npos);  // two dips don't
    CHECK(page.find("28 of 30 comparable measurements agree.") != std::string::npos);
    CHECK(page.find("Diamonds: the official measurements.") != std::string::npos);
    CHECK(page.find("<title>Official (Breitbandmessung), Tue 10 Mar 09:00: 230 Mbit/s</title>") != std::string::npos);
    CHECK(page.find("<h2>Result</h2>") == std::string::npos);
}

TEST_CASE("the report without official measurements says how to import them") {
    report::SpeedData speed;
    speed.file_exists = true;
    report::ReportMeta meta;
    meta.period = kTwoWeeks;
    meta.first_day = day(0);
    meta.last_day = day(13);
    meta.zone = "UTC";
    const auto analysis = report::analyse(speed, {}, meta.contract, report::utc_local(), meta.period);
    const auto page = report::render_html(meta, speed, {}, analysis, report::utc_local());
    CHECK(page.find("id=\"official\"") == std::string::npos);
    CHECK(page.find("<code>meerkat import-bbm</code>") != std::string::npos);
    CHECK(page.find("<h2>Result</h2>") != std::string::npos);
}

TEST_CASE("time chart: a marker series keeps its slot and draws diamonds on top") {
    report::TimeChart chart;
    chart.title = "t";
    chart.unit = "Mbit/s";
    chart.period = {at(), at(24h)};
    chart.series.emplace_back("Cable", std::vector<report::TimePoint>{{at(1h), 200}, {at(1h + 10min), 210}});
    chart.series.emplace_back("Official", std::vector<report::TimePoint>{{at(2h), 150}}, true, 2);
    chart.references = {{"minimum 150", 150}};
    const auto svg = report::time_chart(chart, report::utc_local());
    const auto diamond = svg.find("class=\"mk-dot mk-s3\"");
    REQUIRE(diamond != std::string::npos);
    CHECK(svg.rfind("mk-ref-line") < diamond);  // above the contract lines
    CHECK(svg.find("class=\"mk-swatch mk-s3\"") != std::string::npos);
    CHECK(svg.find("<path class=\"mk-line mk-s1\" d=\"M") != std::string::npos);  // the cable line
}
