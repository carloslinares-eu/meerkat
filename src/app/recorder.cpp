/**
 * @file recorder.cpp
 * @brief Implementation of the run recorder.
 */
#include "app/recorder.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#include "probe/dns.hpp"
#include "probe/ookla.hpp"
#include "probe/ping.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"

namespace meerkat::app {

namespace {

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
 * @brief Time of day of @p t, `HH:MM:SS` (UTC).
 * @param t The time.
 * @return The text.
 */
std::string clock_time(core::Timestamp t) {
    const auto iso = core::to_iso8601(t);  // YYYY-MM-DDTHH:MM:SSZ
    return iso.size() >= 19 ? iso.substr(11, 8) : iso;
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

RecorderProbes system_recorder_probes(const core::Config& config) {
    RecorderProbes p;
    p.sensors = net::system_link_sensors();
    p.ping = [](std::string_view t) { return probe::ping(t); };
    p.dns = [](std::string_view h) { return probe::resolve(h); };
    auto backend = std::make_shared<probe::OoklaBackend>(
        probe::OoklaOptions{.program = config.speedtest, .server_id = config.server_id});
    p.speed = [backend] { return backend->measure(); };
    return p;
}

Recorder::Recorder(core::Config config, RecorderProbes probes, Log& log)
    : config_{std::move(config)},
      probes_{std::move(probes)},
      log_{log},
      started_{probes_.now()},
      speed_csv_{std::in_place, config_.data_dir / store::kSpeedFile, store::speed_schema()},
      raw_{config_.data_dir / store::kRawDir},
      guard_{config_.require_wired},
      connectivity_csv_{std::in_place, config_.data_dir / store::kConnectivityFile,
                        store::connectivity_schema()},
      outages_{core::OutageSettings::for_interval(config_.connectivity_interval,
                                                  config_.outage_after)} {}

void Recorder::write(std::optional<store::CsvWriter>& writer, std::string_view file,
                     const store::Schema& schema, const std::vector<std::string>& row) {
    try {
        if (!writer) {
            writer.emplace(config_.data_dir / file, schema);
            log_.notice(std::string{file} + " is writable again");
        }
        writer->write_row(row);
    } catch (const std::exception& e) {
        writer.reset();  // reopened on the next round
        log_.error("can't write " + std::string{file} + ", this row is lost: " + e.what());
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
    seal_previous_day();
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
    write(speed_csv_, store::kSpeedFile, store::speed_schema(), store::speed_row(m));

    {
        std::lock_guard s{status_mutex_};
        speed_status_ = "speed " + clock_time(m.time) + " ";
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
    const auto record_ping = [&](const core::PingResult& r) {
        rows.push_back(store::ping_row({round.time, ctx, r}));
    };
    if (config_.ping_gateway && !ctx.gateway.empty()) {
        round.gateway = probes_.ping(ctx.gateway);
        record_ping(*round.gateway);
    }
    for (const auto& host : config_.ping_hosts) {
        round.hosts.push_back(probes_.ping(host));
        record_ping(round.hosts.back());
    }
    round.dns = probes_.dns(config_.dns_host);
    rows.push_back(store::dns_row({round.time, ctx, *round.dns}));
    const bool during_speed_test = speed_before || speed_running_.load();

    for (const auto& row : rows) {
        write(connectivity_csv_, store::kConnectivityFile, store::connectivity_schema(), row);
    }

    auto a = core::assess(round);
    if (a.reach == core::Reachability::down && during_speed_test) {
        a.reach = core::Reachability::unknown;  // a saturated line can drop pings
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
        }
        connectivity_status_ = "net " + clock_time(round.time) + " " + reach;
        const auto& open = outages_.current();
        outage_status_ = open ? "OUTAGE since " + clock_time(open->start) : "no outage";
    }
    update_status();
}

void Recorder::seal_previous_day() {
    std::lock_guard lock{seal_mutex_};
    const auto now = probes_.now();
    const auto today_start = std::chrono::floor<std::chrono::days>(now);
    const auto day = store::utc_day(core::Timestamp{today_start - std::chrono::days{1}});
    const bool ran_yesterday = started_ < today_start;
    std::error_code ec;
    const bool has_raw = std::filesystem::exists(raw_.file_for(day), ec);
    if (!ran_yesterday && !has_raw) {
        return;
    }
    try {
        const auto m = store::build_manifest(config_.data_dir, day, now, store::daily_files(day));
        const auto path = store::write_manifest(config_.data_dir, m);
        log_.notice("sealed " + day + ": " + path.string() + " (" +
                    std::to_string(m.entries.size()) + " files)");
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::file_exists) {
            log_.error("can't write the manifest for " + day + ": " + e.what());
        }
    } catch (const std::exception& e) {
        log_.error("can't write the manifest for " + day + ": " + e.what());
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
    case core::OutageEnd::gap: how = "end unknown: meerkat wasn't measuring for a while"; break;
    case core::OutageEnd::stopped: how = "end unknown: meerkat is stopping"; break;
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
