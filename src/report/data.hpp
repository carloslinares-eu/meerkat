/**
 * @file data.hpp
 * @brief Loads what `meerkat report` works on from the data directory:
 *        the speed tests of a period, and the outages rebuilt from the
 *        connectivity probes.
 *
 * @namespace meerkat::report
 * @brief `meerkat report`: loading the recorded data, analysing it against
 *        the contract and the BNetzA rules, and writing the report.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/outage.hpp"
#include "core/status.hpp"
#include "core/time.hpp"

namespace meerkat::report {

/// A half-open time range `[from, to)` (UTC).
struct Period {
    core::Timestamp from{};  ///< First moment included.
    core::Timestamp to{};    ///< First moment no longer included.

    /**
     * @brief Whether @p t lies in the period.
     * @param t The time.
     * @return True if `from <= t < to`.
     */
    [[nodiscard]] bool contains(core::Timestamp t) const { return from <= t && t < to; }
};

/// One row of `speed.csv`, with what the report uses.
struct SpeedRow {
    core::Timestamp time{};                      ///< Start of the test (UTC).
    core::Status status = core::Status::error;   ///< How it ended.
    std::string error;                           ///< Why, if not `ok`.
    core::LinkType link_type = core::LinkType::unknown;  ///< Cable, Wi-Fi, tunnel or unknown.
    std::string interface;                       ///< Interface it ran over.
    std::optional<double> link_speed_mbps;       ///< Negotiated Ethernet speed.
    std::optional<double> download_mbps;         ///< Download.
    std::optional<double> upload_mbps;           ///< Upload.
    std::optional<double> latency_ms;            ///< Idle latency.
    std::optional<double> jitter_ms;             ///< Idle jitter.
    std::optional<double> packet_loss_percent;   ///< Packet loss.
    std::optional<std::int64_t> server_id;       ///< Test server.
    std::string server_name;                     ///< Test server's operator / name.
    std::string server_location;                 ///< Test server's city.

    /// @return Whether it counts as evidence: completed (`ok`) over a cable.
    [[nodiscard]] bool counts() const { return status == core::Status::ok && link_type == core::LinkType::wired; }
};

/// The speed tests of a period.
struct SpeedData {
    std::vector<SpeedRow> rows;    ///< In time order (sorted; ties in file order).
    std::uint64_t skipped = 0;     ///< Torn rows (see store::CsvReadStats::skipped).
    /// Rows older than the row before them in the file, where either lies
    /// in the period: the clock went back (or was ahead before). `rows` are
    /// sorted then; reported.
    std::uint64_t out_of_order = 0;
    /// Complete rows whose time, status or link type can't be read: never
    /// written by meerkat, so the file was edited. Reported, not used.
    std::uint64_t unreadable = 0;
    bool file_exists = false;      ///< speed.csv was there.
};

/**
 * @brief Reads the speed tests in @p period from `speed.csv`. Reading
 *        stops only once the rows have been a day past the period, so a
 *        row stamped ahead doesn't hide the ones after it.
 * @param data_dir The data directory.
 * @param period   Which tests.
 * @return The tests; empty (`file_exists` false) without a speed.csv.
 * @throws store::SchemaMismatch, std::system_error, sys::NotRegularFile as
 *         store::read_csv() (except for a missing file).
 */
SpeedData load_speed(const std::filesystem::path& data_dir, const Period& period);

/// How long a speed test that ended normally (`ok`, `error`) may have
/// kept the line busy: an Ookla test takes about 30 s; 90 s leaves room
/// for a slow one. The CSV has the start only.
inline constexpr std::chrono::seconds kSpeedTestBusy{90};

/// How long a test that ran into its time limit (`timeout`) kept the line
/// busy: the Ookla CLI's limit plus the SIGTERM → SIGKILL grace.
inline constexpr std::chrono::seconds kTimedOutTestBusy{122};

/// Rounds this long before the period (and after it) are replayed too, so
/// an outage across its edges keeps its real start and end.
inline constexpr std::chrono::hours kReplayMargin{24};

/// A time span `[from, to)`.
using Span = std::pair<core::Timestamp, core::Timestamp>;

/// What the replay of connectivity.csv needs besides the rows: what
/// `meerkat run` knew at the time but the CSV doesn't say.
struct ReplayContext {
    core::OutageSettings settings;              ///< Outage rules (as `meerkat run` used).
    std::chrono::seconds interval{30};          ///< Time between connectivity rounds.
    /// When speed tests kept the line busy (any order, may overlap): a
    /// failing round overlapping one is inconclusive, as `meerkat run`
    /// judged it.
    std::vector<Span> busy;
    /// When speed tests paused for an official measurement (any order, may
    /// overlap): the
    /// recorder probed less often then and counted the slots it left out
    /// as inconclusive, so they bridge instead of ending an outage.
    std::vector<Span> paused;
};

/**
 * @brief The busy and paused spans from the speed tests.
 *
 * Busy: each test that ran (`ok`, `error`, `timeout`) from @p round_time
 * before its start (a round started then still runs into it) to
 * kSpeedTestBusy after it (kTimedOutTestBusy for `timeout`). Paused: each
 * run of consecutive `paused_official` rows, widened by @p speed_interval
 * on both sides (the pause began and ended somewhere in between).
 * @param rows                  The speed tests, in time order.
 * @param round_time            The longest a connectivity round takes.
 * @param speed_interval        Time between speed tests.
 * @param connectivity_interval Time between connectivity rounds.
 * @param outage_after          Down rounds in a row that make an outage.
 * @return The complete context: the spans, and the outage rules as
 *         `meerkat run` derived them (core::OutageSettings::for_interval()).
 */
ReplayContext replay_context(const std::vector<SpeedRow>& rows, std::chrono::seconds round_time,
                             std::chrono::seconds speed_interval,
                             std::chrono::seconds connectivity_interval, int outage_after);

/// The outages of a period, rebuilt from `connectivity.csv`.
struct OutageData {
    /// The outages that overlap the period, in time order, all ended. One
    /// that began before the period keeps its real start.
    std::vector<core::Outage> outages;
    std::uint64_t rounds = 0;           ///< Probe rounds in the period.
    std::uint64_t down_rounds = 0;      ///< Of those, rounds without internet.
    std::uint64_t skipped = 0;          ///< Torn rows.
    std::uint64_t unreadable = 0;       ///< Rows that can't be read (see SpeedData).
    /// Rows older than the one before, near the period (see SpeedData). The
    /// replay ends an outage at each and starts afresh.
    std::uint64_t out_of_order = 0;
    bool file_exists = false;           ///< connectivity.csv was there.
};

/**
 * @brief Rebuilds the outages in @p period from `connectivity.csv`, the way
 *        `meerkat run` found them (core::assess(), core::OutageTracker).
 *
 * The rows of one round share its time; the ping whose target is the row's
 * gateway is the gateway ping. A down round inside a busy span counts as
 * inconclusive; inside a paused span, the slots without a round are fed as
 * inconclusive. Rounds from kReplayMargin before and after the period are
 * replayed as well; reading stops after that. An outage still open at the
 * end of the data ends there (core::OutageEnd::stopped).
 *
 * @param data_dir The data directory.
 * @param period   Which outages and rounds to report.
 * @param context  The replay context (replay_context()).
 * @return The outages and counts.
 * @throws as load_speed().
 */
OutageData load_outages(const std::filesystem::path& data_dir, const Period& period,
                        const ReplayContext& context);

}  // namespace meerkat::report
