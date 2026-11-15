/**
 * @file test_svg.cpp
 * @brief Tests for the report's SVG charts (task 3.2): golden files in
 *        tests/fixtures/svg/, with invented data and the made-up tariff
 *        250/200/150 down.
 *
 * Run with `MEERKAT_UPDATE_GOLDEN=1` to write the golden files anew after a
 * deliberate change, then look at them in a browser (in a page with
 * report::chart_style(), light and dark) before committing.
 */
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "core/outage.hpp"
#include "report/svg.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace std::chrono_literals;

namespace {

/// 2026-03-09T23:00:00Z (local midnight in UTC+1) plus @p offset.
core::Timestamp at(std::chrono::seconds offset = {}) {
    return core::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 9}} + 23h + offset;
}

/// A fixed UTC+1 zone (German winter time, no DST switch in March 9–16).
report::LocalOf utc_plus_one() {
    return [](core::Timestamp t) {
        const auto wall = t + 1h;
        const auto day = std::chrono::floor<std::chrono::days>(wall);
        return report::LocalTime{day, static_cast<int>(std::chrono::floor<std::chrono::hours>(wall - day).count()),
                                 1h};
    };
}

/// Compares @p svg with the golden file, or writes it under MEERKAT_UPDATE_GOLDEN.
void golden(const std::string& name, const std::string& svg) {
    const auto path = test::fixture_path("svg/" + name);
    if (std::getenv("MEERKAT_UPDATE_GOLDEN") != nullptr) {
        std::ofstream{path, std::ios::binary} << svg;
        MESSAGE("wrote " << path.string());
        return;
    }
    CHECK(svg == test::fixture("svg/" + name));
}

/// Tariff lines of the made-up contract, download.
std::vector<report::ReferenceLine> download_lines() {
    return {{"maximum 250", 250}, {"90 % of max. 225", 225}, {"normal 200", 200}, {"minimum 150", 150}};
}

/// Three days of invented downloads every 20 min, with an evening dip,
/// a gap of two hours and one lone test.
report::TimeSeries invented_downloads() {
    report::TimeSeries s{"Download (cable)", {}};
    for (auto t = at(); t < at(72h); t += 20min) {
        const auto since = t - at();
        if (since >= 30h && since < 32h) {
            continue;  // meerkat was off
        }
        if (since > 32h && since < 34h && since != 33h) {
            continue;  // one lone test at 33 h
        }
        const double hour = std::fmod(static_cast<double>(since.count()) / 3600.0, 24.0);
        const double dip = hour >= 18 && hour < 23 ? 80 : 0;
        const double wobble = 12 * std::sin(static_cast<double>(since.count()) / 4000.0);
        s.points.push_back({t, 238 - dip + wobble});
    }
    return s;
}

}  // namespace

TEST_CASE("escape_xml escapes the five XML characters") {
    CHECK(report::escape_xml(R"(a<b>&"c"'d')") == "a&lt;b&gt;&amp;&quot;c&quot;&apos;d&apos;");
    CHECK(report::escape_xml("Mbit/s") == "Mbit/s");
}

TEST_CASE("svg_number: one decimal at most, no -0") {
    CHECK(report::svg_number(12) == "12");
    CHECK(report::svg_number(12.25) == "12.2");  // printf rounding, deterministic
    CHECK(report::svg_number(12.26) == "12.3");
    CHECK(report::svg_number(-0.01) == "0");
    CHECK(report::svg_number(-3.2) == "-3.2");
    CHECK(report::svg_number(std::nan("")) == "0");
}

TEST_CASE("time chart: download over three days with the contract lines") {
    report::TimeChart chart;
    chart.title = "Download over time";
    chart.description = "Cable tests against the contract figures.";
    chart.unit = "Mbit/s";
    chart.period = {at(), at(72h)};
    chart.series = {invented_downloads()};
    chart.references = download_lines();
    chart.max_gap = 45min;  // tests every 20 min
    const auto svg = report::time_chart(chart, utc_plus_one());
    golden("time_download.svg", svg);
    CHECK(svg.find("mk-legend") == std::string::npos);  // one series: no legend
    CHECK(svg.find("<circle") != std::string::npos);    // the lone test
    CHECK(svg.find("Tue 10 Mar") != std::string::npos); // local day ticks
}

TEST_CASE("time chart: two series get a legend, a fourth is left out") {
    report::TimeChart chart;
    chart.title = "Download, cable and Wi-Fi";
    chart.unit = "Mbit/s";
    chart.period = {at(), at(24h)};
    report::TimeSeries wifi{"Download (Wi-Fi)", {}};
    for (auto t = at(1h); t < at(24h); t += 1h) {
        wifi.points.push_back({t, 90 + static_cast<double>((t - at()).count() % 7200) / 360});
    }
    auto cable = invented_downloads();
    std::erase_if(cable.points, [](const report::TimePoint& p) { return p.time >= at(24h); });
    chart.series = {cable, wifi};
    chart.references = {{"minimum 150", 150}};
    chart.max_gap = 90min;  // the Wi-Fi tests are an hour apart
    const auto svg = report::time_chart(chart, utc_plus_one());
    golden("time_two_series.svg", svg);
    CHECK(svg.find("mk-legend") != std::string::npos);

    chart.series = {cable, wifi, wifi, wifi};
    const auto four = report::time_chart(chart, utc_plus_one());
    CHECK(four.find("mk-s3") != std::string::npos);
    CHECK(four.find("mk-s4") == std::string::npos);
}

TEST_CASE("time chart: an empty period says so") {
    report::TimeChart chart;
    chart.title = "Upload over time";
    chart.unit = "Mbit/s";
    chart.period = {at(), at(48h)};
    chart.series = {{"Upload (cable)", {{at(-1h), 40}, {at(49h), 41}}}};  // both outside
    chart.references = {{"minimum 30", 30}};
    const auto svg = report::time_chart(chart, utc_plus_one());
    golden("time_empty.svg", svg);
    CHECK(svg.find("No data in this period") != std::string::npos);
    CHECK(svg.find("<path") == std::string::npos);
}

TEST_CASE("hour chart: median per local hour, tooltips per hour") {
    report::HourChart chart;
    chart.title = "Median download by hour of day";
    chart.unit = "Mbit/s";
    for (int h = 0; h < 24; ++h) {
        if (h == 4) {
            continue;  // no data at 04:00
        }
        const double dip = h >= 18 && h < 23 ? 70 : 0;
        chart.bars[static_cast<std::size_t>(h)] = {236 - dip - h % 3, std::to_string(9 + h % 4) + " tests"};
    }
    chart.references = {{"normal 200", 200}, {"minimum 150", 150}};
    const auto svg = report::hour_chart(chart);
    golden("hours_download.svg", svg);
    CHECK(svg.find("04:00–05:00: no data") != std::string::npos);
    CHECK(svg.find("20:00–21:00: 164 Mbit/s, 9 tests") != std::string::npos);
}

TEST_CASE("outage chart: closed, open, cut and short outages") {
    report::OutageChart chart;
    chart.title = "Outages";
    chart.period = {at(), at(72h)};

    core::Outage before;  // started before the period: cut at its start
    before.start = at(-2h);
    before.end = at(1h);
    before.scope = core::OutageScope::local;
    before.reason = "gateway <192.0.2.1> didn't answer";

    core::Outage line;
    line.start = at(20h + 5min);
    line.end = at(21h + 10min);
    line.scope = core::OutageScope::internet;
    line.reason = "no answer from 203.0.113.53";

    core::Outage brief;  // narrower than a pixel: drawn 2 px wide
    brief.start = at(40h);
    brief.end = at(40h + 90s);
    brief.scope = core::OutageScope::internet;

    core::Outage gap;
    gap.start = at(50h);
    gap.end = at(50h + 20min);
    gap.end_reason = core::OutageEnd::gap;
    gap.unconfirmed_end = true;
    gap.scope = core::OutageScope::unknown;

    core::Outage open;  // still going at the end
    open.start = at(70h);
    open.scope = core::OutageScope::internet;

    chart.outages = {before, line, brief, gap, open};
    const auto svg = report::outage_chart(chart, utc_plus_one());
    golden("outages.svg", svg);
    CHECK(svg.find("Outage Tue 10 Mar 20:05 – Tue 10 Mar 21:10 (1 h 05 min)") != std::string::npos);
    CHECK(svg.find("gateway &lt;192.0.2.1&gt;") != std::string::npos);
    CHECK(svg.find("(90 s)") == std::string::npos);
    CHECK(svg.find("(1 min)") != std::string::npos);
    CHECK(svg.find("end: gap (unconfirmed)") != std::string::npos);
    CHECK(svg.find("still going at the end of the period") != std::string::npos);
}

TEST_CASE("outage chart: none in the period") {
    report::OutageChart chart;
    chart.title = "Outages";
    chart.period = {at(), at(24h)};
    core::Outage later;
    later.start = at(30h);
    later.end = at(31h);
    chart.outages = {later};
    const auto svg = report::outage_chart(chart, utc_plus_one());
    golden("outages_none.svg", svg);
    CHECK(svg.find("No outages in this period") != std::string::npos);
}

TEST_CASE("day ticks: a period starting at local midnight gets a tick at its start") {
    report::TimeChart chart;
    chart.title = "t";
    chart.period = {at(), at(24h)};
    chart.series = {{"s", {{at(1h), 1}}}};
    const auto from_midnight = report::time_chart(chart, utc_plus_one());
    CHECK(from_midnight.find("Tue 10 Mar") != std::string::npos);
    chart.period = {at(2h), at(30h)};
    const auto from_two = report::time_chart(chart, utc_plus_one());
    CHECK(from_two.find("Tue 10 Mar") == std::string::npos);  // no tick before the next day
    CHECK(from_two.find("Wed 11 Mar") != std::string::npos);
}

TEST_CASE("outage chart: an outage ending at the start of the period isn't in it (#80)") {
    report::OutageChart chart;
    chart.title = "Outages";
    chart.period = {at(), at(24h)};
    core::Outage before;
    before.start = at(-1h);
    before.end = at();
    chart.outages = {before};
    const auto svg = report::outage_chart(chart, utc_plus_one());
    CHECK(svg.find("mk-outage") == std::string::npos);
    CHECK(svg.find("No outages in this period") != std::string::npos);
}

TEST_CASE("outage chart: an unconfirmed recovery says so in the end clause (#80)") {
    report::OutageChart chart;
    chart.title = "Outages";
    chart.period = {at(), at(24h)};
    core::Outage o;
    o.start = at(1h);
    o.end = at(2h);
    o.unconfirmed_end = true;
    o.scope = core::OutageScope::internet;
    chart.outages = {o};
    const auto svg = report::outage_chart(chart, utc_plus_one());
    CHECK(svg.find("line (beyond the router), end: recovered (unconfirmed)") != std::string::npos);
}

TEST_CASE("outage chart: tool text in tooltips is made visible (#81)") {
    report::OutageChart chart;
    chart.title = "Outages";
    chart.period = {at(), at(24h)};
    core::Outage o;
    o.start = at(1h);
    o.end = at(2h);
    o.reason = "bad\x1b[2Jtext\xff\xe2\x80\xae";
    chart.outages = {o};
    const auto svg = report::outage_chart(chart, utc_plus_one());
    CHECK(svg.find("bad\\x1b[2Jtext\\xff\\xe2\\x80\\xae") != std::string::npos);
    CHECK(svg.find('\x1b') == std::string::npos);
    CHECK(report::escape_xml(std::string{"a\x01" "b\tc\x7f" "d"}) == "ab\tcd");
}

TEST_CASE("hour chart: a 0 draws a stub, no data draws nothing (#80)") {
    report::HourChart chart;
    chart.title = "t";
    chart.bars[3] = {0.0, {}};
    const auto svg = report::hour_chart(chart);
    CHECK(svg.find("mk-bar") != std::string::npos);
    std::size_t bars = 0;
    for (auto pos = svg.find("mk-bar"); pos != std::string::npos; pos = svg.find("mk-bar", pos + 1)) {
        ++bars;
    }
    CHECK(bars == 1);
}

TEST_CASE("reference labels stay inside the chart (#80)") {
    report::HourChart chart;
    chart.title = "t";
    chart.bars[0] = {100.0, {}};
    chart.references = {{"a", 3}, {"b", 2}, {"c", 1}, {"d", 0.5}};
    const auto svg = report::hour_chart(chart);  // plot bottom at 196
    for (const char* label : {">a<", ">b<", ">c<", ">d<"}) {
        const auto end = svg.find(label);
        REQUIRE(end != std::string::npos);
        const auto y = svg.rfind("y=\"", end);
        CHECK(std::stod(svg.substr(y + 3)) <= 200.0);
    }
}

TEST_CASE("small scales get one-decimal ticks (#80)") {
    report::HourChart chart;
    chart.title = "t";
    chart.bars[0] = {0.2, {}};
    auto svg = report::hour_chart(chart);
    CHECK(svg.find(">0.1</text>") != std::string::npos);
    CHECK(svg.find(">0.2</text>") != std::string::npos);
    CHECK(svg.find(">0.05") == std::string::npos);
    chart.bars[0] = {1.0, {}};
    svg = report::hour_chart(chart);
    CHECK(svg.find(">0.5</text>") != std::string::npos);
    CHECK(svg.find(">0.2</text>") == std::string::npos);  // no 0.25 steps
}

TEST_CASE("day ticks at local midnight in a half-hour zone (#80)") {
    const report::LocalOf plus_half = [](core::Timestamp t) {
        const auto wall = t + 5h + 30min;
        const auto day = std::chrono::floor<std::chrono::days>(wall);
        return report::LocalTime{day, static_cast<int>(std::chrono::floor<std::chrono::hours>(wall - day).count()),
                                 5h + 30min};
    };
    const auto midnight = core::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 10}} - 5h - 30min;
    report::OutageChart chart;
    chart.title = "t";
    chart.period = {midnight - 12h, midnight + 12h};  // the tick sits in the middle
    const auto svg = report::outage_chart(chart, plus_half);
    CHECK(svg.find("<line class=\"mk-grid\" x1=\"326\"") != std::string::npos);  // 44 + 564 / 2
    CHECK(svg.find("Tue 10 Mar") != std::string::npos);
}
