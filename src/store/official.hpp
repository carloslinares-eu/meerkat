/**
 * @file official.hpp
 * @brief The official measurements in the data directory: the files the
 *        Breitbandmessung app exported, kept unchanged under `official/`,
 *        and their measurements in `official.csv` (task 3.6).
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "probe/bbm.hpp"
#include "store/csv.hpp"
#include "store/manifest.hpp"

namespace meerkat::store {

/// The largest file `import-bbm` takes (an export or protocol is far smaller).
inline constexpr std::size_t kMaxOfficialFile = 16 * 1024 * 1024;

/**
 * @brief The layout of `official.csv`.
 *
 * Columns: time (UTC), source (`breitbandmessung`), number, download_mbps,
 * upload_mbps, latency_ms, contract_down_min, contract_down_normal,
 * contract_down_max, contract_up_min, contract_up_normal, contract_up_max,
 * connection (as the app saw it, e.g. `LAN`), on_power, nic_sufficient,
 * data_rate_limited (`yes`/`no`), test_id, app_version, and file (the
 * export under `official/` the row came from).
 * @return The schema (version 1).
 */
const Schema& official_schema();

/**
 * @brief The `official.csv` row for one measurement.
 * @param m    The measurement.
 * @param file The stored export, relative to the data directory.
 * @return One field per column of official_schema().
 */
std::vector<std::string> official_row(const probe::OfficialMeasurement& m, const std::string& file);

/// What import_official() did.
struct OfficialImport {
    std::string stored;          ///< The copy, relative to the data directory (`official/<name>`).
    bool already_stored = false; ///< The same file was imported before.
    bool pdf = false;            ///< A PDF (the protocol): kept, not read.
    std::size_t measurements = 0;  ///< Measurements in the export.
    std::size_t added = 0;         ///< Of those, new in official.csv (by Test-ID).
};

/**
 * @brief Imports a file exported by the Breitbandmessung app.
 *
 * A CSV export is read first (so a file that isn't one is refused before
 * anything is written), then copied unchanged to `official/<its name>`
 * (mode 0440), and its measurements whose Test-ID isn't in `official.csv`
 * yet are appended. A PDF (the measurement protocol) is only copied, and
 * only if the app made it (its Creator names `Festnetz-Check`). The
 * next daily manifest seals both (see daily_files()). Importing the same
 * file again adds nothing. When run as root, the new files get the data
 * directory's owner and group, so the service can go on writing.
 * @param data_dir The data directory; it must exist.
 * @param source   The exported file.
 * @param to_utc   Converts the export's local times.
 * @return What was done.
 * @throws probe::BbmFormatError if a CSV isn't an export meerkat can read.
 * @throws std::runtime_error if `official/<name>` holds a different file,
 *         the file is too big, or a PDF the app didn't make; std::system_error if a file can't be
 *         read or written.
 */
OfficialImport import_official(const std::filesystem::path& data_dir, const std::filesystem::path& source,
                               const probe::LocalToUtc& to_utc);

}  // namespace meerkat::store
