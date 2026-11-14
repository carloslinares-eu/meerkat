/**
 * @file data.hpp
 * @brief Loads what `meerkat report` works on from the data directory:
 *        the speed tests of a period, and the outages rebuilt from the
 *        connectivity probes.
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
};

/// The speed tests of a period.
struct SpeedData {
    std::vector<SpeedRow> rows;    ///< In file order (which is time order).
    std::uint64_t skipped = 0;     ///< Torn rows (see store::CsvReadStats::skipped).
    /// Complete rows whose time, status or link type can't be read: never
    /// written by meerkat, so the file was edited. Reported, not used.
    std::uint64_t unreadable = 0;
    bool file_exists = false;      ///< speed.csv was there.
};

/**
 * @brief Reads the speed tests in @p period from `speed.csv`.
 * @param data_dir The data directory.
 * @param period   Which tests.
 * @return The tests; empty (`file_exists` false) without a speed.csv.
 * @throws store::SchemaMismatch, std::system_error, sys::NotRegularFile as
 *         store::read_csv() (except for a missing file).
 */
SpeedData load_speed(const std::filesystem::path& data_dir, const Period& period);

/// After a speed test starts, connectivity rounds that fail are taken as
/// inconclusive for this long: a test fills the line and can drop pings,
/// which the recorder knows at the time but the CSV doesn't say. An Ookla
/// test takes about 30 s, 90 s leaves room for a slow one.
inline constexpr std::chrono::seconds kSpeedTestShadow{90};

/// The outages of a period, rebuilt from `connectivity.csv`.
struct OutageData {
    std::vector<core::Outage> outages;  ///< In time order, all ended.
    std::uint64_t rounds = 0;           ///< Probe rounds in the period.
    std::uint64_t down_rounds = 0;      ///< Of those, rounds without internet.
    std::uint64_t skipped = 0;          ///< Torn rows.
    std::uint64_t unreadable = 0;       ///< Rows that can't be read (see SpeedData).
    bool file_exists = false;           ///< connectivity.csv was there.
};

/**
 * @brief Rebuilds the outages in @p period from `connectivity.csv`, the way
 *        `meerkat run` found them (core::assess(), core::OutageTracker).
 *
 * The rows of one round share its time; the ping whose target is the row's
 * gateway is the gateway ping. A down round within kSpeedTestShadow after
 * one of @p speed_starts counts as inconclusive. An outage still open at
 * the end of the data ends there (core::OutageEnd::stopped).
 *
 * @param data_dir     The data directory.
 * @param period       Which rounds.
 * @param settings     Outage rules; see core::OutageSettings::for_interval().
 * @param speed_starts Start times of the speed tests, sorted.
 * @return The outages and counts.
 * @throws as load_speed().
 */
OutageData load_outages(const std::filesystem::path& data_dir, const Period& period,
                        const core::OutageSettings& settings,
                        const std::vector<core::Timestamp>& speed_starts);

}  // namespace meerkat::report
