/**
 * @file verify.hpp
 * @brief Checks the whole evidence trail of a data directory: every
 *        manifest, the chain between them, and the data no manifest covers
 *        (`meerkat verify`).
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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

/// One manifest file as read, before its entries are checked.
struct ManifestFile {
    std::string sha256;                ///< Of the file; empty if it can't be read.
    std::optional<Manifest> manifest;  ///< Its content; empty if it can't be read or parsed.
    bool missing = false;              ///< There is no such file.
    /// Why it can't be used, e.g. `can't be read: …`; empty if it can.
    std::string problem;
};

/**
 * @brief Reads, hashes and parses the manifest of @p day, the same way for
 *        `verify` and the report's manifest table.
 * @param data_dir The data directory.
 * @param day      `YYYY-MM-DD`.
 * @return The file; never throws for a file that can't be read or parsed.
 */
ManifestFile read_manifest_file(const std::filesystem::path& data_dir, const std::string& day);

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
    std::chrono::sys_days today{};          ///< The day it was checked as of (UTC).

    /// @return The number of problems: findings of level `problem` and
    ///         entries whose outcome isn't `ok`.
    std::size_t problems() const;
    /// @return Whether there are no problems (notes are fine).
    bool ok() const { return problems() == 0; }

    /// @return Every size the readable manifests sealed, by path (in
    ///         manifest order): what the checks hash, and what an export
    ///         must keep of each file.
    std::map<std::string, std::vector<std::uint64_t>> sealed_sizes() const;
};

/**
 * @brief Checks the evidence trail in @p data_dir.
 *
 * - Every file `manifest/<YYYY-MM-DD>.sha256` is read and parsed, and each
 *   file it lists is checked with verify_manifest() (ok / changed / grew /
 *   shorter / missing). A manifest that can't be parsed, says it is for
 *   another day, or isn't a regular file is a problem.
 * - The chain: each manifest after the first must list the one before it
 *   (and only that one). If the first lists one, that one is gone: its
 *   entry is `missing`, and a note says the start of the chain was removed.
 * - Gaps: a day between two manifests without its own manifest can't be
 *   sealed any more.
 * - Unsealed days: from the first day with data (first_data_day()) on, a
 *   day before the first manifest can't be sealed any more. After the last
 *   one, days up to the day before yesterday are problems: `meerkat run`
 *   seals them when it runs, but only the last 31 days, so older ones can't
 *   be sealed any more. Yesterday is a note.
 * - A manifest dated today or later (written with a wrong clock) is a
 *   problem: `meerkat run` seals nothing after it until the clock passes it.
 * - Raw logs: `raw/<D>.jsonl` must be in the manifest of the day after D if
 *   that one exists (else it was created after it was sealed). If only D's
 *   own manifest exists and doesn't list it, that's a note (a test that
 *   began before midnight logs after it). A raw log dated after today is a
 *   note (the clock was wrong).
 * - What was added to the CSVs after the last manifest is a note.
 *
 * The CSVs are hashed once for all manifests (sha256_prefixes()).
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
