/**
 * @file recorder.hpp
 * @brief What `meerkat run` does in each scheduled round: measure, write
 *        the rows, log what matters.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "app/log.hpp"
#include "app/once.hpp"
#include "core/config.hpp"
#include "core/link_guard.hpp"
#include "core/measurement.hpp"
#include "core/outage.hpp"
#include "net/sense.hpp"
#include "store/csv.hpp"
#include "store/raw_log.hpp"

namespace meerkat::app {

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
    Recorder(core::Config config, Probes probes, Log& log);

    /**
     * @brief Start-up: warns about the connection right away (Wi-Fi,
     *        offline) and seals the days still unsealed (see seal_pending()).
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
     * @brief Writes the manifests of the past days (UTC) that don't have one.
     *
     * The days run from the day after the latest manifest (or, without one,
     * from the first day with data in the CSVs or `raw/`) to yesterday, in
     * order, so each manifest chains to the one before. Days meerkat was off
     * or the computer suspended at midnight are caught up this way. At most
     * the last 31 days are sealed at once (a warning names the rest).
     */
    void seal_pending();

    /// Shutdown: ends an open outage (as `stopped`) and logs it.
    void finish();

    /// @return One line about the latest results, for the terminal.
    [[nodiscard]] std::string status_line() const;

private:
    /// One CSV file and whether writing to it currently fails.
    struct DataFile {
        std::string_view name;                  ///< File name in the data directory.
        const store::Schema& schema;            ///< Its layout.
        std::optional<store::CsvWriter> writer; ///< Open writer; empty after a failure.
        bool failing = false;                   ///< The last write failed.
        std::uint64_t lost = 0;                 ///< Rows lost since it started failing.
    };

    /**
     * @brief Appends a row, opening the file again if an earlier write failed.
     *
     * The first failure is logged as an error; later ones are only counted
     * until a write works again, which is logged with the number of rows lost.
     * @param file The file.
     * @param row  The row.
     */
    void write(DataFile& file, const std::vector<std::string>& row);

    /**
     * @brief The first day (UTC) with data: the first row of either CSV, or
     *        the oldest raw log.
     * @return The day, or nothing if there's no data yet.
     */
    std::optional<std::chrono::sys_days> first_data_day() const;

    /**
     * @brief Logs an outage event.
     * @param e The event.
     */
    void log_outage(const core::OutageEvent& e);

    /// Pushes status_line() to the log.
    void update_status();

    core::Config config_;                          ///< The settings.
    Probes probes_;                                ///< How to measure.
    Log& log_;                                     ///< Where to log.

    std::mutex speed_mutex_;                       ///< One speed round at a time.
    DataFile speed_csv_;                           ///< speed.csv.
    store::RawLog raw_;                            ///< the daily raw log (`raw/`).
    core::LinkGuard guard_;                        ///< WLAN warning and skip decision.
    std::atomic<bool> speed_running_{false};       ///< A speed test is under way.

    std::mutex connectivity_mutex_;                ///< One connectivity round at a time.
    DataFile connectivity_csv_;                    ///< connectivity.csv.
    core::OutageTracker outages_;                  ///< Outage state machine.

    std::mutex seal_mutex_;                        ///< One seal at a time.

    mutable std::mutex status_mutex_;              ///< Guards the status texts.
    std::string speed_status_ = "no speed test yet";       ///< Latest speed result.
    std::string connectivity_status_ = "no probe yet";     ///< Latest connectivity result.
    std::string outage_status_ = "no outage";              ///< Current outage, if any.
};

}  // namespace meerkat::app
