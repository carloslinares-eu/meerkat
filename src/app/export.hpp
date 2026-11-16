/**
 * @file export.hpp
 * @brief `meerkat export`: one ZIP file for the provider with the report,
 *        the data, the raw logs, the manifests and the `meerkat verify`
 *        result.
 */
#pragma once

#include <ostream>

#include "app/report.hpp"
#include "core/config.hpp"

namespace meerkat::app {

/**
 * @brief Writes the export bundle.
 *
 * The ZIP holds one folder `meerkat-export-FROM-to-TO/` with:
 * - `README.txt`: what is in it and how to check it;
 * - `report.html`: the report for the days asked for (as `meerkat report`);
 * - `verify.txt`: the check of the whole evidence trail (`meerkat verify`),
 *   made just before;
 * - `data/`: speed.csv, connectivity.csv, `raw/` and `manifest/`, byte for
 *   byte, so `meerkat verify --data-dir data` works on it. Always the whole
 *   recording: the manifests seal the files as a whole. A file still being
 *   written is cut at its last complete line;
 * - `SHA256SUMS`: of every file above, for `sha256sum -c`.
 *
 * The file is written to a temporary name and renamed, readable by its
 * owner only. Problems found by the check don't stop the export: they are
 * in `verify.txt` and printed.
 * @param config  The settings.
 * @param options `--from`, `--to` (the report's days) and `-o` (default
 *                `meerkat-export-FROM-to-TO.zip`).
 * @param env     Zone, clock and version.
 * @param out     Where the summary goes.
 * @param err     Where errors go.
 * @return 0 (also when the check found problems); 1 if the data can't be
 *         read or the file can't be written; 2 for a bad range or an
 *         output file in the data directory.
 */
int export_command(const core::Config& config, const ReportOptions& options, const ReportEnv& env,
                   std::ostream& out, std::ostream& err);

}  // namespace meerkat::app
