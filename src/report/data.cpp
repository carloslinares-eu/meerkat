/**
 * @file data.cpp
 * @brief Implementation of the report's data loading.
 */
#include "report/data.hpp"

#include <algorithm>
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
        return static_cast<std::size_t>(
            std::find(names_.begin(), names_.end(), name) - names_.begin());
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
                return;
            }
            if (!period.contains(*time)) {
                return;
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
        },
        d.file_exists);
    d.skipped = stats.skipped;
    return d;
}

OutageData load_outages(const std::filesystem::path& data_dir, const Period& period,
                        const core::OutageSettings& settings,
                        const std::vector<core::Timestamp>& speed_starts) {
    OutageData d;
    core::OutageTracker tracker{settings};
    const auto keep = [&d](const std::vector<core::OutageEvent>& events) {
        for (const auto& e : events) {
            if (e.kind == core::OutageEvent::Kind::ended) {
                d.outages.push_back(e.outage);
            }
        }
    };
    const auto shadowed = [&speed_starts](core::Timestamp t) {
        // The latest speed test that started at or before t.
        auto it = std::upper_bound(speed_starts.begin(), speed_starts.end(), t);
        return it != speed_starts.begin() && t - *std::prev(it) < kSpeedTestShadow;
    };

    core::ConnectivityRound round;
    bool open = false;
    const auto flush = [&] {
        if (!open) {
            return;
        }
        auto a = core::assess(round);
        ++d.rounds;
        if (a.reach == core::Reachability::down) {
            ++d.down_rounds;
            if (shadowed(round.time)) {
                a = {core::Reachability::inconclusive, {}, {}};
            }
        }
        keep(tracker.observe(round.time, a));
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
                return;
            }
            if (!period.contains(*time)) {
                return;
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
                return;
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
        },
        d.file_exists);
    flush();
    if (const auto e = tracker.finish()) {
        d.outages.push_back(e->outage);
    }
    d.skipped = stats.skipped;
    return d;
}

}  // namespace meerkat::report
