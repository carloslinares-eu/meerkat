/**
 * @file recorder.hpp
 * @brief What `meerkat run` does in each scheduled round: measure, write
 *        the rows, log what matters.
 */
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "app/log.hpp"
#include "core/config.hpp"
#include "core/link_guard.hpp"
#include "core/measurement.hpp"
#include "core/outage.hpp"
#include "net/sense.hpp"
#include "store/csv.hpp"
#include "store/raw_log.hpp"

namespace meerkat::app {

/// The measuring functions the recorder uses; replaceable in tests.
struct RecorderProbes {
    net::LinkSensors sensors;                                     ///< Route, link, Wi-Fi details.
    std::function<core::PingResult(std::string_view)> ping;       ///< Pings one target.
    std::function<core::DnsResult(std::string_view)> dns;         ///< Times one lookup.
    std::function<core::SpeedResult()> speed;                     ///< Runs one speed test.
    std::function<core::Timestamp()> now = core::now;             ///< The wall clock.
};

/**
 * @brief The real probes for @p config: system sensors, `ping`, the
 *        resolver and the Ookla CLI.
 * @param config The settings (speed-test program and server).
 * @return The probes.
 */
RecorderProbes system_recorder_probes(const core::Config& config);

/**
 * @brief Measures and records, one round at a time.
 *
 * speed_round() and connectivity_round() may run at the same time on
 * different threads; each writes its own file. Write errors are logged and
 * the file is opened again on the next round, so one failed write doesn't
 * stop the recording.
 */
class Recorder {
public:
    /**
     * @brief Opens the data files in `config.data_dir`, which must exist.
     * @param config The settings.
     * @param probes How to measure.
     * @param log    Where to log; must outlive the recorder.
     * @throws store::SchemaMismatch if a CSV has another layout.
     * @throws std::system_error     if a file can't be opened, or another
     *                               meerkat is already writing it.
     */
    Recorder(core::Config config, RecorderProbes probes, Log& log);

    /**
     * @brief Start-up: warns about the connection right away (Wi-Fi,
     *        offline) and seals the previous day (see seal_previous_day()).
     */
    void startup();

    /// One speed test: the link guard decides, then a test or a skip row.
    void speed_round();

    /**
     * @brief One connectivity round: gateway and public pings, the DNS
     *        lookup, a row each; outages are logged as they start and end.
     *
     * A round that fails while a speed test is running doesn't count towards
     * an outage (a full line can drop pings); its rows are written as usual.
     */
    void connectivity_round();

    /**
     * @brief Writes yesterday's (UTC) manifest unless it exists.
     *
     * Only if meerkat recorded yesterday: this recorder was running before
     * midnight, or yesterday has a raw log. A first start doesn't seal a
     * day without data.
     */
    void seal_previous_day();

    /// Shutdown: ends an open outage (as `stopped`) and logs it.
    void finish();

    /// @return One line about the latest results, for the terminal.
    [[nodiscard]] std::string status_line() const;

private:
    /**
     * @brief Appends a row, opening the file again if an earlier write failed.
     * @param writer The writer (empty after a failure).
     * @param file   File name in the data directory.
     * @param schema Its schema.
     * @param row    The row.
     */
    void write(std::optional<store::CsvWriter>& writer, std::string_view file,
               const store::Schema& schema, const std::vector<std::string>& row);

    /**
     * @brief Logs an outage event.
     * @param e The event.
     */
    void log_outage(const core::OutageEvent& e);

    /// Pushes status_line() to the log.
    void update_status();

    core::Config config_;                          ///< The settings.
    RecorderProbes probes_;                        ///< How to measure.
    Log& log_;                                     ///< Where to log.
    core::Timestamp started_{};                    ///< When the recorder was made.

    std::mutex speed_mutex_;                       ///< One speed round at a time.
    std::optional<store::CsvWriter> speed_csv_;    ///< speed.csv; empty after a failure.
    store::RawLog raw_;                            ///< the daily raw log (`raw/`).
    core::LinkGuard guard_;                        ///< WLAN warning and skip decision.
    std::atomic<bool> speed_running_{false};       ///< A speed test is under way.

    std::mutex connectivity_mutex_;                ///< One connectivity round at a time.
    std::optional<store::CsvWriter> connectivity_csv_;  ///< connectivity.csv; empty after a failure.
    core::OutageTracker outages_;                  ///< Outage state machine.

    std::mutex seal_mutex_;                        ///< One seal at a time.

    mutable std::mutex status_mutex_;              ///< Guards the status texts.
    std::string speed_status_ = "no speed test yet";       ///< Latest speed result.
    std::string connectivity_status_ = "no probe yet";     ///< Latest connectivity result.
    std::string outage_status_ = "no outage";              ///< Current outage, if any.
};

}  // namespace meerkat::app
