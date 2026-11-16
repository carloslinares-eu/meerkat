/**
 * @file advise.cpp
 * @brief `meerkat advise-bbm`.
 */
#include "app/advise.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
#include <vector>

#include "core/text.hpp"
#include "report/data.hpp"
#include "store/manifest.hpp"

namespace meerkat::app {

namespace {

using std::chrono::days;
using std::chrono::sys_days;

/// Exit code for a bad argument.
constexpr int kUsageError = 2;
/// Hours listed as the worst.
constexpr std::size_t kListedHours = 5;

/// @return `Mon` … `Sun` for a weekday's C encoding (0 = Sunday).
std::string weekday_name(unsigned wd) {
    static constexpr std::array<const char*, 7> names{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return names.at(wd);
}

/// @return E.g. `Tue 2026-11-17`.
std::string day_with_weekday(sys_days d) {
    return weekday_name(std::chrono::weekday{d}.c_encoding()) + " " + day_text(d);
}

/// @return `HH:00`.
std::string hour_text(int h) {
    std::string s = std::to_string(h);
    return (s.size() < 2 ? "0" + s : s) + ":00";
}

/// @return @p x as a whole percentage, e.g. `45 %`.
std::string percent(std::size_t part, std::size_t whole) {
    const auto p = whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    return std::to_string(static_cast<long>(std::lround(p))) + " %";
}

/// @return @p s padded with spaces to @p width.
std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) {
        s.append(width - s.size(), ' ');
    }
    return s;
}

/// @return "download", "upload" or "download and upload": what counts.
std::string directions(const report::Advice& a) {
    return a.download && a.upload ? "download or upload" : a.download ? "download" : "upload";
}

}  // namespace

std::string advice_text(const report::Advice& a, const ReportRange& range, const std::string& zone) {
    using core::text::plural;
    std::string s;
    s += "Based on " + plural(a.tests, "cable test") + " on " + plural(a.days, "day") + " (" +
         day_text(range.from) + " to " + day_text(range.to) + ", hours in " + zone +
         "). A test counts as below a speed if its " + directions(a) + " is.\n";

    // The worst hours, ranked hours only.
    std::vector<int> ranked;
    for (int h = 0; h < 24; ++h) {
        if (a.hours[static_cast<std::size_t>(h)].ranked()) {
            ranked.push_back(h);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&a](int x, int y) {
        return a.hours[static_cast<std::size_t>(x)].score() > a.hours[static_cast<std::size_t>(y)].score();
    });
    const bool any_below = std::any_of(a.hours.begin(), a.hours.end(),
                                       [](const report::SlotStats& h) { return h.below_normal > 0; });
    if (ranked.empty()) {
        s += "\nToo little data to rank the hours: each needs " + std::to_string(report::kMinSlotTests) +
             " cable tests. Let `meerkat run` record for a week or more.\n";
    } else {
        s += "\nWorst hours (share of tests below the normally available / the minimum speed):\n";
        for (std::size_t i = 0; i < std::min(kListedHours, ranked.size()); ++i) {
            const auto h = ranked[i];
            const auto& st = a.hours[static_cast<std::size_t>(h)];
            s += "  " + hour_text(h) + "–" + hour_text((h + 1) % 24) + "  " + pad(plural(st.tests, "test"), 10) +
                 "  below normal " + pad(percent(st.below_normal, st.tests), 5) + "  below minimum " +
                 percent(st.below_minimum, st.tests) + "\n";
        }
    }
    if (a.weekdays_ranked) {
        std::vector<unsigned> wd(7);
        std::iota(wd.begin(), wd.end(), 0u);
        std::stable_sort(wd.begin(), wd.end(), [&a](unsigned x, unsigned y) {
            return a.weekdays[x].score() > a.weekdays[y].score();
        });
        s += "\nWeekdays, worst first:";
        for (const auto w : wd) {
            s += " " + weekday_name(w) + " (" + percent(a.weekdays[w].below_normal, a.weekdays[w].tests) + ")";
        }
        s += "\n";
    } else {
        s += "\nWeekdays aren't ranked: each needs " + std::to_string(report::kMinWeekdayDays) +
             " days of data, so the days below are simply the earliest possible.\n";
    }

    s += "\nSuggested campaign (Breitbandmessung desktop app, over a cable):\n";
    for (std::size_t i = 0; i < a.plan.size(); ++i) {
        s += "  Day " + std::to_string(i + 1) + "  " + day_with_weekday(a.plan[i]) + "\n";
    }
    if (a.blocks) {
        s += "  Each day: 5 measurements from " + hour_text(a.blocks->first) + ", 5 more from " +
             hour_text(a.blocks->second) + ".\n";
        if (!any_below) {
            s += "  No test was below the normally available speed, so the data shows no bad time:\n"
                 "  these hours are as good as any.\n";
        }
    } else {
        s += "  Hours: no advice yet (too little data).\n";
    }
    s += "\nThe campaign's rules (the app enforces them):\n"
         "  - 30 measurements: 10 on each of 3 calendar days, all within 14 days.\n"
         "  - At least one calendar day without measurement between two measurement days.\n"
         "  - At least 5 minutes between measurements, and at least 3 hours between the\n"
         "    5th and the 6th of a day.\n"
         "  - Over a cable to the router, with nothing else using the line. meerkat\n"
         "    pauses its own speed tests while the app runs.\n"
         "A deviation needs only one of the three rules, for download or upload: e.g.\n"
         "below the minimum at least once on 2 of the 3 days.\n";
    return s;
}

int advise_command(const core::Config& config, const AdviseOptions& options, const ReportEnv& clock_env,
                   std::ostream& out, std::ostream& err) {
    const auto env = fixed_clock(clock_env);
    if (!config.contract.download && !config.contract.upload) {
        err << "meerkat: advise-bbm needs the contract figures: [contract] in the config, or "
               "--contract-down/--contract-up\n";
        return kUsageError;
    }
    ReportOptions ro;
    ro.to = options.to;
    ro.from = options.from;
    if (ro.from.empty()) {
        // Four weeks back from --to (or today); report_range() checks the rest.
        const auto to = options.to.empty() ? std::optional{env.zone.local(env.now()).day} : store::parse_day(options.to);
        if (to) {
            ro.from = day_text(*to - (kAdviceSpan - days{1}));
        }
    }
    const auto range = report_range(config, ro, env, err);
    if (!range) {
        return kUsageError;
    }
    sys_days start = env.zone.local(env.now()).day + days{1};
    if (!options.start.empty()) {
        const auto s = store::parse_day(options.start);
        if (!s) {
            err << "meerkat: --start must be a date like 2026-11-20 (got '" << core::text::visible(options.start)
                << "')\n";
            return kUsageError;
        }
        start = *s;
    }
    try {
        const report::Period period{env.zone.midnight(range->from), env.zone.midnight(range->to + days{1})};
        const auto speed = report::load_speed(config.data_dir, period);
        const auto advice = report::advise(speed, config.contract, env.zone.local, period, start);
        out << advice_text(advice, *range, env.zone.name);
        if (!speed.file_exists) {
            out << "No speed.csv in " << core::text::visible(config.data_dir.string()) << " (see --data-dir).\n";
        }
    } catch (const std::exception& e) {
        err << "meerkat: " << core::text::visible(e.what()) << '\n';
        return 1;
    }
    return 0;
}

}  // namespace meerkat::app
