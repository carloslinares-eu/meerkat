/**
 * @file test_advice.cpp
 * @brief Tests for the campaign advice (`meerkat advise-bbm`, task 3.5), on
 *        invented data with the made-up tariff 250/200/150 down, 50/40/30 up.
 */
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "app/advise.hpp"
#include "app/report.hpp"
#include "report/advice.hpp"
#include "store/csv.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace std::chrono_literals;
using std::chrono::days;
using std::chrono::sys_days;

namespace {

/// @return Day @p d of March 2026 (the 2nd is a Monday).
sys_days mar(unsigned d) { return sys_days{std::chrono::year{2026} / std::chrono::month{3} / std::chrono::day{d}}; }

/// The made-up contract.
core::Contract contract() {
    core::Contract c;
    c.download = core::ContractSpeeds{250, 200, 150};
    c.upload = core::ContractSpeeds{50, 40, 30};
    return c;
}

/// One row.
report::SpeedRow row(core::Timestamp t, double down, double up, core::LinkType link = core::LinkType::wired,
                     core::Status status = core::Status::ok) {
    report::SpeedRow r;
    r.time = t;
    r.status = status;
    r.link_type = link;
    r.download_mbps = down;
    r.upload_mbps = up;
    return r;
}

/**
 * @brief Four weeks from Mon 2026-03-02, a test an hour (UTC): below the
 *        minimum at 19:00, below the normal speed at 20:00 and 23:00 (upload
 *        only at 23:00), the download below normal on Tuesdays, Thursdays
 *        and Saturdays; plus Wi-Fi and failed tests that must not count.
 */
report::SpeedData four_weeks() {
    report::SpeedData d;
    d.file_exists = true;
    for (auto t = core::Timestamp{mar(2)}; t < core::Timestamp{mar(30)}; t += 1h) {
        const auto day = std::chrono::floor<days>(t);
        const auto hour = (t - day) / 1h;
        const auto wd = std::chrono::weekday{day}.c_encoding();
        double down = 240;
        double up = 48;
        if (wd == 2 || wd == 4 || wd == 6) {
            down = 190;
        }
        if (hour == 19) {
            down = 120;
        } else if (hour == 20) {
            down = 180;
        } else if (hour == 23) {
            up = 35;
        }
        d.rows.push_back(row(t, down, up));
        if (hour == 3) {
            d.rows.push_back(row(t + 10min, 10, 1, core::LinkType::wifi));  // never counted
            d.rows.push_back(row(t + 20min, 10, 1, core::LinkType::wired, core::Status::error));
        }
    }
    return d;
}

/// The period of four_weeks().
report::Period four_week_period() { return {core::Timestamp{mar(2)}, core::Timestamp{mar(30)}}; }

}  // namespace

TEST_CASE("advise: the worst hours, far enough apart, on the worst weekdays") {
    const auto a = report::advise(four_weeks(), contract(), report::utc_local(), four_week_period(), mar(30));
    CHECK(a.download);
    CHECK(a.upload);
    CHECK(a.tests == 28 * 24);
    CHECK(a.days == 28);
    CHECK(a.hours[19].tests == 28);
    CHECK(a.hours[19].below_minimum == 28);
    CHECK(a.hours[19].score() == doctest::Approx(2.0));
    CHECK(a.hours[23].below_normal == 28);  // the upload
    CHECK(a.hours[23].below_minimum == 0);
    CHECK(a.hours[3].tests == 28);           // no Wi-Fi, no failed tests
    CHECK(a.weekdays[2].days == 4);
    CHECK(a.weekdays_ranked);
    // 19 and 20 are the two worst, but only 4 h apart or more counts.
    REQUIRE(a.blocks.has_value());
    CHECK(a.blocks->first == 19);
    CHECK(a.blocks->second == 23);
    // From Mon 30 March: Tue, Thu, Sat.
    CHECK(a.plan[0] == mar(31));
    CHECK(a.plan[1] == mar(31) + days{2});
    CHECK(a.plan[2] == mar(31) + days{4});
}

TEST_CASE("advise: only one direction with contract figures") {
    auto c = contract();
    c.download.reset();
    const auto a = report::advise(four_weeks(), c, report::utc_local(), four_week_period(), mar(30));
    CHECK_FALSE(a.download);
    CHECK(a.upload);
    CHECK(a.hours[19].below_normal == 0);
    REQUIRE(a.blocks.has_value());
    CHECK(a.blocks->second == 23);
}

TEST_CASE("advise: too little data, and no contract") {
    report::SpeedData d;
    for (int i = 0; i < 5; ++i) {
        d.rows.push_back(row(core::Timestamp{mar(2)} + 19h + std::chrono::minutes{i * 5}, 120, 48));
    }
    const report::Period p{core::Timestamp{mar(2)}, core::Timestamp{mar(3)}};
    const auto a = report::advise(d, contract(), report::utc_local(), p, mar(4));
    CHECK(a.tests == 5);
    CHECK_FALSE(a.hours[19].ranked());
    CHECK_FALSE(a.blocks.has_value());
    CHECK_FALSE(a.weekdays_ranked);
    CHECK(a.plan[0] == mar(4));  // the earliest days
    CHECK(a.plan[1] == mar(6));
    CHECK(a.plan[2] == mar(8));

    const auto none = report::advise(four_weeks(), {}, report::utc_local(), four_week_period(), mar(30));
    CHECK_FALSE(none.download);
    CHECK_FALSE(none.upload);
    CHECK(none.tests == 0);
}

TEST_CASE("advise: rows outside the period don't count") {
    const report::Period one_week{core::Timestamp{mar(2)}, core::Timestamp{mar(9)}};
    const auto a = report::advise(four_weeks(), contract(), report::utc_local(), one_week, mar(30));
    CHECK(a.tests == 7 * 24);
    CHECK_FALSE(a.weekdays_ranked);  // each weekday once
}

TEST_CASE("advice_text") {
    const auto a = report::advise(four_weeks(), contract(), report::utc_local(), four_week_period(), mar(30));
    const auto text = app::advice_text(a, "UTC");
    CHECK(text.find("Based on 672 cable tests on 28 days (2026-03-02 to 2026-03-29, hours in UTC)") !=
          std::string::npos);
    CHECK(text.find("  19:00–20:00  28 tests    below normal 100.0 %  below minimum 100.0 %") != std::string::npos);
    CHECK(text.find("Weekdays, worst first: Tue") != std::string::npos);
    CHECK(text.find("  Day 1  Tue 2026-03-31\n  Day 2  Thu 2026-04-02\n  Day 3  Sat 2026-04-04\n") !=
          std::string::npos);
    CHECK(text.find("Each day: 5 measurements from 19:00, 5 more from 23:00.") != std::string::npos);
    CHECK(text.find("at least 3 hours between the\n    5th and the 6th") != std::string::npos);
    CHECK(text.find("shows no bad time") == std::string::npos);
    CHECK(text.find("Keep this plan to yourself") != std::string::npos);
    CHECK(report::worst_hours(a, 2) == std::vector<int>{19, 20});
}

TEST_CASE("advise: a clean line, and weekdays with too few tests (#93)") {
    report::SpeedData d;
    // Two weeks, one test a day at 12:00 and many at 19:00, never below.
    for (unsigned day = 2; day < 16; ++day) {
        d.rows.push_back(row(core::Timestamp{mar(day)} + 12h, 240, 48));
        for (int i = 0; i < 6; ++i) {
            d.rows.push_back(row(core::Timestamp{mar(day)} + 19h + std::chrono::minutes{i * 5}, 240, 48));
        }
        d.rows.push_back(row(core::Timestamp{mar(day)} + 23h, 240, 48));
    }
    const report::Period p{core::Timestamp{mar(2)}, core::Timestamp{mar(16)}};
    const auto a = report::advise(d, contract(), report::utc_local(), p, mar(16));
    CHECK(a.weekdays[1].days == 2);
    CHECK(a.weekdays[1].tests == 16);
    CHECK(a.weekdays_ranked);
    CHECK_FALSE(a.any_below());
    CHECK(app::advice_text(a, "UTC").find("these hours are as good as any") != std::string::npos);

    report::SpeedData few;
    for (unsigned day = 2; day < 16; ++day) {
        few.rows.push_back(row(core::Timestamp{mar(day)} + 12h, 120, 48));  // one test a day
    }
    const auto b = report::advise(few, contract(), report::utc_local(), p, mar(16));
    CHECK(b.weekdays[1].days == 2);
    CHECK_FALSE(b.weekdays_ranked);  // 2 tests per weekday aren't enough
    CHECK(b.plan[0] == mar(16));
}

TEST_CASE("meerkat advise-bbm and the report section") {
    test::TempDir dir;
    {
        store::CsvWriter w{dir.path / store::kSpeedFile, store::speed_schema()};
        for (const auto& r : four_weeks().rows) {
            core::Measurement<core::SpeedResult> m;
            m.time = r.time;
            m.context.link_type = r.link_type;
            m.context.interface = "eth0";
            m.result.status = r.status;
            m.result.download_mbps = r.download_mbps;
            m.result.upload_mbps = r.upload_mbps;
            w.write_row(store::speed_row(m));
        }
    }
    core::Config c;
    c.data_dir = dir.path;
    c.contract = contract();
    app::ReportEnv env;
    env.zone = app::utc_zone();
    env.now = [] { return core::Timestamp{mar(29)} + 12h; };
    env.version = "0.0.0-test";

    std::ostringstream out;
    std::ostringstream err;
    CHECK(app::advise_command(c, {}, env, out, err) == 0);  // the last 28 days, from tomorrow
    CHECK(out.str().find("(2026-03-02 to 2026-03-29") != std::string::npos);
    CHECK(out.str().find("Day 1  Tue 2026-03-31") != std::string::npos);
    CHECK(err.str().empty());

    std::ostringstream out2;
    CHECK(app::advise_command(c, {"", "", "2026-04-01"}, env, out2, err) == 0);
    CHECK(out2.str().find("Day 1  Thu 2026-04-02") != std::string::npos);

    std::ostringstream err3;
    CHECK(app::advise_command(c, {"", "", "1.4.2026"}, env, out, err3) == 2);
    CHECK(err3.str().find("--start must be a date") != std::string::npos);
    std::ostringstream err5;
    CHECK(app::advise_command(c, {"", "", "2026-03-29"}, env, out, err5) == 2);  // today: too early
    CHECK(err5.str().find("must be tomorrow (2026-03-30) or later") != std::string::npos);

    auto no_contract = c;
    no_contract.contract = {};
    std::ostringstream err4;
    CHECK(app::advise_command(no_contract, {}, env, out, err4) == 2);
    CHECK(err4.str().find("needs the contract figures") != std::string::npos);

    // The report has the same advice only when asked (#92): it may go to
    // the provider, as the export's report does.
    const auto plain = app::build_report(c, {mar(2), mar(29)}, env).html;
    CHECK(plain.find("When to run the official measurement") == std::string::npos);
    const auto page = app::build_report(c, {mar(1), mar(29)}, env, true).html;
    CHECK(page.find("<h2>When to run the official measurement</h2>") != std::string::npos);
    CHECK(page.find("leave it out of a copy for the provider") != std::string::npos);
    CHECK(page.find("From the last four weeks of the period (2026-03-02 to 2026-03-29, 672 cable tests; "
                    "a test counts if its download or upload was") != std::string::npos);
    CHECK(page.find("5 measurements from 19:00 and 5 more from 23:00 on each of Tue 2026-03-31") !=
          std::string::npos);
    // The four weeks end at the report's last day, not today (#98).
    CHECK(page.find("<code>meerkat advise-bbm --to 2026-03-29</code> gives the same advice") != std::string::npos);
    auto up_only = c;
    up_only.contract.download.reset();
    CHECK(app::build_report(up_only, {mar(2), mar(29)}, env, true).html.find("a test counts if its upload was") !=
          std::string::npos);
    const auto bare = app::build_report(no_contract, {mar(2), mar(29)}, env, true).html;
    CHECK(bare.find("When to run the official measurement") == std::string::npos);
}
