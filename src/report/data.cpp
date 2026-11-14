/**
 * @file data.cpp
 * @brief Implementation of the report's data loading.
 */
#include "report/data.hpp"

#include <algorithm>
#include <stdexcept>
#include <system_error>

#include "core/text.hpp"
#include "store/csv_reader.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"

namespace meerkat::report {

namespace {

/// Column positions of a schema, by name.
class Columns {
public:
    /**
     * @brief Indexes @p schema.
     * @param schema The schema.
     */
    explicit Columns(const store::Schema& schema) : names_{schema.columns} {}

    /**
     * @brief The position of a column.
     * @param name Its name; must be in the schema.
     * @return The index.
     */
    [[nodiscard]] std::size_t operator[](std::string_view name) const {
        const auto it = std::find(names_.begin(), names_.end(), name);
        if (it == names_.end()) {  // a schema change the report wasn't updated for
            throw std::logic_error("report: no column '" + std::string{name} + "'");
        }
        return static_cast<std::size_t>(it - names_.begin());
    }

private:
    std::vector<std::string> names_;  ///< The column names.
};

/**
 * @brief An optional number: empty text is "not measured".
 * @param s The field.
 * @return The number, or nothing.
 */
std::optional<double> number(const std::string& s) { return core::text::parse_double(s); }

/**
 * @brief An optional integer.
 * @param s The field.
 * @return The number, or nothing.
 */
template <typename T>
std::optional<T> integer(const std::string& s) {
    return core::text::parse_int<T>(s);
}

/**
 * @brief Reads one file, treating a missing one as empty.
 * @param path   The file.
 * @param schema Its schema.
 * @param row    Gets each row.
 * @param exists Set to whether it exists.
 * @return The read stats.
 */
store::CsvReadStats read_if_there(const std::filesystem::path& path, const store::Schema& schema,
                                  const store::CsvRowFn& row, bool& exists) {
    try {
        auto stats = store::read_csv(path, schema, row);
        exists = true;
        return stats;
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::no_such_file_or_directory) {
            exists = false;
            return {};
        }
        throw;
    }
}

}  // namespace

SpeedData load_speed(const std::filesystem::path& data_dir, const Period& period) {
    SpeedData d;
    const auto& schema = store::speed_schema();
    const Columns col{schema};
    const auto t = col["time"], st = col["status"], er = col["error"], lt = col["link_type"],
               ifc = col["interface"], ls = col["link_speed_mbps"], dn = col["download_mbps"],
               up = col["upload_mbps"], la = col["latency_ms"], ji = col["jitter_ms"],
               pl = col["packet_loss_percent"], si = col["server_id"], sn = col["server_name"],
               sl = col["server_location"];
    const auto stats = read_if_there(
        data_dir / store::kSpeedFile, schema,
        [&](const std::vector<std::string>& f) {
            const auto time = core::parse_iso8601(f[t]);
            const auto status = core::parse_status(f[st]);
            const auto link = core::parse_link_type(f[lt]);
            if (!time || !status || !link) {
                ++d.unreadable;
                return true;
            }
            if (*time >= period.to) {
                return false;  // in time order: nothing after this is wanted
            }
            if (*time < period.from) {
                return true;
            }
            d.rows.push_back({.time = *time,
                              .status = *status,
                              .error = f[er],
                              .link_type = *link,
                              .interface = f[ifc],
                              .link_speed_mbps = number(f[ls]),
                              .download_mbps = number(f[dn]),
                              .upload_mbps = number(f[up]),
                              .latency_ms = number(f[la]),
                              .jitter_ms = number(f[ji]),
                              .packet_loss_percent = number(f[pl]),
                              .server_id = integer<std::int64_t>(f[si]),
                              .server_name = f[sn],
                              .server_location = f[sl]});
            return true;
        },
        d.file_exists);
    d.skipped = stats.skipped;
    return d;
}

ReplayContext replay_context(const std::vector<SpeedRow>& rows, std::chrono::seconds round_time,
                             std::chrono::seconds speed_interval) {
    ReplayContext c;
    std::optional<Span> pause;
    for (const auto& r : rows) {
        switch (r.status) {
        case core::Status::ok:
        case core::Status::error:
        case core::Status::timeout:
            c.busy.emplace_back(r.time - round_time,
                                r.time + (r.status == core::Status::timeout ? kTimedOutTestBusy
                                                                            : kSpeedTestBusy));
            break;
        default:
            break;  // never started: no traffic
        }
        if (r.status == core::Status::paused_official) {
            if (pause) {
                pause->second = r.time + speed_interval;
            } else {
                pause = Span{r.time - speed_interval, r.time + speed_interval};
            }
        } else if (pause) {
            c.paused.push_back(*pause);
            pause.reset();
        }
    }
    if (pause) {
        c.paused.push_back(*pause);
    }
    return c;
}

namespace {

/**
 * @brief Whether @p t lies in one of @p spans.
 * @param spans Sorted by start; they may overlap.
 * @param t     The time.
 * @return True if some span has `from <= t < to`.
 */
bool inside(const std::vector<Span>& spans, core::Timestamp t) {
    // Spans start in order and are short, so the few starting just before t
    // are the only candidates.
    auto it = std::upper_bound(spans.begin(), spans.end(), t,
                               [](core::Timestamp v, const Span& s) { return v < s.first; });
    for (int back = 0; back < 4 && it != spans.begin(); ++back) {
        --it;
        if (t < it->second) {
            return true;
        }
    }
    return false;
}

}  // namespace

OutageData load_outages(const std::filesystem::path& data_dir, const Period& period,
                        const ReplayContext& context) {
    OutageData d;
    const core::Timestamp from = period.from - kReplayMargin;
    const core::Timestamp to = period.to + kReplayMargin;
    core::OutageTracker tracker{context.settings};
    const auto keep = [&](const core::Outage& o) {
        if (o.start < period.to && o.end.value_or(o.start) >= period.from) {
            d.outages.push_back(o);
        }
    };
    const auto observe = [&](core::Timestamp t, const core::RoundAssessment& a) {
        for (const auto& e : tracker.observe(t, a)) {
            if (e.kind == core::OutageEvent::Kind::ended) {
                keep(e.outage);
            }
        }
    };

    core::ConnectivityRound round;
    bool open = false;
    std::optional<core::Timestamp> last;  // the last round fed
    const auto flush = [&] {
        if (!open) {
            return;
        }
        // Slots the recorder left out during an official pause count as
        // inconclusive, as they did at the time.
        if (last && context.interval.count() > 0 && round.time - *last <= std::chrono::days{1}) {
            for (auto slot = *last + context.interval; slot < round.time; slot += context.interval) {
                if (inside(context.paused, slot)) {
                    observe(slot, {core::Reachability::inconclusive, {}, {}});
                }
            }
        }
        auto a = core::assess(round);
        const bool counted = period.contains(round.time);
        if (counted) {
            ++d.rounds;
        }
        if (a.reach == core::Reachability::down) {
            if (counted) {
                ++d.down_rounds;
            }
            if (inside(context.busy, round.time)) {
                a = {core::Reachability::inconclusive, {}, {}};
            }
        }
        observe(round.time, a);
        last = round.time;
        round = {};
        open = false;
    };

    const auto& schema = store::connectivity_schema();
    const Columns col{schema};
    const auto t = col["time"], pr = col["probe"], tg = col["target"], st = col["status"],
               er = col["error"], gw = col["gateway"], se = col["sent"], re = col["received"],
               rmin = col["rtt_min_ms"], ravg = col["rtt_avg_ms"], rmax = col["rtt_max_ms"],
               rdev = col["rtt_mdev_ms"], du = col["duration_ms"];
    const auto stats = read_if_there(
        data_dir / store::kConnectivityFile, schema,
        [&](const std::vector<std::string>& f) {
            const auto time = core::parse_iso8601(f[t]);
            const auto status = core::parse_status(f[st]);
            if (!time || !status || (f[pr] != "ping" && f[pr] != "dns")) {
                ++d.unreadable;
                return true;
            }
            if (*time >= to) {
                return false;
            }
            if (*time < from) {
                return true;
            }
            if (open && *time != round.time) {
                flush();
            }
            open = true;
            round.time = *time;
            if (f[pr] == "dns") {
                round.dns = core::DnsResult{.status = *status,
                                            .error = f[er],
                                            .host = f[tg],
                                            .duration_ms = number(f[du])};
                return true;
            }
            core::PingResult p;
            p.status = *status;
            p.error = f[er];
            p.target = f[tg];
            p.sent = integer<int>(f[se]);
            p.received = integer<int>(f[re]);
            p.rtt_min_ms = number(f[rmin]);
            p.rtt_avg_ms = number(f[ravg]);
            p.rtt_max_ms = number(f[rmax]);
            p.rtt_mdev_ms = number(f[rdev]);
            if (!f[gw].empty() && f[tg] == f[gw]) {
                round.gateway = std::move(p);
            } else {
                round.hosts.push_back(std::move(p));
            }
            return true;
        },
        d.file_exists);
    flush();
    if (const auto e = tracker.finish()) {
        keep(e->outage);
    }
    d.skipped = stats.skipped;
    return d;
}

}  // namespace meerkat::report
