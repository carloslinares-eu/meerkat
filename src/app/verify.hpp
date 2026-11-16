/**
 * @file verify.hpp
 * @brief `meerkat verify`: checks the evidence trail before it's handed over.
 */
#pragma once

#include <filesystem>
#include <ostream>
#include <string>

#include "core/time.hpp"
#include "store/verify.hpp"

namespace meerkat::app {

/**
 * @brief The result of store::verify_data() as text: each manifest with
 *        every file it lists and its outcome, then the problems, the notes
 *        and a one-line result.
 * @param r        The result.
 * @param data_dir The data directory it is about (shown in the first line).
 * @return The text; control characters in names are made visible.
 */
std::string verify_text(const store::VerifyResult& r, const std::filesystem::path& data_dir);

/**
 * @brief Runs `meerkat verify`.
 * @param data_dir The data directory.
 * @param now      The current time (today, in UTC, decides what may still
 *                 be unsealed).
 * @param out      Where the result goes.
 * @param err      Where errors go.
 * @param as_of    `--as-of YYYY-MM-DD`: check as if it were that day (UTC),
 *                 e.g. an export bundle as of the day it was made; not
 *                 after today. Empty: today.
 * @return 0 if there is no problem; 1 if there is one, or the data
 *         directory can't be read; 2 for a bad `--as-of`.
 */
int verify_command(const std::filesystem::path& data_dir, core::Timestamp now, std::ostream& out,
                   std::ostream& err, const std::string& as_of = {});

}  // namespace meerkat::app
