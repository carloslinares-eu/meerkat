/**
 * @file html.cpp
 * @brief The report page.
 */
#include "report/html.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string_view>
#include <tuple>

#include "core/status.hpp"
#include "core/text.hpp"
#include "report/format.hpp"
#include "report/svg.hpp"

namespace meerkat::report {

namespace {

using std::chrono::seconds;

/// @return @p s escaped for HTML text and attributes.
std::string h(std::string_view s) { return escape_xml(s); }

/// @return External text (tool output, names from the CSVs) made visible,
///         then escaped.
std::string ext(std::string_view s) { return escape_xml(core::text::visible(s)); }

/// @return E.g. `187.3`, or `–` if missing.
std::string mbps(std::optional<double> v) {
    if (!v || !std::isfinite(*v)) {
        return "–";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", *v);
    return buf;
}

/// @return @p v as text without a trailing `.0` (contract figures).
std::string figure(double v) { return svg_number(v); }

/// @return Local `YYYY-MM-DD HH:MM`.
std::string moment(core::Timestamp t, const LocalOf& local) {
    const auto lt = local(t);
    const auto wall = t + lt.offset;
    const auto mins = std::chrono::floor<std::chrono::minutes>(wall - std::chrono::floor<std::chrono::days>(wall)).count();
    return date(lt.day) + " " + two(mins / 60) + ":" + two(mins % 60);
}

/// @return E.g. `45 s`, `27 min`, `2 h 05 min`.
std::string duration(seconds d) {
    const auto s = d.count();
    if (s < 60) {
        return std::to_string(s) + " s";
    }
    const auto m = s / 60;
    if (m < 60) {
        return std::to_string(m) + " min";
    }
    return std::to_string(m / 60) + " h " + two(m % 60) + " min";
}

using core::text::plural;

/// @return E.g. `5 min`, `1 h`, `90 s`.
std::string interval(seconds d) {
    const auto s = d.count();
    if (s > 0 && s % 3600 == 0) {
        return std::to_string(s / 3600) + " h";
    }
    if (s > 0 && s % 60 == 0) {
        return std::to_string(s / 60) + " min";
    }
    return std::to_string(s) + " s";
}

/// @return What a scope means.
std::string scope_text(core::OutageScope s) {
    switch (s) {
    case core::OutageScope::internet: return "line (beyond the router)";
    case core::OutageScope::local:    return "local network";
    case core::OutageScope::unknown:  return "unclear";
    }
    return "unclear";
}

/// @return What a link type means.
std::string link_text(core::LinkType t) {
    switch (t) {
    case core::LinkType::wired:   return "cable (Ethernet)";
    case core::LinkType::wifi:    return "Wi-Fi";
    case core::LinkType::tunnel:  return "VPN / tunnel";
    case core::LinkType::unknown: return "unknown link";
    }
    return "unknown link";
}

/// The page's style: tokens for light and dark, print layout.
constexpr std::string_view kPageStyle = R"css(
:root{color-scheme:light;--bg:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink-2:#52514e;--muted:#898781;
--line:#e1e0d9;--warn-bg:#fdf3dc;--warn-edge:#fab219;--bad-bg:#fbe6e6;--bad-edge:#d03b3b;--good-bg:#e5f3e5;--good-edge:#0ca30c}
@media screen and (prefers-color-scheme:dark){:root:where(:not([data-theme="light"])){color-scheme:dark;--bg:#0d0d0d;
--surface:#1a1a19;--ink:#f0efec;--ink-2:#c3c2b7;--muted:#898781;--line:#2c2c2a;--warn-bg:#2e2510;--bad-bg:#2e1515;--good-bg:#132613}}
@media screen{:root[data-theme="dark"]{color-scheme:dark;--bg:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;--ink-2:#c3c2b7;
--line:#2c2c2a;--warn-bg:#2e2510;--bad-bg:#2e1515;--good-bg:#132613}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:860px;margin:0 auto;padding:24px 16px 48px}
h1{font-size:24px;margin:0 0 4px}
h2{font-size:18px;margin:32px 0 8px;padding-top:8px;border-top:1px solid var(--line)}
h3{font-size:15px;margin:20px 0 6px}
p{margin:6px 0}
.lede{color:var(--ink-2)}
.note{color:var(--ink-2);font-size:13px}
.box{border-left:4px solid var(--line);background:var(--surface);padding:8px 12px;margin:10px 0}
.box.warn{border-color:var(--warn-edge);background:var(--warn-bg)}
.box.bad{border-color:var(--bad-edge);background:var(--bad-bg)}
.box.good{border-color:var(--good-edge);background:var(--good-bg)}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
.cards .box{margin:0}
.verdict{font-weight:600}
table{border-collapse:collapse;width:100%;margin:6px 0 12px;background:var(--surface);font-size:13px}
th,td{padding:4px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{color:var(--ink-2);font-weight:600}
td.d{white-space:nowrap}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}
.hash{font-family:ui-monospace,"DejaVu Sans Mono",monospace;font-size:11px;word-break:break-all}
figure{margin:8px 0 16px;break-inside:avoid}
figcaption{color:var(--ink-2);font-size:13px;margin-bottom:4px}
.wrap{overflow-x:auto}
@page{size:A4;margin:14mm}
@media print{body{background:#fff;font-size:11pt}main{max-width:none;padding:0}
h2{break-after:avoid}table,.box{break-inside:avoid}tr{break-inside:avoid}.wrap{overflow:visible}}
)css";

/// Builds the page.
class Page {
public:
    /// @param meta, speed, outages, analysis, local As render_html().
    Page(const ReportMeta& meta, const SpeedData& speed, const OutageData& outages,
         const Analysis& analysis, const LocalOf& local)
        : meta_(meta), speed_(speed), outages_(outages), a_(analysis), local_(local) {}

    /// @return The document.
    std::string render() {
        out_ += "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                "<title>Internet speed report " + h(date(meta_.first_day)) + " to " +
                h(date(meta_.last_day)) + "</title>\n<style>" + std::string{kPageStyle} +
                chart_style() + "</style>\n</head>\n<body>\n<main>\n";
        header();
        verdicts();
        summary();
        charts();
        if (meta_.advice) {
            campaign(*meta_.advice);
        }
        days();
        outage_list();
        other_links();
        setup();
        quality();
        manifests();
        method();
        out_ += "</main>\n</body>\n</html>\n";
        return std::move(out_);
    }

private:
    /// Title, period, where it comes from.
    void header() {
        out_ += "<h1>Internet speed report</h1>\n<p class=\"lede\">" + h(weekday_date(meta_.first_day)) +
                " to " + h(weekday_date(meta_.last_day)) + " (local time, " + ext(meta_.zone) +
                "). Generated " + h(moment(meta_.generated, local_)) + " by meerkat " +
                h(meta_.version) + ".</p>\n";
        out_ += "<div class=\"box\"><p>These are meerkat's own measurements: a speed test every " +
                h(interval(meta_.speed_interval)) +
                " and connectivity probes, recorded continuously on this computer. They are "
                "long-term supporting evidence. The measurement that counts in law (§ 57 TKG) is "
                "the measurement campaign with the Bundesnetzagentur's Breitbandmessung app.</p></div>\n";
        if (!speed_.file_exists) {
            out_ += "<div class=\"box bad\"><p class=\"verdict\">⚠ No speed.csv in the data "
                    "directory: there are no speed tests to report.</p></div>\n";
        }
    }

    /// The verdict card of one direction.
    void verdict(std::string_view name, const DirectionReport& d,
                 const std::optional<core::ContractSpeeds>& c) {
        const auto deviating = d.deviating_windows();
        std::string cls = "box";
        std::string text;
        if (!c) {
            cls += " warn";
            text = "⚠ No contract figures: nothing compared";
        } else if (d.windows.empty()) {
            cls += " warn";
            text = "⚠ Too few measurement days for a three-day window";
        } else if (deviating > 0) {
            cls += " bad";
            text = "✕ Deviation in " + std::to_string(deviating) + " of " +
                   std::to_string(d.windows.size()) + " three-day windows (" +
                   std::to_string(d.separate_deviations()) + " without overlap)";
        } else {
            cls += " good";
            text = "✓ No deviation in " + std::to_string(d.windows.size()) + " three-day windows";
        }
        out_ += "<div class=\"" + cls + "\"><h3>" + h(name) + "</h3><p class=\"verdict\">" + h(text) +
                "</p>\n";
        out_ += "<p>" + std::to_string(d.speeds.count) + " cable tests, median " +
                mbps(d.speeds.median) + " Mbit/s.";
        if (c) {
            out_ += " Contract " + figure(c->maximum) + " / " + figure(c->normal) + " / " +
                    figure(c->minimum) + " Mbit/s (maximum / normally / minimum). Below the normal "
                    "speed: " + percent(d.below_normal, d.speeds.count) + " of the tests; below the "
                    "minimum: " + std::to_string(d.below_minimum) + ".";
        } else {
            out_ += " Set the contract in the config (<code>[contract]</code>) or with "
                    "<code>--contract-" + std::string{name == "Download" ? "down" : "up"} + "</code>.";
        }
        out_ += "</p></div>\n";
    }

    /// Both verdicts.
    void verdicts() {
        out_ += "<h2>Result</h2>\n<div class=\"cards\">\n";
        verdict("Download", a_.download, meta_.contract.download);
        verdict("Upload", a_.upload, meta_.contract.upload);
        out_ += "</div>\n<p class=\"note\">A three-day window shows a deviation under the rules of the "
                "Bundesnetzagentur if, over three measurement days: 90 % of the maximum speed isn't "
                "reached on two of the days, or the normally available speed isn't reached in 90 % "
                "of the tests, or the speed falls below the minimum on two of the days (meerkat: at "
                "least " + std::to_string(kMinBelowMinimumPerDay) + " tests below it on each, or one on a day with "
                "only " + std::to_string(kMinTestsPerDay) + " tests). For each "
                "measurement day the window is the worst choice of three days a campaign starting "
                "that day could make. See "
                "<a href=\"#method\">How this was measured</a>.</p>\n";
    }

    /// One row of the summary table.
    void summary_row(std::string_view name, const DirectionReport& d) {
        const auto& s = d.speeds;
        out_ += "<tr><th>" + h(name) + "</th><td class=\"n\">" + std::to_string(s.count) +
                "</td><td class=\"n\">" + mbps(s.min) + "</td><td class=\"n\">" + mbps(s.p10) +
                "</td><td class=\"n\">" + mbps(s.median) + "</td><td class=\"n\">" + mbps(s.p90) +
                "</td><td class=\"n\">" + mbps(s.max) + "</td><td class=\"n\">" + mbps(s.mean) +
                "</td></tr>\n";
    }

    /// One row of the contract table.
    void contract_row(std::string_view name, const DirectionReport& d) {
        if (!d.thresholds) {
            out_ += "<tr><th>" + h(name) + "</th><td colspan=\"6\">no contract figures</td></tr>\n";
            return;
        }
        const auto n = d.speeds.count;
        const auto& t = *d.thresholds;
        out_ += "<tr><th>" + h(name) + "</th><td class=\"n\">" + figure(t.max90) + "</td><td class=\"n\">" +
                std::to_string(d.below_max90) + " (" + percent(d.below_max90, n) + ")</td><td class=\"n\">" +
                figure(t.normal) + "</td><td class=\"n\">" + std::to_string(d.below_normal) + " (" +
                percent(d.below_normal, n) + ")</td><td class=\"n\">" + figure(t.minimum) +
                "</td><td class=\"n\">" + std::to_string(d.below_minimum) + " (" +
                percent(d.below_minimum, n) + ")</td></tr>\n";
    }

    /// Spread of the speeds and how many tests missed each threshold.
    void summary() {
        out_ += "<h2>Summary</h2>\n<p>Valid speed tests over a cable, in Mbit/s.</p>\n<div class=\"wrap\"><table>\n"
                "<tr><th></th><th class=\"n\">Tests</th><th class=\"n\">Min</th><th class=\"n\">10 %</th>"
                "<th class=\"n\">Median</th><th class=\"n\">90 %</th><th class=\"n\">Max</th>"
                "<th class=\"n\">Mean</th></tr>\n";
        summary_row("Download", a_.download);
        summary_row("Upload", a_.upload);
        out_ += "</table></div>\n<div class=\"wrap\"><table>\n<tr><th></th><th class=\"n\">90 % of max.</th>"
                "<th class=\"n\">Tests below</th><th class=\"n\">Normally</th><th class=\"n\">Tests below</th>"
                "<th class=\"n\">Minimum</th><th class=\"n\">Tests below</th></tr>\n";
        contract_row("Download", a_.download);
        contract_row("Upload", a_.upload);
        out_ += "</table></div>\n";
    }

    /// @return The contract lines of one direction.
    static std::vector<ReferenceLine> lines(const std::optional<core::ContractSpeeds>& c) {
        if (!c) {
            return {};
        }
        const auto t = Thresholds::from(*c);
        return {{"maximum " + figure(c->maximum), c->maximum},
                {"90 % of max. " + figure(t.max90), t.max90},
                {"normally " + figure(c->normal), c->normal},
                {"minimum " + figure(c->minimum), c->minimum}};
    }

    /// One time chart: cable tests, and other links as a second series.
    std::string speed_chart(bool down) {
        TimeChart chart;
        const std::string what = down ? "Download" : "Upload";
        chart.title = what + " over time";
        chart.description = what + " speed of each test; the lines are the contract figures.";
        chart.unit = "Mbit/s";
        chart.period = meta_.period;
        chart.max_gap = meta_.speed_interval * 5 / 2;
        TimeSeries cable{"Cable", {}};
        TimeSeries other{"Wi-Fi, VPN, unknown (not counted)", {}};
        for (const auto& r : speed_.rows) {
            const auto& v = down ? r.download_mbps : r.upload_mbps;
            if (r.status != core::Status::ok || !v || !meta_.period.contains(r.time)) {
                continue;
            }
            (r.link_type == core::LinkType::wired ? cable : other).points.push_back({r.time, *v});
        }
        chart.series.push_back(std::move(cable));
        if (!other.points.empty()) {
            chart.series.push_back(std::move(other));
        }
        chart.references = lines(down ? meta_.contract.download : meta_.contract.upload);
        return time_chart(chart, local_);
    }

    /// One hour chart.
    std::string hours_chart(bool down) {
        HourChart chart;
        const std::string what = down ? "download" : "upload";
        chart.title = "Median " + what + " by hour of day";
        chart.unit = "Mbit/s";
        for (std::size_t i = 0; i < 24; ++i) {
            const auto& b = a_.hours[i];
            chart.bars[i] = {down ? b.median_down : b.median_up, std::to_string(b.tests) + " tests"};
        }
        const auto& c = down ? meta_.contract.download : meta_.contract.upload;
        if (c) {
            chart.references = {{"normally " + figure(c->normal), c->normal},
                                {"minimum " + figure(c->minimum), c->minimum}};
        }
        return hour_chart(chart);
    }

    /// All charts, with the hour table as their table view.
    void charts() {
        out_ += "<h2>Speed over time</h2>\n";
        out_ += "<figure><figcaption>Download, Mbit/s. Each cable test; gaps are times without "
                "a test (computer off, meerkat paused).</figcaption>" + speed_chart(true) + "</figure>\n";
        out_ += "<figure><figcaption>Upload, Mbit/s.</figcaption>" + speed_chart(false) + "</figure>\n";
        out_ += "<figure><figcaption>Outages (connectivity probes), on the same time axis.</figcaption>";
        OutageChart oc;
        oc.title = "Outages";
        oc.period = meta_.period;
        oc.outages = outages_.outages;
        out_ += outage_chart(oc, local_) + "</figure>\n";

        out_ += "<h2>By hour of day</h2>\n";
        out_ += "<figure><figcaption>Median download per local hour, all days.</figcaption>" +
                hours_chart(true) + "</figure>\n";
        out_ += "<figure><figcaption>Median upload per local hour, all days.</figcaption>" +
                hours_chart(false) + "</figure>\n";
        out_ += "<div class=\"wrap\"><table>\n<tr><th>Hour</th><th class=\"n\">Tests</th>"
                "<th class=\"n\">Median down</th><th class=\"n\">Below normally</th>"
                "<th class=\"n\">Median up</th><th class=\"n\">Below normally</th></tr>\n";
        for (std::size_t i = 0; i < 24; ++i) {
            const auto& b = a_.hours[i];
            const auto hr = static_cast<long>(i);
            out_ += "<tr><td>" + two(hr) + ":00–" + two((hr + 1) % 24) + ":00</td><td class=\"n\">" +
                    std::to_string(b.tests) + "</td><td class=\"n\">" + mbps(b.median_down) +
                    "</td><td class=\"n\">" +
                    (a_.download.thresholds ? std::to_string(b.below_normal_down) : "–") +
                    "</td><td class=\"n\">" + mbps(b.median_up) + "</td><td class=\"n\">" +
                    (a_.upload.thresholds ? std::to_string(b.below_normal_up) : "–") + "</td></tr>\n";
        }
        out_ += "</table></div>\n";
    }

    /// When to run the official campaign: the worst hours and a plan.
    void campaign(const Advice& a) {
        out_ += "<h2>When to run the official measurement</h2>\n<div class=\"box\"><p>A planning aid "
                "(<code>--advice</code>): leave it out of a copy for the provider.</p></div>\n"
                "<p>The measurement that counts in law is a campaign with the Bundesnetzagentur's "
                "Breitbandmessung desktop app: 30 measurements, 10 on each of 3 calendar days within 14 days, "
                "a day without measurement between two measurement days, at least 5 minutes apart and at "
                "least 3 hours between the 5th and the 6th of a day, over a cable.</p>\n<p>From the last "
                "four weeks of the period (" + h(date(a.first_day)) + " to " + h(date(a.last_day)) + ", " +
                std::to_string(a.tests) + " cable tests; a test counts if its " + h(a.directions()) +
                " was below the speed):</p>\n";
        const auto worst = worst_hours(a, 5);
        if (worst.empty()) {
            out_ += "<p>Too little data to rank the hours: each needs " + std::to_string(kMinSlotTests) +
                    " cable tests.</p>\n";
        } else {
            out_ += "<div class=\"wrap\"><table>\n<tr><th>Worst hours</th><th class=\"n\">Tests</th>"
                    "<th class=\"n\">Below normally</th><th class=\"n\">Below minimum</th></tr>\n";
            for (const auto hr : worst) {
                const auto& s = a.hours[static_cast<std::size_t>(hr)];
                out_ += "<tr><td>" + hour_slot(hr) + "</td><td class=\"n\">" + std::to_string(s.tests) +
                        "</td><td class=\"n\">" + percent(s.below_normal, s.tests) + "</td><td class=\"n\">" +
                        percent(s.below_minimum, s.tests) + "</td></tr>\n";
            }
            out_ += "</table></div>\n";
        }
        out_ += "<p>Suggested: ";
        if (a.blocks) {
            out_ += "5 measurements from " + two(a.blocks->first) + ":00 and 5 more from " +
                    two(a.blocks->second) + ":00 on each of ";
        } else {
            out_ += "measurements on ";
        }
        out_ += h(weekday_date(a.plan[0])) + ", " + h(weekday_date(a.plan[1])) + " and " +
                h(weekday_date(a.plan[2])) + (a.weekdays_ranked ? " (the worst weekdays)" : "") + ".";
        if (a.blocks && !a.any_below()) {
            out_ += " No test was below the normally available speed, so the data shows no bad time: "
                    "these hours are as good as any.";
        }
        out_ += " <code>meerkat advise-bbm</code> gives the same advice for the four weeks up to today.</p>\n";
    }

    /// @return `yes` or `–`.
    static std::string yes(bool b) { return b ? "yes" : "–"; }

    /// @return The day table's rule 3 cell: how many tests were below the
    ///         minimum, and on a measurement day whether that counts.
    static std::string below_minimum(const DayVerdict& day) {
        const auto n = day.tests_below_minimum;
        if (n == 0) {
            return "–";
        }
        const auto tests = std::to_string(n) + (n == 1 ? " test" : " tests");
        if (!day.measurement_day) {
            return tests;  // never in a window: neither yes nor no
        }
        return day.below_minimum() ? "yes (" + tests + ")"
                                   : "– (" + tests + "; needs " + std::to_string(day.dips_needed()) + ")";
    }

    /// Day and window tables of one direction.
    void direction_days(std::string_view name, const DirectionReport& d) {
        out_ += "<h3>" + h(name) + "</h3>\n";
        if (d.days.empty()) {
            out_ += "<p>No cable tests.</p>\n";
            return;
        }
        const bool t = d.thresholds.has_value();
        out_ += "<div class=\"wrap\"><table>\n<tr><th>Day</th><th class=\"n\">Tests</th><th>Measurement day</th>"
                "<th class=\"n\">Best</th><th class=\"n\">Worst</th>";
        if (t) {
            out_ += "<th>90 % of max. reached</th><th class=\"n\">Normally reached</th><th>Below minimum</th>";
        }
        out_ += "</tr>\n";
        for (const auto& day : d.days) {
            out_ += "<tr><td class=\"d\">" + h(weekday_date(day.day)) + "</td><td class=\"n\">" +
                    std::to_string(day.tests) + "</td><td>" + yes(day.measurement_day) +
                    "</td><td class=\"n\">" + mbps(day.best) + "</td><td class=\"n\">" + mbps(day.worst) + "</td>";
            if (t) {
                out_ += "<td>" + yes(day.reached_max90) + "</td><td class=\"n\">" +
                        std::to_string(day.reached_normal) + " (" + percent(day.reached_normal, day.tests) +
                        ")</td><td>" + below_minimum(day) + "</td>";
            }
            out_ += "</tr>\n";
        }
        out_ += "</table></div>\n";
        if (!t || d.windows.empty()) {
            return;
        }
        out_ += "<p>Three-day windows (" + std::to_string(d.deviating_windows()) + " of " +
                std::to_string(d.windows.size()) + " with a deviation, " +
                std::to_string(d.separate_deviations()) +
                " of them without overlap in time). One window per measurement day it can start on: "
                "the worst days a campaign starting then could have used. Windows overlap, so the "
                "same bad days can show in several.</p>\n<div class=\"wrap\"><table>\n"
                "<tr><th>Measurement days</th><th class=\"n\">Tests</th><th class=\"n\">Normally reached</th>"
                "<th>Rule 1 (90 % of max.)</th><th>Rule 2 (normally)</th><th>Rule 3 (minimum)</th></tr>\n";
        const auto mark = [](bool deviation) { return deviation ? std::string{"✕ deviation"} : std::string{"–"}; };
        for (const auto& w : d.windows) {
            char share[32];
            std::snprintf(share, sizeof share, "%.1f %%", 100.0 * w.normal_share);
            out_ += "<tr><td>" + h(date(w.days[0])) + ", " + h(date(w.days[1])) + ", " + h(date(w.days[2])) +
                    "</td><td class=\"n\">" + std::to_string(w.tests) + "</td><td class=\"n\">" + share +
                    "</td><td>" + mark(w.rule_max90) + "</td><td>" + mark(w.rule_normal) + "</td><td>" +
                    mark(w.rule_minimum) + "</td></tr>\n";
        }
        out_ += "</table></div>\n";
    }

    /// Per local day.
    void days() {
        out_ += "<h2>Day by day</h2>\n<p>Cable tests per local calendar day, in Mbit/s. A measurement "
                "day has at least " + std::to_string(kMinTestsPerDay) + " tests.</p>\n";
        direction_days("Download", a_.download);
        direction_days("Upload", a_.upload);
    }

    /// The outage summary and list.
    void outage_list() {
        out_ += "<h2>Outages</h2>\n";
        if (!outages_.file_exists) {
            out_ += "<p>No connectivity.csv in the data directory: no outage data.</p>\n";
            return;
        }
        const auto& o = a_.outages;
        out_ += "<p>" + std::to_string(outages_.rounds) + " probe rounds, " +
                std::to_string(outages_.down_rounds) + " of them without internet (" +
                percent(static_cast<std::size_t>(outages_.down_rounds), static_cast<std::size_t>(outages_.rounds)) +
                "). " + plural(o.count, "outage") + ", " + duration(o.total) + " in total";
        if (o.count > 0) {
            out_ += ", the longest " + duration(o.longest);
        }
        out_ += ".</p>\n";
        if (outages_.outages.empty()) {
            return;
        }
        out_ += "<div class=\"wrap\"><table>\n<tr><th>Start</th><th>End</th><th class=\"n\">Duration</th>"
                "<th>Where</th><th>Detail</th></tr>\n";
        for (const auto& x : outages_.outages) {
            std::string detail = ext(x.reason);
            if (x.end_reason != core::OutageEnd::recovered || x.unconfirmed_end) {
                detail += (detail.empty() ? "" : "; ") + std::string{"end: "} +
                          h(core::to_string(x.end_reason)) + (x.unconfirmed_end ? " (unconfirmed)" : "");
            }
            out_ += "<tr><td class=\"d\">" + h(moment(x.start, local_)) + "</td><td class=\"d\">" +
                    (x.end ? h(moment(*x.end, local_)) : std::string{"–"}) + "</td><td class=\"n\">" +
                    (x.duration() ? duration(*x.duration()) : std::string{"–"}) + "</td><td>" +
                    h(scope_text(x.scope)) + "</td><td>" + detail + "</td></tr>\n";
        }
        out_ += "</table></div>\n<p class=\"note\">An outage starts at the first of the probe rounds "
                "in a row that got no answer; the line may have gone down up to one probe interval "
                "earlier. “Line” means the router answered but nothing beyond it did.</p>\n";
    }

    /// Tests over Wi-Fi, VPN or an unknown link.
    void other_links() {
        if (a_.other_links.empty()) {
            return;
        }
        std::size_t n = 0;
        for (const auto& g : a_.other_links) {
            n += g.tests;
        }
        out_ += "<h2>Tests over other links</h2>\n<div class=\"box warn\"><p class=\"verdict\">⚠ " +
                std::to_string(n) + " valid tests ran over Wi-Fi, a VPN or a link meerkat couldn't "
                "classify. They are not compared with the contract: a provider rightly points to "
                "the Wi-Fi or the VPN. Only cable tests count above.</p></div>\n"
                "<div class=\"wrap\"><table>\n<tr><th>Link</th><th class=\"n\">Tests</th>"
                "<th class=\"n\">Median down</th><th class=\"n\">Median up</th></tr>\n";
        for (const auto& g : a_.other_links) {
            out_ += "<tr><td>" + h(link_text(g.type)) + "</td><td class=\"n\">" + std::to_string(g.tests) +
                    "</td><td class=\"n\">" + mbps(g.download.median) + "</td><td class=\"n\">" +
                    mbps(g.upload.median) + "</td></tr>\n";
        }
        out_ += "</table></div>\n";
    }

    /// Interfaces, link speeds and test servers seen in the period.
    void setup() {
        using IfKey = std::tuple<std::string, core::LinkType, std::string>;
        using ServerKey = std::tuple<std::string, std::string, std::string>;
        std::map<IfKey, std::size_t> ifs;
        std::map<ServerKey, std::size_t> servers;
        for (const auto& r : speed_.rows) {
            if (!meta_.period.contains(r.time) || r.status != core::Status::ok) {
                continue;
            }
            ++ifs[{r.interface, r.link_type, r.link_speed_mbps ? figure(*r.link_speed_mbps) + " Mbit/s" : "–"}];
            ++servers[{r.server_id ? std::to_string(*r.server_id) : "–", r.server_name, r.server_location}];
        }
        out_ += "<h2>Measurement setup</h2>\n<p>Speed tests: Ookla Speedtest CLI, every " +
                h(interval(meta_.speed_interval)) + ".</p>\n";
        if (ifs.empty()) {
            out_ += "<p>No valid tests in the period.</p>\n";
            return;
        }
        out_ += "<div class=\"wrap\"><table>\n<tr><th>Interface</th><th>Link</th><th>Link speed</th>"
                "<th class=\"n\">Tests</th></tr>\n";
        for (const auto& [k, n] : ifs) {
            out_ += "<tr><td>" + ext(std::get<0>(k).empty() ? "–" : std::get<0>(k)) + "</td><td>" +
                    h(link_text(std::get<1>(k))) + "</td><td>" + h(std::get<2>(k)) + "</td><td class=\"n\">" +
                    std::to_string(n) + "</td></tr>\n";
        }
        out_ += "</table></div>\n<div class=\"wrap\"><table>\n<tr><th>Server id</th><th>Server</th>"
                "<th>Location</th><th class=\"n\">Tests</th></tr>\n";
        for (const auto& [k, n] : servers) {
            out_ += "<tr><td>" + ext(std::get<0>(k)) + "</td><td>" + ext(std::get<1>(k)) + "</td><td>" +
                    ext(std::get<2>(k)) + "</td><td class=\"n\">" + std::to_string(n) + "</td></tr>\n";
        }
        out_ += "</table></div>\n";
    }

    /// Status counts and rows that couldn't be read.
    void quality() {
        out_ += "<h2>Data quality</h2>\n<div class=\"wrap\"><table>\n<tr><th>Speed test result</th>"
                "<th class=\"n\">Cable</th><th class=\"n\">Other links</th></tr>\n";
        std::set<core::Status> statuses;
        for (const auto& [s, n] : a_.wired_status) {
            statuses.insert(s);
        }
        for (const auto& [s, n] : a_.other_status) {
            statuses.insert(s);
        }
        const auto count = [](const std::map<core::Status, std::size_t>& m, core::Status s) {
            const auto it = m.find(s);
            return it == m.end() ? std::size_t{0} : it->second;
        };
        for (const auto s : statuses) {
            out_ += "<tr><td><code>" + h(core::to_string(s)) + "</code></td><td class=\"n\">" +
                    std::to_string(count(a_.wired_status, s)) + "</td><td class=\"n\">" +
                    std::to_string(count(a_.other_status, s)) + "</td></tr>\n";
        }
        if (statuses.empty()) {
            out_ += "<tr><td colspan=\"3\">no speed tests</td></tr>\n";
        }
        out_ += "</table></div>\n<p class=\"note\"><code>paused_official</code>: meerkat stood back "
                "while the Breitbandmessung app measured. <code>skipped_wlan</code> / "
                "<code>skipped_tunnel</code>: not run over Wi-Fi or a VPN. "
                "<code>no_network</code>: the line looked down.</p>\n";
        const auto torn = speed_.skipped + outages_.skipped;
        const auto bad = speed_.unreadable + outages_.unreadable;
        const auto order = speed_.out_of_order + outages_.out_of_order;
        // Counted over the files up to a day past the period (where the
        // reading stops): rows that can't be read have no time to place
        // them by.
        if (torn + bad > 0) {
            out_ += "<div class=\"box warn\"><p>⚠ The data files, read up to a day past the period, have " +
                    std::to_string(torn) +
                    " incomplete rows (a write cut short, e.g. by a power cut) and " +
                    std::to_string(bad) + " unreadable rows. They have no readable time, so they may "
                    "lie outside the period; they were left out.</p></div>\n";
        } else {
            out_ += "<p>Every row of the data files, read up to a day past the period, could be read.</p>\n";
        }
        if (order > 0) {
            out_ += "<div class=\"box warn\"><p>⚠ " + std::to_string(order) +
                    " rows in or near the period are older than the row written before them: the "
                    "computer's clock was changed (e.g. corrected after a start with a wrong time). "
                    "Times written while the clock was wrong may be off, and the outage list starts "
                    "afresh at each such jump.</p></div>\n";
        }
    }

    /// The manifests: what proves the data wasn't changed afterwards.
    void manifests() {
        out_ += "<h2>Manifests</h2>\n<p>At the end of each day (UTC) meerkat writes a manifest with "
                "the SHA-256 of every data file, chained to the manifest of the day before. Changing "
                "the data afterwards changes these hashes: <code>meerkat verify</code> hashes the files "
                "again and lists every one that was changed, shortened or added to since.</p>\n";
        if (meta_.manifests.empty()) {
            out_ += "<p>No days to list.</p>\n";
            return;
        }
        out_ += "<div class=\"wrap\"><table>\n<tr><th>Day (UTC)</th><th>File</th><th class=\"n\">Bytes</th>"
                "<th>SHA-256</th></tr>\n";
        for (const auto& m : meta_.manifests) {
            if (!m.manifest) {
                out_ += "<tr><td>" + h(m.day) + "</td><td colspan=\"3\">" +
                        ext(m.problem.empty() ? "no manifest" : m.problem) + "</td></tr>\n";
                continue;
            }
            out_ += "<tr><td>" + h(m.day) + "</td><td><b>manifest</b></td><td class=\"n\"></td><td class=\"hash\">" +
                    h(m.sha256) + "</td></tr>\n";
            for (const auto& e : m.manifest->entries) {
                out_ += "<tr><td></td><td>" + ext(e.path) + "</td><td class=\"n\">" + std::to_string(e.size) +
                        "</td><td class=\"hash\">" + ext(e.sha256) + "</td></tr>\n";
            }
        }
        out_ += "</table></div>\n";
    }

    /// How the numbers were made.
    void method() {
        out_ += "<h2 id=\"method\">How this was measured</h2>\n<ul>\n"
                "<li>Only speed tests that completed (<code>ok</code>) over a cable count. Tests over "
                "Wi-Fi, a VPN or an unknown link are listed apart.</li>\n"
                "<li>Thresholds per test: 90 % of the maximum speed, the normally available speed and "
                "the minimum speed from the contract.</li>\n"
                "<li>Measurement days follow the official campaign: a local calendar day with at least " +
                std::to_string(kMinTestsPerDay) + " tests; three of them, with at least one calendar day "
                "between two of them, within " + std::to_string(kWindowSpan.count() + 1) +
                " days. meerkat uses every test of such a day, not just ten. So that a single dip among "
                "many tests doesn't decide rule 3, a day counts as below the minimum only with at least " +
                std::to_string(kMinBelowMinimumPerDay) + " tests below it. A measurement day with only " +
                std::to_string(kMinTestsPerDay) + " tests is the same sample as a campaign day, so there "
                "one is enough. The day table shows how many. "
                "For each start day it "
                "shows the worst set of days a campaign starting then could have used.</li>\n"
                "<li>Percentiles are interpolated linearly between the nearest ranks.</li>\n"
                "<li>Outages come from the connectivity probes (pings to the router and to public "
                "hosts, a DNS lookup). A failed round while a speed test ran doesn't count.</li>\n"
                "<li>Times are local (" + ext(meta_.zone) + "); the data files store UTC.</li>\n</ul>\n";
    }

    const ReportMeta& meta_;      ///< Period and setup.
    const SpeedData& speed_;      ///< The speed tests.
    const OutageData& outages_;   ///< The outages.
    const Analysis& a_;           ///< The analysis.
    const LocalOf& local_;        ///< Local time.
    std::string out_;             ///< The page so far.
};

}  // namespace

std::string render_html(const ReportMeta& meta, const SpeedData& speed, const OutageData& outages,
                        const Analysis& analysis, const LocalOf& local) {
    return Page{meta, speed, outages, analysis, local}.render();
}

}  // namespace meerkat::report
