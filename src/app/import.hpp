/**
 * @file import.hpp
 * @brief `meerkat import-bbm`: brings the official measurements of the
 *        Breitbandmessung app into the data directory (task 3.6).
 */
#pragma once

#include <filesystem>
#include <ostream>
#include <vector>

#include "probe/bbm.hpp"

namespace meerkat::app {

/**
 * @brief Runs `meerkat import-bbm FILE...`.
 *
 * Each file is a CSV export or the PDF protocol of the app. See
 * store::import_official() for what happens to it. Files are imported one
 * by one; a file that fails is reported and the others still go in.
 *
 * Run as root on a data directory another user owns (the service's), it
 * reads the files first and then becomes that user for good
 * (sys::become()), so it can't be led outside the data directory (#133)
 * and the new files belong to the service.
 * @param data_dir The data directory; it must exist.
 * @param files    The exported files.
 * @param to_utc   Converts the export's local times.
 * @param out      Where the result goes.
 * @param err      Where errors go.
 * @return 0 if every file was imported (or was already), 1 otherwise.
 */
int import_bbm_command(const std::filesystem::path& data_dir, const std::vector<std::filesystem::path>& files,
                       const probe::LocalToUtc& to_utc, std::ostream& out, std::ostream& err);

}  // namespace meerkat::app
