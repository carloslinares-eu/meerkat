/**
 * @file advice.cpp
 * @brief Implementation of the campaign advice.
 */
#include "report/advice.hpp"

#include <set>

namespace meerkat::report {

namespace {

using std::chrono::days;
using std::chrono::sys_days;

/// The calendar days the official campaign spans at most (day 1 to day 14).
constexpr int kCampaignDays = 14;

}  // namespace

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
        a.weekdays_ranked = a.weekdays_ranked && a.weekdays[w].days >= kMinWeekdayDays;
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
            if (score > best) {  // ties: the earlier pair
                best = score;
                a.blocks = std::pair{h1, h2};
            }
        }
    }

    // Three days, two or more apart, within the campaign's 14 days.
    const auto weekday_score = [&a](sys_days d) {
        return a.weekdays_ranked ? a.weekdays[std::chrono::weekday{d}.c_encoding()].score() : 0.0;
    };
    double best_days = -1;
    for (int d1 = 0; d1 < kCampaignDays; ++d1) {
        for (int d2 = d1 + 2; d2 < kCampaignDays; ++d2) {
            for (int d3 = d2 + 2; d3 < kCampaignDays; ++d3) {
                const std::array<sys_days, 3> p{start + days{d1}, start + days{d2}, start + days{d3}};
                const double score = weekday_score(p[0]) + weekday_score(p[1]) + weekday_score(p[2]);
                if (score > best_days + 1e-12) {  // ties: the earliest days
                    best_days = score;
                    a.plan = p;
                }
            }
        }
    }
    return a;
}

}  // namespace meerkat::report
