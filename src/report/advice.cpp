/**
 * @file advice.cpp
 * @brief Implementation of the campaign advice.
 */
#include "report/advice.hpp"

#include <algorithm>
#include <set>

namespace meerkat::report {

namespace {

using std::chrono::days;
using std::chrono::sys_days;


}  // namespace

bool Advice::any_below() const {
    return std::any_of(hours.begin(), hours.end(), [](const SlotStats& h) { return h.below_normal > 0; });
}

std::string Advice::directions() const {
    return download && upload ? "download or upload" : download ? "download" : "upload";
}

std::vector<int> worst_hours(const Advice& a, std::size_t n) {
    std::vector<int> out;
    for (int h = 0; h < 24; ++h) {
        if (a.hours[static_cast<std::size_t>(h)].ranked()) {
            out.push_back(h);
        }
    }
    std::stable_sort(out.begin(), out.end(), [&a](int x, int y) {
        return a.hours[static_cast<std::size_t>(x)].score() > a.hours[static_cast<std::size_t>(y)].score();
    });
    if (out.size() > n) {
        out.resize(n);
    }
    return out;
}

double SlotStats::score() const {
    if (tests == 0) {
        return 0;
    }
    return static_cast<double>(below_normal + below_minimum) / static_cast<double>(tests);
}

Advice advise(const SpeedData& speed, const core::Contract& contract, const LocalOf& local,
              const Period& period, std::chrono::sys_days start) {
    Advice a;
    a.period = period;
    a.first_day = local(period.from).day;
    a.start = start;
    a.last_day = local(period.to - std::chrono::seconds{1}).day;
    a.download = contract.download.has_value();
    a.upload = contract.upload.has_value();
    if (!a.download && !a.upload) {
        return a;
    }

    std::set<sys_days> all_days;
    std::array<std::set<sys_days>, 24> hour_days{};
    std::array<std::set<sys_days>, 7> weekday_days{};
    for (const auto& row : speed.rows) {
        if (!period.contains(row.time) || row.status != core::Status::ok ||
            row.link_type != core::LinkType::wired) {
            continue;
        }
        // A direction counts if it has contract figures and a value.
        bool counted = false;
        bool normal = false;
        bool minimum = false;
        const auto check = [&](const std::optional<core::ContractSpeeds>& c, const std::optional<double>& v) {
            if (c && v) {
                counted = true;
                normal = normal || *v < c->normal;
                minimum = minimum || *v < c->minimum;
            }
        };
        check(contract.download, row.download_mbps);
        check(contract.upload, row.upload_mbps);
        if (!counted) {
            continue;
        }
        const auto lt = local(row.time);
        const auto wd = std::chrono::weekday{lt.day}.c_encoding();
        const auto hour = static_cast<std::size_t>(lt.hour);
        for (auto* s : {&a.hours[hour], &a.weekdays[wd]}) {
            ++s->tests;
            s->below_normal += normal ? 1 : 0;
            s->below_minimum += minimum ? 1 : 0;
        }
        ++a.tests;
        all_days.insert(lt.day);
        hour_days[hour].insert(lt.day);
        weekday_days[wd].insert(lt.day);
    }
    a.days = all_days.size();
    for (std::size_t h = 0; h < 24; ++h) {
        a.hours[h].days = hour_days[h].size();
    }
    a.weekdays_ranked = true;
    for (std::size_t w = 0; w < 7; ++w) {
        a.weekdays[w].days = weekday_days[w].size();
        a.weekdays_ranked =
            a.weekdays_ranked && a.weekdays[w].days >= kMinWeekdayDays && a.weekdays[w].ranked();
    }

    // The two blocks of a day: the best pair of ranked hours far enough apart.
    double best = -1;
    for (int h1 = 0; h1 < 24; ++h1) {
        for (int h2 = h1 + kBlockGapHours; h2 < 24; ++h2) {
            const auto& s1 = a.hours[static_cast<std::size_t>(h1)];
            const auto& s2 = a.hours[static_cast<std::size_t>(h2)];
            if (!s1.ranked() || !s2.ranked()) {
                continue;
            }
            const double score = s1.score() + s2.score();
            if (score > best + kScoreEpsilon) {  // ties: the earlier pair
                best = score;
                a.blocks = std::pair{h1, h2};
            }
        }
    }

    // Three days a campaign could use, all within the 14 days from start.
    const auto weekday_score = [&a](sys_days d) {
        return a.weekdays_ranked ? a.weekdays[std::chrono::weekday{d}.c_encoding()].score() : 0.0;
    };
    std::vector<sys_days> candidates;
    for (auto d = start; d <= start + kWindowSpan; d += days{1}) {
        candidates.push_back(d);
    }
    double best_days = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        for_each_campaign(candidates, i, [&](std::size_t j, std::size_t k) {
            const std::array<sys_days, 3> p{candidates[i], candidates[j], candidates[k]};
            const double score = weekday_score(p[0]) + weekday_score(p[1]) + weekday_score(p[2]);
            if (score > best_days + kScoreEpsilon) {  // ties: the earliest days
                best_days = score;
                a.plan = p;
            }
        });
    }
    return a;
}

}  // namespace meerkat::report
