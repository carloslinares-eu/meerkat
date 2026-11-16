/**
 * @file official.cpp
 * @brief Implementation of the official measurements in the report.
 */
#include "report/official.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <system_error>

#include "core/text.hpp"
#include "store/csv_reader.hpp"
#include "store/manifest.hpp"
#include "store/official.hpp"

namespace meerkat::report {

namespace {

/// @return The index of column @p name of the official schema.
std::size_t column(std::string_view name) {
    const auto& c = store::official_schema().columns;
    return static_cast<std::size_t>(std::find(c.begin(), c.end(), name) - c.begin());
}

/// @return `yes` → true, `no` → false, else nothing.
std::optional<bool> yes_no(const std::string& s) {
    if (s == "yes") {
        return true;
    }
    if (s == "no") {
        return false;
    }
    return std::nullopt;
}

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

/// @return The median of @p v, or nothing.
std::optional<double> median_of(std::vector<double> v) { return summarize(std::move(v)).median; }

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
    const auto t = contract ? std::optional{Thresholds::from(*contract)} : std::nullopt;
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
            ++d.tests;
            d.best = std::max(d.best.value_or(*v), *v);
            d.worst = std::min(d.worst.value_or(*v), *v);
            if (t) {
                d.reached_max90 = d.reached_max90 || *v >= t->max90;
                d.reached_normal += *v >= t->normal ? 1U : 0U;
                d.tests_below_minimum += *v < t->minimum ? 1U : 0U;
            }
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

/// @return The date as `YYYY-MM-DD`.
std::string date_text(std::chrono::sys_days d) { return core::to_iso_day(d); }

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
            c.problems.push_back(date_text(dates[k - 1]) + " and " + date_text(dates[k]) +
                                 " have no day without measurement between them");
        }
    }
    if (dates[2] - dates[0] > kWindowSpan) {
        c.problems.push_back("the three days span more than 14 days");
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

OfficialData load_official(const std::filesystem::path& data_dir) {
    OfficialData d;
    static const auto t = column("time"), nr = column("number"), dn = column("download_mbps"),
                      up = column("upload_mbps"), la = column("latency_ms"), dmin = column("contract_down_min"),
                      dnor = column("contract_down_normal"), dmax = column("contract_down_max"),
                      umin = column("contract_up_min"), unor = column("contract_up_normal"),
                      umax = column("contract_up_max"), cn = column("connection"), pw = column("on_power"),
                      nic = column("nic_sufficient"), lim = column("data_rate_limited"), id = column("test_id"),
                      ver = column("app_version"), file = column("file");
    const auto num = [](const std::string& s) { return core::text::parse_double(s); };
    try {
        const auto stats = store::read_csv(data_dir / store::kOfficialFile, store::official_schema(),
                                           [&](const std::vector<std::string>& f) {
                                               const auto time = core::parse_iso8601(f[t]);
                                               if (!time) {
                                                   ++d.unreadable;
                                                   return true;
                                               }
                                               OfficialRow r;
                                               auto& m = r.m;
                                               m.time = *time;
                                               m.number = core::text::parse_int<int>(f[nr]);
                                               m.download_mbps = num(f[dn]);
                                               m.upload_mbps = num(f[up]);
                                               m.latency_ms = num(f[la]);
                                               m.contract_down_min = num(f[dmin]);
                                               m.contract_down_normal = num(f[dnor]);
                                               m.contract_down_max = num(f[dmax]);
                                               m.contract_up_min = num(f[umin]);
                                               m.contract_up_normal = num(f[unor]);
                                               m.contract_up_max = num(f[umax]);
                                               m.connection = f[cn];
                                               m.on_power = yes_no(f[pw]);
                                               m.nic_sufficient = yes_no(f[nic]);
                                               m.data_rate_limited = yes_no(f[lim]);
                                               m.test_id = f[id];
                                               m.app_version = f[ver];
                                               r.file = f[file];
                                               d.rows.push_back(std::move(r));
                                               return true;
                                           });
        d.file_exists = true;
        d.skipped = stats.skipped;
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
    }
    std::stable_sort(d.rows.begin(), d.rows.end(),
                     [](const OfficialRow& a, const OfficialRow& b) { return a.m.time < b.m.time; });
    return d;
}

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
    std::map<std::chrono::sys_days, std::vector<const OfficialRow*>> by_day;
    for (const auto& r : official.rows) {
        if (!period.contains(r.m.time)) {
            ++a.outside;
            continue;
        }
        a.rows.push_back(r);
    }
    for (const auto& r : a.rows) {
        by_day[local(r.m.time).day].push_back(&r);
    }
    if (!a.rows.empty()) {
        const auto& last = a.rows.back().m;
        a.app_contract.download = down_contract(last);
        a.app_contract.upload = up_contract(last);
    }

    // Campaigns: full days, three by three.
    std::vector<std::chrono::sys_days> full;
    for (const auto& [day, rows] : by_day) {
        (rows.size() >= kMinTestsPerDay ? full : a.short_days).push_back(day);
    }
    std::size_t i = 0;
    for (; i + 3 <= full.size(); i += 3) {
        const std::array dates{full[i], full[i + 1], full[i + 2]};
        a.campaigns.push_back(campaign({by_day[dates[0]], by_day[dates[1]], by_day[dates[2]]}, dates));
    }
    a.open_days.assign(full.begin() + static_cast<std::ptrdiff_t>(i), full.end());

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
            if (it->status != core::Status::ok || it->link_type != core::LinkType::wired) {
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
        c.meerkat_down = median_of(std::move(down));
        c.meerkat_up = median_of(std::move(up));
        a.comparisons.push_back(c);
    }
    return a;
}

}  // namespace meerkat::report
