/**
 * @file manifest.hpp
 * @brief The daily SHA-256 manifest: proof that the data files weren't edited
 *        after the fact.
 *
 * For each UTC day, `<data dir>/manifest/<YYYY-MM-DD>.sha256` lists the SHA-256
 * and size of the raw logs of that day and the day before (a test that
 * started before midnight may be logged after the previous manifest was
 * written), of the CSV files as they were when the manifest was written, and
 * of the previous manifest. The data files are append-only, so a later check
 * hashes only the first `size` bytes of them: if any old row was changed,
 * that hash no longer matches. Manifests are written once, so they must
 * match exactly. Because every manifest includes the one before, removing,
 * replacing or extending a manifest breaks the chain too.
 *
 * File format (LF line endings, read-only once written):
 * @code
 * # meerkat manifest v1
 * # day 2026-11-11
 * # created 2026-11-12T00:00:05Z
 * <64 hex digits>  <size in bytes>  <path relative to the data dir>
 * @endcode
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/time.hpp"

namespace meerkat::store {

/// Name of the speed-test CSV in the data directory.
inline constexpr std::string_view kSpeedFile = "speed.csv";
/// Name of the connectivity CSV in the data directory.
inline constexpr std::string_view kConnectivityFile = "connectivity.csv";
/// Folder of the raw log in the data directory.
inline constexpr std::string_view kRawDir = "raw";
/// Folder of the manifests in the data directory.
inline constexpr std::string_view kManifestDir = "manifest";

/// One file in a manifest.
struct ManifestEntry {
    std::string sha256;      ///< Lowercase hex of the first `size` bytes.
    std::uint64_t size = 0;  ///< How many bytes were hashed.
    std::string path;        ///< Relative to the data directory, e.g. `raw/2026-11-11.jsonl`.
};

/// One day's manifest.
struct Manifest {
    std::string day;                     ///< `YYYY-MM-DD` (UTC).
    core::Timestamp created{};           ///< When it was written.
    std::vector<ManifestEntry> entries;  ///< The files, in the order they were hashed.
};

/**
 * @brief Checks that @p day is a real date in the form `YYYY-MM-DD`.
 * @param day The text.
 * @return True if valid.
 */
bool valid_day(std::string_view day);

/**
 * @brief The files a day's manifest covers, relative to the data directory:
 *        the raw logs of the day before and of the day, then speed.csv and
 *        connectivity.csv.
 * @param day `YYYY-MM-DD`.
 * @return The paths; build_manifest() skips those that don't exist.
 * @throws std::invalid_argument if @p day isn't valid.
 */
std::vector<std::string> daily_files(std::string_view day);

/**
 * @brief Where a day's manifest goes.
 * @param data_dir The data directory.
 * @param day      `YYYY-MM-DD`.
 * @return `<data_dir>/manifest/<day>.sha256`.
 */
std::filesystem::path manifest_path(const std::filesystem::path& data_dir, std::string_view day);

/**
 * @brief Hashes the files for @p day's manifest.
 *
 * The first entry is the latest existing manifest of an earlier day (the
 * chain), if there is one. Then each of @p files that exists, with its size
 * at this moment; missing ones are left out.
 *
 * @param data_dir The data directory.
 * @param day      `YYYY-MM-DD`.
 * @param created  The manifest's creation time.
 * @param files    Paths relative to @p data_dir; see daily_files().
 * @return The manifest.
 * @throws std::invalid_argument if @p day isn't valid or a path isn't a
 *                               plain relative path.
 * @throws std::runtime_error    if a file exists but can't be checked or
 *                               read, or isn't a regular file (leaving it
 *                               out would seal the gap for good).
 * @throws std::filesystem::filesystem_error if `manifest/` can't be listed.
 */
Manifest build_manifest(const std::filesystem::path& data_dir, std::string_view day,
                        core::Timestamp created, const std::vector<std::string>& files);

/**
 * @brief The text of a manifest file.
 * @param m The manifest.
 * @return The file content (see the format in this file's description).
 */
std::string format_manifest(const Manifest& m);

/**
 * @brief Parses a manifest file.
 * @param text The file content.
 * @return The manifest, or `std::nullopt` if it isn't a valid v1 manifest
 *         (also when a path is absolute or contains `..`).
 */
std::optional<Manifest> parse_manifest(std::string_view text);

/**
 * @brief Writes @p m to manifest_path(), atomically and only once.
 *
 * The text goes to a temporary file of this process (mode 0440), is synced,
 * and is then renamed into place with `RENAME_NOREPLACE` (or hard-linked, on
 * filesystems without that flag), so a reader never sees half a manifest
 * and an existing one is never replaced. Filesystems with neither (rare)
 * get an error.
 *
 * @param data_dir The data directory; `manifest/` is created if needed.
 * @param m        The manifest.
 * @return The path written.
 * @throws std::system_error with `std::errc::file_exists` if that day's
 *         manifest already exists; other `std::system_error`s or
 *         `std::filesystem::filesystem_error`s if writing fails.
 * @note If only the final directory sync fails, the manifest is already in
 *       place: a retry then reports `file_exists`, which means it's done.
 */
std::filesystem::path write_manifest(const std::filesystem::path& data_dir, const Manifest& m);

/// The check of one manifest entry against the files on disk.
struct EntryCheck {
    /// What was found.
    enum class Outcome {
        ok,       ///< The first `size` bytes still have the recorded hash
                  ///< (a manifest must also not have grown).
        changed,  ///< They don't, or a manifest grew: the file was edited.
        missing,  ///< The file is gone or can't be read.
        shorter,  ///< The file is shorter than recorded: data was removed.
    };
    ManifestEntry entry;               ///< The entry checked.
    Outcome outcome = Outcome::missing;  ///< The result.
};

/**
 * @brief Re-hashes every file of @p m.
 * @param data_dir The data directory.
 * @param m        The manifest.
 * @return One check per entry, in order.
 */
std::vector<EntryCheck> verify_manifest(const std::filesystem::path& data_dir, const Manifest& m);

}  // namespace meerkat::store
