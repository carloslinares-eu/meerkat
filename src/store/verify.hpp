/**
 * @file verify.hpp
 * @brief Checks the whole evidence trail of a data directory: every
 *        manifest, the chain between them, and the data no manifest covers
 *        (`meerkat verify`).
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "store/manifest.hpp"

namespace meerkat::store {

/// One manifest file and the check of everything it lists.
struct ManifestCheck {
    std::string day;                    ///< `YYYY-MM-DD`, from the file name.
    std::string sha256;                 ///< Of the manifest file; empty if it can't be read.
    std::optional<Manifest> manifest;   ///< Its content; empty if it can't be read.
    std::vector<EntryCheck> entries;    ///< verify_manifest() of it, in order.
};

/// Something verify_data() found beyond the per-file outcomes.
struct VerifyFinding {
    /// How serious it is.
    enum class Level {
        note,     ///< Not (yet) a gap in the evidence: e.g. today, not sealed yet.
        problem,  ///< The trail has a hole or was edited.
    };
    Level level = Level::problem;  ///< How serious.
    std::string day;               ///< The day it is about (`YYYY-MM-DD`); empty: none.
    std::string text;              ///< What was found, in a sentence.
};

/// The result of verify_data().
struct VerifyResult {
    std::vector<ManifestCheck> manifests;   ///< All manifests, in date order.
    std::vector<VerifyFinding> findings;    ///< Chain, gaps, unsealed data, odd files.

    /// @return The number of problems: findings of level `problem` and
    ///         entries whose outcome isn't `ok`.
    std::size_t problems() const;
    /// @return Whether there are no problems (notes are fine).
    bool ok() const { return problems() == 0; }
};

/**
 * @brief Checks the evidence trail in @p data_dir.
 *
 * - Every file `manifest/<YYYY-MM-DD>.sha256` is read and parsed, and each
 *   file it lists is checked with verify_manifest() (ok / changed / grew /
 *   shorter / missing). A manifest that can't be parsed, says it is for
 *   another day, or isn't a regular file is a problem.
 * - The chain: each manifest after the first must list the one before it
 *   (and only that one); the first must list none, or one that's gone.
 * - Gaps: a day between two manifests without its own manifest can't be
 *   sealed any more.
 * - Unsealed days: from the first day with data (first_data_day()) to the
 *   day before yesterday, a day after the last manifest without one is a
 *   problem (`meerkat run` seals them when it runs); yesterday's and
 *   today's are notes.
 * - Raw logs: `raw/<D>.jsonl` must be in the manifest of the day after D if
 *   that one exists (else it was created after it was sealed). A raw log
 *   dated after today is a note (the clock was wrong).
 * - What was added to the CSVs after the last manifest is a note.
 *
 * Reads only; `meerkat run` may keep recording meanwhile.
 * @param data_dir The data directory.
 * @param today    Today (UTC).
 * @return The result.
 * @throws std::filesystem::filesystem_error if `manifest/` or `raw/` exist
 *         but can't be listed.
 */
VerifyResult verify_data(const std::filesystem::path& data_dir, std::chrono::sys_days today);

/**
 * @brief A short word for an outcome, as `meerkat verify` prints it.
 * @param o The outcome.
 * @return `ok`, `changed`, `grew`, `shorter` or `missing`.
 */
std::string outcome_name(EntryCheck::Outcome o);

/**
 * @brief What an outcome means, for someone who reads the result.
 * @param o The outcome.
 * @return A short explanation, e.g. "the sealed part was edited".
 */
std::string outcome_meaning(EntryCheck::Outcome o);

}  // namespace meerkat::store
