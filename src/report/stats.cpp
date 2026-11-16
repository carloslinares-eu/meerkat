/**
 * @file stats.cpp
 * @brief Implementation of the report's analysis.
 */
#include "report/stats.hpp"

#include <algorithm>
#include <numeric>

namespace meerkat::report {

namespace {

/**
 * @brief The value at quantile @p q of sorted values (linear interpolation).
 * @param v Sorted, not empty.
 * @param q In [0, 1].
 * @return The value.
 */
double quantile(const std::vector<double>& v, double q) {
    const double pos = q * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return v[lo] + (v[hi] - v[lo]) * frac;
}

/**
 * @brief The median of @p v, or nothing.
 * @param v Values in any order.
 * @return The median.
 */
std::optional<double> median(std::vector<double> v) { return summarize(std::move(v)).median; }

/// A valid cable test's value for one direction, with its local time.
struct Sample {
    LocalTime at;    ///< When, local.
    double mbps = 0; ///< Speed.
};

/**
 * @brief The days, rule counts and windows of one direction.
 * @param samples    Its valid cable tests, in time order.
 * @param thresholds The thresholds, if there is a contract.
 * @return The report.
 */
DirectionReport direction(const std::vector<Sample>& samples,
                          const std::optional<Thresholds>& thresholds) {
    DirectionReport r;
    r.thresholds = thresholds;
    std::vector<double> values;
    values.reserve(samples.size());
    std::map<std::chrono::sys_days, DayVerdict> days;
    for (const auto& s : samples) {
        values.push_back(s.mbps);
        auto& d = days[s.at.day];
        d.day = s.at.day;
        ++d.tests;
        d.best = std::max(d.best.value_or(s.mbps), s.mbps);
        d.worst = std::min(d.worst.value_or(s.mbps), s.mbps);
        if (thresholds) {
            const auto& t = *thresholds;
            if (s.mbps >= t.max90) {
                d.reached_max90 = true;
            } else {
                ++r.below_max90;
            }
            if (s.mbps >= t.normal) {
                ++d.reached_normal;
            } else {
                ++r.below_normal;
            }
            if (s.mbps < t.minimum) {
                ++d.tests_below_minimum;
                ++r.below_minimum;
            }
        }
    }
    r.speeds = summarize(std::move(values));
    for (auto& [day, d] : days) {
        d.measurement_day = d.tests >= kMinTestsPerDay;
        d.below_minimum = d.tests_below_minimum >= kMinBelowMinimumPerDay;
        r.days.push_back(d);
    }
    if (!thresholds) {
        return r;
    }
    std::vector<const DayVerdict*> measured;
    for (const auto& d : r.days) {
        if (d.measurement_day) {
            measured.push_back(&d);
        }
    }
    const auto judge = [](const std::array<const DayVerdict*, 3>& w) {
        WindowVerdict v;
        int missed_max90 = 0;
        int below_min = 0;
        std::size_t reached = 0;
        for (std::size_t k = 0; k < 3; ++k) {
            v.days[k] = w[k]->day;
            v.tests += w[k]->tests;
            reached += w[k]->reached_normal;
            missed_max90 += w[k]->reached_max90 ? 0 : 1;
            below_min += w[k]->below_minimum ? 1 : 0;
        }
        v.normal_share = static_cast<double>(reached) / static_cast<double>(v.tests);
        v.rule_max90 = missed_max90 >= 2;
        v.rule_normal = v.normal_share < 0.9;
        v.rule_minimum = below_min >= 2;
        v.bad_days = missed_max90 + below_min;
        return v;
    };
    for (std::size_t i = 0; i < measured.size(); ++i) {
        // Every set of days a campaign starting on day i could use (#97).
        std::optional<WindowVerdict> worst;
        const auto last = measured[i]->day + kWindowSpan;
        for (std::size_t j = i + 1; j < measured.size() && measured[j]->day <= last; ++j) {
            if (measured[j]->day - measured[i]->day < kDayGap) {
                continue;
            }
            for (std::size_t k = j + 1; k < measured.size() && measured[k]->day <= last; ++k) {
                if (measured[k]->day - measured[j]->day < kDayGap) {
                    continue;
                }
                const auto v = judge({measured[i], measured[j], measured[k]});
                if (!worst || worse_window(v, *worst)) {
                    worst = v;
                }
            }
        }
        if (worst) {
            r.windows.push_back(*worst);
        }
    }
    return r;
}

}  // namespace

LocalOf system_local() {
    const std::chrono::time_zone* zone = nullptr;
    try {
        zone = std::chrono::current_zone();
    } catch (const std::exception&) {
        return utc_local();  // no time zone data: UTC, as the CSVs are
    }
    return [zone](core::Timestamp t) {
        const auto info = zone->get_info(std::chrono::floor<std::chrono::seconds>(t));
        const auto local = t + info.offset;
        const auto day = std::chrono::floor<std::chrono::days>(local);
        const auto hour = std::chrono::floor<std::chrono::hours>(local - day).count();
        return LocalTime{day, static_cast<int>(hour), info.offset};
    };
}

LocalOf utc_local() {
    return [](core::Timestamp t) {
        const auto day = std::chrono::floor<std::chrono::days>(t);
        return LocalTime{day, static_cast<int>(std::chrono::floor<std::chrono::hours>(t - day).count())};
    };
}

Summary summarize(std::vector<double> values) {
    Summary s;
    s.count = values.size();
    if (values.empty()) {
        return s;
    }
    std::sort(values.begin(), values.end());
    s.min = values.front();
    s.max = values.back();
    s.p10 = quantile(values, 0.1);
    s.median = quantile(values, 0.5);
    s.p90 = quantile(values, 0.9);
    s.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    return s;
}

Thresholds Thresholds::from(const core::ContractSpeeds& c) {
    return {.max90 = 0.9 * c.maximum, .normal = c.normal, .minimum = c.minimum};
}

bool worse_window(const WindowVerdict& a, const WindowVerdict& b) {
    if (a.rules_held() != b.rules_held()) {
        return a.rules_held() > b.rules_held();
    }
    if (a.bad_days != b.bad_days) {
        return a.bad_days > b.bad_days;
    }
    if (a.normal_share != b.normal_share) {
        return a.normal_share < b.normal_share;
    }
    return a.days < b.days;
}

std::size_t DirectionReport::separate_deviations() const {
    // The windows are in order of their first day; picking the one that
    // ends first each time gives the most that don't overlap.
    std::vector<const WindowVerdict*> bad;
    for (const auto& w : windows) {
        if (w.deviation()) {
            bad.push_back(&w);
        }
    }
    std::sort(bad.begin(), bad.end(), [](const auto* a, const auto* b) { return a->days[2] < b->days[2]; });
    std::size_t n = 0;
    std::optional<std::chrono::sys_days> end;
    for (const auto* w : bad) {
        if (!end || w->days[0] > *end) {
            ++n;
            end = w->days[2];
        }
    }
    return n;
}

std::size_t DirectionReport::deviating_windows() const {
    return static_cast<std::size_t>(std::count_if(
        windows.begin(), windows.end(), [](const WindowVerdict& w) { return w.deviation(); }));
}

Analysis analyse(const SpeedData& speed, const OutageData& outages,
                 const core::Contract& contract, const LocalOf& local, const Period& period) {
    Analysis a;
    a.period = period;
    std::vector<Sample> down;
    std::vector<Sample> up;
    std::array<std::vector<double>, 24> hour_down{};
    std::array<std::vector<double>, 24> hour_up{};
    std::map<core::LinkType, std::pair<std::vector<double>, std::vector<double>>> others;
    std::map<core::LinkType, std::size_t> other_tests;

    const std::optional<Thresholds> t_down =
        contract.download ? std::optional{Thresholds::from(*contract.download)} : std::nullopt;
    const std::optional<Thresholds> t_up =
        contract.upload ? std::optional{Thresholds::from(*contract.upload)} : std::nullopt;

    for (const auto& row : speed.rows) {
        if (!period.contains(row.time)) {
            continue;
        }
        const bool wired = row.link_type == core::LinkType::wired;
        ++(wired ? a.wired_status : a.other_status)[row.status];
        if (row.status != core::Status::ok) {
            continue;
        }
        if (!wired) {
            auto& [d, u] = others[row.link_type];
            ++other_tests[row.link_type];
            if (row.download_mbps) {
                d.push_back(*row.download_mbps);
            }
            if (row.upload_mbps) {
                u.push_back(*row.upload_mbps);
            }
            continue;
        }
        const auto at = local(row.time);
        auto& h = a.hours[static_cast<std::size_t>(at.hour)];
        ++h.tests;
        if (row.download_mbps) {
            down.push_back({at, *row.download_mbps});
            hour_down[static_cast<std::size_t>(at.hour)].push_back(*row.download_mbps);
            if (t_down && *row.download_mbps < t_down->normal) {
                ++h.below_normal_down;
            }
        }
        if (row.upload_mbps) {
            up.push_back({at, *row.upload_mbps});
            hour_up[static_cast<std::size_t>(at.hour)].push_back(*row.upload_mbps);
            if (t_up && *row.upload_mbps < t_up->normal) {
                ++h.below_normal_up;
            }
        }
    }
    a.download = direction(down, t_down);
    a.upload = direction(up, t_up);
    for (std::size_t i = 0; i < 24; ++i) {
        a.hours[i].median_down = median(std::move(hour_down[i]));
        a.hours[i].median_up = median(std::move(hour_up[i]));
    }
    for (auto& [type, values] : others) {
        a.other_links.push_back({.type = type,
                                 .tests = other_tests[type],
                                 .download = summarize(std::move(values.first)),
                                 .upload = summarize(std::move(values.second))});
    }
    for (const auto& o : outages.outages) {
        ++a.outages.count;
        const auto d = o.duration().value_or(std::chrono::seconds{0});
        a.outages.total += d;
        a.outages.longest = std::max(a.outages.longest, d);
        ++a.outages.by_scope[o.scope];
    }
    return a;
}

}  // namespace meerkat::report
