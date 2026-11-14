/**
 * @file recorder.cpp
 * @brief Implementation of the run recorder.
 */
#include "app/recorder.hpp"

#include <chrono>
#include <exception>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "store/manifest.hpp"
#include "store/schema.hpp"

namespace meerkat::app {

namespace {

/**
 * @brief The longest one connectivity round can take: every ping and the
 *        DNS lookup running into its limit, plus a second for the rest.
 * @param c The configuration (hosts, gateway).
 * @param p The probes, with their limits.
 * @return The time, rounded up to whole seconds.
 */
std::chrono::seconds worst_round_time(const core::Config& c, const Probes& p) {
    const auto pings = static_cast<int>(c.ping_hosts.size()) + (c.ping_gateway ? 1 : 0);
    return std::chrono::ceil<std::chrono::seconds>(pings * p.ping_limit + p.dns_limit) +
           std::chrono::seconds{1};
}

/**
 * @brief A number with one decimal, or `–` if missing.
 * @param v The value.
 * @return The text.
 */
std::string one_decimal(std::optional<double> v) {
    const auto s = store::format_number(v, 1);
    return s.empty() ? "–" : s;
}

/**
 * @brief What an outage's scope means, for the log.
 * @param s The scope.
 * @return A short explanation.
 */
std::string scope_text(core::OutageScope s) {
    switch (s) {
    case core::OutageScope::internet: return "the line: the router answers, the internet doesn't";
    case core::OutageScope::local: return "local: no route, or the router doesn't answer";
    case core::OutageScope::unknown: break;
    }
    return "the router wasn't checked";
}

/// Sets a flag for the lifetime of the object.
class FlagGuard {
public:
    /**
     * @brief Sets @p flag.
     * @param flag The flag; cleared again by the destructor.
     */
    explicit FlagGuard(std::atomic<bool>& flag) : flag_{flag} { flag_.store(true); }
    /// Clears the flag.
    ~FlagGuard() { flag_.store(false); }
    FlagGuard(const FlagGuard&) = delete;             ///< Not copyable.
    FlagGuard& operator=(const FlagGuard&) = delete;  ///< Not copyable.

private:
    std::atomic<bool>& flag_;  ///< The flag.
};

}  // namespace

Recorder::Recorder(core::Config config, Probes probes, Log& log)
    : config_{std::move(config)},
      probes_{std::move(probes)},
      log_{log},
      speed_csv_{store::kSpeedFile, store::speed_schema(),
                 store::CsvWriter{config_.data_dir / store::kSpeedFile, store::speed_schema()}},
      raw_{config_.data_dir / store::kRawDir},
      guard_{config_.require_wired},
      connectivity_csv_{store::kConnectivityFile, store::connectivity_schema(),
                        store::CsvWriter{config_.data_dir / store::kConnectivityFile,
                                         store::connectivity_schema()}},
      outages_{core::OutageSettings::for_interval(config_.connectivity_interval,
                                                  config_.outage_after,
                                                  worst_round_time(config_, probes_))} {}

void Recorder::write(DataFile& file, const std::vector<std::string>& row) {
    const std::string name{file.name};
    try {
        if (!file.writer) {
            file.writer.emplace(config_.data_dir / file.name, file.schema);
        }
        file.writer->write_row(row);
    } catch (const std::exception& e) {
        file.writer.reset();  // opened again on the next row
        ++file.lost;
        if (!file.failing) {
            file.failing = true;
            log_.error("can't write " + name + ", rows are lost until it works again: " +
                       e.what());
        }
        return;
    }
    if (file.failing) {
        log_.notice(name + " is writable again; " + std::to_string(file.lost) + " row(s) lost");
        file.failing = false;
        file.lost = 0;
    }
}

void Recorder::startup() {
    {
        std::lock_guard lock{speed_mutex_};
        const auto check = guard_.check(net::sense_link(probes_.sensors));
        for (const auto& n : check.notices) {
            n.warning ? log_.warning(n.text) : log_.notice(n.text);
        }
    }
    seal_pending();
    update_status();
}

void Recorder::speed_round() {
    if (!probes_.speed) {
        return;
    }
    std::lock_guard lock{speed_mutex_};
    const auto check = guard_.check(net::sense_link(probes_.sensors));
    for (const auto& n : check.notices) {
        n.warning ? log_.warning(n.text) : log_.notice(n.text);
    }

    core::Measurement<core::SpeedResult> m;
    m.time = probes_.now();
    m.context = check.snapshot.context;
    if (check.skip) {
        m.result = *check.skip;
        log_.info("speed test skipped: " + m.result.error);
    } else {
        {
            const FlagGuard running{speed_running_};
            m.result = probes_.speed();
        }
        try {
            raw_.append(m);
        } catch (const std::exception& e) {
            log_.error(std::string{"can't write the raw log: "} + e.what());
        }
        if (m.result.status == core::Status::ok) {
            log_.info("speed test: " + one_decimal(m.result.download_mbps) + " down / " +
                      one_decimal(m.result.upload_mbps) + " up Mbit/s, latency " +
                      one_decimal(m.result.latency_ms) + " ms");
        } else {
            log_.warning("speed test failed: " +
                         core::status_text(m.result.status, m.result.error));
        }
    }
    write(speed_csv_, store::speed_row(m));

    {
        std::lock_guard s{status_mutex_};
        speed_status_ = "speed " + log_.clock(m.time) + " ";
        speed_status_ += m.result.status == core::Status::ok
                             ? one_decimal(m.result.download_mbps) + "/" +
                                   one_decimal(m.result.upload_mbps) + " Mbit/s"
                             : std::string{core::to_string(m.result.status)};
    }
    update_status();
}

void Recorder::connectivity_round() {
    std::lock_guard lock{connectivity_mutex_};
    const bool speed_before = speed_running_.load();
    net::LinkSensors sensors = probes_.sensors;
    sensors.wifi = nullptr;  // no `iw` every 30 s; the speed rows have the Wi-Fi details
    const auto snapshot = net::sense_link(sensors);

    core::ConnectivityRound round;
    round.time = probes_.now();
    const auto& ctx = snapshot.context;
    std::vector<std::vector<std::string>> rows;
    const auto probe_ping = [&](const std::string& target) {
        auto r = probes_.ping(target);
        r.status = snapshot.probe_status(r.status);
        rows.push_back(store::ping_row({round.time, ctx, r}));
        return r;
    };
    if (config_.ping_gateway && !ctx.gateway.empty()) {
        round.gateway = probe_ping(ctx.gateway);
    }
    for (const auto& host : config_.ping_hosts) {
        round.hosts.push_back(probe_ping(host));
    }
    round.dns = probes_.dns(config_.dns_host);
    round.dns->status = snapshot.probe_status(round.dns->status);
    rows.push_back(store::dns_row({round.time, ctx, *round.dns}));
    const bool during_speed_test = speed_before || speed_running_.load();

    for (const auto& row : rows) {
        write(connectivity_csv_, row);
    }

    auto a = core::assess(round);
    if (a.reach == core::Reachability::down && during_speed_test) {
        a.reach = core::Reachability::inconclusive;  // a saturated line can drop pings
    }
    for (const auto& e : outages_.observe(round.time, a)) {
        log_outage(e);
    }

    {
        std::lock_guard s{status_mutex_};
        std::string reach;
        switch (a.reach) {
        case core::Reachability::up: reach = "up"; break;
        case core::Reachability::down: reach = "DOWN"; break;
        case core::Reachability::unknown: reach = "unknown"; break;
        case core::Reachability::inconclusive: reach = "busy (speed test)"; break;
        }
        connectivity_status_ = "net " + log_.clock(round.time) + " " + reach;
        const auto& open = outages_.current();
        outage_status_ = open ? "OUTAGE since " + log_.clock(open->start) : "no outage";
    }
    update_status();
}

std::optional<std::chrono::sys_days> Recorder::first_data_day() const {
    std::optional<std::chrono::sys_days> first;
    const auto consider = [&first](std::string_view day) {
        if (const auto d = store::parse_day(day); d && (!first || *d < *first)) {
            first = d;
        }
    };
    for (const auto file : {store::kSpeedFile, store::kConnectivityFile}) {
        // Line 1 is the version, line 2 the columns, line 3 the first row.
        std::ifstream in{config_.data_dir / file};
        std::string line;
        for (int i = 0; i < 3 && std::getline(in, line); ++i) {
        }
        if (in && line.size() >= 10) {
            consider(std::string_view{line}.substr(0, 10));
        }
    }
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator{config_.data_dir / store::kRawDir, ec}) {
        if (const auto day = store::raw_log_day(e.path().filename().string())) {
            consider(*day);
        }
    }
    return first;
}

void Recorder::seal_pending() {
    using std::chrono::days;
    std::lock_guard lock{seal_mutex_};
    const auto now = probes_.now();
    const auto today = std::chrono::floor<days>(now);
    const auto first = first_data_day();
    if (!first) {
        return;  // nothing recorded yet
    }

    // The newest existing manifest, by the same rules as the chain uses; the
    // days after it are still open.
    std::optional<std::chrono::sys_days> last_sealed;
    try {
        if (const auto day = store::latest_manifest_day(config_.data_dir)) {
            last_sealed = store::parse_day(*day);
        }
    } catch (const std::exception& e) {
        log_.error(std::string{"can't list the manifests: "} + e.what());
        return;  // try again next time
    }
    auto start = last_sealed ? std::max(*last_sealed + days{1}, *first) : *first;
    constexpr days kMaxBacklog{31};
    if (today - start > kMaxBacklog) {
        log_.warning("days " + store::utc_day(core::Timestamp{start}) + " to " +
                     store::utc_day(core::Timestamp{today - kMaxBacklog - days{1}}) +
                     " have no manifest; only the last 31 days are sealed now");
        start = today - kMaxBacklog;
    }

    for (auto d = start; d < today; d += days{1}) {
        const auto day = store::utc_day(core::Timestamp{d});
        try {
            const auto m =
                store::build_manifest(config_.data_dir, day, now, store::daily_files(day));
            const auto path = store::write_manifest(config_.data_dir, m);
            log_.notice("sealed " + day + ": " + path.string() + " (" +
                        std::to_string(m.entries.size()) + " files)");
        } catch (const std::system_error& e) {
            if (e.code() != std::errc::file_exists) {
                log_.error("can't write the manifest for " + day + ": " + e.what());
                return;  // the chain would skip it; try again next time
            }
        } catch (const std::exception& e) {
            log_.error("can't write the manifest for " + day + ": " + e.what());
            return;
        }
    }
}

void Recorder::finish() {
    std::lock_guard lock{connectivity_mutex_};
    if (const auto e = outages_.finish()) {
        log_outage(*e);
    }
}

void Recorder::log_outage(const core::OutageEvent& e) {
    const auto& o = e.outage;
    if (e.kind == core::OutageEvent::Kind::started) {
        log_.warning("outage since " + core::to_iso8601(o.start) + " (" + scope_text(o.scope) +
                     "): " + o.reason);
        return;
    }
    std::string how;
    switch (o.end_reason) {
    case core::OutageEnd::recovered: how = "the connection is back"; break;
    case core::OutageEnd::gap:
        how = o.unconfirmed_end ? "the connection answered again, then meerkat wasn't measuring "
                                  "for a while, so that wasn't confirmed"
                                : "end unknown: meerkat wasn't measuring for a while";
        break;
    case core::OutageEnd::stopped:
        how = o.unconfirmed_end ? "the connection answered again; meerkat is stopping before "
                                  "that was confirmed"
                                : "end unknown: meerkat is stopping";
        break;
    }
    log_.notice("outage from " + core::to_iso8601(o.start) + " to " + core::to_iso8601(*o.end) +
                " (" + core::format_duration(o.duration().value_or(std::chrono::seconds{0})) +
                ", " + std::to_string(o.down_rounds) + " failed rounds, " +
                core::to_string(o.scope) + "): " + how);
}

std::string Recorder::status_line() const {
    std::lock_guard s{status_mutex_};
    return speed_status_ + " | " + connectivity_status_ + " | " + outage_status_;
}

void Recorder::update_status() { log_.status(status_line()); }

}  // namespace meerkat::app
