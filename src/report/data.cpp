/**
 * @file data.cpp
 * @brief Implementation of the report's data loading.
 */
#include "report/data.hpp"

#include "report/official.hpp"
#include "store/official.hpp"

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

/// Rows of file time past the end after which the rest of a file isn't
/// read: by then they aren't a clock that ran ahead for a while (#96).
constexpr std::chrono::hours kPastEndStop{24};

/**
 * @brief Watches the time order of a file's rows: counts the jumps back in
 *        time near a window, and says when the rows have been past its end
 *        long enough to stop reading.
 */
class TimeOrder {
public:
    /**
     * @brief Starts watching.
     * @param from The window's start.
     * @param to   The window's end (exclusive).
     */
    TimeOrder(core::Timestamp from, core::Timestamp to) : from_{from}, to_{to} {}

    /**
     * @brief Takes the next row's time.
     * @param t The row's time.
     * @return True if it is earlier than the row before (the clock went
     *         back); counted if either time lies in the window.
     */
    bool step(core::Timestamp t) {
        const bool back = previous_ && t < *previous_;
        if (back && (near(t) || near(*previous_))) {
            ++count_;
        }
        if (t >= to_) {
            if (!past_since_) {
                past_since_ = t;
            }
            done_ = t - *past_since_ > kPastEndStop;
        } else {
            past_since_.reset();
        }
        previous_ = t;
        return back;
    }

    /// @return Whether the rows have run kPastEndStop past the end.
    [[nodiscard]] bool done() const { return done_; }

    /// @return The jumps back counted.
    [[nodiscard]] std::uint64_t count() const { return count_; }

private:
    [[nodiscard]] bool near(core::Timestamp t) const { return t >= from_ && t < to_; }

    core::Timestamp from_;
    core::Timestamp to_;
    std::optional<core::Timestamp> previous_;
    std::optional<core::Timestamp> past_since_;
    std::uint64_t count_ = 0;
    bool done_ = false;
};

}  // namespace

OfficialData load_official(const std::filesystem::path& data_dir) {
    OfficialData d;
    const auto& schema = store::official_schema();
    const Columns col{schema};
    const auto t = col["time"], nr = col["number"], dn = col["download_mbps"], up = col["upload_mbps"],
               la = col["latency_ms"], dmin = col["contract_down_min"], dnor = col["contract_down_normal"],
               dmax = col["contract_down_max"], umin = col["contract_up_min"], unor = col["contract_up_normal"],
               umax = col["contract_up_max"], cn = col["connection"], pw = col["on_power"],
               nic = col["nic_sufficient"], lim = col["data_rate_limited"], id = col["test_id"],
               ver = col["app_version"], file = col["file"];
    const auto yes_no = [](const std::string& s) -> std::optional<bool> {
        if (s == "yes" || s == "no") {
            return s == "yes";
        }
        return std::nullopt;
    };
    const auto stats = read_if_there(
        data_dir / store::kOfficialFile, schema,
        [&](const std::vector<std::string>& f) {
            const auto time = core::parse_iso8601(f[t]);
            if (!time) {
                ++d.unreadable;
                return true;
            }
            OfficialRow r;
            auto& m = r.m;
            m.time = *time;
            m.number = integer<int>(f[nr]);
            m.download_mbps = number(f[dn]);
            m.upload_mbps = number(f[up]);
            m.latency_ms = number(f[la]);
            m.contract_down_min = number(f[dmin]);
            m.contract_down_normal = number(f[dnor]);
            m.contract_down_max = number(f[dmax]);
            m.contract_up_min = number(f[umin]);
            m.contract_up_normal = number(f[unor]);
            m.contract_up_max = number(f[umax]);
            m.connection = f[cn];
            m.on_power = yes_no(f[pw]);
            m.nic_sufficient = yes_no(f[nic]);
            m.data_rate_limited = yes_no(f[lim]);
            m.test_id = f[id];
            m.app_version = f[ver];
            r.file = f[file];
            d.rows.push_back(std::move(r));
            return true;
        },
        d.file_exists);
    d.skipped = stats.skipped;
    std::stable_sort(d.rows.begin(), d.rows.end(),
                     [](const OfficialRow& a, const OfficialRow& b) { return a.m.time < b.m.time; });
    return d;
}

SpeedData load_speed(const std::filesystem::path& data_dir, const Period& period) {
    SpeedData d;
    const auto& schema = store::speed_schema();
    const Columns col{schema};
    const auto t = col["time"], st = col["status"], er = col["error"], lt = col["link_type"],
               ifc = col["interface"], ls = col["link_speed_mbps"], dn = col["download_mbps"],
               up = col["upload_mbps"], la = col["latency_ms"], ji = col["jitter_ms"],
               pl = col["packet_loss_percent"], si = col["server_id"], sn = col["server_name"],
               sl = col["server_location"];
    TimeOrder order{period.from, period.to};
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
            // Not the first row past the end: a row stamped ahead (a clock
            // corrected at boot) mustn't hide the ones after it (#96).
            order.step(*time);
            if (order.done()) {
                return false;
            }
            if (!period.contains(*time)) {
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
    d.out_of_order = order.count();
    if (d.out_of_order > 0) {
        std::stable_sort(d.rows.begin(), d.rows.end(),
                         [](const SpeedRow& a, const SpeedRow& b) { return a.time < b.time; });
    }
    return d;
}

namespace {

/**
 * @brief Sorts @p spans and merges the ones that overlap or touch.
 * @param spans Any spans.
 * @return Disjoint spans in time order.
 */
std::vector<Span> merged(std::vector<Span> spans) {
    std::sort(spans.begin(), spans.end());
    std::vector<Span> out;
    for (const auto& s : spans) {
        if (!out.empty() && s.first <= out.back().second) {
            out.back().second = std::max(out.back().second, s.second);
        } else {
            out.push_back(s);
        }
    }
    return out;
}

}  // namespace

ReplayContext replay_context(const std::vector<SpeedRow>& rows, std::chrono::seconds round_time,
                             std::chrono::seconds speed_interval,
                             std::chrono::seconds connectivity_interval, int outage_after) {
    ReplayContext c;
    c.settings = core::OutageSettings::for_interval(connectivity_interval, outage_after, round_time);
    c.interval = connectivity_interval;
    std::optional<Span> pause;
    for (const auto& r : rows) {
        // A refused test did reach the service; one skipped while waiting
        // for its limit ("not started: …") sent nothing (#126).
        const bool refused_run = r.status == core::Status::rate_limited && !r.error.starts_with("not started");
        switch (refused_run ? core::Status::error : r.status) {
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
 * @param spans Disjoint, in time order (merged()).
 * @param t     The time.
 * @return True if some span has `from <= t < to`.
 */
bool inside(const std::vector<Span>& spans, core::Timestamp t) {
    const auto it = std::upper_bound(spans.begin(), spans.end(), t,
                                     [](core::Timestamp v, const Span& s) { return v < s.first; });
    return it != spans.begin() && t < std::prev(it)->second;
}

}  // namespace

OutageData load_outages(const std::filesystem::path& data_dir, const Period& period,
                        const ReplayContext& context) {
    OutageData d;
    // Disjoint, so only the last span starting at or before a time can hold
    // it, however long the spans are (#96).
    const auto busy = merged(context.busy);
    const auto paused = merged(context.paused);
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

    std::optional<core::Timestamp> last;  // the last round fed
    const auto replay = [&](core::ConnectivityRound& round) {
        // Slots the recorder left out during an official pause count as
        // inconclusive, as they did at the time.
        if (last && context.interval.count() > 0 && round.time - *last <= std::chrono::days{1}) {
            for (auto slot = *last + context.interval; slot < round.time; slot += context.interval) {
                if (inside(paused, slot)) {
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
            if (inside(busy, round.time)) {
                a = {core::Reachability::inconclusive, {}, {}};
            }
        }
        observe(round.time, a);
        last = round.time;
    };

    // Rows of a round are written together, the DNS lookup last.
    core::ConnectivityRound round;
    bool open = false;
    const auto close = [&] {
        if (open) {
            replay(round);
            round = {};
            open = false;
        }
    };
    // The clock went back: the rounds before and after don't follow each
    // other, so the replay starts afresh instead of joining them (#96).
    const auto restart = [&] {
        if (const auto e = tracker.finish()) {
            keep(e->outage);
        }
        tracker = core::OutageTracker{context.settings};
        last.reset();
    };
    TimeOrder order{from, to};

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
            order.step(*time);
            if (order.done()) {
                return false;
            }
            if (*time >= to || *time < from) {
                return true;
            }
            if (open && *time != round.time) {
                close();
            }
            if (!open && last && *time <= *last) {
                restart();
            }
            open = true;
            round.time = *time;
            if (f[pr] == "dns") {
                round.dns = core::DnsResult{.status = *status,
                                            .error = f[er],
                                            .host = f[tg],
                                            .duration_ms = number(f[du])};
                close();  // the round's last row: a round with the same time is another
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
    close();
    d.out_of_order = order.count();
    if (const auto e = tracker.finish()) {
        keep(e->outage);
    }
    d.skipped = stats.skipped;
    return d;
}

}  // namespace meerkat::report
