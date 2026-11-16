/**
 * @file manifest.hpp
 * @brief The daily SHA-256 manifest: proof that the data files weren't edited
 *        after the fact.
 *
 * For each UTC day, `<data dir>/manifest/<YYYY-MM-DD>.sha256` lists the SHA-256
 * and size of the raw logs of that day and the day before (a test that
 * started before midnight may be logged after the previous manifest was
 * written), of the CSV files as they were when the manifest was written, and
 * of the previous manifest. The CSVs and the day's own raw log are
 * append-only and may still grow, so a later check hashes only the first
 * `size` bytes of them: if any old row was changed, that hash no longer
 * matches. The raw log of the day before is complete when the day is sealed
 * (each test is logged under the day it started), and manifests are written
 * once: those must not have grown either, so lines added to them later show
 * up (as `grew`).
 * Because every manifest includes the one before, removing, replacing or
 * extending a manifest breaks the chain too.
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

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
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
/// The official measurements, one row each (see store/official.hpp).
inline constexpr std::string_view kOfficialFile = "official.csv";

/// The CSV files rows are appended to: every manifest seals what they hold
/// so far, and a later one may find them longer.
inline constexpr std::array<std::string_view, 3> kGrowingCsvs{kSpeedFile, kConnectivityFile, kOfficialFile};

/**
 * @brief The raw log of a day, relative to the data directory.
 * @param day `YYYY-MM-DD`.
 * @return `raw/<day>.jsonl`.
 */
inline std::string raw_log_path(std::string_view day) {
    return std::string{kRawDir} + "/" + std::string{day} + ".jsonl";
}
/// The files the Breitbandmessung app exported, byte for byte.
inline constexpr std::string_view kOfficialDir = "official";
/// Folder of the manifests in the data directory.
inline constexpr std::string_view kManifestDir = "manifest";

/// How many past days `meerkat run` seals at most when it finds them
/// unsealed; older ones stay without a manifest.
inline constexpr std::chrono::days kSealBacklog{31};

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

    /**
     * @brief The entry of a file.
     * @param path Its path, relative to the data directory.
     * @return The entry, or nullptr if the manifest doesn't list it.
     */
    [[nodiscard]] const ManifestEntry* find(std::string_view path) const {
        for (const auto& e : entries) {
            if (e.path == path) {
                return &e;
            }
        }
        return nullptr;
    }
};

/**
 * @brief Reads a day in the form `YYYY-MM-DD`.
 * @param day The text.
 * @return The day, or nothing if @p day isn't a real date in exactly that
 *         form.
 */
std::optional<std::chrono::sys_days> parse_day(std::string_view day);

/**
 * @brief Checks that @p day is a real date in the form `YYYY-MM-DD`.
 * @param day The text.
 * @return True if valid.
 */
bool valid_day(std::string_view day);

/**
 * @brief The day of a raw log, from its file name.
 * @param name A file name in `raw/`, e.g. `2026-11-11.jsonl`.
 * @return The day (`YYYY-MM-DD`), or nothing if @p name isn't the name of
 *         a raw log.
 */
std::optional<std::string> raw_log_day(std::string_view name);

/**
 * @brief The day of a manifest, from its file name.
 * @param name A file name in `manifest/`, e.g. `2026-11-11.sha256`.
 * @return The day (`YYYY-MM-DD`), or nothing if @p name isn't the name of
 *         a manifest (temp files and notes aren't).
 */
std::optional<std::string> manifest_day(std::string_view name);

/// One file in `manifest/` or `raw/`, as listed.
struct FolderEntry {
    std::string name;                ///< The file name.
    std::optional<std::string> day;  ///< Its day, if the name is one of the folder's kind.
    /// A regular file (not a symlink, folder, …); only known for an entry
    /// with a day, false otherwise.
    bool regular = false;
};

/**
 * @brief Everything in `<data_dir>/manifest/`, for the commands that report
 *        what isn't a manifest (manifest_days() leaves it out).
 * @param data_dir The data directory.
 * @return The entries, sorted by name; empty if there is no `manifest/`.
 * @throws std::filesystem::filesystem_error if it exists but can't be listed.
 */
std::vector<FolderEntry> manifest_folder(const std::filesystem::path& data_dir);

/**
 * @brief Everything in `<data_dir>/raw/` (see manifest_folder()).
 * @param data_dir The data directory.
 * @return The entries, sorted by name; empty if there is no `raw/`.
 * @throws std::filesystem::filesystem_error if it exists but can't be listed.
 */
std::vector<FolderEntry> raw_folder(const std::filesystem::path& data_dir);

/**
 * @brief The days of the manifests in `<data_dir>/manifest/`: regular files
 *        named `<YYYY-MM-DD>.sha256` with a real date. Symlinks, temp files
 *        and anything else are left out.
 * @param data_dir The data directory.
 * @return The days, sorted; empty if there is no `manifest/`.
 * @throws std::filesystem::filesystem_error if `manifest/` exists but can't
 *         be listed.
 */
std::vector<std::string> manifest_days(const std::filesystem::path& data_dir);

/**
 * @brief The days of the raw logs in `<data_dir>/raw/`: regular files named
 *        `<YYYY-MM-DD>.jsonl` with a real date.
 * @param data_dir The data directory.
 * @return The days, sorted; empty if there is no `raw/`.
 * @throws std::filesystem::filesystem_error if `raw/` exists but can't be
 *         listed.
 */
std::vector<std::string> raw_log_days(const std::filesystem::path& data_dir);

/**
 * @brief The latest day with a manifest in `<data_dir>/manifest/`.
 *
 * Only regular files named `<YYYY-MM-DD>.sha256` with a real date count;
 * symlinks, temp files, notes and anything else there are ignored.
 * @param data_dir The data directory.
 * @param before   `YYYY-MM-DD`: only earlier days count. Empty: any day.
 * @return The day, or nothing if there is no such manifest.
 * @throws std::filesystem::filesystem_error if `manifest/` exists but can't
 *         be listed: silently restarting the chain would hide a gap.
 */
std::optional<std::string> latest_manifest_day(const std::filesystem::path& data_dir,
                                               std::string_view before = {});

/**
 * @brief The first day (UTC) with data: the first row of either CSV, or
 *        the oldest raw log.
 * @param data_dir The data directory.
 * @return The day, or nothing if there's no data yet.
 */
std::optional<std::chrono::sys_days> first_data_day(const std::filesystem::path& data_dir);

/**
 * @brief The recorded files a day's manifest covers, relative to the data
 *        directory: the raw logs of the day before and of the day, then
 *        kGrowingCsvs. sealed_files() adds the imported ones.
 * @param day `YYYY-MM-DD`.
 * @return The paths; build_manifest() skips those that don't exist.
 * @throws std::invalid_argument if @p day isn't valid.
 */
std::vector<std::string> daily_files(std::string_view day);

/**
 * @brief Everything a day's manifest covers: daily_files() and the files
 *        imported from the Breitbandmessung app.
 * @param day      `YYYY-MM-DD`.
 * @param official official_files() of the data directory (listed once by
 *                 a caller that seals several days).
 * @return The paths.
 * @throws std::invalid_argument if @p day isn't valid.
 */
std::vector<std::string> sealed_files(std::string_view day, const std::vector<std::string>& official);

/**
 * @brief Whether a file under `official/` may have this name: not empty, no
 *        leading `.` (a copy still being written), no `/`, no control
 *        characters (a manifest line can't hold them).
 * @param name The file name.
 * @return True if so.
 */
bool importable_name(std::string_view name);

/**
 * @brief The files exported by the Breitbandmessung app and kept under
 *        `official/`, relative to the data directory, sorted. Every manifest
 *        seals all of them: they are small and never change.
 * @param data_dir The data directory.
 * @throws std::system_error if `official/` exists but can't be listed.
 * @return The paths; empty if there is no `official/`.
 */
std::vector<std::string> official_files(const std::filesystem::path& data_dir);

/**
 * @brief Where a day's manifest goes.
 * @param data_dir The data directory.
 * @param day      `YYYY-MM-DD`.
 * @return `<data_dir>/manifest/<day>.sha256`.
 */
std::filesystem::path manifest_path(const std::filesystem::path& data_dir, std::string_view day);

/// The entries already hashed in one sealing run, by path. Every day of a
/// run is sealed at the same moment, so each file is hashed once and every
/// manifest of the run records that same snapshot of it (#36). Use one per
/// sealing run, never longer.
using HashCache = std::map<std::string, ManifestEntry>;

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
 * @param cache    Entries from earlier days of the same sealing run: a file
 *                 in it is taken from there (same size and hash), others
 *                 are hashed and added; may be null.
 * @return The manifest.
 * @throws std::invalid_argument if @p day isn't valid or a path isn't a
 *                               plain relative path.
 * @throws std::runtime_error    if a file exists but can't be checked or
 *                               read, or isn't a regular file, a symlink
 *                               included (leaving it out would seal the gap
 *                               for good).
 * @throws std::filesystem::filesystem_error if `manifest/` can't be listed.
 */
Manifest build_manifest(const std::filesystem::path& data_dir, std::string_view day,
                        core::Timestamp created, const std::vector<std::string>& files,
                        HashCache* cache = nullptr);

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
 * @throws std::runtime_error if something other than a regular file is at
 *         manifest_path() (a directory or a symlink, say): that isn't a
 *         manifest.
 * @note If only the final directory sync fails, the manifest is already in
 *       place: a retry then reports `file_exists`, which means it's done.
 */
std::filesystem::path write_manifest(const std::filesystem::path& data_dir, const Manifest& m);

/// Hashes of file prefixes: path (relative to the data directory) → length
/// → lowercase hex SHA-256 of that many bytes.
using PrefixHashes = std::map<std::string, std::map<std::uint64_t, std::string>>;

/// The check of one manifest entry against the files on disk.
struct EntryCheck {
    /// What was found.
    enum class Outcome {
        ok,       ///< The first `size` bytes still have the recorded hash,
                  ///< and a file that was complete then hasn't grown.
        changed,  ///< They don't: the sealed part was edited.
        grew,     ///< The sealed part is intact, but a file that was complete
                  ///< then (a manifest, or the raw log of an earlier day)
                  ///< has more now: lines were added later, by hand, or for
                  ///< a raw log by meerkat itself after the clock was set
                  ///< back by more than a day.
        missing,  ///< The file is gone, can't be read, or isn't a regular
                  ///< file any more (a symlink, say).
        shorter,  ///< The file is shorter than recorded: data was removed.
    };
    ManifestEntry entry;               ///< The entry checked.
    Outcome outcome = Outcome::missing;  ///< The result.
};

/**
 * @brief Re-hashes every file of @p m.
 *
 * Manifests and the raw logs of days before `m.day` must match at their
 * full size; the CSVs and the raw log of `m.day` only up to `size`.
 * @param data_dir The data directory.
 * @param m        The manifest.
 * @param known    Hashes already taken of file prefixes (sha256_prefixes()),
 *                 used instead of reading the file again; may be null.
 * @return One check per entry, in order.
 */
std::vector<EntryCheck> verify_manifest(const std::filesystem::path& data_dir, const Manifest& m,
                                        const PrefixHashes* known = nullptr);

}  // namespace meerkat::store
