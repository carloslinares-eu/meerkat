/**
 * @file official.cpp
 * @brief Implementation of the official measurements in the report.
 */
#include "report/official.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/text.hpp"
#include "report/format.hpp"
#include "store/manifest.hpp"

namespace meerkat::report {

namespace {

/// @return The contract figures of one direction, if all three are there.
std::optional<core::ContractSpeeds> contract_of(const std::optional<double>& max, const std::optional<double>& normal,
                                                const std::optional<double>& min) {
    if (!max || !normal || !min) {
        return std::nullopt;
    }
    return core::ContractSpeeds{.maximum = *max, .normal = *normal, .minimum = *min};
}

/// @return The download contract entered in the app for @p m.
std::optional<core::ContractSpeeds> down_contract(const probe::OfficialMeasurement& m) {
    return contract_of(m.contract_down_max, m.contract_down_normal, m.contract_down_min);
}

/// @return The upload contract entered in the app for @p m.
std::optional<core::ContractSpeeds> up_contract(const probe::OfficialMeasurement& m) {
    return contract_of(m.contract_up_max, m.contract_up_normal, m.contract_up_min);
}

/**
 * @brief Judges one direction of a campaign by the rules as written.
 * @param days     The rows of each of the three days.
 * @param dates    The three dates.
 * @param value    Gets the speed of a row.
 * @param contract The contract entered in the app.
 * @return The finding.
 */
template <typename Value>
OfficialDirection judge(const std::array<std::vector<const OfficialRow*>, 3>& days,
                        const std::array<std::chrono::sys_days, 3>& dates, Value value,
                        const std::optional<core::ContractSpeeds>& contract) {
    OfficialDirection r;
    r.contract = contract;
    const auto t = Thresholds::of(contract);
    std::size_t reached = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        auto& d = r.days[k];
        d.day = dates[k];
        r.verdict.days[k] = dates[k];
        for (const auto* row : days[k]) {
            const auto v = value(row->m);
            if (!v) {
                continue;
            }
            d.add(*v, t);
        }
        d.measurement_day = d.tests >= kMinTestsPerDay;
        r.verdict.tests += d.tests;
        reached += d.reached_normal;
        r.verdict.tests_below_minimum += d.tests_below_minimum;
        if (t && d.tests > 0) {
            r.days_missing_max90 += d.reached_max90 ? 0 : 1;
            r.days_below_minimum += d.tests_below_minimum > 0 ? 1 : 0;  // one is enough, as written
        }
    }
    if (t && r.verdict.tests > 0) {
        r.verdict.normal_share = static_cast<double>(reached) / static_cast<double>(r.verdict.tests);
        r.verdict.rule_max90 = r.days_missing_max90 >= 2;
        r.verdict.rule_normal = r.verdict.normal_share < 0.9;
        r.verdict.rule_minimum = r.days_below_minimum >= 2;
        r.verdict.bad_days = r.days_missing_max90 + r.days_below_minimum;
    }
    return r;
}


/**
 * @brief Builds one campaign from three days.
 * @param days  The rows of each day.
 * @param dates The dates.
 * @return The campaign.
 */
OfficialCampaign campaign(const std::array<std::vector<const OfficialRow*>, 3>& days,
                          const std::array<std::chrono::sys_days, 3>& dates) {
    OfficialCampaign c;
    c.days = dates;
    const auto& last = days[2].back()->m;  // the figures as last entered
    c.download = judge(days, dates, [](const probe::OfficialMeasurement& m) { return m.download_mbps; },
                       down_contract(last));
    c.upload = judge(days, dates, [](const probe::OfficialMeasurement& m) { return m.upload_mbps; },
                     up_contract(last));

    std::size_t not_lan = 0, battery = 0, weak_nic = 0, limited = 0;
    bool contract_changed = false;
    for (const auto& day : days) {
        c.measurements += day.size();
        for (const auto* row : day) {
            const auto& m = row->m;
            not_lan += !m.connection.empty() && m.connection != "LAN" ? 1U : 0U;
            battery += m.on_power == false ? 1U : 0U;
            weak_nic += m.nic_sufficient == false ? 1U : 0U;
            limited += m.data_rate_limited == true ? 1U : 0U;
            contract_changed = contract_changed || down_contract(m) != c.download.contract ||
                               up_contract(m) != c.upload.contract;
        }
    }
    for (std::size_t k = 1; k < 3; ++k) {
        if (dates[k] - dates[k - 1] < kDayGap) {
            c.problems.push_back(date(dates[k - 1]) + " and " + date(dates[k]) +
                                 " have no day without measurement between them");
        }
    }
    for (std::size_t k = 0; k < 3; ++k) {
        for (const auto* dir : {&c.download, &c.upload}) {
            const auto n = dir->days[k].tests;
            if (n < kMinTestsPerDay) {
                c.problems.push_back(date(dates[k]) + ": only " + std::to_string(n) + " " +
                                     (dir == &c.download ? "download" : "upload") + " results");
            }
        }
    }
    const auto count = [&](std::size_t n, const std::string& what) {
        if (n > 0) {
            c.problems.push_back(core::text::plural(n, "measurement") + " " + what);
        }
    };
    count(not_lan, "not over a cable (the app saw another connection)");
    count(battery, "with the computer on battery");
    count(weak_nic, "with a network card the app found too slow");
    count(limited, "with the data rate marked as limited (data cap reached)");
    if (contract_changed) {
        c.problems.push_back("the contract figures entered in the app change within the campaign; the last ones are used");
    }
    return c;
}

}  // namespace

std::optional<double> Comparison::difference_down() const {
    if (!official_down || !meerkat_down || *meerkat_down <= 0) {
        return std::nullopt;
    }
    return (*official_down - *meerkat_down) / *meerkat_down;
}

std::optional<double> Comparison::difference_up() const {
    if (!official_up || !meerkat_up || *meerkat_up <= 0) {
        return std::nullopt;
    }
    return (*official_up - *meerkat_up) / *meerkat_up;
}

std::optional<bool> Comparison::agrees() const {
    const auto down = difference_down();
    const auto up = difference_up();
    if (!down && !up) {
        return std::nullopt;
    }
    return (!down || std::abs(*down) <= kAgreement) && (!up || std::abs(*up) <= kAgreement);
}

OfficialAnalysis analyse_official(const OfficialData& official, const SpeedData& speed, const LocalOf& local,
                                  const Period& period) {
    OfficialAnalysis a;
    a.unusable = official.skipped + official.unreadable;
    // The period's local days, inclusive.
    const auto first_day = local(period.from).day;
    const auto last_day = local(period.to - std::chrono::seconds{1}).day;
    const auto in_period = [&](std::chrono::sys_days d) { return d >= first_day && d <= last_day; };

    // All rows by day: a campaign across the period's edge stays whole.
    std::map<std::chrono::sys_days, std::vector<const OfficialRow*>> by_day;
    for (const auto& r : official.rows) {
        by_day[local(r.m.time).day].push_back(&r);
        if (period.contains(r.m.time)) {
            a.rows.push_back(r);
        } else {
            ++a.outside;
        }
    }
    if (!a.rows.empty()) {
        const auto& last = a.rows.back().m;
        a.app_contract.download = down_contract(last);
        a.app_contract.upload = up_contract(last);
    }

    // Campaigns: full days in order, up to three within kWindowSpan of the first.
    std::vector<std::chrono::sys_days> group;
    const auto give_up = [&] {
        for (const auto d : group) {
            if (in_period(d)) {
                a.incomplete_days.push_back(d);
            }
        }
        group.clear();
    };
    for (const auto& [day, rows] : by_day) {
        if (rows.size() < kMinTestsPerDay) {
            if (in_period(day)) {
                a.short_days.push_back(day);
            }
            continue;
        }
        if (!group.empty() && day - group.front() > kWindowSpan) {
            give_up();  // too late for the campaign begun then: this day starts a new one
        }
        group.push_back(day);
        if (group.size() == 3) {
            const std::array dates{group[0], group[1], group[2]};
            group.clear();
            if (!in_period(dates[0]) && !in_period(dates[1]) && !in_period(dates[2])) {
                continue;
            }
            a.campaigns.push_back(campaign({by_day[dates[0]], by_day[dates[1]], by_day[dates[2]]}, dates));
            if (!in_period(dates[0]) || !in_period(dates[2])) {
                a.campaigns.back().problems.push_back(
                    "part of the campaign lies outside this report's period; its days are all used here");
            }
        }
    }
    give_up();

    // meerkat's cable tests around each official result.
    for (const auto& r : a.rows) {
        Comparison c;
        c.time = r.m.time;
        c.official_down = r.m.download_mbps;
        c.official_up = r.m.upload_mbps;
        std::vector<double> down;
        std::vector<double> up;
        const auto from = r.m.time - kCompareSpan;
        const auto to = r.m.time + kCompareSpan;
        const auto first = std::lower_bound(speed.rows.begin(), speed.rows.end(), from,
                                            [](const SpeedRow& s, core::Timestamp t) { return s.time < t; });
        for (auto it = first; it != speed.rows.end() && it->time <= to; ++it) {
            if (!it->counts()) {
                continue;
            }
            ++c.tests;
            if (it->download_mbps) {
                down.push_back(*it->download_mbps);
            }
            if (it->upload_mbps) {
                up.push_back(*it->upload_mbps);
            }
        }
        c.meerkat_down = median(std::move(down));
        c.meerkat_up = median(std::move(up));
        a.comparisons.push_back(c);
    }
    return a;
}

}  // namespace meerkat::report
