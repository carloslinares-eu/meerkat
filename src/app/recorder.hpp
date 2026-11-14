/**
 * @file recorder.hpp
 * @brief What `meerkat run` does in each scheduled round: measure, write
 *        the rows, log what matters.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "app/log.hpp"
#include "app/once.hpp"
#include "core/config.hpp"
#include "core/link_guard.hpp"
#include "core/measurement.hpp"
#include "core/official_guard.hpp"
#include "core/outage.hpp"
#include "net/sense.hpp"
#include "store/csv.hpp"
#include "store/dir_lock.hpp"
#include "store/raw_log.hpp"

namespace meerkat::app {

/**
 * @brief Another meerkat is already recording into the data directory: it
 *        holds the directory's store::DirectoryLock.
 */
class DataDirBusy : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;  ///< Takes the message.
};

/// How long lock_data_dir() keeps trying while someone else holds the lock:
/// long enough for a `meerkat check` testing it, which lets go at once, and
/// short enough that a second `meerkat run` stops without a noticeable wait.
inline constexpr std::chrono::milliseconds kDataDirPatience{100};

/**
 * @brief Locks @p data_dir for recording: one meerkat per data directory.
 *
 * Recorder takes its lock this way, and `meerkat check` tests it the same
 * way, so both judge the directory alike.
 * @param data_dir The data directory; it must exist.
 * @param warn     Gets a warning if the file system can't lock it (NFS, for
 *                 example); the recording then relies on the per-file locks
 *                 of the data files alone.
 * @return The lock: held, or unsupported after @p warn.
 * @throws DataDirBusy       if another meerkat holds it for longer than
 *                           kDataDirPatience.
 * @throws std::system_error if @p data_dir can't be opened.
 */
store::DirectoryLock lock_data_dir(const std::filesystem::path& data_dir,
                                   const std::function<void(const std::string&)>& warn);

/**
 * @brief Measures and records, one round at a time.
 *
 * speed_round() and connectivity_round() may run at the same time on
 * different threads; each writes its own file. Write errors are logged and
 * the file is opened again on the next round, so one failed write doesn't
 * stop the recording. The data directory itself stays locked meanwhile
 * (store::DirectoryLock), so no second meerkat can start writing into it.
 */
class Recorder {
public:
    /**
     * @brief Opens the data files in `config.data_dir`, which must exist.
     * @param config The settings.
     * @param probes How to measure.
     * @param log    Where to log; must outlive the recorder.
     * @throws store::SchemaMismatch if a CSV has another layout.
     * @throws DataDirBusy           if another meerkat is already recording
     *                               into `config.data_dir`.
     * @throws std::system_error     if the data directory or a data file
     *                               can't be opened or locked.
     */
    Recorder(core::Config config, Probes probes, Log& log);

    /**
     * @brief Start-up: warns about the connection right away (Wi-Fi,
     *        offline) and seals the days still unsealed (see seal_pending()).
     */
    void startup();

    /**
     * @brief One speed test: the official guard and the link guard decide,
     *        then a test or a skip row (`paused_official`, `no_network`,
     *        `skipped_wlan`).
     */
    void speed_round();

    /**
     * @brief One connectivity round: gateway and public pings, the DNS
     *        lookup, a row each; outages are logged as they start and end.
     *
     * A round that fails while a speed test is running doesn't count towards
     * an outage (a full line can drop pings); its rows are written as usual.
     * While speed tests pause for an official measurement, rounds run only
     * every `official_connectivity_interval`; the rounds in between are left
     * out without breaking an outage (they count as inconclusive). The
     * rounds that do run still detect outages: the app is idle most of the
     * time it is open, and one official test (a minute or two) is shorter
     * than `outage_after` slow rounds.
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

    /**
     * @brief Logs the link guard's notices: warnings as warnings, good news
     *        as notices.
     * @param notices From core::LinkGuard::check().
     */
    void log_link_notices(const std::vector<core::LinkNotice>& notices);

    /**
     * @brief Asks the official guard whether to stand back now, and logs
     *        when that changes.
     * @return Why speed tests pause; nothing if they may run.
     */
    std::optional<std::string> official_pause();

    core::Config config_;                          ///< The settings.
    /// The data directory, locked for the recorder's whole life: one
    /// meerkat per data directory, even while a CSV is closed after a write
    /// error. Declared before the data files, so it's taken first.
    store::DirectoryLock run_lock_;
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

    std::mutex official_mutex_;                    ///< Guards official_.
    core::OfficialGuard official_;                 ///< Pause for official measurements.
    std::string last_pause_error_;                 ///< Why the pause file was last unusable; logged once.
    /// Time of the last connectivity round probed during a pause (only
    /// connectivity_round() uses it, under connectivity_mutex_).
    std::optional<core::Timestamp> last_paused_probe_;

    mutable std::mutex status_mutex_;              ///< Guards the status texts.
    std::string speed_status_ = "no speed test yet";       ///< Latest speed result.
    std::string connectivity_status_ = "no probe yet";     ///< Latest connectivity result.
    std::string outage_status_ = "no outage";              ///< Current outage, if any.
};

}  // namespace meerkat::app
